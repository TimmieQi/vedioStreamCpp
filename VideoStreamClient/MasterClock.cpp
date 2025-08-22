#include "MasterClock.h"
#include <QDebug>

MasterClock::MasterClock()
{
    reset();
}

void MasterClock::reset()
{
    m_is_started.store(false);
    m_is_paused.store(false);
    m_start_system_time_ms = 0;
    m_start_pts_ms = 0;
    m_paused_pts_ms = -1; // -1 表示没有有效的暂停时间点
}

// 修改：start方法现在接受一个初始PTS
void MasterClock::start(int64_t pts_ms)
{
    // 使用 compare_exchange_strong 确保只启动一次
    bool expected = false;
    if (m_is_started.compare_exchange_strong(expected, true)) {
        m_start_system_time_ms = QDateTime::currentMSecsSinceEpoch();
        m_start_pts_ms = pts_ms;
        m_is_paused.store(false);
        qDebug() << "[时钟] 主时钟已由第一个媒体包启动。初始PTS:" << pts_ms << "ms";
    }
}

void MasterClock::seek(int64_t pts_ms)
{
    m_start_system_time_ms = QDateTime::currentMSecsSinceEpoch();
    m_start_pts_ms = pts_ms;
    m_is_started.store(true);
    if (m_is_paused.load()) {
        m_paused_pts_ms = pts_ms;
    }
    qDebug() << "[时钟] 主时钟跳转到:" << pts_ms << "ms";
}

int64_t MasterClock::get_time_ms() const
{
    if (!m_is_started.load()) {
        return -1;
    }
    if (m_is_paused.load()) {
        return m_paused_pts_ms;
    }
    qint64 current_system_time = QDateTime::currentMSecsSinceEpoch();
    return (current_system_time - m_start_system_time_ms) + m_start_pts_ms;
}

void MasterClock::pause()
{
    if (!m_is_paused.exchange(true)) {
        m_paused_pts_ms = get_time_ms();
        qDebug() << "[时钟] 主时钟暂停于:" << m_paused_pts_ms << "ms";
    }
}

void MasterClock::resume()
{
    if (m_is_paused.exchange(false)) {
        m_start_system_time_ms = QDateTime::currentMSecsSinceEpoch();
        m_start_pts_ms = m_paused_pts_ms;
        qDebug() << "[时钟] 主时钟从" << m_start_pts_ms << "ms 恢复。";
    }
}

bool MasterClock::is_paused() const
{
    return m_is_paused.load();
}

// 新增实现
bool MasterClock::is_started() const
{
    return m_is_started.load();
}