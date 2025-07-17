#pragma once

#include <atomic>
#include <cstdint> // 用于 int64_t 等类型

// 主时钟，由音频播放的PTS驱动，用于音视频同步
class MasterClock
{
public:
    MasterClock();

    void reset();
    void start(int64_t pts_ms);
    void update_time(int64_t pts_ms);
    int64_t get_time_ms() const;

    void pause();
    void resume();
    bool is_paused() const;

private:
    // 使用原子变量来保证多线程访问的安全性，且无锁开销
    std::atomic<int64_t> current_pts_ms_;
    std::atomic<bool> paused_;
};