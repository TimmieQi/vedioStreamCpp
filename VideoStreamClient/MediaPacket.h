#pragma once
#include <cstdint>
#include <QByteArray> 

struct MediaPacket {
    uint32_t seq;
    int64_t ts;
    QByteArray payload; //类型改为 QByteArray

    bool operator>(const MediaPacket& other) const {
        return seq > other.seq;
    }
};