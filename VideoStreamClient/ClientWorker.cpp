// ClientWorker.cpp

#include "ClientWorker.h"
#include "NetworkMonitor.h"
#include "JitterBuffer.h"
#include "UdpReceiver.h"
#include "shared_config.h"
#include <QDebug>
#include <QThread>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QHostAddress>
#include <cstring> // for memcpy

// 构造函数
ClientWorker::ClientWorker(
    NetworkMonitor& monitor,
    JitterBuffer& videoBuffer,
    JitterBuffer& audioBuffer,
    QObject* parent)
    : QObject(parent),
    m_monitor(monitor),
    m_isConnected(false),
    m_videoRecvThread(nullptr),
    m_videoReceiver(nullptr),
    m_videoJitterBuffer(videoBuffer),
    m_audioRecvThread(nullptr),
    m_audioReceiver(nullptr),
    m_audioJitterBuffer(audioBuffer)
{
    // 创建套接字和定时器
    m_controlSocket = new QUdpSocket(this);
    m_timeoutTimer = new QTimer(this);
    m_heartbeatTimer = new QTimer(this); // <--- 【新增】创建心跳定时器

    // 设置超时定时器为单次触发
    m_timeoutTimer->setSingleShot(true);

    // 连接内部信号槽
    connect(m_controlSocket, &QUdpSocket::readyRead, this, &ClientWorker::onControlSocketReadyRead);
    connect(m_controlSocket, &QUdpSocket::errorOccurred, this, &ClientWorker::onSocketError);
    connect(m_timeoutTimer, &QTimer::timeout, this, &ClientWorker::onConnectionTimeout);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &ClientWorker::sendHeartbeat); // <--- 【新增】连接心跳信号
}

ClientWorker::~ClientWorker()
{
    stopMediaReceivers();
    qDebug() << "[Worker] ClientWorker 销毁。";
}

// 和服务端链接
void ClientWorker::connectToServer(const QString& ip, quint16 port)
{
    QString logMessage = QString("[Worker] 收到连接请求，目标: %1:%2").arg(ip).arg(port);
    qDebug() << logMessage;

    if (m_isConnected) {
        qDebug() << "[Worker] 已经连接，先断开。";
        disconnectFromServer();
    }

    QJsonObject requestObject;
    requestObject["command"] = "get_list";
    QJsonDocument requestDoc(requestObject);
    QByteArray datagram = requestDoc.toJson(QJsonDocument::Compact);
    m_serverAddress = QHostAddress(ip);
    m_controlSocket->writeDatagram(datagram, m_serverAddress, port);
    m_timeoutTimer->start(5000);
}

void ClientWorker::disconnectFromServer()
{
    if (!m_isConnected && m_controlSocket->state() == QAbstractSocket::UnconnectedState) {
        return;
    }
    qDebug() << "[Worker] 断开与服务器的连接。";
    stopMediaReceivers();
    cleanup();
}

// 播放请求函数
void ClientWorker::requestPlay(const QString& source)
{
    if (!m_isConnected) return;
    qDebug() << "[Worker] 请求播放:" << source;

    stopMediaReceivers();
    m_videoJitterBuffer.reset();
    m_audioJitterBuffer.reset();
    m_monitor.reset();

    QJsonObject requestObject;
    requestObject["command"] = "play";
    requestObject["source"] = source;
    QJsonDocument requestDoc(requestObject);
    m_controlSocket->writeDatagram(requestDoc.toJson(QJsonDocument::Compact), m_serverAddress, AppConfig::CONTROL_PORT);
}

void ClientWorker::onControlSocketReadyRead()
{
    m_timeoutTimer->stop();

    while (m_controlSocket->hasPendingDatagrams())
    {
        QByteArray datagram;
        datagram.resize(static_cast<int>(m_controlSocket->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort;
        m_controlSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        qDebug() << "[Worker] 收到控制数据:" << datagram;

        if (datagram.isEmpty()) {
            qDebug() << "[Worker] 收到空控制包，忽略。";
            continue;
        }

        QJsonParseError parseError;
        QJsonDocument responseDoc = QJsonDocument::fromJson(datagram, &parseError);

        if (parseError.error != QJsonParseError::NoError) {
            qDebug() << "[Worker] 控制包JSON解析错误:" << parseError.errorString();
            continue;
        }

        bool should_start_heartbeat = false;

        if (responseDoc.isArray()) {
            QList<QString> videoList;
            QJsonArray videoArray = responseDoc.array();
            for (const QJsonValue& value : videoArray) {
                if (value.isString()) {
                    videoList.append(value.toString());
                }
            }
            m_isConnected = true;
            if (!sender.isNull()) {
                m_serverAddress = sender;
            }
            emit connectionSuccess(videoList);
            should_start_heartbeat = true; // 标记需要启动心跳
        }
        else if (responseDoc.isObject()) {
            QJsonObject obj = responseDoc.object();
            QString command = obj["command"].toString();
            if (obj.contains("command") && obj["command"].toString() == "play_info") {
                double duration = obj.value("duration").toDouble(0.0);
                emit playInfoReceived(duration);
                startMediaReceivers();
                should_start_heartbeat = true; // 标记需要启动/继续心跳
            }
            else if (command == "heartbeat_reply") {
                if (obj.contains("client_ts")) {
                    qint64 client_ts = obj["client_ts"].toVariant().toLongLong();
                    qint64 now_ts = QDateTime::currentMSecsSinceEpoch();
                    double rtt = static_cast<double>(now_ts - client_ts);

                    // 单向延迟通常估计为 RTT 的一半
                    double one_way_latency = rtt / 2.0;

                    // 发射信号，将计算出的延迟传递给UI线程
                    emit latencyUpdated(one_way_latency);
                }
            }
        }
        else {
            qDebug() << "[Worker] 收到未知格式的JSON响应。";
        }

        // <--- 【新增】在函数末尾统一处理心跳启动 ---
        if (should_start_heartbeat && !m_heartbeatTimer->isActive()) {
            qDebug() << "[Worker] 启动心跳定时器。";
            m_heartbeatTimer->start(1000); // 每秒发送一次
        }
    }
}

// 实现心跳函数
void ClientWorker::sendHeartbeat()
{
    if (!m_isConnected) {
        m_heartbeatTimer->stop();
        return;
    }

    NetworkStats stats = m_monitor.get_statistics();

    QJsonObject heartbeatObject;
    heartbeatObject["command"] = "heartbeat";
    heartbeatObject["loss_rate"] = stats.loss_rate;
    heartbeatObject["bitrate_bps"] = stats.bitrate_bps;
    heartbeatObject["client_ts"] = QDateTime::currentMSecsSinceEpoch();
    QJsonDocument doc(heartbeatObject);
    m_controlSocket->writeDatagram(doc.toJson(QJsonDocument::Compact), m_serverAddress, AppConfig::CONTROL_PORT);
}


void ClientWorker::startMediaReceivers()
{
    if (m_videoReceiver || m_audioReceiver) {
        qDebug() << "[Worker] 接收器已在运行，不重复启动。";
        return;
    }
    qDebug() << "[Worker] 启动媒体接收线程...";
    m_videoRecvThread = new QThread(this);
    m_videoReceiver = new UdpReceiver(AppConfig::VIDEO_PORT);
    m_videoReceiver->moveToThread(m_videoRecvThread);
    connect(m_videoReceiver, &UdpReceiver::packetReceived, this, &ClientWorker::processVideoPacket, Qt::QueuedConnection);
    connect(m_videoRecvThread, &QThread::started, m_videoReceiver, &UdpReceiver::startReceiving);
    connect(m_videoRecvThread, &QThread::finished, m_videoReceiver, &QObject::deleteLater);
    m_videoRecvThread->setObjectName("VideoRecvThread");
    m_videoRecvThread->start();
    m_audioRecvThread = new QThread(this);
    m_audioReceiver = new UdpReceiver(AppConfig::AUDIO_PORT);
    m_audioReceiver->moveToThread(m_audioRecvThread);
    connect(m_audioReceiver, &UdpReceiver::packetReceived, this, &ClientWorker::processAudioPacket, Qt::QueuedConnection);
    connect(m_audioRecvThread, &QThread::started, m_audioReceiver, &UdpReceiver::startReceiving);
    connect(m_audioRecvThread, &QThread::finished, m_audioReceiver, &QObject::deleteLater);
    m_audioRecvThread->setObjectName("AudioRecvThread");
    m_audioRecvThread->start();
}

void ClientWorker::stopMediaReceivers()
{
    if (m_videoRecvThread && m_videoRecvThread->isRunning()) {
        QMetaObject::invokeMethod(m_videoReceiver, "stopReceiving", Qt::QueuedConnection);
        m_videoRecvThread->quit();
        if (!m_videoRecvThread->wait(500)) {
            qDebug() << "[Worker] 警告: 视频接收线程超时未停止。";
        }
    }
    m_videoRecvThread = nullptr;
    m_videoReceiver = nullptr;
    if (m_audioRecvThread && m_audioRecvThread->isRunning()) {
        QMetaObject::invokeMethod(m_audioReceiver, "stopReceiving", Qt::QueuedConnection);
        m_audioRecvThread->quit();
        if (!m_audioRecvThread->wait(500)) {
            qDebug() << "[Worker] 警告: 音频接收线程超时未停止。";
        }
    }
    m_audioRecvThread = nullptr;
    m_audioReceiver = nullptr;
    qDebug() << "[Worker] 媒体接收线程已请求停止。";
}

void ClientWorker::processVideoPacket(const QByteArray& packet)
{
    if (packet.size() < 7) return;
    const uchar* data = reinterpret_cast<const uchar*>(packet.constData());
    uint16_t seq = qFromBigEndian<uint16_t>(data);
    int32_t ts = qFromBigEndian<int32_t>(data + 2);
    m_monitor.record_packet(seq, packet.size());
    auto mediaPacket = std::make_unique<MediaPacket>();
    mediaPacket->seq = seq;
    mediaPacket->ts = ts;
    mediaPacket->payload.assign(packet.constData(), packet.constData() + packet.size());
    m_videoJitterBuffer.add_packet(std::move(mediaPacket));
}

void ClientWorker::processAudioPacket(const QByteArray& packet)
{
    if (packet.size() < 8) return;
    const uchar* data = reinterpret_cast<const uchar*>(packet.constData());
    uint32_t seq = qFromBigEndian<uint32_t>(data);
    int32_t ts = qFromBigEndian<int32_t>(data + 4);
    auto mediaPacket = std::make_unique<MediaPacket>();
    mediaPacket->seq = seq;
    mediaPacket->ts = ts;
    mediaPacket->payload.assign(packet.constData() + 8, packet.constData() + packet.size());
    m_audioJitterBuffer.add_packet(std::move(mediaPacket));
}

void ClientWorker::onSocketError(QAbstractSocket::SocketError socketError)
{
    // 在出错时停止心跳 
    m_timeoutTimer->stop();
    m_heartbeatTimer->stop();
    qDebug() << "[Worker] Socket 错误:" << m_controlSocket->errorString();
    emit connectionFailed(m_controlSocket->errorString());
    cleanup();
}

void ClientWorker::onConnectionTimeout()
{
    if (!m_isConnected) {
        // 在超时时停止心跳 
        m_heartbeatTimer->stop();
        qDebug() << "[Worker] 连接超时，服务器无响应。";
        emit connectionFailed("连接超时，服务器无响应。");
        cleanup();
    }
}

void ClientWorker::cleanup()
{
    m_isConnected = false;
    m_timeoutTimer->stop();
    m_heartbeatTimer->stop(); //在清理时停止心跳
    if (m_controlSocket->state() != QAbstractSocket::UnconnectedState) {
        m_controlSocket->close();
    }
}

void ClientWorker::requestSeek(double timeSec)
{
    if (!m_isConnected) return;
    qDebug() << "[Worker] 请求跳转到" << timeSec << "秒";

    // 构建并发送 seek 命令
    QJsonObject requestObject;
    requestObject["command"] = "seek";
    requestObject["time"] = timeSec;
    QJsonDocument doc(requestObject);
    m_controlSocket->writeDatagram(doc.toJson(QJsonDocument::Compact), m_serverAddress, AppConfig::CONTROL_PORT);
}