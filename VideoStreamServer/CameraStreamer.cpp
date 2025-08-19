#include "CameraStreamer.h"
#include "shared_config.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <opencv2/videoio.hpp> 

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

CameraStreamer::CameraStreamer(
    const QUIC_API_TABLE* msquic, HQUIC connection,
    std::shared_ptr<AdaptiveStreamController> controller)
    : BaseStreamer(msquic, connection, controller)
{
    m_yuv_frame = av_frame_alloc();
}

CameraStreamer::~CameraStreamer() {
    cleanup();
}

void CameraStreamer::start() {
    if (initialize_video_capture() && initialize_audio_capture()) {
        m_controller->set_video_resolution(m_frame_size.width, m_frame_size.height);

        m_control_block->running = true;
        m_start_time = std::chrono::steady_clock::now();
        m_audio_thread = std::thread(&CameraStreamer::audio_stream_loop, this);
        video_stream_loop();
    }
    else {
        std::cerr << "[摄像头推流] 启动失败，初始化未完成。" << std::endl;
        m_control_block->running = false;
    }
}

void CameraStreamer::cleanup()
{
    // 【核心修复】使用新的、不会出错的“执行一次”守护逻辑
    if (m_is_cleaned_up.exchange(true)) {
        return; // 如果之前已经是 true，说明已经清理过，直接返回
    }

    // 停止循环（以防万一 stop() 没被调用）
    m_control_block->running = false;

    std::cout << "[摄像头推流] 开始清理摄像头特定资源..." << std::endl;

    if (m_audio_thread.joinable()) {
        m_audio_thread.join();
        std::cout << "[摄像头推流] 音频线程已汇合。" << std::endl;
    }

    if (m_audio_stream) {
        if (Pa_IsStreamActive(m_audio_stream) > 0) { // Pa_IsStreamActive 返回 1 表示活跃, 0 不活跃, < 0 错误
            Pa_StopStream(m_audio_stream);
        }
        Pa_CloseStream(m_audio_stream);
        m_audio_stream = nullptr;
    }
    Pa_Terminate();
    std::cout << "[摄像头推流] PortAudio 已清理。" << std::endl;

    if (m_video_capture && m_video_capture->isOpened()) {
        m_video_capture->release();
    }
    m_video_capture.reset();
    std::cout << "[摄像头推流] OpenCV 摄像头已释放。" << std::endl;

    if (m_sws_ctx_bgr_to_yuv) {
        sws_freeContext(m_sws_ctx_bgr_to_yuv);
        m_sws_ctx_bgr_to_yuv = nullptr;
    }
    if (m_yuv_frame) {
        av_frame_free(&m_yuv_frame);
        m_yuv_frame = nullptr;
    }

    std::cout << "[摄像头推流] 摄像头特定资源已清理。" << std::endl;

    BaseStreamer::cleanup();
}

bool CameraStreamer::initialize_video_capture() {
    m_video_capture = std::make_unique<cv::VideoCapture>(0);
    if (!m_video_capture->isOpened()) return false;
    m_frame_size.width = static_cast<int>(m_video_capture->get(cv::CAP_PROP_FRAME_WIDTH));
    m_frame_size.height = static_cast<int>(m_video_capture->get(cv::CAP_PROP_FRAME_HEIGHT));
    if (!m_yuv_frame) return false;
    m_yuv_frame->format = AV_PIX_FMT_YUV420P;
    m_yuv_frame->width = m_frame_size.width;
    m_yuv_frame->height = m_frame_size.height;
    return av_frame_get_buffer(m_yuv_frame, 0) >= 0;
}

bool CameraStreamer::initialize_audio_capture() {
    if (Pa_Initialize() != paNoError) return false;
    return Pa_OpenDefaultStream(&m_audio_stream, AppConfig::AUDIO_CHANNELS, 0, paInt16, AppConfig::AUDIO_RATE, AppConfig::AUDIO_CHUNK_SAMPLES, nullptr, nullptr) == paNoError;
}

void CameraStreamer::video_stream_loop() {
    cv::Mat bgr_frame;
    // 暂时定义一个固定的目标帧率
    const int TARGET_FPS = 30;
    const auto target_interval = std::chrono::microseconds(1000000 / TARGET_FPS);

    while (m_control_block->running) {
        auto start_capture_time = std::chrono::steady_clock::now();
        if (!m_video_capture || !m_video_capture->isOpened() || !m_video_capture->read(bgr_frame) || bgr_frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        m_sws_ctx_bgr_to_yuv = sws_getCachedContext(m_sws_ctx_bgr_to_yuv, m_frame_size.width, m_frame_size.height, AV_PIX_FMT_BGR24, m_frame_size.width, m_frame_size.height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        const int stride[] = { static_cast<int>(bgr_frame.step[0]) };
        sws_scale(m_sws_ctx_bgr_to_yuv, &bgr_frame.data, stride, 0, m_frame_size.height, m_yuv_frame->data, m_yuv_frame->linesize);
        m_yuv_frame->pts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start_time).count();

        encode_and_send_video(m_yuv_frame);

        // 【修改】移除对旧 strategy 的引用，使用固定间隔来休眠
        auto process_duration = std::chrono::steady_clock::now() - start_capture_time;
        if (target_interval > process_duration) {
            std::this_thread::sleep_for(target_interval - process_duration);
        }
    }
    encode_and_send_video(nullptr);
    std::cout << "[摄像头推流] 视频循环结束。" << std::endl;
}

void CameraStreamer::audio_stream_loop() {
    if (Pa_StartStream(m_audio_stream) != paNoError) {
        std::cerr << "[摄像头推流] 错误: 无法启动 PortAudio 流。" << std::endl;
        return;
    }

    std::vector<int16_t> audio_buffer(AppConfig::AUDIO_CHUNK_SAMPLES * AppConfig::AUDIO_CHANNELS);
    while (m_control_block->running) {
        PaError err = Pa_ReadStream(m_audio_stream, audio_buffer.data(), AppConfig::AUDIO_CHUNK_SAMPLES);
        if (err == paNoError || err == paInputOverflowed) {
            int64_t timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start_time).count();
            send_quic_data(AppConfig::PacketType::Audio, reinterpret_cast<uint8_t*>(audio_buffer.data()), audio_buffer.size() * sizeof(int16_t), timestamp_ms);
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::cout << "[摄像头推流] 音频循环结束。" << std::endl;
}