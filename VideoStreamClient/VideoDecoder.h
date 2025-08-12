#pragma once

#include <QObject>
#include <QThread>
#include <QTimer> // 【新增】用于清理超时的分片
#include <atomic>
#include <map>
#include <vector>
#include <QByteArray> // 【新增】
#include <QDateTime>  // 【新增】

class JitterBuffer;
class DecodedFrameBuffer;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
class MediaPacket; // 前向声明

class VideoDecoder : public QObject
{
    Q_OBJECT

public:
    VideoDecoder(JitterBuffer& inputBuffer, DecodedFrameBuffer& outputBuffer, QObject* parent = nullptr);
    ~VideoDecoder();

public slots:
    void startDecoding();
    void stopDecoding();

private slots:
    // 【新增】定时清理超时的未完成帧
    void cleanupReassemblyBuffer();

private:
    bool initFFmpeg();
    void cleanupFFmpeg();
    void decodeLoop();

    // 【新增】处理单个数据报（可能是分片）的函数
    void processDatagram(const MediaPacket& packet);

    // 【新增】用于重组的结构和缓冲区
    struct FragmentedFrame {
        uint16_t fragment_count = 0;
        QDateTime first_received_time;
        // 使用 map 自动按分片索引排序
        std::map<uint16_t, QByteArray> fragments;
    };
    std::map<int64_t, FragmentedFrame> m_reassemblyBuffer; // Key: timestamp (Frame ID)

    QTimer* m_cleanupTimer; // 【新增】

    std::atomic<bool> m_isDecoding;
    JitterBuffer& m_inputBuffer;
    DecodedFrameBuffer& m_outputBuffer;

    AVCodecContext* m_codecContext = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_packet = nullptr;
};