// FileStreamer.h

#pragma once

#include "BaseStreamer.h"
#include <string>
#include <deque>
#include <utility>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct SwrContext; // 用于音频
struct AVStream;
struct SwsContext; // 【新增】用于视频

class FileStreamer final : public BaseStreamer
{
public:
    FileStreamer(
        const QUIC_API_TABLE* msquic,
        HQUIC connection,
        std::shared_ptr<AdaptiveStreamController> controller,
        const std::string& video_path
    );
    ~FileStreamer();

    void start() override;

private:
    bool initialize_ffmpeg();
    void cleanup() override;
    void stream_loop();
    void resample_and_send_audio(AVFrame* frame);

    // --- FileStreamer 特有的成员 ---
    std::string m_video_path;

    AVFormatContext* m_format_ctx = nullptr;
    AVCodecContext* m_video_decoder_ctx = nullptr;
    AVCodecContext* m_audio_decoder_ctx = nullptr;
    const AVStream* m_video_stream = nullptr;
    const AVStream* m_audio_stream = nullptr;
    int m_video_stream_index = -1;
    int m_audio_stream_index = -1;

    // 音频重采样上下文
    SwrContext* m_swr_ctx = nullptr;
    // 视频像素格式转换上下文
    SwsContext* m_sws_ctx_video = nullptr;

    AVFrame* m_decoded_frame = nullptr;
    // 用于存放 YUV420P 格式的帧
    AVFrame* m_yuv_frame = nullptr;

    std::deque<std::pair<double, AVFrame*>> m_decoded_frame_buffer;

    std::atomic<bool> m_is_cleaned_up{ false };

};