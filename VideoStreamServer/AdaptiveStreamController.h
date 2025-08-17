#pragma once
#include <string>
#include <mutex>
#include <chrono>
#include <atomic>

class AdaptiveStreamController
{
public:
    AdaptiveStreamController();

    int64_t get_target_bitrate();
    void update_client_feedback(const std::string& trend);

private:
    std::mutex m_mutex;
    std::atomic<int64_t> m_target_bitrate_bps{ 0 };

    // 状态机参数
    const int64_t MIN_BITRATE = 250 * 1024;
    const int64_t MAX_BITRATE = 4000 * 1024;
    const int64_t START_BITRATE = 1500 * 1024;
};