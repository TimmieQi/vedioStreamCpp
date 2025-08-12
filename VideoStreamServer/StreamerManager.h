#pragma once

#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include "nlohmann/json.hpp"
#include <msquic.h> // 包含 msquic.h

// 前向声明
class IStreamer;
class AdaptiveStreamController;
class QuicServer; // 前向声明 QuicServer

class StreamerManager
{
public:
    // 构造函数不再需要 io_context
    StreamerManager(std::shared_ptr<AdaptiveStreamController> controller);
    ~StreamerManager();

    // 启动推流的接口已改变：
    // 不再接收 udp::endpoint，而是接收 QUIC 连接句柄和 QuicServer 指针
    nlohmann::json start_stream(const std::string& source, HQUIC connection, QuicServer* quic_server);

    // 停止当前推流
    void stop_stream();

    // 跳转到指定时间
    void seek_stream(double target_time_sec);

    // 获取码率控制器
    std::shared_ptr<AdaptiveStreamController> get_controller();

private:
    std::mutex m_mutex;
    std::thread m_stream_thread;
    std::shared_ptr<IStreamer> m_current_streamer;

    // m_io_context 已被移除
    std::shared_ptr<AdaptiveStreamController> m_controller;
};