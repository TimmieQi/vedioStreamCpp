#pragma once

// 定义一个命名空间来组织所有配置，避免全局污染
namespace AppConfig {
    // --- 视频流参数 (应用级常量) ---
    constexpr const char* VIDEO_CODEC = "hevc";

    // --- 音频流参数 (应用级常量) ---
    constexpr int AUDIO_CHUNK_SAMPLES = 256; // 样本数
    constexpr int AUDIO_CHANNELS = 1;      // 单声道
    constexpr int AUDIO_RATE = 16000;      // 采样率 (Hz)

    // --- 应用层协议常量 ---
    enum class PacketType : uint8_t {
        Video = 0,
        Audio = 1
    };
}