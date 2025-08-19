#include "AdaptiveStreamController.h"
#include <iostream>
#include <iomanip> 
#include <algorithm>

AdaptiveStreamController::AdaptiveStreamController()
{
    // 初始化时使用一个默认值，稍后会被 set_video_resolution 覆盖
    initialize_quality_levels(1080);
}

ABRDecision AdaptiveStreamController::get_decision()
{
    // 返回当前的决策，这些值是原子变量，线程安全
    return {
        m_target_bitrate_bps.load(),
        m_target_fps.load(),
        m_target_height.load()
    };
}

void AdaptiveStreamController::set_video_resolution(int width, int height)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // 根据源视频的高度，初始化可用的质量层级列表
    initialize_quality_levels(height);

    // 设置初始状态
    m_current_level_index = 0; // 默认从最高质量开始
    const auto& initial_level = m_quality_levels[m_current_level_index];

    m_target_bitrate_bps.store(initial_level.start_bitrate_bps);
    m_target_fps.store(initial_level.target_fps);
    m_target_height.store(initial_level.height);
    m_change_state = ChangeState::Stable;

    std::cout << "[服务端-控制器] 源分辨率 " << width << "x" << height
        << ", ABR已初始化。起始目标: " << initial_level.height << "p@" << initial_level.target_fps << "fps, "
        << initial_level.start_bitrate_bps / 1024 << " kbps" << std::endl;
}

void AdaptiveStreamController::update_client_feedback(const std::string& trend)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_quality_levels.empty()) return;

    // 1. 根据反馈调整当前码率
    int64_t current_bitrate = m_target_bitrate_bps.load();
    int64_t new_bitrate = current_bitrate;
    const auto& current_level = m_quality_levels[m_current_level_index];

    if (trend == "increase") {
        new_bitrate = static_cast<int64_t>(current_bitrate * 1.10); // 每次增加10%
    }
    else if (trend == "decrease") {
        new_bitrate = static_cast<int64_t>(current_bitrate * 0.85); // 每次降低15%
    }

    // 将码率限制在当前分辨率层级的范围内
    new_bitrate = std::max(current_level.min_bitrate_bps, std::min(current_level.max_bitrate_bps, new_bitrate));

    if (new_bitrate != current_bitrate) {
        m_target_bitrate_bps.store(new_bitrate);
        std::cout << "[服务端-控制器] 收到反馈 '" << trend
            << "', 调整目标码率至: " << new_bitrate / 1024 << " kbps" << std::endl;
    }

    // 2. 检查是否需要考虑升/降档 (分辨率和帧率)
    auto now = std::chrono::steady_clock::now();

    // 检查是否需要升档
    if (m_current_level_index > 0 && new_bitrate >= current_level.max_bitrate_bps) {
        if (m_change_state != ChangeState::ConsideringUpgrade) {
            m_change_state = ChangeState::ConsideringUpgrade;
            m_change_state_start_time = now;
        }
        if (now - m_change_state_start_time >= UPGRADE_CONFIRMATION_TIME) {
            // 确认升档！
            m_current_level_index--;
            const auto& next_level = m_quality_levels[m_current_level_index];
            m_target_bitrate_bps.store(next_level.start_bitrate_bps);
            m_target_fps.store(next_level.target_fps);
            m_target_height.store(next_level.height);
            m_change_state = ChangeState::Stable;
            std::cout << "[服务端-控制器] ***** 确认升档! 新目标: " << next_level.height << "p@" << next_level.target_fps << "fps *****" << std::endl;
        }
    }
    // 检查是否需要降档
    else if (m_current_level_index < m_quality_levels.size() - 1 && new_bitrate <= current_level.min_bitrate_bps) {
        if (m_change_state != ChangeState::ConsideringDowngrade) {
            m_change_state = ChangeState::ConsideringDowngrade;
            m_change_state_start_time = now;
        }
        if (now - m_change_state_start_time >= DOWNGRADE_CONFIRMATION_TIME) {
            // 确认降档！
            m_current_level_index++;
            const auto& next_level = m_quality_levels[m_current_level_index];
            m_target_bitrate_bps.store(next_level.start_bitrate_bps);
            m_target_fps.store(next_level.target_fps);
            m_target_height.store(next_level.height);
            m_change_state = ChangeState::Stable;
            std::cout << "[服务端-控制器] ***** 确认降档! 新目标: " << next_level.height << "p@" << next_level.target_fps << "fps *****" << std::endl;
        }
    }
    // 如果码率在健康范围内，则重置状态
    else {
        m_change_state = ChangeState::Stable;
    }
}

// 【新增】根据源视频高度，动态生成一个可用的质量层级列表
void AdaptiveStreamController::initialize_quality_levels(int source_height)
{
    m_quality_levels.clear();

    // 模板，可拓展添加
    const std::vector<QualityLevel> all_levels = {
        {2160, 3840, 60, 4000 * 1024, 30000 * 1024, 8000 * 1024}, // 4K
        {1440, 2560, 60, 2000 * 1024, 8000 * 1024,  3000 * 1024}, // 2K
        {1080, 1920, 60, 500 * 1024,  4000 * 1024,  1500 * 1024}, // 1080p
        {720,  1280, 30, 200 * 1024,  1500 * 1024,  800 * 1024},  // 720p
        {480,  640,  30, 100 * 1024,  800 * 1024,   400 * 1024}   // 480p
    };

    // 只添加那些分辨率不高于源视频的层级
    for (const auto& level : all_levels) {
        if (level.height <= source_height) {
            m_quality_levels.push_back(level);
        }
    }

    if (m_quality_levels.empty()) {
        // 如果源视频太小，至少添加一个最低质量的
        m_quality_levels.push_back(all_levels.back());
    }
}