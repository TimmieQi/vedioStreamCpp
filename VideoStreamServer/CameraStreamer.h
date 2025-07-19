#pragma once
#include "IStreamer.h"
#include "AdaptiveStreamController.h"
#include <string>
#include <boost/asio.hpp>
#include <memory>
#include <thread>
#include <opencv2/core.hpp> // OpenCV 核心
#include <portaudio.h>
// 前向声明
namespace cv { class VideoCapture; }
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct StreamStrategy;


using boost::asio::ip::udp;

class CameraStreamer : public IStreamer, public std::enable_shared_from_this<CameraStreamer>
{
public:
    CameraStreamer(
        boost::asio::io_context& io_context,
        std::shared_ptr<AdaptiveStreamController> controller,
        udp::endpoint client_endpoint
    );
    ~CameraStreamer();

    // IStreamer 接口实现
    void start() override;
    void stop() override;
    void seek(double time_sec) override; // 摄像头直播不支持seek

private:
    // 初始化和清理
    bool initialize_video();
    bool initialize_audio();
    void cleanup();

    // 推流主循环
    void video_stream_loop();
    void audio_stream_loop();

    // 编码和发送
    bool initialize_video_encoder(const StreamStrategy& strategy);
    void encode_and_send_video(AVFrame* frame);
    int send_packet_fragmented(udp::socket& sock, const udp::endpoint& endpoint, uint32_t seq, int64_t ts, const AVPacket* packet, bool is_video);

    // --- 成员变量 ---
    std::shared_ptr<StreamControlBlock> m_control_block;

    // 网络
    udp::socket m_video_socket;
    udp::socket m_audio_socket;
    udp::endpoint m_client_video_endpoint;
    udp::endpoint m_client_audio_endpoint;

    // 逻辑
    std::shared_ptr<AdaptiveStreamController> m_controller;
    StreamStrategy m_last_strategy = { 0.0, 0 };
    int64_t m_base_bitrate = 1500 * 1024;
    std::chrono::steady_clock::time_point m_start_time;

    // --- 视频采集 (OpenCV) ---
    std::unique_ptr<cv::VideoCapture> m_video_capture;
    cv::Size m_frame_size;
    SwsContext* m_sws_ctx_bgr_to_yuv = nullptr;

    // --- 音频采集 (PortAudio) ---
    std::thread m_audio_thread;
    PaStream* m_audio_stream = nullptr;

    // --- FFmpeg 编码 ---
    AVCodecContext* m_video_encoder_ctx = nullptr;
    AVFrame* m_yuv_frame = nullptr;
    AVPacket* m_encoded_packet = nullptr;

    uint32_t m_video_seq = 0;
    uint32_t m_audio_seq = 0;
};