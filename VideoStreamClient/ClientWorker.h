// ClientWorker.h

#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QUdpSocket>
#include <QTimer>
#include <QHostAddress>

// 前向声明
class NetworkMonitor;
class JitterBuffer;
class UdpReceiver;

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
    void requestSeek(double timeSec); // 进度条拖动槽函数
private slots:
    void onControlSocketReadyRead();
    void onSocketError(QAbstractSocket::SocketError socketError);
    void onConnectionTimeout();
    void processVideoPacket(const QByteArray& packet);
    void processAudioPacket(const QByteArray& packet);
    void sendHeartbeat(); // 心跳槽函数

private:
    void cleanup();
    void startMediaReceivers();
    void stopMediaReceivers();

signals:
    void connectionSuccess(const QList<QString>& videoList);
    void connectionFailed(const QString& reason);
    void playInfoReceived(double duration);
    void latencyUpdated(double latencyMs);
private:
    QHostAddress m_serverAddress;

    QUdpSocket* m_controlSocket;
    QTimer* m_timeoutTimer;
    QTimer* m_heartbeatTimer; // 心跳定时器
    NetworkMonitor& m_monitor;
    bool m_isConnected;

    QThread* m_videoRecvThread;
    UdpReceiver* m_videoReceiver;
    JitterBuffer& m_videoJitterBuffer;

    QThread* m_audioRecvThread;
    UdpReceiver* m_audioReceiver;
    JitterBuffer& m_audioJitterBuffer;
};