#pragma once

#include <atomic>
#include <cstdint>
#include <QDateTime>

class MasterClock
{
public:
    MasterClock();

    void reset();

    // 修改：启动时钟，并设置初始的媒体时间
    void start(int64_t pts_ms);

    void seek(int64_t pts_ms);
    int64_t get_time_ms() const;
    void pause();
    void resume();
    bool is_paused() const;

    // 新增：判断时钟是否已启动
    bool is_started() const;

private:
    std::atomic<bool> m_is_started;
    std::atomic<bool> m_is_paused;
    qint64 m_start_system_time_ms;
    int64_t m_start_pts_ms;
    int64_t m_paused_pts_ms;
};