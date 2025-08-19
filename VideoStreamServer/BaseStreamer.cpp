#define NOMINMAX
#include "BaseStreamer.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm> // 【新增】为 std::min 提供头文件

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
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
    m_scaled_frame = av_frame_alloc();
}

BaseStreamer::~BaseStreamer()
{
    stop();
    if (m_scaled_frame) {
        av_frame_free(&m_scaled_frame);
    }
    if (m_scaler_ctx) {
        sws_freeContext(m_scaler_ctx);
    }
}

void BaseStreamer::stop()
{
    m_control_block->running = false;
}

void BaseStreamer::seek(double time_sec)
{
    m_control_block->seek_to = time_sec;
}

void BaseStreamer::pause()
{
    m_control_block->paused = true;
}

void BaseStreamer::resume()
{
    m_control_block->paused = false;
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
    if (m_scaler_ctx) {
        sws_freeContext(m_scaler_ctx);
        m_scaler_ctx = nullptr;
    }
    std::cout << "[BaseStreamer] 基类资源已清理。" << std::endl;
}

bool BaseStreamer::initialize_video_encoder(int width, int height, int fps)
{
    // 1. 清理旧的编码器上下文
    if (m_video_encoder_ctx) {
        avcodec_free_context(&m_video_encoder_ctx);
    }
    const AVCodec* encoder = avcodec_find_encoder_by_name("hevc_nvenc");
    if (!encoder) {
        std::cerr << "[BaseStreamer] 错误: 找不到 hevc_nvenc 编码器。" << std::endl;
        return false;
    }
    m_video_encoder_ctx = avcodec_alloc_context3(encoder);
    if (!m_video_encoder_ctx) {
        std::cerr << "[BaseStreamer] 错误: 无法分配编码器上下文。" << std::endl;
        return false;
    }

    // 2. 从控制器获取当前的目标码率 (控制器已经被外部设置了正确的分辨率)
    int64_t target_bitrate = m_controller->get_decision().target_bitrate_bps;

    // 3. 配置编码器参数
    m_video_encoder_ctx->width = width;
    m_video_encoder_ctx->height = height;
    m_video_encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    m_video_encoder_ctx->time_base = { 1, 1000 }; // 时间基为毫秒
    m_video_encoder_ctx->bit_rate = target_bitrate;
    m_video_encoder_ctx->framerate = { fps, 1 };

    // NVENC 特定参数
    av_opt_set(m_video_encoder_ctx->priv_data, "preset", "p1", 0); // p1-p7, p1=fastest
    av_opt_set(m_video_encoder_ctx->priv_data, "tune", "ll", 0);   // ll=low latency
    av_opt_set(m_video_encoder_ctx->priv_data, "rc", "vbr", 0);    // 可变码率
    av_opt_set(m_video_encoder_ctx->priv_data, "cq", "21", 0);     // 恒定质量模式下的质量值

    // 4. 打开编码器
    if (avcodec_open2(m_video_encoder_ctx, encoder, nullptr) < 0) {
        std::cerr << "[BaseStreamer] 错误: 无法打开视频编码器。" << std::endl;
        avcodec_free_context(&m_video_encoder_ctx);
        m_video_encoder_ctx = nullptr;
        return false;
    }

    // 5. 更新状态变量
    m_last_set_bitrate = target_bitrate;
    m_last_set_height = height;
    m_last_set_fps = fps;

    std::cout << "[BaseStreamer] 编码器已初始化/重新初始化 -> "
        << width << "x" << height << "@" << fps << "fps, "
        << "目标码率: " << target_bitrate / 1024 << " kbps" << std::endl;

    return true;
}

void BaseStreamer::encode_and_send_video(AVFrame* frame)
{
    // 如果是 flush 操作 (frame == nullptr)，直接发送 null 帧给编码器
    if (frame == nullptr) {
        if (m_video_encoder_ctx) {
            avcodec_send_frame(m_video_encoder_ctx, nullptr);
        }
        else {
            return; // 编码器未初始化，无需冲洗
        }
    }
    else { // 正常的帧编码流程
        // 1. 获取ABR控制器的最新决策
        ABRDecision decision = m_controller->get_decision();

        // 2. 检查是否需要重新初始化编码器 (首次编码或分辨率/帧率变化)
        if (!m_video_encoder_ctx || decision.target_height != m_last_set_height || decision.target_fps != m_last_set_fps) {

            // 计算新的目标宽度以保持宽高比
            float scale = static_cast<float>(decision.target_height) / static_cast<float>(frame->height);
            int target_width = static_cast<int>(frame->width * scale);
            // 确保宽度是偶数
            target_width = (target_width / 2) * 2;

            if (!initialize_video_encoder(target_width, decision.target_height, decision.target_fps)) {
                std::cerr << "[BaseStreamer] 错误: 在编码循环中重新初始化编码器失败。" << std::endl;
                return;
            }
        }

        // 3. 检查是否需要动态调整码率 (在不重置编码器的情况下)
        int64_t target_bitrate = decision.target_bitrate_bps;
        if (std::abs(target_bitrate - m_last_set_bitrate) > m_last_set_bitrate * 0.05) {
            std::cout << "[BaseStreamer] 动态调整编码器码率 -> " << target_bitrate / 1024 << " kbps" << std::endl;
            m_video_encoder_ctx->bit_rate = target_bitrate;
            m_last_set_bitrate = target_bitrate;
        }

        // 4. 如果需要降采样，创建一个临时帧
        AVFrame* frame_to_encode = frame;
        AVFrame* scaled_frame = nullptr;

        if (m_video_encoder_ctx->width != frame->width || m_video_encoder_ctx->height != frame->height) {
            // 需要进行降采样
            scaled_frame = av_frame_alloc();
            scaled_frame->width = m_video_encoder_ctx->width;
            scaled_frame->height = m_video_encoder_ctx->height;
            scaled_frame->format = frame->format;
            av_frame_get_buffer(scaled_frame, 0);

            SwsContext* sws_ctx_scale = sws_getContext(
                frame->width, frame->height, (AVPixelFormat)frame->format,
                scaled_frame->width, scaled_frame->height, (AVPixelFormat)scaled_frame->format,
                SWS_BILINEAR, nullptr, nullptr, nullptr);

            sws_scale(sws_ctx_scale, (const uint8_t* const*)frame->data, frame->linesize,
                0, frame->height, scaled_frame->data, scaled_frame->linesize);

            sws_freeContext(sws_ctx_scale);
            scaled_frame->pts = frame->pts; // 传递时间戳
            frame_to_encode = scaled_frame;
        }

        // 5. 发送帧给编码器
        if (avcodec_send_frame(m_video_encoder_ctx, frame_to_encode) < 0) {
            // 错误处理
        }

        // 如果创建了临时缩放帧，释放它
        if (scaled_frame) {
            av_frame_free(&scaled_frame);
        }
    }

    // 6. 从编码器接收编码后的包
    int ret = 0;
    while (ret >= 0) {
        ret = avcodec_receive_packet(m_video_encoder_ctx, m_encoded_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        else if (ret < 0) {
            // 错误处理
            break;
        }
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