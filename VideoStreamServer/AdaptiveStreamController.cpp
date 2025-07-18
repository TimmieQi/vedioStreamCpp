#include "AdaptiveStreamController.h"
#include <iostream>
#include <iomanip> // for std::fixed, std::setprecision

AdaptiveStreamController::AdaptiveStreamController()
    : m_current_strategy_name("good")
{
    m_good_strategy = { 1.0, 60 };
    m_medium_strategy = { 0.5, 30 };
    m_poor_strategy = { 0.25, 20 };
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
        // 使用 iomanip 来格式化输出百分比
        std::cout << "[服务端-控制器] 丢包率: "
            << std::fixed << std::setprecision(2) << loss_rate * 100.0
            << "%, 切换策略至: " << new_strategy_name << std::endl;
        m_current_strategy_name = new_strategy_name;
    }
}