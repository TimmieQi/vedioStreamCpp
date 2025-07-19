#include "StreamerManager.h"
#include "IStreamer.h"
#include "FileStreamer.h" 
#include "CameraStreamer.h" 
#include "AdaptiveStreamController.h"
#include "shared_config.h"
#include <iostream>
#include <filesystem> // 引入 C++17 文件系统库

namespace fs = std::filesystem;

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
        m_current_streamer = std::make_shared<CameraStreamer>(m_io_context, m_controller, client_endpoint);
    }
    else {
        fs::path source_path = fs::u8path(source);
        fs::path video_fs_path = fs::path("videos") / source_path;

        // 检查文件是否存在
        if (!fs::exists(video_fs_path)) {
            // 打印时，为了在 Windows 控制台正确显示，可以转换为本地编码
            // 但这不是必须的，如果控制台本身支持 UTF-8，u8string() 更好
            std::wcerr << L"[服务端-管理器] 错误: 找不到视频文件 " << video_fs_path.wstring() << std::endl;
            return nullptr;
        }

        // 获取 UTF-8 编码的路径字符串，用于传递给 FFmpeg
        std::string video_path_utf8 = video_fs_path.u8string();

        AVFormatContext* format_ctx = nullptr;
        if (avformat_open_input(&format_ctx, video_path_utf8.c_str(), nullptr, nullptr) == 0) {

            // **【关键新增】** 调用此函数来填充流信息，包括时长
            if (avformat_find_stream_info(format_ctx, nullptr) >= 0) {
                if (format_ctx->duration != AV_NOPTS_VALUE) {
                    response["duration"] = static_cast<double>(format_ctx->duration) / AV_TIME_BASE;
                }
            }
            else {
                std::cerr << "[服务端-管理器] 警告: 无法获取流信息，时长可能不准。" << std::endl;
            }

            avformat_close_input(&format_ctx);
        }
        else {
            std::cerr << "[服务端-管理器] 错误: 无法用 FFmpeg 打开文件以获取时长 (路径: " << video_path_utf8 << ")" << std::endl;
            return nullptr;
        }

        std::cout << "[服务端-管理器] 启动文件点播: " << source << std::endl;

        // 将 UTF-8 路径字符串传递给 FileStreamer
        m_current_streamer = std::make_shared<FileStreamer>(m_io_context, m_controller, video_path_utf8, client_endpoint);
    }

    if (m_current_streamer) {
        m_stream_thread = std::thread([this] {
            if (m_current_streamer) m_current_streamer->start();
            });
    }

    response["command"] = "play_info";
    return response;
}

void StreamerManager::stop_stream()
{
    if (m_current_streamer) {
        std::cout << "[服务端-管理器] 发送停止信号到当前推流线程..." << std::endl;
        m_current_streamer->stop();
    }
    if (m_stream_thread.joinable()) {
        m_stream_thread.join();
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