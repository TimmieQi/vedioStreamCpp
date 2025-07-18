#include "StreamerManager.h"
#include "IStreamer.h"
#include "FileStreamer.h" 
// #include "CameraStreamer.h" 
#include "AdaptiveStreamController.h"
#include "shared_config.h"
#include <iostream>
#include <nlohmann/json.hpp>

// PyAV/FFmpeg 的头文件，用于获取视频时长
extern "C" {
#include <libavformat/avformat.h>
}

StreamerManager::StreamerManager(boost::asio::io_context& io_context, std::shared_ptr<AdaptiveStreamController> controller)
    : m_io_context(io_context),
    m_controller(controller)
{
}

StreamerManager::~StreamerManager()
{
    stop_stream();
}

nlohmann::json StreamerManager::start_stream(const std::string& source, udp::endpoint client_endpoint)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cout << "[服务端-管理器] 请求开启新推流..." << std::endl;

    stop_stream(); // 停止上一个流

    nlohmann::json response;
    response["duration"] = 0.0;

    if (source == "camera") {
        std::cout << "[服务端-管理器] 启动摄像头直播... (功能待实现)" << std::endl;
        // m_current_streamer = std::make_shared<CameraStreamer>(...);
    }
    else {
        std::string video_path = "videos/" + source;
        // 检查文件是否存在
        if (!std::filesystem::exists(video_path)) {
            std::cerr << "[服务端-管理器] 错误: 找不到视频文件 " << video_path << std::endl;
            return nullptr;
        }

        // 使用 FFmpeg 获取视频时长
        AVFormatContext* format_ctx = nullptr;
        if (avformat_open_input(&format_ctx, video_path.c_str(), nullptr, nullptr) == 0) {
            if (format_ctx->duration != AV_NOPTS_VALUE) {
                response["duration"] = static_cast<double>(format_ctx->duration) / AV_TIME_BASE;
            }
            avformat_close_input(&format_ctx);
        }
        else {
            std::cerr << "[服务端-管理器] 错误: 无法用 FFmpeg 打开文件以获取时长 " << video_path << std::endl;
            return nullptr;
        }

        std::cout << "[服务端-管理器] 启动文件点播: " << source << std::endl;
        m_current_streamer = std::make_shared<FileStreamer>(m_io_context, m_controller, video_path, client_endpoint);
    }

    if (m_current_streamer) {
        // 创建并分离一个新线程来运行推流器的 start() 方法
        m_stream_thread = std::thread([this] {
            m_current_streamer->start();
            });
    }

    response["command"] = "play_info";
    return response;
}

void StreamerManager::stop_stream()
{
    // 这里不需要锁，因为 m_current_streamer 的读写由 start_stream 的锁保护
    // stop() 方法内部是线程安全的
    if (m_current_streamer) {
        std::cout << "[服务端-管理器] 发送停止信号到当前推流线程..." << std::endl;
        m_current_streamer->stop();
    }

    if (m_stream_thread.joinable()) {
        m_stream_thread.join(); // 等待线程结束
    }

    m_current_streamer = nullptr;
    std::cout << "[服务端-管理器] 推流已确认停止。" << std::endl;
}

void StreamerManager::seek_stream(double target_time_sec)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_current_streamer) {
        std::cout << "[服务端-管理器] 请求跳转到 " << target_time_sec << " 秒" << std::endl;
        m_current_streamer->seek(target_time_sec);
    }
}

std::shared_ptr<AdaptiveStreamController> StreamerManager::get_controller()
{
    return m_controller;
}