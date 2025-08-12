#pragma once

// 定义一个命名空间来组织所有配置，避免全局污染
namespace AppConfig {
    // --- 网络配置 ---
    constexpr const char* SERVER_HOST = "127.0.0.1"; // 客户端默认连接IP
    constexpr int VIDEO_PORT = 9999;
    constexpr int AUDIO_PORT = 9997;
    constexpr int CONTROL_PORT = 9998;
    constexpr int FEC_PORT = 9996;
    // --- 视频流参数 ---
    constexpr const char* VIDEO_CODEC = "hevc";

    // --- 音频流参数 ---
    constexpr int AUDIO_CHUNK_SAMPLES = 256; // 样本数
    constexpr int AUDIO_CHANNELS = 1;      // 单声道
    constexpr int AUDIO_RATE = 16000;      // 采样率 (Hz)

    // 【新增】定义应用层数据包类型
    enum class PacketType : uint8_t {
        Video = 0,
        Audio = 1
    };
}