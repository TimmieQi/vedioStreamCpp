#pragma once

#include "BaseStreamer.h"
#include <string>
#include <thread>
#include <opencv2/core.hpp>
#include <portaudio.h>
#include <atomic> // 【新增】为 std::atomic

namespace cv { class VideoCapture; }
struct AVFrame;
struct SwsContext;

class CameraStreamer final : public BaseStreamer
{
public:
    CameraStreamer(
        const QUIC_API_TABLE* msquic,
        HQUIC connection,
        std::shared_ptr<AdaptiveStreamController> controller
    );
    ~CameraStreamer();

    void start() override;

private:
    bool initialize_video_capture();
    bool initialize_audio_capture();
    void cleanup() override;

    void video_stream_loop();
    void audio_stream_loop();

    std::chrono::steady_clock::time_point m_start_time;

    std::unique_ptr<cv::VideoCapture> m_video_capture;
    cv::Size m_frame_size;
    SwsContext* m_sws_ctx_bgr_to_yuv = nullptr;

    std::thread m_audio_thread;
    PaStream* m_audio_stream = nullptr;

    AVFrame* m_yuv_frame = nullptr;

    // 【新增】一个健壮的“清理一次”标志
    std::atomic<bool> m_is_cleaned_up{ false };
};