#pragma once

#include "IStreamer.h"
#include "AdaptiveStreamController.h"
#include "shared_config.h"
#include <memory>
#include <msquic.h>
#include <vector>

struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwsContext;
class BaseStreamer : public IStreamer, public std::enable_shared_from_this<BaseStreamer>
{
public:
    BaseStreamer(
        const QUIC_API_TABLE* msquic,
        HQUIC connection,
        std::shared_ptr<AdaptiveStreamController> controller
    );
    virtual ~BaseStreamer();

    void stop() final;
    void seek(double time_sec) override;
    void pause() final;
    void resume() final;
protected:
    bool initialize_video_encoder(int width, int height, int fps);
    void encode_and_send_video(AVFrame* frame);
    // 【修改】send_quic_data 现在内部处理分片
    void send_quic_data(AppConfig::PacketType type, const uint8_t* payload, uint32_t payload_size, int64_t pts);
    virtual void cleanup();
    // 【新增】用于缩放的上下文和目标帧
    SwsContext* m_scaler_ctx = nullptr;
    AVFrame* m_scaled_frame = nullptr;
    // 【新增】跟踪当前编码器使用的分辨率
    int m_current_encoder_height = 0;
private:
    // 【新增】一个辅助函数专门用于发送单个数据报（可能是分片）
    void SendDatagram(const uint8_t* data, uint32_t length);

    struct SendRequestContext {
        QUIC_BUFFER QuicBuffer;
        std::vector<uint8_t> Data;
    };

protected:
    const QUIC_API_TABLE* m_msquic;
    HQUIC m_connection;

    std::shared_ptr<StreamControlBlock> m_control_block;
    std::shared_ptr<AdaptiveStreamController> m_controller;

    AVCodecContext* m_video_encoder_ctx = nullptr;
    AVPacket* m_encoded_packet = nullptr;

    // 不再需要m_last_strategy和m_base_bitrate
    // StreamStrategy m_last_strategy = { 0.0, 0 };
    // int64_t m_base_bitrate = 1500 * 1024;

    // 跟踪上次设置的码率
    int64_t m_last_set_bitrate = 0;
    int m_last_set_height = 0;
    int m_last_set_fps = 0;
    // 定义一个安全的数据报负载大小阈值(MTU)
    const uint32_t MAX_DATAGRAM_PAYLOAD_SIZE = 1200;

};