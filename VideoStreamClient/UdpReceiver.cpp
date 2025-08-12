#include "UdpReceiver.h"
#include <QDebug>
#include <QThread> // 用于msleep
#include <QNetworkDatagram>
UdpReceiver::UdpReceiver(quint16 port, QObject* parent)
    : QObject(parent), m_port(port), m_isReceiving(false)
{
}
UdpReceiver::~UdpReceiver()
{
    // 如果 socket 仍然存在，删除它
    // 尽管在正常流程中它会在 stopReceiving 后被删除
    if (m_socket) {
        delete m_socket;
        m_socket = nullptr;
    }
}

void UdpReceiver::startReceiving()
{
    qDebug() << "[Receiver] 线程" << QThread::currentThreadId() << "- 尝试在端口" << m_port << "上开始接收...";

    // 在startReceiving被调用时创建socket，确保它属于当前线程
    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead, this, &UdpReceiver::onReadyRead);

    if (!m_socket->bind(QHostAddress::Any, m_port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        qDebug() << "[Receiver] 错误: 无法绑定到端口" << m_port << ":" << m_socket->errorString();
        emit bindFailed(m_socket->errorString());
        delete m_socket;
        m_socket = nullptr;
        return;
    }

    qDebug() << "[Receiver] 端口" << m_port << "绑定成功，等待数据...";
}



void UdpReceiver::stopReceiving()
{
    if (m_socket) {
        qDebug() << "[Receiver] 线程" << QThread::currentThreadId() << "- 停止接收 (端口: " << m_port << ")";
        m_socket->close();
        // deleteLater 会在当前线程的事件循环空闲时安全地删除对象
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void UdpReceiver::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        emit packetReceived(m_socket->receiveDatagram().data());
    }
}