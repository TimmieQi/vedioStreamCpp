#pragma once

#include <QObject>
#include <QList>
#include <qtimer.h>
#include <deque> // 【新增】

// 前向声明
class NetworkMonitor;
class JitterBuffer;
class QuicClient;

// 【新增】定义网络趋势反馈类型
enum class NetworkTrend {
    Increase,
    Decrease,
    Hold
};

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
    void requestPause();
    void requestResume();
private slots:
    void onQuicConnectionSuccess(const QList<QString>& videoList);
    void onQuicConnectionFailed(const QString& reason);
    void onQuicPlayInfoReceived(double duration);
    void onQuicLatencyUpdated(double latencyMs);
    void onBandwidthUpdated(uint64_t bits_per_second); // 保留但逻辑上不再核心
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

    // 【新增】用于网络趋势分析的成员
    void analyzePacketArrival(int64_t timestamp_ms, int packet_size);
    NetworkTrend getNetworkTrend();

    struct PacketArrivalInfo {
        qint64 arrival_time_ms;
        int64_t media_timestamp_ms;
        int size;
    };
    std::deque<PacketArrivalInfo> m_packet_history;
    const int HISTORY_SIZE = 100;
};