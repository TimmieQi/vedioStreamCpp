#pragma once
#include <cstdint>
#include <vector>

struct MediaPacket {
    uint32_t seq;
    int64_t ts;
    std::vector<uint8_t> payload;

    bool operator>(const MediaPacket& other) const {
        return seq > other.seq;
    }
};