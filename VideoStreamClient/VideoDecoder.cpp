#include "VideoDecoder.h"
#include "JitterBuffer.h"
#include "DecodedFrameBuffer.h"
#include "shared_config.h"
#include "MediaPacket.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h> 
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

static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts)
{
    VideoDecoder* decoder = static_cast<VideoDecoder*>(ctx->opaque);
    if (!decoder) {
        return AV_PIX_FMT_NONE;
    }

    enum AVPixelFormat target_pix_fmt = decoder->get_hw_pixel_format();

    const enum AVPixelFormat* p;
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == target_pix_fmt) {
            return *p;
        }
    }
    qDebug() << "[Decoder] 错误: 找不到匹配的硬件像素格式。";
    return AV_PIX_FMT_NONE;
}

static const char* get_hw_device_type_name(enum AVHWDeviceType type) {
    switch (type) {
    case AV_HWDEVICE_TYPE_CUDA: return "cuda";
    case AV_HWDEVICE_TYPE_D3D11VA: return "d3d11va";
    case AV_HWDEVICE_TYPE_QSV: return "qsv";
    case AV_HWDEVICE_TYPE_DXVA2: return "dxva2";
    case AV_HWDEVICE_TYPE_VAAPI: return "vaapi";
    case AV_HWDEVICE_TYPE_VDPAU: return "vdpau";
    case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: return "videotoolbox";
    default: return "none";
    }
}


VideoDecoder::VideoDecoder(JitterBuffer& inputBuffer, DecodedFrameBuffer& outputBuffer, MasterClock& clock, QObject* parent)
    : QObject(parent),
    m_isDecoding(false),
    m_inputBuffer(inputBuffer),
    m_outputBuffer(outputBuffer),
    m_clock(clock)
{
    m_cleanupTimer = new QTimer(this);
    connect(m_cleanupTimer, &QTimer::timeout, this, &VideoDecoder::cleanupReassemblyBuffer);

    // 【新增】分配用于格式净化的帧
    m_sw_frame = av_frame_alloc();
}

VideoDecoder::~VideoDecoder()
{
    stopDecoding();
    cleanupFFmpeg();

    // 【新增】确保释放 m_sw_frame
    if (m_sw_frame) {
        av_frame_free(&m_sw_frame);
        m_sw_frame = nullptr;
    }
}

bool VideoDecoder::initFFmpeg()
{
    const char* codec_name = AppConfig::VIDEO_CODEC;
    const AVCodec* codec = nullptr;

    const struct {
        const char* name;
        enum AVHWDeviceType type;
    } hw_decoders[] = {
        { "hevc_cuvid", AV_HWDEVICE_TYPE_CUDA },
        { "hevc_nvdec", AV_HWDEVICE_TYPE_D3D11VA },
        { "hevc_qsv",   AV_HWDEVICE_TYPE_QSV },
        { "hevc_d3d11va", AV_HWDEVICE_TYPE_D3D11VA },
        { "hevc_amf",   AV_HWDEVICE_TYPE_D3D11VA },
        { nullptr,      AV_HWDEVICE_TYPE_NONE }
    };

    for (int i = 0; hw_decoders[i].name != nullptr; ++i) {
        codec = avcodec_find_decoder_by_name(hw_decoders[i].name);
        if (!codec) continue;

        if (av_hwdevice_ctx_create(&m_hw_device_ctx, hw_decoders[i].type, nullptr, nullptr, 0) < 0) {
            codec = nullptr;
            continue;
        }

        m_hw_pix_fmt = AV_PIX_FMT_NONE;
        for (int j = 0; ; j++) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec, j);
            if (!config) break;
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
                m_hw_pix_fmt = config->pix_fmt;
                break;
            }
        }

        if (m_hw_pix_fmt != AV_PIX_FMT_NONE) {
            m_hw_device_type = hw_decoders[i].type;
            qDebug() << "[Decoder] 成功选择硬件解码器:" << hw_decoders[i].name
                << " (设备类型: " << get_hw_device_type_name(m_hw_device_type) << ")";
            break;
        }
        else {
            av_buffer_unref(&m_hw_device_ctx);
            codec = nullptr;
        }
    }

    if (!codec) {
        qDebug() << "[Decoder] 所有硬件解码器均不可用，降级到软件解码。";
        m_hw_device_type = AV_HWDEVICE_TYPE_NONE;
        codec = avcodec_find_decoder_by_name(codec_name);
        if (!codec) {
            qDebug() << "[Decoder] 致命错误: 无法找到 " << codec_name << " 软件解码器！";
            return false;
        }
    }

    m_codecContext = avcodec_alloc_context3(codec);
    if (!m_codecContext) return false;

    if (m_hw_device_type != AV_HWDEVICE_TYPE_NONE) {
        m_codecContext->hw_device_ctx = av_buffer_ref(m_hw_device_ctx);
        m_codecContext->get_format = get_hw_format;
        m_codecContext->opaque = this;
    }

    if (avcodec_open2(m_codecContext, codec, nullptr) < 0) {
        cleanupFFmpeg();
        return false;
    }

    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();
    if (m_hw_device_type != AV_HWDEVICE_TYPE_NONE) {
        m_hw_frame = av_frame_alloc();
    }

    if (!m_packet || !m_frame || !m_sw_frame || (m_hw_device_type != AV_HWDEVICE_TYPE_NONE && !m_hw_frame)) {
        cleanupFFmpeg();
        return false;
    }

    qDebug() << "[Decoder] FFmpeg 初始化成功，解码模式:"
        << (m_hw_device_type != AV_HWDEVICE_TYPE_NONE ? "硬件加速" : "软件");
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
    if (m_hw_frame) {
        av_frame_free(&m_hw_frame);
        m_hw_frame = nullptr;
    }
    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    if (m_hw_device_ctx) {
        av_buffer_unref(&m_hw_device_ctx);
        m_hw_device_ctx = nullptr;
    }
    if (m_sws_ctx_fixup) {
        sws_freeContext(m_sws_ctx_fixup);
        m_sws_ctx_fixup = nullptr;
    }
    m_hw_device_type = AV_HWDEVICE_TYPE_NONE;
    m_hw_pix_fmt = AV_PIX_FMT_NONE;
}

void VideoDecoder::startDecoding()
{
    if (m_isDecoding) {
        m_isDecoding = false;
        m_cleanupTimer->stop();
        cleanupFFmpeg();
    }
    if (!initFFmpeg()) {
        qDebug() << "[Decoder] 启动失败，FFmpeg 初始化未完成。";
        return;
    }

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

        // 【核心修正】检查时钟暂停状态
        if (m_clock.is_paused()) {
            QThread::msleep(10);
            continue;
        }

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
    const QByteArray payload = data.mid(HEADER_SIZE);
    QByteArray frame_to_decode;

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

    if (frame_to_decode.isEmpty()) return;

    av_packet_unref(m_packet);
    if (av_new_packet(m_packet, frame_to_decode.size()) < 0) return;
    memcpy(m_packet->data, frame_to_decode.constData(), frame_to_decode.size());
    m_packet->pts = packet.ts;

    int ret = avcodec_send_packet(m_codecContext, m_packet);
    if (ret >= 0) {
        while (ret >= 0) {
            AVFrame* received_frame = (m_hw_device_type != AV_HWDEVICE_TYPE_NONE) ? m_hw_frame : m_frame;
            ret = avcodec_receive_frame(m_codecContext, received_frame);

            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            else if (ret < 0) break;

            AVFrame* final_cpu_frame = nullptr;
            if (m_hw_device_type != AV_HWDEVICE_TYPE_NONE) {
                if (av_hwframe_transfer_data(m_frame, m_hw_frame, 0) < 0) {
                    av_frame_unref(m_hw_frame);
                    continue;
                }
                final_cpu_frame = m_frame;
            }
            else {
                final_cpu_frame = m_frame;
            }

            // 【核心修正】净化帧数据
            m_sws_ctx_fixup = sws_getCachedContext(m_sws_ctx_fixup,
                final_cpu_frame->width, final_cpu_frame->height, (AVPixelFormat)final_cpu_frame->format,
                final_cpu_frame->width, final_cpu_frame->height, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!m_sws_ctx_fixup) {
                av_frame_unref(m_hw_frame);
                av_frame_unref(m_frame);
                continue;
            }

            av_frame_unref(m_sw_frame);
            m_sw_frame->width = final_cpu_frame->width;
            m_sw_frame->height = final_cpu_frame->height;
            m_sw_frame->format = AV_PIX_FMT_YUV420P;
            if (av_frame_get_buffer(m_sw_frame, 0) < 0) {
                av_frame_unref(m_hw_frame);
                av_frame_unref(m_frame);
                continue;
            }

            sws_scale(m_sws_ctx_fixup, (const uint8_t* const*)final_cpu_frame->data, final_cpu_frame->linesize,
                0, final_cpu_frame->height, m_sw_frame->data, m_sw_frame->linesize);

            m_sw_frame->pts = final_cpu_frame->pts;

            AVFrame* frame_clone = av_frame_clone(m_sw_frame);
            if (frame_clone) {
                auto decodedFrame = std::make_unique<DecodedFrame>(frame_clone);
                m_outputBuffer.add_frame(std::move(decodedFrame));
            }

            av_frame_unref(m_hw_frame);
            av_frame_unref(m_frame);
            av_frame_unref(m_sw_frame);
        }
    }
    av_packet_unref(m_packet);
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