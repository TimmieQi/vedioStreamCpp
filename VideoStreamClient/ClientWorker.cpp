#include "ClientWorker.h"
#include "QuicClient.h"
#include "NetworkMonitor.h"
#include "JitterBuffer.h"
#include "shared_config.h"
#include "MediaPacket.h"

#include <QDebug>
#include <QThread>
#include <QTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDateTime>

#ifdef _WIN32
#include <winsock2.h> 
#else
#include <arpa/inet.h>
#include <byteswap.h>
#endif

#ifndef _WIN32
#define _byteswap_uint64 bswap_64
#endif

inline uint64_t ntohll_portable(uint64_t value) {
    const int num = 1;
    if (*(char*)&num == 1) { return _byteswap_uint64(value); }
    else { return value; }
}

ClientWorker::ClientWorker(
    NetworkMonitor& monitor,
    JitterBuffer& videoBuffer,
    JitterBuffer& audioBuffer,
    QObject* parent)
    : QObject(parent),
    m_monitor(monitor),
    m_videoJitterBuffer(videoBuffer),
    m_audioJitterBuffer(audioBuffer),
    m_isConnected(false)
{
    m_quicThread = new QThread(this);
    m_quicClient = new QuicClient();
    m_quicClient->moveToThread(m_quicThread);

    connect(m_quicClient, &QuicClient::connectionSuccess, this, &ClientWorker::onQuicConnectionSuccess);
    connect(m_quicClient, &QuicClient::connectionFailed, this, &ClientWorker::onQuicConnectionFailed);
    connect(m_quicClient, &QuicClient::playInfoReceived, this, &ClientWorker::onQuicPlayInfoReceived);
    connect(m_quicClient, &QuicClient::latencyUpdated, this, &ClientWorker::onQuicLatencyUpdated);
    connect(m_quicClient, &QuicClient::videoPacketReceived, this, &ClientWorker::processVideoPacket);
    connect(m_quicClient, &QuicClient::audioPacketReceived, this, &ClientWorker::processAudioPacket);

    connect(m_quicThread, &QThread::finished, m_quicClient, &QObject::deleteLater);
    m_quicThread->start();

    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &ClientWorker::sendHeartbeat);
}

ClientWorker::~ClientWorker()
{
    if (m_quicThread && m_quicThread->isRunning()) {
        QMetaObject::invokeMethod(m_quicClient, "disconnectFromServer", Qt::QueuedConnection);
        m_quicThread->quit();
        m_quicThread->wait(1000);
    }
    qDebug() << "[Worker] ClientWorker 销毁。";
}

void ClientWorker::connectToServer(const QString& ip, quint16 port)
{
    QMetaObject::invokeMethod(m_quicClient, "connectToServer", Qt::QueuedConnection,
        Q_ARG(QString, ip),
        Q_ARG(quint16, port));
}

void ClientWorker::disconnectFromServer()
{
    QMetaObject::invokeMethod(m_quicClient, "disconnectFromServer", Qt::QueuedConnection);
}

void ClientWorker::requestPlay(const QString& source)
{
    QJsonObject req;
    req["command"] = "play";
    req["source"] = source;
    QByteArray command = QJsonDocument(req).toJson(QJsonDocument::Compact);
    QMetaObject::invokeMethod(m_quicClient, "sendControlCommand", Qt::QueuedConnection, Q_ARG(QByteArray, command));
}

void ClientWorker::requestSeek(double timeSec)
{
    QJsonObject req;
    req["command"] = "seek";
    req["time"] = timeSec;
    QByteArray command = QJsonDocument(req).toJson(QJsonDocument::Compact);
    QMetaObject::invokeMethod(m_quicClient, "sendControlCommand", Qt::QueuedConnection, Q_ARG(QByteArray, command));
}

void ClientWorker::onQuicConnectionSuccess(const QList<QString>& videoList)
{
    qDebug() << "[Worker] 连接成功，启动心跳。";
    m_isConnected = true;
    m_heartbeatTimer->start(1000);
    emit connectionSuccess(videoList);
}

void ClientWorker::onQuicConnectionFailed(const QString& reason)
{
    qDebug() << "[Worker] 连接失败:" << reason;
    m_isConnected = false;
    m_heartbeatTimer->stop();
    emit connectionFailed(reason);
}

void ClientWorker::onQuicPlayInfoReceived(double duration)
{
    qDebug() << "[Worker] 收到播放信息，视频时长:" << duration;
    m_monitor.reset();
    m_videoJitterBuffer.reset();
    m_audioJitterBuffer.reset();
    emit playInfoReceived(duration);
}

void ClientWorker::onQuicLatencyUpdated(double latencyMs)
{
    emit latencyUpdated(latencyMs);
}

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

    QByteArray command = QJsonDocument(heartbeatObject).toJson(QJsonDocument::Compact);
    QMetaObject::invokeMethod(m_quicClient, "sendControlCommand", Qt::QueuedConnection, Q_ARG(QByteArray, command));
}

void ClientWorker::processVideoPacket(const QByteArray& packet)
{
    const int TYPE_H_SIZE = sizeof(AppConfig::PacketType);
    const int PTS_H_SIZE = sizeof(int64_t);

    if (packet.size() < (TYPE_H_SIZE + PTS_H_SIZE)) return;

    int64_t pts_net;
    memcpy(&pts_net, packet.constData() + TYPE_H_SIZE, PTS_H_SIZE);
    int64_t ts = ntohll_portable(pts_net);

    auto mediaPacket = std::make_unique<MediaPacket>();
    mediaPacket->ts = ts;
    static uint32_t video_seq = 0;
    mediaPacket->seq = video_seq++;

    // 【修正】直接赋值 QByteArray
    mediaPacket->payload = packet;

    m_monitor.record_packet(mediaPacket->seq, packet.size());
    m_videoJitterBuffer.add_packet(std::move(mediaPacket));
}

void ClientWorker::processAudioPacket(const QByteArray& packet)
{
    const int HEADER_SIZE = 1 + 8 + 2 + 2;
    if (packet.size() <= HEADER_SIZE) return;

    const char* data_ptr = packet.constData();

    int64_t pts_net;
    memcpy(&pts_net, data_ptr + 1, sizeof(int64_t));
    int64_t ts = ntohll_portable(pts_net);

    auto mediaPacket = std::make_unique<MediaPacket>();
    mediaPacket->ts = ts;
    static uint32_t audio_seq = 0;
    mediaPacket->seq = audio_seq++;

    // 【修正】使用 mid() 提取负载，这是一个 QByteArray
    mediaPacket->payload = packet.mid(HEADER_SIZE);

    m_audioJitterBuffer.add_packet(std::move(mediaPacket));
}