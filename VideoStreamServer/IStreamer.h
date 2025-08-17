// IStreamer.h
#pragma once

#include <atomic>
#include <memory>

// 一个共享的结构体，用于从主线程控制推流线程
struct StreamControlBlock {
    std::atomic<bool> running{ false };
    std::atomic<double> seek_to{ -1.0 };
    std::atomic<bool> paused{ false };
};

// 推流器接口
class IStreamer
{
public:
    virtual ~IStreamer() = default;

    // 启动推流。这是一个阻塞操作，应该在新线程中运行。
    virtual void start() = 0;

    // 请求停止推流
    virtual void stop() = 0;

    // 请求跳转
    virtual void seek(double time_sec) = 0;

    // 暂停和恢复接口
    virtual void pause() = 0;
    virtual void resume() = 0;
};