#pragma once

#include <QObject>
#include <QThread>
#include <atomic>
#include <map>
#include <vector>

// 前向声明
class JitterBuffer;
class DecodedFrameBuffer;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

class VideoDecoder : public QObject
{
    Q_OBJECT

public:
    // 构造函数需要输入（JitterBuffer）和输出（DecodedFrameBuffer）的引用
    VideoDecoder(JitterBuffer& inputBuffer, DecodedFrameBuffer& outputBuffer, QObject* parent = nullptr);
    ~VideoDecoder();

public slots:
    // 启动解码循环
    void startDecoding();
    // 停止解码循环
    void stopDecoding();

private:
    bool initFFmpeg();
    void cleanupFFmpeg();
    void decodeLoop(); // 解码主循环

    // 帧重组逻辑
    std::vector<uint8_t> reassembleFrame(const QByteArray& packet);

private:
    std::atomic<bool> m_isDecoding;
    JitterBuffer& m_inputBuffer;
    DecodedFrameBuffer& m_outputBuffer;

    AVCodecContext* m_codecContext = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_packet = nullptr;

    // 用于缓存分片数据的字典 {timestamp: [payload_chunk1, payload_chunk2, ...]}
    std::map<int64_t, std::vector<QByteArray>> m_reassemblyBuffer;
};