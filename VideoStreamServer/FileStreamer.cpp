// FileStreamer.cpp

#include "FileStreamer.h"
#include "shared_config.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <iomanip>
#include <string> // Added for std::string conversion

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h> // For av_error
#include <libavutil/hwcontext.h> // For hardware context functions
#include <libavutil/pixfmt.h>    // For AVPixelFormat
#include <libswscale/swscale.h> // Added for SwsContext and sws_scale
}


void log_ffmpeg_error(const std::string& message, int error_code) {
    char err_buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(error_code, err_buf, AV_ERROR_MAX_STRING_SIZE);
    std::cerr << "[推流] FFmpeg 错误: " << message << " (code " << error_code << "): " << err_buf << std::endl;
}

// ... (构造函数, 析构函数, start, stop, seek 保持不变) ...
FileStreamer::FileStreamer(
    boost::asio::io_context& io_context,
    std::shared_ptr<AdaptiveStreamController> controller,
    const std::string& video_path,
    udp::endpoint client_endpoint)
    : m_control_block(std::make_shared<StreamControlBlock>()),
    m_video_socket(io_context),
    m_audio_socket(io_context),
    m_controller(controller),
    m_video_path(video_path)
{
    m_video_socket.open(udp::v4());
    m_audio_socket.open(udp::v4());

    m_client_video_endpoint = udp::endpoint(client_endpoint.address(), AppConfig::VIDEO_PORT);
    m_client_audio_endpoint = udp::endpoint(client_endpoint.address(), AppConfig::AUDIO_PORT);
}
FileStreamer::~FileStreamer() { stop(); cleanup(); }
void FileStreamer::start() { if (initialize()) { m_control_block->running = true; stream_loop(); } }
void FileStreamer::stop() { m_control_block->running = false; }
void FileStreamer::seek(double time_sec) { m_control_block->seek_to = time_sec; }
// ... (以上函数不变) ...


bool FileStreamer::initialize()
{
    // ... (您现有的 initialize 代码是正确的，保持不变)
    if (avformat_open_input(&m_format_ctx, m_video_path.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "[推流] 错误: 无法打开输入文件 " << m_video_path << std::endl;
        return false;
    }
    if (avformat_find_stream_info(m_format_ctx, nullptr) < 0) {
        std::cerr << "[推流] 错误: 无法获取流信息" << std::endl;
        return false;
    }
    for (unsigned int i = 0; i < m_format_ctx->nb_streams; i++) {
        AVStream* stream = m_format_ctx->streams[i];
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) continue;
        AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
        if (!codec_ctx) continue;
        avcodec_parameters_to_context(codec_ctx, stream->codecpar);
        if (avcodec_open2(codec_ctx, decoder, nullptr) < 0) {
            avcodec_free_context(&codec_ctx);
            continue;
        }
        if (decoder->type == AVMEDIA_TYPE_VIDEO && m_video_stream_index < 0) {
            m_video_stream_index = i; m_video_stream = stream; m_video_decoder_ctx = codec_ctx;
        }
        else if (decoder->type == AVMEDIA_TYPE_AUDIO && m_audio_stream_index < 0) {
            m_audio_stream_index = i; m_audio_stream = stream; m_audio_decoder_ctx = codec_ctx;
        }
        else {
            avcodec_free_context(&codec_ctx);
        }
    }
    if (m_video_stream_index < 0) {
        std::cerr << "[推流] 错误: 文件中未找到视频流" << std::endl; return false;
    }
    if (m_audio_stream_index >= 0) {
        int input_channels = m_audio_decoder_ctx->ch_layout.nb_channels; // Use decoder's channel layout for input
        AVChannelLayout in_layout;
        if (m_audio_decoder_ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_default(&in_layout, input_channels); // Default based on number of channels
        }
        else {
            in_layout = m_audio_decoder_ctx->ch_layout;
        }

        m_swr_ctx = swr_alloc();
        if (!m_swr_ctx) { std::cerr << "[推流] 错误: 无法分配音频重采样上下文" << std::endl; return false; }

        AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_MONO; // Target output layout

        // Corrected: Use in_layout for input channel layout
        av_opt_set_chlayout(m_swr_ctx, "in_chlayout", &in_layout, 0);
        av_opt_set_int(m_swr_ctx, "in_sample_rate", m_audio_decoder_ctx->sample_rate, 0);
        av_opt_set_sample_fmt(m_swr_ctx, "in_sample_fmt", m_audio_decoder_ctx->sample_fmt, 0);

        av_opt_set_chlayout(m_swr_ctx, "out_chlayout", &out_layout, 0);
        av_opt_set_int(m_swr_ctx, "out_sample_rate", AppConfig::AUDIO_RATE, 0);
        av_opt_set_sample_fmt(m_swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

        if (swr_init(m_swr_ctx) < 0) {
            std::cerr << "[推流] 错误: 无法初始化音频重采样器" << std::endl;
            swr_free(&m_swr_ctx);
            return false;
        }
    }
    m_decoded_frame = av_frame_alloc();
    m_encoded_packet = av_packet_alloc();
    if (!m_decoded_frame || !m_encoded_packet) {
        std::cerr << "[推流] 错误: 无法分配 AVFrame 或 AVPacket" << std::endl;
        return false;
    }
    std::cout << "[推流] FFmpeg 初始化成功。" << std::endl;
    return true;
}


void FileStreamer::cleanup()
{
    if (m_format_ctx) { avformat_close_input(&m_format_ctx); }
    if (m_video_decoder_ctx) { avcodec_free_context(&m_video_decoder_ctx); }
    if (m_audio_decoder_ctx) { avcodec_free_context(&m_audio_decoder_ctx); }
    if (m_video_encoder_ctx) { avcodec_free_context(&m_video_encoder_ctx); }
    if (m_swr_ctx) { swr_free(&m_swr_ctx); }
    if (m_decoded_frame) { av_frame_free(&m_decoded_frame); }
    if (m_encoded_packet) { av_packet_free(&m_encoded_packet); }
    for (auto& pair : m_decoded_frame_buffer) { av_frame_free(&pair.second); }
    m_decoded_frame_buffer.clear();

    // Clean up hardware contexts
    if (m_hwFramesContext) {
        av_buffer_unref(&m_hwFramesContext);
        m_hwFramesContext = nullptr;
    }
    m_hwDeviceContext = nullptr;
    m_hwPixelFormat = AV_PIX_FMT_NONE;
    m_hwDeviceType = AV_HWDEVICE_TYPE_NONE;

    std::cout << "[推流] FFmpeg 资源已清理。" << std::endl;
}

void FileStreamer::stream_loop()
{
    AVPacket* demux_packet = av_packet_alloc();
    if (!demux_packet) { std::cerr << "无法分配 demux packet" << std::endl; return; }
    bool first_frame_processed = false;
    auto start_time_perf = std::chrono::high_resolution_clock::now();
    double start_pts_sec = 0.0;
    while (m_control_block->running) {
        double seek_time = m_control_block->seek_to.load();
        if (seek_time >= 0) {
            m_control_block->seek_to = -1.0;
            int64_t seek_ts = static_cast<int64_t>(seek_time / av_q2d(m_video_stream->time_base));
            if (av_seek_frame(m_format_ctx, m_video_stream_index, seek_ts, AVSEEK_FLAG_BACKWARD) >= 0) {
                if (m_video_decoder_ctx) avcodec_flush_buffers(m_video_decoder_ctx);
                if (m_audio_decoder_ctx) avcodec_flush_buffers(m_audio_decoder_ctx);
                if (m_video_encoder_ctx) avcodec_flush_buffers(m_video_encoder_ctx);
                for (auto& pair : m_decoded_frame_buffer) av_frame_free(&pair.second);
                m_decoded_frame_buffer.clear();
                first_frame_processed = false;
                std::cout << "[推流] 执行跳转到: " << seek_time << "s" << std::endl;
            }
        }
        while (m_decoded_frame_buffer.size() < 60 && m_control_block->running) {
            if (av_read_frame(m_format_ctx, demux_packet) < 0) { m_control_block->running = false; break; }
            AVStream* packet_stream = m_format_ctx->streams[demux_packet->stream_index];
            AVCodecContext* current_decoder = (demux_packet->stream_index == m_video_stream_index) ? m_video_decoder_ctx : m_audio_decoder_ctx;
            if (!current_decoder) { av_packet_unref(demux_packet); continue; }
            if (avcodec_send_packet(current_decoder, demux_packet) == 0) {
                while (avcodec_receive_frame(current_decoder, m_decoded_frame) == 0) {
                    AVFrame* frame_clone = av_frame_clone(m_decoded_frame);
                    double pts_sec = frame_clone->pts * av_q2d(packet_stream->time_base);
                    m_decoded_frame_buffer.emplace_back(pts_sec, frame_clone);
                }
            }
            av_packet_unref(demux_packet);
        }
        if (m_decoded_frame_buffer.empty()) break;
        std::sort(m_decoded_frame_buffer.begin(), m_decoded_frame_buffer.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        auto [current_pts_sec, frame_to_process] = m_decoded_frame_buffer.front();
        m_decoded_frame_buffer.pop_front();
        if (!first_frame_processed) {
            start_pts_sec = current_pts_sec;
            start_time_perf = std::chrono::high_resolution_clock::now();
            first_frame_processed = true;
        }
        auto target_elapsed = std::chrono::duration<double>(current_pts_sec - start_pts_sec);
        auto real_elapsed = std::chrono::high_resolution_clock::now() - start_time_perf;
        if ((target_elapsed - real_elapsed).count() > 0.001) {
            std::this_thread::sleep_for(target_elapsed - real_elapsed);
        }
        int64_t ts_ms = static_cast<int64_t>(current_pts_sec * 1000);
        if (frame_to_process->width > 0) { // Check if it's a video frame
            encode_and_send_video(frame_to_process, ts_ms);
        }
        else if (frame_to_process->nb_samples > 0) { // Check if it's an audio frame
            resample_and_send_audio(frame_to_process, ts_ms);
        }
        av_frame_free(&frame_to_process);
    }

    if (m_video_encoder_ctx) {
        std::cout << "[推流] 冲洗视频编码器..." << std::endl;
        encode_and_send_video(nullptr, 0); // 发送一个空帧来 flush
    }

    av_packet_free(&demux_packet);
    std::cout << "[服务端-推流] 文件推流循环结束。" << std::endl;
}

// FFmpeg 硬件编码回调函数 (如果需要)
AVPixelFormat FileStreamer::GetEncoderHwFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts)
{
    FileStreamer* pThis = static_cast<FileStreamer*>(ctx->opaque);
    if (!pThis) {
        std::cerr << "[Encoder] GetHwFormat: Opaque pointer is null." << std::endl;
        return AV_PIX_FMT_NONE;
    }

    for (const AVPixelFormat* p = pix_fmts; *p != -1; p++) {
        if (*p == pThis->m_hwPixelFormat) {
            const char* pix_fmt_name_c = av_get_pix_fmt_name(*p); // Get C-style string
            std::cout << "[Encoder] GetHwFormat: Found matching hardware pixel format: " << (pix_fmt_name_c ? std::string(pix_fmt_name_c) : "NONE") << std::endl;
            return *p;
        }
    }

    std::cerr << "[Encoder] GetHwFormat: Failed to get HW surface format. Falling back to software." << std::endl;
    return AV_PIX_FMT_NONE; // Fallback to software format
}


bool FileStreamer::initialize_video_encoder(const StreamStrategy& strategy)
{
    if (m_video_encoder_ctx) {
        avcodec_free_context(&m_video_encoder_ctx);
    }
    // 使用 NVCODEC 的 H.265 编码器
    const AVCodec* encoder = avcodec_find_encoder_by_name("hevc_nvenc");
    if (!encoder) {
        std::cerr << "[推流] 错误: 找不到 hevc_nvenc 编码器。请确认 FFmpeg 构建时已包含 NVENC 支持。" << std::endl;
        return false;
    }
    m_video_encoder_ctx = avcodec_alloc_context3(encoder);
    if (!m_video_encoder_ctx) return false;
    m_video_encoder_ctx->width = m_video_stream->codecpar->width;
    m_video_encoder_ctx->height = m_video_stream->codecpar->height;
    m_video_encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    m_video_encoder_ctx->time_base = { 1, 1000 };
    m_video_encoder_ctx->bit_rate = static_cast<int64_t>(m_base_bitrate * strategy.multiplier);
    m_video_encoder_ctx->framerate = { strategy.fps_limit, 1 };

    // 设置 NVCODEC 编码器的参数
    av_opt_set(m_video_encoder_ctx->priv_data, "preset", "p1", 0); // 预设，p1 为最快
    av_opt_set(m_video_encoder_ctx->priv_data, "tune", "hq", 0);   // 调优，hq 为高质量

    int ret = avcodec_open2(m_video_encoder_ctx, encoder, nullptr);
    if (ret < 0) {
        log_ffmpeg_error("无法打开视频编码器", ret);
        return false;
    }
    std::cout << "[推流] 视频编码器已初始化。" << std::endl;
    return true;
}

void FileStreamer::encode_and_send_video(AVFrame* frame, int64_t ts_ms)
{
    StreamStrategy current_strategy = m_controller->get_current_strategy();
    if (!m_video_encoder_ctx || current_strategy.multiplier != m_last_strategy.multiplier || current_strategy.fps_limit != m_last_strategy.fps_limit) {

        // Re-initialize encoder if strategy changes significantly
        if (!initialize_video_encoder(current_strategy)) {
            std::cerr << "[推流] 致命错误: 视频编码器初始化失败，视频流将中断。" << std::endl;
            return;
        }
        m_last_strategy = current_strategy;
    }

    AVFrame* frame_to_encode = frame; // Default to input frame (software frame)
    AVFrame* temp_hw_frame = nullptr; // Temporary hardware frame

    if (frame) { // Only process if a valid frame is provided (not for flushing)
        frame->pts = ts_ms;
        frame->pict_type = AV_PICTURE_TYPE_NONE; // Ensure picture type is not set to I/P/B for encoder to decide

        // If hardware acceleration is enabled, we need to transfer the software frame to a hardware frame
        if (m_hwDeviceType != AV_HWDEVICE_TYPE_NONE && m_hwFramesContext) { // Check m_hwFramesContext
            temp_hw_frame = av_frame_alloc(); // Allocate a new AVFrame structure
            if (!temp_hw_frame) {
                log_ffmpeg_error("无法分配临时硬件帧结构", AVERROR(ENOMEM));
                // Fallback to software frame
            }
            else {
                // Get a buffer for the hardware frame from the hardware frame context
                int ret_get_buffer = av_hwframe_get_buffer(m_hwFramesContext, temp_hw_frame, 0);
                if (ret_get_buffer < 0) {
                    log_ffmpeg_error("无法从硬件帧上下文获取缓冲区", ret_get_buffer);
                    av_frame_free(&temp_hw_frame); // Free the allocated structure
                    temp_hw_frame = nullptr; // Ensure it's null for fallback
                    // Fallback to software frame
                }
                else {
                    // Transfer data from software frame to hardware frame
                    int transfer_ret = av_hwframe_transfer_data(temp_hw_frame, frame, 0);
                    if (transfer_ret < 0) {
                        log_ffmpeg_error("无法将软帧传输到硬件帧", transfer_ret);
                        av_frame_free(&temp_hw_frame); // Free the hardware frame buffer and structure
                        temp_hw_frame = nullptr; // Ensure it's null for fallback
                        // Fallback to software frame
                    }
                    else {
                        av_frame_copy_props(temp_hw_frame, frame); // Copy PTS and other properties
                        frame_to_encode = temp_hw_frame; // Use the hardware frame for encoding
                    }
                }
            }
        }
    }

    // Send frame to encoder (frame_to_encode can be nullptr for flushing)
    int ret = avcodec_send_frame(m_video_encoder_ctx, frame_to_encode);
    if (ret < 0) {
        log_ffmpeg_error("avcodec_send_frame 失败", ret);
    }

    // Loop to receive encoded packets
    while (ret >= 0) {
        ret = avcodec_receive_packet(m_video_encoder_ctx, m_encoded_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        else if (ret < 0) {
            log_ffmpeg_error("avcodec_receive_packet 失败", ret);
            break;
        }

        int num_sent = send_packet_fragmented(m_video_socket, m_client_video_endpoint, m_video_seq, m_encoded_packet->pts, m_encoded_packet, true);
        m_video_seq += num_sent;

        av_packet_unref(m_encoded_packet);
    }

    // IMPORTANT: If a temporary hardware frame was used, it must be unreferenced/freed *after* send_frame and receive_packet loop.
    // The encoder takes a reference, so the frame can be unreferenced here.
    if (temp_hw_frame) { // If temp_hw_frame was successfully allocated and used
        av_frame_unref(temp_hw_frame); // Unreference the hardware frame
        av_frame_free(&temp_hw_frame); // Free the AVFrame structure
    }
}

void FileStreamer::resample_and_send_audio(AVFrame* frame, int64_t ts_ms)
{
    if (!m_swr_ctx || !frame) return;

    uint8_t** output_buffer_array = nullptr;  // 二级指针
    int linesize = 0;

    // 计算重采样后的样本数
    int output_samples = av_rescale_rnd(
        swr_get_delay(m_swr_ctx, frame->sample_rate) + frame->nb_samples,
        AppConfig::AUDIO_RATE,
        frame->sample_rate,
        AV_ROUND_UP
    );

    // 分配音频缓冲区
    int ret = av_samples_alloc_array_and_samples(
        &output_buffer_array,  // 需要三级指针，所以取地址
        &linesize,
        AppConfig::AUDIO_CHANNELS,
        output_samples,
        AV_SAMPLE_FMT_S16,
        0
    );

    if (ret < 0) {
        fprintf(stderr, "无法分配音频缓冲区\n");
        return;
    }

    // 执行音频重采样
    int samples_converted = swr_convert(
        m_swr_ctx,
        output_buffer_array,  // 直接使用二级指针
        output_samples,
        (const uint8_t**)frame->data,
        frame->nb_samples
    );

    if (samples_converted > 0) {
        // 计算实际数据大小
        int data_size = av_samples_get_buffer_size(
            nullptr,
            AppConfig::AUDIO_CHANNELS,
            samples_converted,
            AV_SAMPLE_FMT_S16,
            1
        );

        // 创建并发送音频包
        AVPacket audio_packet = { 0 };
        audio_packet.data = output_buffer_array[0];  // 使用第一个声道的数据
        audio_packet.size = data_size;

        send_packet_fragmented(
            m_audio_socket,
            m_client_audio_endpoint,
            m_audio_seq,
            ts_ms,
            &audio_packet,
            false
        );

        m_audio_seq++;
    }

    // 释放内存
    if (output_buffer_array) {
        av_freep(&output_buffer_array[0]);  // 释放数据缓冲区
        av_freep(&output_buffer_array);     // 释放指针数组
    }
}

int FileStreamer::send_packet_fragmented(udp::socket& sock, const udp::endpoint& endpoint, uint32_t seq, int64_t ts, const AVPacket* packet, bool is_video)
{
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
