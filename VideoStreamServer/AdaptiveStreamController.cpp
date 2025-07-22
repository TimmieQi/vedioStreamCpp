#include "AdaptiveStreamController.h"
#include <iostream>
#include <iomanip> // for std::fixed, std::setprecision

AdaptiveStreamController::AdaptiveStreamController()
    : m_current_strategy_name("good")
{
    m_good_strategy = { 1.0, 60 };
    m_medium_strategy = { 0.5, 30 };
    m_poor_strategy = { 0.25, 20 };
    m_pending_strategy_name = m_current_strategy_name;
}

StreamStrategy AdaptiveStreamController::get_current_strategy()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_current_strategy_name == "good") return m_good_strategy;
    if (m_current_strategy_name == "medium") return m_medium_strategy;
    return m_poor_strategy;
}

void AdaptiveStreamController::update_strategy(double loss_rate)
{
    std::string new_strategy_name = "good";
    if (loss_rate >= 0.1) {
        new_strategy_name = "poor";
    }
    else if (loss_rate >= 0.03) {
        new_strategy_name = "medium";
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_current_strategy_name != new_strategy_name) {
        if (m_pending_strategy_name != new_strategy_name) {
            // 新的待更改策略，重新计时
            m_pending_strategy_name = new_strategy_name;
            m_pending_start_time = std::chrono::steady_clock::now();
        }
        else {
            // 检查是否达到阈值时间
            auto elapsed_time = std::chrono::steady_clock::now() - m_pending_start_time;
            if (elapsed_time >= m_threshold_time) {
                // 使用 iomanip 来格式化输出百分比
                std::cout << "[服务端-控制器] 丢包率: "
                    << std::fixed << std::setprecision(2) << loss_rate * 100.0
                    << "%, 切换策略至: " << new_strategy_name << std::endl;
                m_current_strategy_name = new_strategy_name;
            }
        }
    }
    else {
        // 当前策略未改变，重置待更改策略
        m_pending_strategy_name = m_current_strategy_name;
    }
}