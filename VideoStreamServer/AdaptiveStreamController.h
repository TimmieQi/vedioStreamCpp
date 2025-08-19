#pragma once
#include <string>
#include <mutex>
#include <chrono>
#include <atomic>
#include <vector> // 【新增】

// 【新增】定义一个结构体来描述每个分辨率/质量层级
struct QualityLevel {
    int height;
    int width;
    int target_fps;
    int64_t min_bitrate_bps;
    int64_t max_bitrate_bps;
    int64_t start_bitrate_bps;
};

// 【新增】控制器现在不仅输出码率，还可能建议一个新的分辨率层级
struct ABRDecision {
    int64_t target_bitrate_bps;
    int target_fps;
    int target_height; // 使用 height 作为层级的唯一标识
};


class AdaptiveStreamController
{
public:
    AdaptiveStreamController();

    // 【修改】接口现在返回一个包含所有决策的结构体
    ABRDecision get_decision();

    void update_client_feedback(const std::string& trend);
    void set_video_resolution(int width, int height);

private:
    void initialize_quality_levels(int source_height);

    std::mutex m_mutex;

    // 决策结果
    std::atomic<int64_t> m_target_bitrate_bps;
    std::atomic<int> m_target_fps;
    std::atomic<int> m_target_height;

    // 状态变量
    int m_current_level_index = 0;
    std::vector<QualityLevel> m_quality_levels;

    // 【新增】用于升/降档延迟确认的成员
    enum class ChangeState { Stable, ConsideringUpgrade, ConsideringDowngrade };
    ChangeState m_change_state = ChangeState::Stable;
    std::chrono::steady_clock::time_point m_change_state_start_time;
    const std::chrono::seconds UPGRADE_CONFIRMATION_TIME{ 5 };   // 码率持续高于上限5秒后才升级
    const std::chrono::seconds DOWNGRADE_CONFIRMATION_TIME{ 8 }; // 码率持续低于下限8秒后才降级
};