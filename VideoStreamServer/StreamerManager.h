#pragma once
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <boost/asio.hpp> // 为了 udp::socket
#include "nlohmann/json.hpp"

// 前向声明
class IStreamer;
class AdaptiveStreamController;

using boost::asio::ip::udp;

class StreamerManager
{
public:
    StreamerManager(boost::asio::io_context& io_context, std::shared_ptr<AdaptiveStreamController> controller);
    ~StreamerManager();

    // 启动推流，返回包含时长的JSON对象
    nlohmann::json start_stream(const std::string& source, udp::endpoint client_endpoint);

    // 停止当前推流
    void stop_stream();

    // 跳转到指定时间
    void seek_stream(double target_time_sec);

    // 获取码率控制器，供外部（如ControlChannelServer）使用
    std::shared_ptr<AdaptiveStreamController> get_controller();

private:
    std::mutex m_mutex;
    std::thread m_stream_thread;
    std::shared_ptr<IStreamer> m_current_streamer;

    boost::asio::io_context& m_io_context;
    std::shared_ptr<AdaptiveStreamController> m_controller;
};