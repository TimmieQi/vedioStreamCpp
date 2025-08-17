#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QList>
#include <msquic.h>
#include <atomic>
#include "shared_config.h" // 包含 shared_config.h 以获取 PacketType

class QuicClient : public QObject
{
    Q_OBJECT

public:
    explicit QuicClient(QObject* parent = nullptr);
    ~QuicClient();

public slots:
    void connectToServer(const QString& host, quint16 port);
    void disconnectFromServer();
    void sendControlCommand(const QByteArray& command);

signals:
    void connectionSuccess(const QList<QString>& videoList);
    void connectionFailed(const QString& reason);
    void playInfoReceived(double duration);
    // 【修改】信号传递的是完整的包（包含自定义头）
    void videoPacketReceived(const QByteArray& packet);
    void audioPacketReceived(const QByteArray& packet);
    void latencyUpdated(double latencyMs);
    // 传递估计的带宽
    void bandwidthUpdated(uint64_t bits_per_second);
private:
    struct SendRequest {
        QUIC_BUFFER QuicBuffer;
        QByteArray Data;
    };

    const QUIC_API_TABLE* m_msquic = nullptr;
    HQUIC m_registration = nullptr;
    HQUIC m_configuration = nullptr;
    HQUIC m_connection = nullptr;

    HQUIC m_control_stream = nullptr;
    // 【移除】不再需要音视频流句柄
    // HQUIC m_video_stream = nullptr;
    // HQUIC m_audio_stream = nullptr;

    std::atomic<bool> m_is_running{ false };

    // 【移除】不再需要接收缓冲区和处理函数
    // QByteArray m_video_receive_buffer;
    // QByteArray m_audio_receive_buffer;
    // void ProcessStreamBuffer(HQUIC Stream);

    void cleanup();

    static QUIC_STATUS QUIC_API ConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
    static QUIC_STATUS QUIC_API StreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);

    QUIC_STATUS HandleConnectionEvent(HQUIC Connection, QUIC_CONNECTION_EVENT* Event);
    QUIC_STATUS HandleStreamEvent(HQUIC Stream, QUIC_STREAM_EVENT* Event);
};