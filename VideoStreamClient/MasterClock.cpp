#include "MasterClock.h"
#include <QDebug> //  qDebug 进行日志输出

MasterClock::MasterClock()
{
    reset();
}

void MasterClock::reset()
{
    // C++11风格的原子变量初始化
    current_pts_ms_.store(-1);
    paused_.store(false);
}

void MasterClock::start(int64_t pts_ms)
{
    // 仅在第一次接收到音频时调用
    int64_t expected = -1;
    // 使用 compare_exchange_strong 来原子地检查并设置值
    // 如果 current_pts_ms_ 的当前值是 expected(-1)，则将其更新为 pts_ms
    if (this->current_pts_ms_.compare_exchange_strong(expected, pts_ms)) {
        qDebug() << "[时钟] 主时钟启动。初始PTS:" << pts_ms << "ms";
    }
}

void MasterClock::update_time(int64_t pts_ms)
{
    // 由音频播放线程调用，用实际播放的PTS来驱动时钟前进
    if (!paused_.load() && pts_ms >= 0) {
        current_pts_ms_.store(pts_ms);
    }
}

int64_t MasterClock::get_time_ms() const
{
    return current_pts_ms_.load();
}

void MasterClock::pause()
{
    paused_.store(true);
}



void MasterClock::resume()
{
    paused_.store(false);
}

bool MasterClock::is_paused() const
{
    return paused_.load();
}