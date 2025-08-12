#pragma once
#include <string>
#include <mutex>
#include <chrono>

struct StreamStrategy
{
    double multiplier;
    int fps_limit;
};

class AdaptiveStreamController
{
public:
    AdaptiveStreamController();
    StreamStrategy get_current_strategy();
    void update_strategy(double loss_rate);

private:
    std::string m_current_strategy_name;
    StreamStrategy m_good_strategy;
    StreamStrategy m_medium_strategy;
    StreamStrategy m_poor_strategy;
    std::mutex m_mutex;

    // 新增成员变量
    std::string m_pending_strategy_name;
    std::chrono::steady_clock::time_point m_pending_start_time;
    const std::chrono::seconds m_threshold_time = std::chrono::seconds(3); // 设定阈值时间为5秒
};