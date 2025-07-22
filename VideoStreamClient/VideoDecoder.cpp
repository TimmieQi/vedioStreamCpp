#include "VideoDecoder.h"
#include "JitterBuffer.h"
#include "DecodedFrameBuffer.h"
#include "shared_config.h"

// 必须用 extern "C" 来包含 C 语言的 FFmpeg 头文件
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

#include <QDebug>

VideoDecoder::VideoDecoder(JitterBuffer& inputBuffer, DecodedFrameBuffer& outputBuffer, QObject* parent)
    : QObject(parent),
    m_isDecoding(false),
    m_inputBuffer(inputBuffer),
    m_outputBuffer(outputBuffer)
{
}

VideoDecoder::~VideoDecoder()
{
    stopDecoding();
    cleanupFFmpeg();
}

bool VideoDecoder::initFFmpeg()
{
    // 查找 H.265 (HEVC) 解码器
    const AVCodec* codec = avcodec_find_decoder_by_name(AppConfig::VIDEO_CODEC);
    if (!codec) {
        qDebug() << "[Decoder] 错误: 找不到解码器" << AppConfig::VIDEO_CODEC;
        return false;
    }

    m_codecContext = avcodec_alloc_context3(codec);
    if (!m_codecContext) {
        qDebug() << "[Decoder] 错误: 无法分配解码器上下文";
        return false;
    }

    // 打开解码器
    if (avcodec_open2(m_codecContext, codec, nullptr) < 0) {
        qDebug() << "[Decoder] 错误: 无法打开解码器";
        return false;
    }

    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_frame || !m_packet) {
        qDebug() << "[Decoder] 错误: 无法分配 AVFrame 或 AVPacket";
        return false;
    }

    qDebug() << "[Decoder] FFmpeg 解码器初始化成功。";
    return true;
}

void VideoDecoder::cleanupFFmpeg()
{
    if (m_codecContext) {
        avcodec_free_context(&m_codecContext);
        m_codecContext = nullptr;
    }
    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    qDebug() << "[Decoder] FFmpeg 解码器已清理。";
}

void VideoDecoder::startDecoding()
{
    if (m_isDecoding) return;

    if (!initFFmpeg()) {
        // 初始化失败，不启动循环
        return;
    }

    m_isDecoding = true;
    qDebug() << "[Decoder] 视频解码循环启动。";
    decodeLoop();
}

void VideoDecoder::stopDecoding()
{
    m_isDecoding = false;
}

void VideoDecoder::decodeLoop()
{
    while (m_isDecoding)
    {
        // 从 JitterBuffer 中获取一个原始网络包
        auto mediaPacket = m_inputBuffer.get_packet();
        if (!mediaPacket) {
            // JitterBuffer 为空或者当前期望的包丢失，短暂等待
            QThread::msleep(1);
            continue;
        }

        // --- 帧重组逻辑 ---
        // 原始包的 payload 包含了我们的自定义头部
        const uchar* raw_data = mediaPacket->payload.data();
        int64_t ts = mediaPacket->ts;
        uchar frag_info = raw_data[6];
        bool is_start = (frag_info & 0x80) != 0;
        bool is_end = (frag_info & 0x40) != 0;

        QByteArray payload(reinterpret_cast<const char*>(raw_data + 7), mediaPacket->payload.size() - 7);
        QByteArray full_frame_payload;

        if (is_start && is_end) {
            // 完整包，未分片
            full_frame_payload = payload;
        }
        else {
            // 分片包
            if (is_start) {
                m_reassemblyBuffer[ts] = { payload };
            }
            else if (m_reassemblyBuffer.count(ts)) {
                m_reassemblyBuffer[ts].push_back(payload);
            }

            if (is_end && m_reassemblyBuffer.count(ts)) {
                for (const auto& chunk : m_reassemblyBuffer[ts]) {
                    full_frame_payload.append(chunk);
                }
                m_reassemblyBuffer.erase(ts);
            }
        }
        // --- 帧重组结束 ---

        if (!full_frame_payload.isEmpty())
        {
            // 将重组后的完整帧数据送入解码器
            m_packet->data = reinterpret_cast<uint8_t*>(full_frame_payload.data());
            m_packet->size = full_frame_payload.size();
            m_packet->pts = ts;

            int ret = avcodec_send_packet(m_codecContext, m_packet);
            if (ret < 0) {
                //qDebug() << "[Decoder] avcodec_send_packet 错误:" << ret;
                continue;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(m_codecContext, m_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                else if (ret < 0) {
                    //qDebug() << "[Decoder] avcodec_receive_frame 错误:" << ret;
                    break;
                }

                AVFrame* frame_clone = av_frame_clone(m_frame);
                if (frame_clone) {
                    // 创建 DecodedFrame 并转移所有权
                    auto decodedFrame = std::make_unique<DecodedFrame>(frame_clone);
                    // 将包含完整 AVFrame 的 DecodedFrame 放入缓冲
                    m_outputBuffer.add_frame(std::move(decodedFrame));
                }

            }
        }

        // 清理过时的重组缓冲区条目，防止内存泄漏
        if (m_reassemblyBuffer.size() > 20) {
            m_reassemblyBuffer.erase(m_reassemblyBuffer.begin());
        }
    }

    qDebug() << "[Decoder] 视频解码循环结束。";
}