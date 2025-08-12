#include "VideoDecoder.h"
#include "JitterBuffer.h"
#include "DecodedFrameBuffer.h"
#include "shared_config.h"
#include "MediaPacket.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

#include <QDebug>
#include <QCoreApplication>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
inline uint16_t ntohs_portable(uint16_t value) {
#ifdef _WIN32
    return _byteswap_ushort(value);
#else
    return ntohs(value);
#endif
}

VideoDecoder::VideoDecoder(JitterBuffer& inputBuffer, DecodedFrameBuffer& outputBuffer, QObject* parent)
    : QObject(parent),
    m_isDecoding(false),
    m_inputBuffer(inputBuffer),
    m_outputBuffer(outputBuffer)
{
    m_cleanupTimer = new QTimer(this);
    connect(m_cleanupTimer, &QTimer::timeout, this, &VideoDecoder::cleanupReassemblyBuffer);
}

VideoDecoder::~VideoDecoder()
{
    stopDecoding();
    cleanupFFmpeg();
}

bool VideoDecoder::initFFmpeg()
{
    const AVCodec* codec = avcodec_find_decoder_by_name(AppConfig::VIDEO_CODEC);
    if (!codec) return false;
    m_codecContext = avcodec_alloc_context3(codec);
    if (!m_codecContext) return false;
    if (avcodec_open2(m_codecContext, codec, nullptr) < 0) return false;
    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_frame || !m_packet) return false;
    qDebug() << "[Decoder] FFmpeg 解码器初始化成功。";
    return true;
}

void VideoDecoder::cleanupFFmpeg()
{
    if (m_codecContext) { avcodec_free_context(&m_codecContext); m_codecContext = nullptr; }
    if (m_frame) { av_frame_free(&m_frame); m_frame = nullptr; }
    if (m_packet) { av_packet_free(&m_packet); m_packet = nullptr; }
}

void VideoDecoder::startDecoding()
{
    if (m_isDecoding) {
        m_isDecoding = false;
        m_cleanupTimer->stop();
        cleanupFFmpeg();
    }
    if (!initFFmpeg()) return;

    m_reassemblyBuffer.clear();
    m_isDecoding = true;
    m_cleanupTimer->start(200);
    qDebug() << "[Decoder] 视频解码循环启动。";
    decodeLoop();
    m_cleanupTimer->stop();
    cleanupFFmpeg();
    qDebug() << "[Decoder] 解码线程已干净地退出。";
}

void VideoDecoder::stopDecoding()
{
    m_isDecoding = false;
}

void VideoDecoder::decodeLoop()
{
    while (m_isDecoding)
    {
        QCoreApplication::processEvents();
        auto mediaPacket = m_inputBuffer.get_packet();
        if (!mediaPacket) {
            QThread::msleep(2);
            continue;
        }
        processDatagram(*mediaPacket);
    }
}

void VideoDecoder::processDatagram(const MediaPacket& packet)
{
    // 【修正】现在 packet.payload 就是 QByteArray，data 也是
    const QByteArray& data = packet.payload;
    const int HEADER_SIZE = 1 + 8 + 2 + 2;
    if (data.size() < HEADER_SIZE) return;

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data.constData());

    ptr += sizeof(AppConfig::PacketType);

    int64_t timestamp = packet.ts;
    ptr += sizeof(int64_t);

    uint16_t fragment_count;
    memcpy(&fragment_count, ptr, sizeof(uint16_t));
    fragment_count = ntohs_portable(fragment_count);
    ptr += sizeof(uint16_t);

    uint16_t fragment_index;
    memcpy(&fragment_index, ptr, sizeof(uint16_t));
    fragment_index = ntohs_portable(fragment_index);

    // 【修正】使用 mid() 提取 payload，类型为 QByteArray，无编译错误
    const QByteArray payload = data.mid(HEADER_SIZE);

    QByteArray frame_to_decode; // 用于存放最终要解码的数据

    if (fragment_count == 1) {
        frame_to_decode = payload;
    }
    else {
        auto& frame_data = m_reassemblyBuffer[timestamp];
        if (frame_data.fragment_count == 0) {
            frame_data.fragment_count = fragment_count;
            frame_data.first_received_time = QDateTime::currentDateTime();
        }

        if (frame_data.fragment_count == fragment_count) {
            frame_data.fragments[fragment_index] = payload;
        }

        if (frame_data.fragments.size() == frame_data.fragment_count) {
            for (const auto& pair : frame_data.fragments) {
                frame_to_decode.append(pair.second);
            }
            m_reassemblyBuffer.erase(timestamp);
        }
        else {
            return;
        }
    }

    if (!frame_to_decode.isEmpty()) {
        av_packet_unref(m_packet);
        if (av_new_packet(m_packet, frame_to_decode.size()) < 0) {
            return;
        }
        memcpy(m_packet->data, frame_to_decode.constData(), frame_to_decode.size());
        m_packet->pts = timestamp;

        int ret = avcodec_send_packet(m_codecContext, m_packet);
        if (ret >= 0) {
            while (ret >= 0) {
                ret = avcodec_receive_frame(m_codecContext, m_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                else if (ret < 0) break;

                AVFrame* frame_clone = av_frame_clone(m_frame);
                if (frame_clone) {
                    auto decodedFrame = std::make_unique<DecodedFrame>(frame_clone);
                    m_outputBuffer.add_frame(std::move(decodedFrame));
                }
            }
        }
        av_packet_unref(m_packet);
    }
}

void VideoDecoder::cleanupReassemblyBuffer()
{
    auto it = m_reassemblyBuffer.begin();
    while (it != m_reassemblyBuffer.end()) {
        if (it->second.first_received_time.msecsTo(QDateTime::currentDateTime()) > 500) {
            it = m_reassemblyBuffer.erase(it);
        }
        else {
            ++it;
        }
    }
}