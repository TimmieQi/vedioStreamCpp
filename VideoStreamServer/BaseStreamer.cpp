#define NOMINMAX
#include "BaseStreamer.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm> // 【新增】为 std::min 提供头文件

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#ifdef _WIN32
#include <winsock2.h> 
#else
#include <arpa/inet.h>
#include <byteswap.h>
#endif

// 字节序转换辅助函数
inline uint16_t htons_portable(uint16_t value) {
#ifdef _WIN32
    return _byteswap_ushort(value);
#else
    return htons(value);
#endif
}

inline uint64_t htonll_portable(uint64_t value) {
#ifdef _WIN32
    return _byteswap_uint64(value);
#else
    const int num = 1;
    if (*(char*)&num == 1) { return __builtin_bswap64(value); }
    else { return value; }
#endif
}

BaseStreamer::BaseStreamer(const QUIC_API_TABLE* msquic, HQUIC connection, std::shared_ptr<AdaptiveStreamController> controller)
    : m_msquic(msquic),
    m_connection(connection),
    m_controller(controller),
    m_control_block(std::make_shared<StreamControlBlock>())
{
    m_encoded_packet = av_packet_alloc();
}

BaseStreamer::~BaseStreamer()
{
    stop();
}

void BaseStreamer::stop()
{
    m_control_block->running = false;
}

void BaseStreamer::seek(double time_sec)
{
    m_control_block->seek_to = time_sec;
}

void BaseStreamer::cleanup()
{
    std::cout << "[BaseStreamer] 开始清理基类资源..." << std::endl;
    if (m_video_encoder_ctx) {
        avcodec_free_context(&m_video_encoder_ctx);
        m_video_encoder_ctx = nullptr;
    }
    if (m_encoded_packet) {
        av_packet_free(&m_encoded_packet);
        m_encoded_packet = nullptr;
    }
    std::cout << "[BaseStreamer] 基类资源已清理。" << std::endl;
}

bool BaseStreamer::initialize_video_encoder(const StreamStrategy& strategy, int width, int height)
{
    if (m_video_encoder_ctx) { avcodec_free_context(&m_video_encoder_ctx); }
    const AVCodec* encoder = avcodec_find_encoder_by_name("hevc_nvenc");
    if (!encoder) return false;
    m_video_encoder_ctx = avcodec_alloc_context3(encoder);
    if (!m_video_encoder_ctx) return false;
    m_video_encoder_ctx->width = width;
    m_video_encoder_ctx->height = height;
    m_video_encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    m_video_encoder_ctx->time_base = { 1, 1000 };
    m_video_encoder_ctx->bit_rate = static_cast<int64_t>(m_base_bitrate * strategy.multiplier);
    m_video_encoder_ctx->framerate = { strategy.fps_limit, 1 };
    av_opt_set(m_video_encoder_ctx->priv_data, "preset", "p1", 0);
    av_opt_set(m_video_encoder_ctx->priv_data, "tune", "ll", 0);
    if (avcodec_open2(m_video_encoder_ctx, encoder, nullptr) < 0) {
        avcodec_free_context(&m_video_encoder_ctx);
        m_video_encoder_ctx = nullptr;
        return false;
    }
    return true;
}

void BaseStreamer::encode_and_send_video(AVFrame* frame)
{
    if (frame == nullptr) {
        if (m_video_encoder_ctx) avcodec_send_frame(m_video_encoder_ctx, nullptr); else return;
    }
    else {
        StreamStrategy current_strategy = m_controller->get_current_strategy();
        if (!m_video_encoder_ctx || current_strategy.multiplier != m_last_strategy.multiplier || current_strategy.fps_limit != m_last_strategy.fps_limit || m_video_encoder_ctx->width != frame->width || m_video_encoder_ctx->height != frame->height) {
            if (!initialize_video_encoder(current_strategy, frame->width, frame->height)) return;
            m_last_strategy = current_strategy;
        }
        if (avcodec_send_frame(m_video_encoder_ctx, frame) < 0) return;
    }
    int ret = 0;
    while (ret >= 0) {
        ret = avcodec_receive_packet(m_video_encoder_ctx, m_encoded_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;
        send_quic_data(AppConfig::PacketType::Video, m_encoded_packet->data, m_encoded_packet->size, m_encoded_packet->pts);
        av_packet_unref(m_encoded_packet);
    }
}

void BaseStreamer::send_quic_data(AppConfig::PacketType type, const uint8_t* payload, uint32_t payload_size, int64_t pts)
{
    if (!m_connection || !payload) return;

    const uint32_t HEADER_SIZE = 1 + 8 + 2 + 2; // Type, PTS, Count, Index

    if (payload_size <= MAX_DATAGRAM_PAYLOAD_SIZE) {
        std::vector<uint8_t> buffer(HEADER_SIZE + payload_size);
        uint8_t* ptr = buffer.data();
        *ptr++ = static_cast<uint8_t>(type);
        uint64_t pts_net = htonll_portable(pts);
        memcpy(ptr, &pts_net, sizeof(uint64_t));
        ptr += sizeof(uint64_t);
        uint16_t count_net = htons_portable(1);
        memcpy(ptr, &count_net, sizeof(uint16_t));
        ptr += sizeof(uint16_t);
        uint16_t index_net = htons_portable(0);
        memcpy(ptr, &index_net, sizeof(uint16_t));
        ptr += sizeof(uint16_t);
        memcpy(ptr, payload, payload_size);
        SendDatagram(buffer.data(), static_cast<uint32_t>(buffer.size()));
    }
    else {
        uint16_t fragment_count = static_cast<uint16_t>(std::ceil(static_cast<double>(payload_size) / MAX_DATAGRAM_PAYLOAD_SIZE));
        uint16_t count_net = htons_portable(fragment_count);
        for (uint16_t i = 0; i < fragment_count; ++i) {
            uint32_t offset = i * MAX_DATAGRAM_PAYLOAD_SIZE;
            uint32_t current_payload_size = std::min(MAX_DATAGRAM_PAYLOAD_SIZE, payload_size - offset);
            std::vector<uint8_t> buffer(HEADER_SIZE + current_payload_size);
            uint8_t* ptr = buffer.data();
            *ptr++ = static_cast<uint8_t>(type);
            uint64_t pts_net = htonll_portable(pts);
            memcpy(ptr, &pts_net, sizeof(uint64_t));
            ptr += sizeof(uint64_t);
            memcpy(ptr, &count_net, sizeof(uint16_t));
            ptr += sizeof(uint16_t);
            uint16_t index_net = htons_portable(i);
            memcpy(ptr, &index_net, sizeof(uint16_t));
            ptr += sizeof(uint16_t);
            memcpy(ptr, payload + offset, current_payload_size);
            SendDatagram(buffer.data(), static_cast<uint32_t>(buffer.size()));
        }
    }
}

void BaseStreamer::SendDatagram(const uint8_t* data, uint32_t length) {
    if (!m_connection) return;

    auto* context = new (std::nothrow) SendRequestContext();
    if (!context) {
        std::cerr << "[BaseStreamer] 错误: 无法为 SendRequestContext 分配内存" << std::endl;
        return;
    }

    // .assign的正确用法是 (first_iterator, last_iterator)
    context->Data.assign(data, data + length);
    context->QuicBuffer.Buffer = context->Data.data();
    context->QuicBuffer.Length = static_cast<uint32_t>(context->Data.size());

    QUIC_STATUS Status = m_msquic->DatagramSend(
        m_connection,
        &context->QuicBuffer,
        1,
        QUIC_SEND_FLAG_NONE,
        context
    );

    if (QUIC_FAILED(Status)) {
        std::cerr << "[BaseStreamer] 错误: DatagramSend 失败，代码: 0x" << std::hex << Status << std::endl;
        delete context;
    }
}