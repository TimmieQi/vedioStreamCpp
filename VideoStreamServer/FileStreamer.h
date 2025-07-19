// FileStreamer.h

#pragma once
#include "IStreamer.h"
#include "AdaptiveStreamController.h"
#include <string>
#include <boost/asio.hpp>
#include <deque>
#include <utility>
#include <memory>

// 必须用 extern "C" 来包含 C 语言的 FFmpeg 头文件
extern "C" {
#include <libavutil/hwcontext.h> // For hardware context types and functions
#include <libavutil/pixfmt.h>    // For AVPixelFormat
#include <libavcodec/avcodec.h>  // For AVCodecContext, AVFrame, AVPacket
}

// 前向声明 FFmpeg 结构体
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;
struct AVStream;

using boost::asio::ip::udp;

class FileStreamer : public IStreamer, public std::enable_shared_from_this<FileStreamer>
{
public:
    FileStreamer(
        boost::asio::io_context& io_context,
        std::shared_ptr<AdaptiveStreamController> controller,
        const std::string& video_path,
        udp::endpoint client_endpoint
    );
    ~FileStreamer();

    // IStreamer 接口实现
    void start() override;
    void stop() override;
    void seek(double time_sec) override;

private:
    // 初始化和清理函数
    bool initialize();
    void cleanup();

    // 推流主循环
    void stream_loop();

    // 编码和发送函数
    bool initialize_video_encoder(const StreamStrategy& strategy);
    void encode_and_send_video(AVFrame* frame, int64_t ts_ms);
    void resample_and_send_audio(AVFrame* frame, int64_t ts_ms);
    int send_packet_fragmented(udp::socket& sock, const udp::endpoint& endpoint, uint32_t seq, int64_t ts, const AVPacket* packet, bool is_video);

    // FFmpeg 硬件编码回调函数 (如果需要)
    static AVPixelFormat GetEncoderHwFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);

    // --- 成员变量 ---
    std::shared_ptr<StreamControlBlock> m_control_block;

    // 网络
    udp::socket m_video_socket;
    udp::socket m_audio_socket;
    udp::endpoint m_client_video_endpoint;
    udp::endpoint m_client_audio_endpoint;

    // 逻辑
    std::shared_ptr<AdaptiveStreamController> m_controller;
    std::string m_video_path;
    StreamStrategy m_last_strategy = { 0.0, 0 };
    int64_t m_base_bitrate = 1500 * 1024;

    // --- FFmpeg 相关成员 ---
    AVFormatContext* m_format_ctx = nullptr;
    AVCodecContext* m_video_decoder_ctx = nullptr;
    AVCodecContext* m_audio_decoder_ctx = nullptr;
    AVCodecContext* m_video_encoder_ctx = nullptr;
    const AVStream* m_video_stream = nullptr;
    const AVStream* m_audio_stream = nullptr;
    int m_video_stream_index = -1;
    int m_audio_stream_index = -1;

    SwrContext* m_swr_ctx = nullptr;
    AVFrame* m_decoded_frame = nullptr; // 用于存储解码后的软帧
    AVPacket* m_encoded_packet = nullptr;

    uint32_t m_video_seq = 0;
    uint32_t m_audio_seq = 0;

    // B帧排序缓冲: <PTS in seconds, AVFrame*>
    std::deque<std::pair<double, AVFrame*>> m_decoded_frame_buffer;

    // --- 硬件编码相关 ---
    AVBufferRef* m_hwDeviceContext = nullptr; // 硬件设备上下文
    AVBufferRef* m_hwFramesContext = nullptr; // 硬件帧上下文
    AVPixelFormat m_hwPixelFormat = AV_PIX_FMT_NONE; // 硬件编码器支持的像素格式
    AVHWDeviceType m_hwDeviceType = AV_HWDEVICE_TYPE_NONE; // 硬件设备类型 (CUDA, QSV等)
    AVFrame* m_hw_frame = nullptr; // 用于存储传输到GPU的硬件帧
};
