#include "AdaptiveStreamController.h"
#include <iostream>
#include <iomanip> 
#include <algorithm>

AdaptiveStreamController::AdaptiveStreamController()
{
    m_target_bitrate_bps.store(START_BITRATE);
}

int64_t AdaptiveStreamController::get_target_bitrate()
{
    return m_target_bitrate_bps.load();
}

void AdaptiveStreamController::update_client_feedback(const std::string& trend)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    int64_t current_bitrate = m_target_bitrate_bps.load();
    int64_t new_bitrate = current_bitrate;

    if (trend == "increase") {
        // 温和的乘性增：每次增加8%
        new_bitrate = static_cast<int64_t>(current_bitrate * 1.08);
    }
    else if (trend == "decrease") {
        // 乘性减：每次降低15%
        new_bitrate = static_cast<int64_t>(current_bitrate * 0.85);
    }
    else { // "hold"
        return;
    }

    // 将新码率限制在预设的最小/最大值之间
    new_bitrate = std::max(MIN_BITRATE, std::min(MAX_BITRATE, new_bitrate));

    if (new_bitrate != current_bitrate) {
        m_target_bitrate_bps.store(new_bitrate);
        std::cout << "[服务端-控制器] 收到反馈 '" << trend
            << "', 调整目标码率至: " << new_bitrate / 1024 << " kbps" << std::endl;
    }
}