// ClientWorker.h
#pragma once

#include <QObject>
#include <QList>
#include <qtimer.h>
#include <map> // for std::map

class NetworkMonitor;
class JitterBuffer;
class QuicClient;

class ClientWorker : public QObject
{
    Q_OBJECT

public:
    explicit ClientWorker(
        NetworkMonitor& monitor,
        JitterBuffer& videoBuffer,
        JitterBuffer& audioBuffer,
        QObject* parent = nullptr);

    ~ClientWorker();

public slots:
    void connectToServer(const QString& ip, quint16 port);
    void disconnectFromServer();
    void requestPlay(const QString& source);
    void requestSeek(double timeSec);

private slots:
    void onQuicConnectionSuccess(const QList<QString>& videoList);
    void onQuicConnectionFailed(const QString& reason);
    void onQuicPlayInfoReceived(double duration);
    void onQuicLatencyUpdated(double latencyMs);
    void processVideoPacket(const QByteArray& packet);
    void processAudioPacket(const QByteArray& packet);
    void sendHeartbeat();

signals:
    void connectionSuccess(const QList<QString>& videoList);
    void connectionFailed(const QString& reason);
    void playInfoReceived(double duration);
    void latencyUpdated(double latencyMs);

private:
    QuicClient* m_quicClient;
    QThread* m_quicThread;
    QTimer* m_heartbeatTimer;

    NetworkMonitor& m_monitor;
    JitterBuffer& m_videoJitterBuffer;
    JitterBuffer& m_audioJitterBuffer;

    bool m_isConnected;

    // 【新增】用于视频帧重组的缓冲区
    // Key: Timestamp, Value: map of <FragmentID, Payload>
    std::map<int64_t, std::map<uint32_t, QByteArray>> m_video_reassembly_buffer;
    // Key: Timestamp, Value: FragmentCount
    std::map<int64_t, uint32_t> m_video_fragment_counts;
};