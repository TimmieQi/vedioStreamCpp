#pragma once

#include <QObject>
#include <QUdpSocket>

// 这是一个通用的UDP接收器，它会在自己的线程中运行一个阻塞的接收循环
class UdpReceiver : public QObject
{
    Q_OBJECT

public:
    explicit UdpReceiver(quint16 port, QObject* parent = nullptr);
    ~UdpReceiver();
signals:
    // 每当收到一个完整的数据包时，就发射这个信号
    void packetReceived(const QByteArray& packet);
    void bindFailed(const QString& errorString); // 用于报告绑定失败
public slots:
    // 启动接收循环的槽函数
    void startReceiving();
    // 停止接收的槽函数
    void stopReceiving();
    // 当socket有数据可读时，这个槽会被调用
    void onReadyRead();

private:
    QUdpSocket *m_socket;
    quint16 m_port;
    // 使用原子布尔值来安全地控制循环的停止
    std::atomic<bool> m_isReceiving;
};