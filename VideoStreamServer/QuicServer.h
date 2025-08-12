#pragma once

#include <msquic.h>
#include <string>
#include <memory>
#include <vector>
#include <atomic>
#include "nlohmann/json.hpp"

// 前向声明
class StreamerManager;

class QuicServer
{
public:
    QuicServer(std::shared_ptr<StreamerManager> streamer_manager);
    ~QuicServer();

    bool Start(const std::string& cert_hash, uint16_t port);
    void Stop();
    const QUIC_API_TABLE* GetMsQuicApi() const;

private:
    // 【关键改变】自定义上下文结构体
    // 这个结构体将作为每个 Stream 的上下文，
    // 让我们在 StreamCallback 中能同时访问 QuicServer 实例和 Stream 所属的 Connection
    struct StreamContext {
        QuicServer* Server;
        HQUIC Connection;
    };

    // 用于管理 StreamSend 异步操作内存的辅助结构体
    struct SendRequest {
        QUIC_BUFFER QuicBuffer;
        std::vector<uint8_t> Data;
    };

    // MsQuic 核心对象
    const QUIC_API_TABLE* m_msquic = nullptr;
    HQUIC m_registration = nullptr;
    HQUIC m_configuration = nullptr;
    HQUIC m_listener = nullptr;

    // 关联的业务逻辑
    std::shared_ptr<StreamerManager> m_streamer_manager;
    std::atomic<bool> m_running{ false };

    // --- MsQuic 回调函数 (C-Style, static) ---
    static QUIC_STATUS QUIC_API ListenerCallback(HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event);
    static QUIC_STATUS QUIC_API ConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
    static QUIC_STATUS QUIC_API StreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);

    // --- C++ 成员方法，由上述静态回调函数调用 ---
    QUIC_STATUS HandleListenerEvent(QUIC_LISTENER_EVENT* Event);
    QUIC_STATUS HandleConnectionEvent(HQUIC Connection, QUIC_CONNECTION_EVENT* Event);
    QUIC_STATUS HandleStreamEvent(HQUIC Stream, QUIC_STREAM_EVENT* Event);

    // 辅助函数
    bool LoadConfiguration(const std::string& cert_hash);
    // 【关键改变】HandleControlCommand 需要知道是哪个 Connection
    void HandleControlCommand(HQUIC Connection, HQUIC Stream, const nlohmann::json& command_json);
};