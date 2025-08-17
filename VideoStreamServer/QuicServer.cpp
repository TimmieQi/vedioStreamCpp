#include "QuicServer.h"
#include "StreamerManager.h"
#include "FileSystemManager.h"
#include "AdaptiveStreamController.h"
#include <msquic.h>
#include <iostream>
#include <vector>
#include<fstream>
// Helper functions to decode hex string (for certificate hash)
uint8_t DecodeHexChar(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return 0;
}

uint32_t DecodeHexBuffer(const char* HexBuffer, uint32_t OutBufferLen, uint8_t* OutBuffer)
{
    uint32_t HexBufferLen = (uint32_t)strlen(HexBuffer) / 2;
    if (HexBufferLen > OutBufferLen) {
        return 0;
    }
    for (uint32_t i = 0; i < HexBufferLen; i++) {
        OutBuffer[i] = (DecodeHexChar(HexBuffer[i * 2]) << 4) | DecodeHexChar(HexBuffer[i * 2 + 1]);
    }
    return HexBufferLen;
}


QuicServer::QuicServer(std::shared_ptr<StreamerManager> streamer_manager)
    : m_streamer_manager(streamer_manager) {}

QuicServer::~QuicServer()
{
    Stop();
}

const QUIC_API_TABLE* QuicServer::GetMsQuicApi() const
{
    return m_msquic;
}

bool QuicServer::Start(const std::string& cert_hash, uint16_t port)
{
    if (m_running) return true;

    // 使用 MsQuicOpenVersion 显式请求 V2 API
    const uint32_t DesiredApiVersion = 2;
    if (QUIC_FAILED(MsQuicOpenVersion(DesiredApiVersion, (const void**)&m_msquic))) {
        std::cerr << "[QuicServer] 错误: MsQuicOpenVersion 失败。请确保已安装支持Datagrams的MsQuic版本 (2.0+)。" << std::endl;
        return false;
    }

    const QUIC_REGISTRATION_CONFIG RegConfig = { "VideoStreamServer", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    if (QUIC_FAILED(m_msquic->RegistrationOpen(&RegConfig, &m_registration))) {
        std::cerr << "[QuicServer] 错误: RegistrationOpen 失败" << std::endl; return false;
    }

    if (!LoadConfiguration(cert_hash)) {
        std::cerr << "[QuicServer] 致命错误: LoadConfiguration 失败！" << std::endl;
        return false;
    }

    if (QUIC_FAILED(m_msquic->ListenerOpen(m_registration, ListenerCallback, this, &m_listener))) {
        std::cerr << "[QuicServer] 错误: ListenerOpen 失败" << std::endl; return false;
    }

    QUIC_ADDR Address = {};
    QuicAddrSetFamily(&Address, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&Address, port);
    const QUIC_BUFFER Alpn = { sizeof("vstream") - 1, (uint8_t*)"vstream" };

    QUIC_STATUS StartStatus = m_msquic->ListenerStart(m_listener, &Alpn, 1, &Address);
    if (QUIC_FAILED(StartStatus)) {
        std::cerr << "[QuicServer] 错误: ListenerStart 失败，代码: 0x" << std::hex << StartStatus << std::endl;
        return false;
    }

    std::cout << "[QuicServer] 服务器已在 QUIC 端口 " << port << " 上成功启动并监听。" << std::endl;
    m_running = true;
    return true;
}

void QuicServer::Stop()
{
    if (!m_running.exchange(false)) return;

    if (m_msquic) {
        if (m_listener) m_msquic->ListenerClose(m_listener);
        m_listener = nullptr;
        if (m_configuration) m_msquic->ConfigurationClose(m_configuration);
        m_configuration = nullptr;
        if (m_registration) m_msquic->RegistrationClose(m_registration);
        m_registration = nullptr;

        MsQuicClose(m_msquic);
        m_msquic = nullptr;
    }
    std::cout << "[QuicServer] 服务器已停止。" << std::endl;
}

bool QuicServer::LoadConfiguration(const std::string& cert_hash)
{
    QUIC_SETTINGS Settings = { 0 };
    Settings.IdleTimeoutMs = 10000;
    Settings.IsSet.IdleTimeoutMs = TRUE;
    Settings.ServerResumptionLevel = QUIC_SERVER_RESUME_AND_ZERORTT;
    Settings.IsSet.ServerResumptionLevel = TRUE;
    Settings.CongestionControlAlgorithm = QUIC_CONGESTION_CONTROL_ALGORITHM_BBR;
    // 客户端将打开一个双向流用于控制
    Settings.PeerBidiStreamCount = 1;
    Settings.IsSet.PeerBidiStreamCount = TRUE;

    // 音视频都走Datagram，服务器不需要主动打开任何流
    // Settings.PeerUnidiStreamCount = 0;
    // Settings.IsSet.PeerUnidiStreamCount = TRUE;

    // 允许服务器发送和接收Datagram
    Settings.DatagramReceiveEnabled = TRUE;
    Settings.IsSet.DatagramReceiveEnabled = TRUE;

    bool pacing_enabled = true; // 生产环境默认值
    std::ifstream config_file("config.json");
    if (config_file.is_open()) {
        try {
            nlohmann::json config_json;
            config_file >> config_json;
            // 如果文件中存在该字段，则使用文件中的值
            pacing_enabled = config_json.value("pacing_enabled", true);
        }
        catch (...) {
            /* 忽略解析错误，使用默认值 */ 
        }
    }
    Settings.IsSet.PacingEnabled = TRUE;
    Settings.PacingEnabled = pacing_enabled;

    if (!pacing_enabled) {
        // 2. 调整 HyStart (慢启动算法)
// HyStart 旨在更快地退出慢启动阶段。在本地回环中，禁用它可能有助于避免过早地判断拥塞。
        Settings.IsSet.HyStartEnabled = TRUE;
        Settings.HyStartEnabled = FALSE; // 设置为 FALSE

        // 3. 设置一个非常大的初始拥塞窗口 (CWND)
        // 告诉拥塞控制器，我们一开始就认为网络很好，可以发送大量数据。
        Settings.IsSet.InitialWindowPackets = TRUE;
        Settings.InitialWindowPackets = 100; // 设置一个较大的值，例如100个包
    }




    const QUIC_BUFFER Alpn = { sizeof("vstream") - 1, (uint8_t*)"vstream" };
    if (QUIC_FAILED(m_msquic->ConfigurationOpen(m_registration, &Alpn, 1, &Settings, sizeof(Settings), nullptr, &m_configuration))) {
        std::cerr << "[QuicServer] 错误: ConfigurationOpen 失败" << std::endl;
        return false;
    }

    QUIC_CERTIFICATE_HASH CertHashConfig;
    if (DecodeHexBuffer(cert_hash.c_str(), sizeof(CertHashConfig.ShaHash), CertHashConfig.ShaHash) != sizeof(CertHashConfig.ShaHash)) {
        std::cerr << "[QuicServer] 解码证书指纹失败！" << std::endl;
        return false;
    }

    QUIC_CREDENTIAL_CONFIG CredConfig;
    memset(&CredConfig, 0, sizeof(CredConfig));
    CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH;
    CredConfig.CertificateHash = &CertHashConfig;

    if (QUIC_FAILED(m_msquic->ConfigurationLoadCredential(m_configuration, &CredConfig))) {
        std::cerr << "[QuicServer] ConfigurationLoadCredential 失败！" << std::endl;
        return false;
    }

    std::cout << "[QuicServer] 已成功从证书存储加载证书配置。" << std::endl;
    return true;
}

QUIC_STATUS QUIC_API QuicServer::ListenerCallback(HQUIC, void* Context, QUIC_LISTENER_EVENT* Event) {
    return static_cast<QuicServer*>(Context)->HandleListenerEvent(Event);
}

QUIC_STATUS QuicServer::HandleListenerEvent(QUIC_LISTENER_EVENT* Event) {
    if (Event->Type == QUIC_LISTENER_EVENT_NEW_CONNECTION) {
        m_msquic->SetCallbackHandler(Event->NEW_CONNECTION.Connection, (void*)ConnectionCallback, this);
        return m_msquic->ConnectionSetConfiguration(Event->NEW_CONNECTION.Connection, m_configuration);
    }
    return QUIC_STATUS_NOT_SUPPORTED;
}

QUIC_STATUS QUIC_API QuicServer::ConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {
    return static_cast<QuicServer*>(Context)->HandleConnectionEvent(Connection, Event);
}

QUIC_STATUS QuicServer::HandleConnectionEvent(HQUIC Connection, QUIC_CONNECTION_EVENT* Event) {
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED: {
        std::cout << "[QuicServer] 连接 " << Connection << " 已建立。" << std::endl;

        // 确认连接的Datagram发送能力
        BOOLEAN DatagramSendEnabled = FALSE;
        uint32_t BufferSize = sizeof(DatagramSendEnabled);
        m_msquic->GetParam(Connection, QUIC_PARAM_CONN_DATAGRAM_SEND_ENABLED, &BufferSize, &DatagramSendEnabled);
        if (DatagramSendEnabled) {
            std::cout << "[QuicServer] 确认：此连接的Datagram发送已启用。" << std::endl;
        }
        else {
            std::cerr << "[QuicServer] 警告：此连接的Datagram发送未启用！媒体流将无法发送。" << std::endl;
        }
        break;
    }
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        std::cout << "[QuicServer] 连接 " << Connection << " 已完全关闭。" << std::endl;
        m_streamer_manager->stop_stream();
        m_msquic->ConnectionClose(Connection);
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        // 为每个新来的流创建一个上下文，包含服务器指针和连接句柄
        auto* Ctx = new (std::nothrow) StreamContext{ this, Connection };
        if (!Ctx) return QUIC_STATUS_OUT_OF_MEMORY;
        m_msquic->SetCallbackHandler(Event->PEER_STREAM_STARTED.Stream, (void*)StreamCallback, Ctx);
        break;
    }
    default: break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API QuicServer::StreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    auto* Ctx = static_cast<StreamContext*>(Context);
    return Ctx->Server->HandleStreamEvent(Stream, Event);
}

QUIC_STATUS QuicServer::HandleStreamEvent(HQUIC Stream, QUIC_STREAM_EVENT* Event) {
    // 从流的上下文中获取我们的自定义结构体
    auto* Ctx = static_cast<StreamContext*>(m_msquic->GetContext(Stream));
    if (!Ctx) return QUIC_STATUS_INVALID_STATE;

    switch (Event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        std::string received_data;
        for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
            received_data.append(reinterpret_cast<const char*>(Event->RECEIVE.Buffers[i].Buffer), Event->RECEIVE.Buffers[i].Length);
        }
        try {
            // 将连接句柄传递给命令处理器，以便start_stream可以获取它
            HandleControlCommand(Ctx->Connection, Stream, nlohmann::json::parse(received_data));
        }
        catch (const nlohmann::json::parse_error& e) {
            std::cerr << "[QuicServer] 错误: 解析JSON失败: " << e.what() << std::endl;
        }
        break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
        // 释放为异步发送分配的内存
        auto* request = static_cast<SendRequest*>(Event->SEND_COMPLETE.ClientContext);
        delete request;
        break;
    }
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        // 流已关闭，释放所有相关资源
        m_msquic->StreamClose(Stream);
        delete Ctx;
        break;
    }
    default: break;
    }
    return QUIC_STATUS_SUCCESS;
}

void QuicServer::HandleControlCommand(HQUIC Connection, HQUIC Stream, const nlohmann::json& command_json) {
    nlohmann::json response_json;
    std::string command_str = command_json.value("command", "");

    if (command_str == "get_list") {
        response_json = FileSystemManager::get_video_files();
        response_json.push_back("camera");
    }
    else if (command_str == "play") {
        std::string source = command_json.value("source", "");
        if (!source.empty()) {
            response_json = m_streamer_manager->start_stream(source, Connection, this);
        }
        else {
            response_json["error"] = "Source is empty";
        }
    }
    else if (command_str == "seek") {
        double time = command_json.value("time", -1.0);
        if (time >= 0) m_streamer_manager->seek_stream(time);
        return; // seek命令不需要回复
    }
    else if (command_str == "pause") {
        m_streamer_manager->pause_stream();
        return; // 无需回复
    }
    else if (command_str == "resume") {
        m_streamer_manager->resume_stream();
        return; // 无需回复
    }
    else if (command_str == "heartbeat") {
        std::string trend = command_json.value("trend", "hold");
        m_streamer_manager->get_controller()->update_client_feedback(trend);

        if (command_json.contains("client_ts")) {
            response_json["command"] = "heartbeat_reply";
            response_json["client_ts"] = command_json["client_ts"];
        }
        else {
            return;
        }
    }
    else {
        response_json["error"] = "Unknown command";
    }

    if (response_json.is_null() || (response_json.is_object() && response_json.empty())) {
        return;
    }

    auto response_str = response_json.dump();
    auto* request = new (std::nothrow) SendRequest();
    if (!request) return;

    request->Data.assign(response_str.begin(), response_str.end());
    request->QuicBuffer.Buffer = request->Data.data();
    request->QuicBuffer.Length = static_cast<uint32_t>(request->Data.size());

    // 在控制流上异步发回响应
    if (QUIC_FAILED(m_msquic->StreamSend(Stream, &request->QuicBuffer, 1, QUIC_SEND_FLAG_NONE, request))) {
        delete request;
    }
}