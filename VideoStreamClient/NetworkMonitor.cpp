#include "NetworkMonitor.h"

NetworkMonitor::NetworkMonitor()
{
    reset();
}

void NetworkMonitor::reset()
{
    std::lock_guard<std::mutex> lock(mtx_);
    received_packets_ = 0;
    lost_packets_ = 0;
    expected_seq_ = -1;
    total_bytes_received_ = 0;
    last_reset_time_ = std::chrono::steady_clock::now();
}

void NetworkMonitor::record_packet(uint16_t seq, size_t packet_size)
{
    std::lock_guard<std::mutex> lock(mtx_);

    if (expected_seq_ == -1) {
        expected_seq_ = seq;
    }

    if (seq > expected_seq_) {
        lost_packets_ += (seq - expected_seq_);
    }

    expected_seq_ = static_cast<uint16_t>(seq + 1); // 序列号回环由uint16_t类型自动处理
    received_packets_++;
    total_bytes_received_ += packet_size;
}

NetworkStats NetworkMonitor::get_statistics()
{
    std::lock_guard<std::mutex> lock(mtx_);
    NetworkStats stats;

    uint64_t total_packets = received_packets_ + lost_packets_;
    if (total_packets > 0) {
        stats.loss_rate = static_cast<double>(lost_packets_) / total_packets;
    }

    auto current_time = std::chrono::steady_clock::now();
    std::chrono::duration<double> time_diff = current_time - last_reset_time_;
    double time_diff_sec = time_diff.count();

    if (time_diff_sec > 0) {
        stats.bitrate_bps = (total_bytes_received_ * 8) / time_diff_sec;
    }

    // 重置计数器以便下一次统计
    received_packets_ = 0;
    lost_packets_ = 0;
    total_bytes_received_ = 0;
    last_reset_time_ = current_time;

    return stats;
}