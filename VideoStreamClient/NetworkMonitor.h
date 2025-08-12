#pragma once

#include <mutex>
#include <chrono> // 用于时间处理
#include <cstdint>

// 网络统计信息结构体
struct NetworkStats
{
    double loss_rate = 0.0;
    double bitrate_bps = 0.0;
};

// 跟踪网络状况，如丢包和码率
class NetworkMonitor
{
public:
    NetworkMonitor();

    void reset();
    // 记录收到的包
    void record_packet(uint16_t seq, size_t packet_size);
    // 获取统计信息并重置内部计数器
    NetworkStats get_statistics();

private:
    std::mutex mtx_; // 使用互斥锁保护所有成员变量

    uint64_t received_packets_;
    uint64_t lost_packets_;
    int64_t expected_seq_; // 使用有符号类型以方便处理初始值-1
    uint64_t total_bytes_received_;

    // 使用C++ chrono库进行高精度计时
    std::chrono::steady_clock::time_point last_reset_time_;
};