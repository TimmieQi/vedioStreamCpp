#include "QuicClient.h"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QHostAddress>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <byteswap.h>
#define _byteswap_uint64 bswap_64
#endif

inline uint64_t ntohll_portable(uint64_t value) {
    const int num = 1;
    if (*(char*)&num == 1) { return _byteswap_uint64(value); }
    else { return value; }
}

QuicClient::QuicClient(QObject* parent) : QObject(parent)
{
}

QuicClient::~QuicClient()
{
    disconnectFromServer();
}

void QuicClient::cleanup()
{
    if (!m_is_running.exchange(false)) {
        return;
    }
    if (m_msquic) {
        if (m_connection) {
            m_msquic->ConnectionShutdown(m_connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        }
        if (m_configuration) {
            m_msquic->ConfigurationClose(m_configuration);
            m_configuration = nullptr;
        }
        if (m_registration) {
            m_msquic->RegistrationClose(m_registration);
            m_registration = nullptr;
        }
        MsQuicClose(m_msquic);
        m_msquic = nullptr;
    }
    m_control_stream = nullptr;
    // 【移除】缓冲区清理
}

void QuicClient::connectToServer(const QString& host, quint16 port)
{
    if (m_is_running) {
        qDebug() << "[QuicClient] 已经在连接中，请先断开。";
        return;
    }

    QUIC_STATUS status = QUIC_STATUS_SUCCESS;

    if (QUIC_FAILED(status = MsQuicOpen2(&m_msquic))) {
        emit connectionFailed("MsQuicOpen2 failed");
        return;
    }

    const QUIC_REGISTRATION_CONFIG regConfig = { "VideoStreamClient", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    if (QUIC_FAILED(status = m_msquic->RegistrationOpen(&regConfig, &m_registration))) {
        emit connectionFailed("RegistrationOpen failed");
        cleanup();
        return;
    }

    const QUIC_BUFFER alpn = { sizeof("vstream") - 1, (uint8_t*)"vstream" };
    QUIC_SETTINGS settings = { 0 };
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.IdleTimeoutMs = 10000;

    // 【核心修改】设置 DatagramReceiveEnabled 为 TRUE
    settings.IsSet.DatagramReceiveEnabled = TRUE;
    settings.DatagramReceiveEnabled = TRUE;

    // 我们不再主动打开流，所以可以移除 Peer*StreamCount 的设置
    // settings.PeerUnidiStreamCount = 2;
    // settings.IsSet.PeerUnidiStreamCount = TRUE;
    settings.PeerBidiStreamCount = 1; // 仍然需要一个双向流用于控制
    settings.IsSet.PeerBidiStreamCount = TRUE;

    if (QUIC_FAILED(status = m_msquic->ConfigurationOpen(m_registration, &alpn, 1, &settings, sizeof(settings), nullptr, &m_configuration))) {
        emit connectionFailed("ConfigurationOpen failed");
        cleanup();
        return;
    }

    QUIC_CREDENTIAL_CONFIG credConfig;
    memset(&credConfig, 0, sizeof(credConfig));
    credConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
    credConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

    if (QUIC_FAILED(status = m_msquic->ConfigurationLoadCredential(m_configuration, &credConfig))) {
        emit connectionFailed("ConfigurationLoadCredential failed");
        cleanup();
        return;
    }

    if (QUIC_FAILED(status = m_msquic->ConnectionOpen(m_registration, ConnectionCallback, this, &m_connection))) {
        emit connectionFailed("ConnectionOpen failed");
        cleanup();
        return;
    }

    QHostAddress hostAddr(host);
    QUIC_ADDRESS_FAMILY family = QUIC_ADDRESS_FAMILY_UNSPEC;
    if (hostAddr.protocol() == QAbstractSocket::IPv4Protocol) {
        family = QUIC_ADDRESS_FAMILY_INET;
    }
    else if (hostAddr.protocol() == QAbstractSocket::IPv6Protocol) {
        family = QUIC_ADDRESS_FAMILY_INET6;
    }

    const char* serverName = "localhost";

    m_is_running = true;
    qDebug() << "[QuicClient] 正在连接到主机:" << host << "端口:" << port << "，使用SNI:" << serverName;

    if (QUIC_FAILED(status = m_msquic->ConnectionStart(m_connection, m_configuration, family, serverName, port))) {
        emit connectionFailed(QString("ConnectionStart failed, 0x%x").arg(status));
        m_msquic->ConnectionClose(m_connection);
        m_connection = nullptr;
        cleanup();
        return;
    }
}

void QuicClient::disconnectFromServer()
{
    if (!m_is_running) return;
    qDebug() << "[QuicClient] 请求断开连接。";
    cleanup();
}

void QuicClient::sendControlCommand(const QByteArray& command)
{
    if (!m_is_running || !m_control_stream) {
        return;
    }
    auto* request = new (std::nothrow) SendRequest();
    if (!request) return;
    request->Data = command;
    request->QuicBuffer.Buffer = (uint8_t*)request->Data.data();
    request->QuicBuffer.Length = request->Data.size();

    QUIC_STATUS status = m_msquic->StreamSend(m_control_stream, &request->QuicBuffer, 1, QUIC_SEND_FLAG_NONE, request);
    if (QUIC_FAILED(status)) {
        delete request;
    }
}

QUIC_STATUS QUIC_API QuicClient::ConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {
    auto* pThis = static_cast<QuicClient*>(Context);
    if (!pThis->m_is_running) return QUIC_STATUS_ABORTED;
    return pThis->HandleConnectionEvent(Connection, Event);
}

// 【核心修改】处理数据报接收事件
QUIC_STATUS QuicClient::HandleConnectionEvent(HQUIC Connection, QUIC_CONNECTION_EVENT* Event) {
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        qDebug() << "[QuicClient] 连接成功！";
        if (QUIC_FAILED(m_msquic->StreamOpen(m_connection, QUIC_STREAM_OPEN_FLAG_NONE, StreamCallback, this, &m_control_stream))) {
            emit connectionFailed("无法打开控制流");
        }
        else {
            if (QUIC_FAILED(m_msquic->StreamStart(m_control_stream, QUIC_STREAM_START_FLAG_NONE))) {
                emit connectionFailed("无法启动控制流");
            }
        }
        break;

        // 【移除】不再处理 QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED

        // 【新增】处理数据报接收事件
    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
        const QUIC_BUFFER* datagram = Event->DATAGRAM_RECEIVED.Buffer;

        // 我们只处理一个 buffer 的情况，因为我们发送时也是一个 buffer
        if (datagram->Length < (sizeof(AppConfig::PacketType) + sizeof(int64_t))) {
            // 包太小，无法包含我们的头，直接忽略
            break;
        }

        const uint8_t* buffer_ptr = datagram->Buffer;
        AppConfig::PacketType type = static_cast<AppConfig::PacketType>(*buffer_ptr);

        // 将整个数据报（包含我们的头）封装到 QByteArray 中
        QByteArray packet(reinterpret_cast<const char*>(datagram->Buffer), datagram->Length);

        if (type == AppConfig::PacketType::Video) {
            emit videoPacketReceived(packet);
        }
        else if (type == AppConfig::PacketType::Audio) {
            emit audioPacketReceived(packet);
        }

        break;
    }

                                                // 其他事件处理保持不变...
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        emit connectionFailed("连接中断");
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        emit connectionFailed("服务器断开连接");
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            m_msquic->ConnectionClose(m_connection);
            m_connection = nullptr;
        }
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API QuicClient::StreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    auto* pThis = static_cast<QuicClient*>(Context);
    if (!pThis->m_is_running) return QUIC_STATUS_ABORTED;
    // 这个回调现在只用于控制流
    return pThis->HandleStreamEvent(Stream, Event);
}

// 【核心修改】此函数现在只处理控制流事件
QUIC_STATUS QuicClient::HandleStreamEvent(HQUIC Stream, QUIC_STREAM_EVENT* Event) {
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        // Stream 一定是 m_control_stream
        qDebug() << "[QuicClient] 控制流启动成功，请求文件列表。";
        sendControlCommand("{\"command\":\"get_list\"}");
        break;
    case QUIC_STREAM_EVENT_RECEIVE: {
        QByteArray received_data;
        for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
            received_data.append(
                reinterpret_cast<const char*>(Event->RECEIVE.Buffers[i].Buffer),
                Event->RECEIVE.Buffers[i].Length
            );
        }
        // 以下 JSON 解析逻辑保持不变
        QJsonDocument doc = QJsonDocument::fromJson(received_data);
        if (doc.isArray()) {
            QList<QString> videoList;
            for (const auto& val : doc.array()) videoList.append(val.toString());
            emit connectionSuccess(videoList);
        }
        else if (doc.isObject()) {
            QJsonObject obj = doc.object();
            if (obj.contains("command") && obj["command"] == "play_info") {
                emit playInfoReceived(obj["duration"].toDouble());
            }
            else if (obj.contains("command") && obj["command"] == "heartbeat_reply") {
                qint64 client_ts = obj["client_ts"].toVariant().toLongLong();
                qint64 now_ts = QDateTime::currentMSecsSinceEpoch();
                emit latencyUpdated(static_cast<double>(now_ts - client_ts) / 2.0);
            }
        }
        break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        delete static_cast<SendRequest*>(Event->SEND_COMPLETE.ClientContext);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        qDebug() << "[QuicClient] 控制流" << Stream << "完全关闭。";
        if (Stream == m_control_stream) m_control_stream = nullptr;
        m_msquic->StreamClose(Stream);
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}