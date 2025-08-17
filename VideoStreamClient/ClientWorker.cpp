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
    connect(m_quicClient, &QuicClient::bandwidthUpdated, this, &ClientWorker::onBandwidthUpdated);
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

void ClientWorker::requestPause()
{
    QJsonObject req;
    req["command"] = "pause";
    QByteArray command = QJsonDocument(req).toJson(QJsonDocument::Compact);
    QMetaObject::invokeMethod(m_quicClient, "sendControlCommand", Qt::QueuedConnection, Q_ARG(QByteArray, command));
}

void ClientWorker::requestResume()
{
    QJsonObject req;
    req["command"] = "resume";
    QByteArray command = QJsonDocument(req).toJson(QJsonDocument::Compact);
    QMetaObject::invokeMethod(m_quicClient, "sendControlCommand", Qt::QueuedConnection, Q_ARG(QByteArray, command));
}

void ClientWorker::onQuicConnectionSuccess(const QList<QString>& videoList)
{
    m_isConnected = true;
    m_heartbeatTimer->start(1000); // 心跳间隔1秒
    m_packet_history.clear(); // 清空历史记录
    emit connectionSuccess(videoList);
}

void ClientWorker::onQuicConnectionFailed(const QString& reason)
{
    m_isConnected = false;
    m_heartbeatTimer->stop();
    emit connectionFailed(reason);
}

void ClientWorker::onQuicPlayInfoReceived(double duration)
{
    m_monitor.reset();
    m_videoJitterBuffer.reset();
    m_audioJitterBuffer.reset();
    m_packet_history.clear(); // 开始播放时重置
    emit playInfoReceived(duration);
}

void ClientWorker::onQuicLatencyUpdated(double latencyMs)
{
    emit latencyUpdated(latencyMs);
}

void ClientWorker::onBandwidthUpdated(uint64_t bits_per_second)
{
    // 这个函数现在只用于调试打印，不再驱动ABR逻辑
    // qDebug() << "[Worker] QUIC BWE updated: " << bits_per_second / 1024 << " kbps";
}

void ClientWorker::sendHeartbeat()
{
    if (!m_isConnected) {
        m_heartbeatTimer->stop();
        return;
    }

    NetworkTrend trend = getNetworkTrend();
    QString trend_str = "hold";
    if (trend == NetworkTrend::Increase) {
        trend_str = "increase";
    }
    else if (trend == NetworkTrend::Decrease) {
        trend_str = "decrease";
    }

    QJsonObject heartbeatObject;
    heartbeatObject["command"] = "heartbeat";
    heartbeatObject["trend"] = trend_str;
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

    analyzePacketArrival(ts, packet.size());

    auto mediaPacket = std::make_unique<MediaPacket>();
    mediaPacket->ts = ts;
    static uint32_t video_seq = 0;
    mediaPacket->seq = video_seq++;
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
    mediaPacket->payload = packet.mid(HEADER_SIZE);

    m_audioJitterBuffer.add_packet(std::move(mediaPacket));
}

void ClientWorker::analyzePacketArrival(int64_t timestamp_ms, int packet_size)
{
    m_packet_history.push_back({
        QDateTime::currentMSecsSinceEpoch(),
        timestamp_ms,
        packet_size
        });

    if (m_packet_history.size() > HISTORY_SIZE) {
        m_packet_history.pop_front();
    }
}

NetworkTrend ClientWorker::getNetworkTrend()
{
    if (m_packet_history.size() < 50) {
        return NetworkTrend::Hold;
    }

    int64_t media_delta_ms = m_packet_history.back().media_timestamp_ms - m_packet_history.front().media_timestamp_ms;
    qint64 arrival_delta_ms = m_packet_history.back().arrival_time_ms - m_packet_history.front().arrival_time_ms;

    if (media_delta_ms <= 0) {
        return NetworkTrend::Hold;
    }

    double delay_gradient = static_cast<double>(arrival_delta_ms - media_delta_ms) / media_delta_ms;

    if (delay_gradient > 0.05) { // 阈值可以调整，0.05表示延迟增加了5%
        return NetworkTrend::Decrease;
    }
    else if (delay_gradient < -0.05) { // 延迟减少了5%
        return NetworkTrend::Increase;
    }
    else {
        return NetworkTrend::Hold;
    }
}