#pragma once
#include <vector>
#include <queue>
#include <mutex>
#include <cstdint>
#include <memory> // for std::unique_ptr

#include "MediaPacket.h"

// 模板化的 Jitter Buffer
class JitterBuffer
{
public:
    JitterBuffer(size_t max_size = 300);

    void reset();
    void add_packet(std::unique_ptr<MediaPacket> packet);
    std::unique_ptr<MediaPacket> get_packet();

private:
    std::mutex mtx_;
    // 使用 priority_queue 来自动排序，它底层就是堆（heap）
    std::priority_queue<MediaPacket, std::vector<MediaPacket>, std::greater<MediaPacket>> buffer_;

    int64_t expected_seq_;
    size_t max_size_;
};