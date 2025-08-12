#include "StreamerManager.h"
#include "QuicServer.h" // 包含 QuicServer 的完整定义
#include "IStreamer.h"
#include "FileStreamer.h" 
#include "CameraStreamer.h" 
#include "AdaptiveStreamController.h"
#include "shared_config.h"
#include <iostream>
#include <filesystem> 

namespace fs = std::filesystem;

// 包含 FFmpeg 头文件以获取视频时长
extern "C" {
#include <libavformat/avformat.h>
}

// 构造函数不再需要 io_context
StreamerManager::StreamerManager(std::shared_ptr<AdaptiveStreamController> controller)
    : m_controller(controller)
{
}

StreamerManager::~StreamerManager()
{
    stop_stream();
}

// start_stream 方法的实现已更新
nlohmann::json StreamerManager::start_stream(const std::string& source, HQUIC connection, QuicServer* quic_server)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cout << "[服务端-管理器] 请求开启新 QUIC 推流..." << std::endl;

    stop_stream(); // 停止上一个流

    // 从 QuicServer 获取 MsQuic API 表
    const QUIC_API_TABLE* msquic_api = quic_server->GetMsQuicApi();
    if (!msquic_api) {
        std::cerr << "[服务端-管理器] 错误: 无法从 QuicServer 获取 MsQuic API 表。" << std::endl;
        return nullptr;
    }

    nlohmann::json response;
    response["duration"] = 0.0;

    // 根据数据源选择不同的推流器
    // 注意：我们现在将 msquic_api 和 connection 句柄传递给推流器的构造函数
    // 这会导致编译错误，我们将在下一步修复
    if (source == "camera") {
        std::cout << "[服务端-管理器] 启动摄像头直播" << std::endl;
        m_current_streamer = std::make_shared<CameraStreamer>(msquic_api, connection, m_controller);
    }
    else {
        fs::path source_path = fs::u8path(source);
        fs::path video_fs_path = fs::path("videos") / source_path;

        if (!fs::exists(video_fs_path)) {
            std::wcerr << L"[服务端-管理器] 错误: 找不到视频文件 " << video_fs_path.wstring() << std::endl;
            return nullptr;
        }

        std::string video_path_utf8 = video_fs_path.u8string();

        // 获取视频时长的逻辑保持不变
        AVFormatContext* format_ctx = nullptr;
        if (avformat_open_input(&format_ctx, video_path_utf8.c_str(), nullptr, nullptr) == 0) {
            if (avformat_find_stream_info(format_ctx, nullptr) >= 0) {
                if (format_ctx->duration != AV_NOPTS_VALUE) {
                    response["duration"] = static_cast<double>(format_ctx->duration) / AV_TIME_BASE;
                }
            }
            avformat_close_input(&format_ctx);
        }
        else {
            std::cerr << "[服务端-管理器] 错误: 无法用 FFmpeg 打开文件以获取时长 (路径: " << video_path_utf8 << ")" << std::endl;
            return nullptr;
        }

        std::cout << "[服务端-管理器] 启动文件点播: " << source << std::endl;
        m_current_streamer = std::make_shared<FileStreamer>(msquic_api, connection, m_controller, video_path_utf8);
    }

    // 启动推流线程的逻辑保持不变
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
        std::cout << "[服务端-管理器] 正在停止当前推流..." << std::endl;
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