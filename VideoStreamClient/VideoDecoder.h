#pragma once

#include <QObject>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <map>
#include <vector>
#include <QByteArray>
#include <QDateTime>
#include "MasterClock.h"
// 前向声明 FFmpeg 结构体
extern "C" {
#include <libavcodec/avcodec.h>
    // 【新增】包含 SwsContext 的前向声明
    struct SwsContext;
}

// 前向声明其他类
class JitterBuffer;
class DecodedFrameBuffer;
class MediaPacket;

class VideoDecoder : public QObject
{
    Q_OBJECT

public:
    VideoDecoder(JitterBuffer& inputBuffer, DecodedFrameBuffer& outputBuffer, MasterClock& clock, QObject* parent = nullptr);
    ~VideoDecoder();

    // public getter，用于让回调函数访问私有成员
    enum AVPixelFormat get_hw_pixel_format() const { return m_hw_pix_fmt; }

public slots:
    void startDecoding();
    void stopDecoding();

private slots:
    void cleanupReassemblyBuffer();

private:
    bool initFFmpeg();
    void cleanupFFmpeg();
    void decodeLoop();
    void processDatagram(const MediaPacket& packet);

    // 用于重组的结构和缓冲区
    struct FragmentedFrame {
        uint16_t fragment_count = 0;
        QDateTime first_received_time;
        std::map<uint16_t, QByteArray> fragments;
    };
    std::map<int64_t, FragmentedFrame> m_reassemblyBuffer;

    QTimer* m_cleanupTimer;

    std::atomic<bool> m_isDecoding;
    JitterBuffer& m_inputBuffer;
    DecodedFrameBuffer& m_outputBuffer;
    MasterClock& m_clock;

    AVCodecContext* m_codecContext = nullptr;
    AVFrame* m_frame = nullptr;      // 用于软解或从GPU下载后的CPU帧
    AVFrame* m_hw_frame = nullptr;   // 用于存放GPU解码后的硬件帧
    AVPacket* m_packet = nullptr;

    // 【新增】用于格式规范化的成员
    SwsContext* m_sws_ctx_fixup = nullptr;
    AVFrame* m_sw_frame = nullptr; // 用于存放 sws_scale 输出的标准CPU帧

    // 硬件加速相关成员
    AVBufferRef* m_hw_device_ctx = nullptr;
    enum AVHWDeviceType m_hw_device_type = AV_HWDEVICE_TYPE_NONE;
    enum AVPixelFormat m_hw_pix_fmt = AV_PIX_FMT_NONE;
};