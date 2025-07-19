#include "CameraStreamer.h"
#include "shared_config.h"
#include <iostream>
#include <chrono>
#include <opencv2/videoio.hpp> // For cv::VideoCapture
#include <portaudio.h>


#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/error.h>
}

// 辅助函数 (可以放到一个公共的 .h 文件中)
void log_ffmpeg_error_cs(const std::string& message, int error_code) {
    char err_buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(error_code, err_buf, AV_ERROR_MAX_STRING_SIZE);
    std::cerr << "[摄像头推流] FFmpeg 错误: " << message << " (code " << error_code << "): " << err_buf << std::endl;
}

CameraStreamer::CameraStreamer(
    boost::asio::io_context& io_context,
    std::shared_ptr<AdaptiveStreamController> controller,
    udp::endpoint client_endpoint)
    : m_control_block(std::make_shared<StreamControlBlock>()),
    m_video_socket(io_context),
    m_audio_socket(io_context),
    m_controller(controller)
{
    m_video_socket.open(udp::v4());
    m_audio_socket.open(udp::v4());
    m_client_video_endpoint = udp::endpoint(client_endpoint.address(), AppConfig::VIDEO_PORT);
    m_client_audio_endpoint = udp::endpoint(client_endpoint.address(), AppConfig::AUDIO_PORT);
}

CameraStreamer::~CameraStreamer()
{
    stop();
    cleanup();
}

void CameraStreamer::start()
{
    if (initialize_video() && initialize_audio()) {
        m_control_block->running = true;
        m_start_time = std::chrono::steady_clock::now();

        // 启动音频采集线程
        m_audio_thread = std::thread(&CameraStreamer::audio_stream_loop, this);

        // 在当前线程运行视频采集和编码循环
        video_stream_loop();
    }
}

void CameraStreamer::stop()
{
    m_control_block->running = false;
    if (m_audio_thread.joinable()) {
        m_audio_thread.join();
    }
}

void CameraStreamer::seek(double time_sec)
{
    std::cout << "[摄像头推流] 警告: 直播流不支持跳转 (seek) 操作。" << std::endl;
}

bool CameraStreamer::initialize_video()
{
    m_video_capture = std::make_unique<cv::VideoCapture>(0); // 0 代表默认摄像头
    if (!m_video_capture->isOpened()) {
        std::cerr << "[摄像头推流] 错误: 无法打开摄像头。" << std::endl;
        return false;
    }

    m_frame_size.width = static_cast<int>(m_video_capture->get(cv::CAP_PROP_FRAME_WIDTH));
    m_frame_size.height = static_cast<int>(m_video_capture->get(cv::CAP_PROP_FRAME_HEIGHT));
    std::cout << "[摄像头推流] 摄像头已打开，分辨率: " << m_frame_size.width << "x" << m_frame_size.height << std::endl;

    m_yuv_frame = av_frame_alloc();
    m_encoded_packet = av_packet_alloc();
    if (!m_yuv_frame || !m_encoded_packet) return false;

    // 为YUV帧分配内存
    m_yuv_frame->format = AV_PIX_FMT_YUV420P;
    m_yuv_frame->width = m_frame_size.width;
    m_yuv_frame->height = m_frame_size.height;
    if (av_frame_get_buffer(m_yuv_frame, 0) < 0) {
        std::cerr << "[摄像头推流] 错误: 无法为YUV帧分配内存" << std::endl;
        return false;
    }

    return true;
}

bool CameraStreamer::initialize_audio()
{
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "[摄像头推流] PortAudio 初始化错误: " << Pa_GetErrorText(err) << std::endl;
        return false;
    }

    err = Pa_OpenDefaultStream(
        &m_audio_stream,
        AppConfig::AUDIO_CHANNELS, // input channels
        0,                        // no output channels
        paInt16,
        AppConfig::AUDIO_RATE,
        AppConfig::AUDIO_CHUNK_SAMPLES,
        nullptr, nullptr
    );

    if (err != paNoError) {
        std::cerr << "[摄像头推流] PortAudio 打开流错误: " << Pa_GetErrorText(err) << std::endl;
        Pa_Terminate();
        return false;
    }
    return true;
}


void CameraStreamer::cleanup()
{
    // 音频清理
    if (m_audio_stream) {
        Pa_StopStream(m_audio_stream);
        Pa_CloseStream(m_audio_stream);
        m_audio_stream = nullptr;
    }
    Pa_Terminate();

    // 视频清理
    if (m_video_capture && m_video_capture->isOpened()) {
        m_video_capture->release();
    }
    if (m_sws_ctx_bgr_to_yuv) { sws_freeContext(m_sws_ctx_bgr_to_yuv); m_sws_ctx_bgr_to_yuv = nullptr; }
    if (m_video_encoder_ctx) { avcodec_free_context(&m_video_encoder_ctx); }
    if (m_yuv_frame) { av_frame_free(&m_yuv_frame); }
    if (m_encoded_packet) { av_packet_free(&m_encoded_packet); }

    std::cout << "[摄像头推流] 资源已清理。" << std::endl;
}

void CameraStreamer::video_stream_loop()
{
    cv::Mat bgr_frame;
    while (m_control_block->running)
    {
        StreamStrategy strategy = m_controller->get_current_strategy();
        auto start_capture_time = std::chrono::steady_clock::now();

        // 1. 从摄像头捕获一帧
        if (!m_video_capture->read(bgr_frame) || bgr_frame.empty()) {
            std::cerr << "[摄像头推流] 无法从摄像头读取帧。" << std::endl;
            break;
        }

        // 2. 将 BGR 转换为 YUV420P
        m_sws_ctx_bgr_to_yuv = sws_getCachedContext(m_sws_ctx_bgr_to_yuv,
            m_frame_size.width, m_frame_size.height, AV_PIX_FMT_BGR24,
            m_frame_size.width, m_frame_size.height, AV_PIX_FMT_YUV420P,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

        const int stride[] = { static_cast<int>(bgr_frame.step[0]) };
        sws_scale(m_sws_ctx_bgr_to_yuv, &bgr_frame.data, stride, 0, m_frame_size.height,
            m_yuv_frame->data, m_yuv_frame->linesize);

        // 3. 编码并发送
        auto now = std::chrono::steady_clock::now();
        int64_t timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_start_time).count();
        m_yuv_frame->pts = timestamp_ms;

        encode_and_send_video(m_yuv_frame);

        // 4. 根据策略控制帧率
        auto process_duration = std::chrono::steady_clock::now() - start_capture_time;
        auto target_interval = std::chrono::microseconds(1000000 / strategy.fps_limit);
        if (target_interval > process_duration) {
            std::this_thread::sleep_for(target_interval - process_duration);
        }
    }
    std::cout << "[摄像头推流] 视频循环结束。" << std::endl;
}


void CameraStreamer::audio_stream_loop()
{
    Pa_StartStream(m_audio_stream);
    std::vector<int16_t> audio_buffer(AppConfig::AUDIO_CHUNK_SAMPLES * AppConfig::AUDIO_CHANNELS);

    while (m_control_block->running)
    {
        PaError err = Pa_ReadStream(m_audio_stream, audio_buffer.data(), AppConfig::AUDIO_CHUNK_SAMPLES);
        if (err != paNoError && err != paInputOverflowed) {
            // 忽略溢出错误，但报告其他错误
            std::cerr << "[摄像头推流] PortAudio 读取错误: " << Pa_GetErrorText(err) << std::endl;
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        int64_t timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_start_time).count();

        AVPacket audio_packet = { 0 };
        audio_packet.data = reinterpret_cast<uint8_t*>(audio_buffer.data());
        audio_packet.size = audio_buffer.size() * sizeof(int16_t);

        send_packet_fragmented(m_audio_socket, m_client_audio_endpoint, m_audio_seq, timestamp_ms, &audio_packet, false);
        m_audio_seq++;
    }
    std::cout << "[摄像头推流] 音频循环结束。" << std::endl;
}

bool CameraStreamer::initialize_video_encoder(const StreamStrategy& strategy)
{
    if (m_video_encoder_ctx) {
        avcodec_free_context(&m_video_encoder_ctx);
    }
    // 使用 NVCODEC 编码器
    const AVCodec* encoder = avcodec_find_encoder_by_name("hevc_nvenc");
    if (!encoder) {
        std::cerr << "[摄像头推流] 错误: 找不到 hevc_nvenc 编码器。" << std::endl;
        return false;
    }
    m_video_encoder_ctx = avcodec_alloc_context3(encoder);
    if (!m_video_encoder_ctx) return false;
    m_video_encoder_ctx->width = m_frame_size.width;
    m_video_encoder_ctx->height = m_frame_size.height;
    m_video_encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    m_video_encoder_ctx->time_base = { 1, 1000 };
    m_video_encoder_ctx->bit_rate = static_cast<int64_t>(m_base_bitrate * strategy.multiplier);
    m_video_encoder_ctx->framerate = { strategy.fps_limit, 1 };
    // 设置 NVCODEC 编码器的一些参数
    av_opt_set(m_video_encoder_ctx->priv_data, "preset", "p1", 0);
    av_opt_set(m_video_encoder_ctx->priv_data, "tune", "hq", 0);
    int ret = avcodec_open2(m_video_encoder_ctx, encoder, nullptr);
    if (ret < 0) { log_ffmpeg_error_cs("无法打开视频编码器", ret); return false; }
    std::cout << "[摄像头推流] 视频编码器已初始化。" << std::endl;
    return true;
}


void CameraStreamer::encode_and_send_video(AVFrame* frame)
{
    // ... (这部分代码与 FileStreamer 几乎相同)
    StreamStrategy current_strategy = m_controller->get_current_strategy();
    if (!m_video_encoder_ctx || current_strategy.fps_limit != m_last_strategy.fps_limit) {
        if (!initialize_video_encoder(current_strategy)) {
            std::cerr << "[摄像头推流] 致命错误: 视频编码器初始化失败" << std::endl;
            stop();
            return;
        }
        m_last_strategy = current_strategy;
    }
    int ret = avcodec_send_frame(m_video_encoder_ctx, frame);
    if (ret < 0) return;
    while (ret >= 0) {
        ret = avcodec_receive_packet(m_video_encoder_ctx, m_encoded_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;
        int num_sent = send_packet_fragmented(m_video_socket, m_client_video_endpoint, m_video_seq, m_encoded_packet->pts, m_encoded_packet, true);
        m_video_seq += num_sent;
        av_packet_unref(m_encoded_packet);
    }
}

int CameraStreamer::send_packet_fragmented(udp::socket& sock, const udp::endpoint& endpoint, uint32_t seq, int64_t ts, const AVPacket* packet, bool is_video)
{
    // ... (这部分代码与 FileStreamer 完全相同，必须重构！)
    const uint8_t* packet_data = packet->data;
    size_t packet_size = packet->size;
    if (is_video) {
        const int HEADER_SIZE = 7;
        const size_t MAX_PAYLOAD_SIZE = 1400 - HEADER_SIZE;
        uint16_t video_seq_16 = static_cast<uint16_t>(seq);
        if (packet_size <= MAX_PAYLOAD_SIZE) {
            std::vector<uint8_t> buffer(HEADER_SIZE + packet_size);
            uint16_t seq_net = htons(video_seq_16);
            int32_t ts_net = htonl(static_cast<int32_t>(ts));
            memcpy(buffer.data(), &seq_net, 2);
            memcpy(buffer.data() + 2, &ts_net, 4);
            buffer[6] = 0xC0;
            memcpy(buffer.data() + HEADER_SIZE, packet_data, packet_size);
            sock.send_to(boost::asio::buffer(buffer), endpoint);
            return 1;
        }
        else {
            int num_packets = 0;
            size_t offset = 0;
            while (offset < packet_size) {
                size_t chunk_size = std::min(packet_size - offset, MAX_PAYLOAD_SIZE);
                std::vector<uint8_t> buffer(HEADER_SIZE + chunk_size);
                uint8_t frag_info = 0;
                if (offset == 0) frag_info |= 0x80;
                if (offset + chunk_size >= packet_size) frag_info |= 0x40;
                uint16_t current_seq_net = htons(video_seq_16 + num_packets);
                int32_t ts_net = htonl(static_cast<int32_t>(ts));
                memcpy(buffer.data(), &current_seq_net, 2);
                memcpy(buffer.data() + 2, &ts_net, 4);
                buffer[6] = frag_info;
                memcpy(buffer.data() + HEADER_SIZE, packet_data + offset, chunk_size);
                sock.send_to(boost::asio::buffer(buffer), endpoint);
                offset += chunk_size;
                num_packets++;
            }
            return num_packets;
        }
    }
    else {
        const int HEADER_SIZE = 8;
        std::vector<uint8_t> buffer(HEADER_SIZE + packet_size);
        uint32_t seq_net = htonl(seq);
        int32_t ts_net = htonl(static_cast<int32_t>(ts));
        memcpy(buffer.data(), &seq_net, 4);
        memcpy(buffer.data() + 4, &ts_net, 4);
        memcpy(buffer.data() + HEADER_SIZE, packet_data, packet_size);
        sock.send_to(boost::asio::buffer(buffer), endpoint);
        return 1;
    }
}