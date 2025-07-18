#include "FileStreamer.h"
#include "AdaptiveStreamController.h"
#include <iostream>
#include <thread> // for this_thread

FileStreamer::FileStreamer(
    boost::asio::io_context& io_context,
    std::shared_ptr<AdaptiveStreamController> controller,
    const std::string& video_path,
    udp::endpoint client_endpoint)
    : m_control_block(std::make_shared<StreamControlBlock>()),
    m_video_socket(io_context),
    m_audio_socket(io_context),
    m_client_endpoint(client_endpoint),
    m_controller(controller),
    m_video_path(video_path)
{
    // 打开 socket 以便发送
    m_video_socket.open(udp::v4());
    m_audio_socket.open(udp::v4());
}

void FileStreamer::start()
{
    m_control_block->running = true;
    stream_loop();
}

void FileStreamer::stop()
{
    m_control_block->running = false;
}

void FileStreamer::seek(double time_sec)
{
    m_control_block->seek_to = time_sec;
}

void FileStreamer::stream_loop()
{
    std::cout << "[服务端-推流] 文件推流循环启动: " << m_video_path << std::endl;

    // 这是一个占位循环，之后将替换为真正的 FFmpeg 推流逻辑
    while (m_control_block->running)
    {
        // 检查是否需要 seek
        double seek_time = m_control_block->seek_to.load();
        if (seek_time >= 0) {
            std::cout << "[服务端-推流-占位] 执行跳转到: " << seek_time << "s" << std::endl;
            m_control_block->seek_to = -1.0; // 重置 seek 标志
        }

        // 模拟推流
        // std::cout << "."; // 打印点来表示正在推流

        // 模拟帧率
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    std::cout << "[服务端-推流] 文件推流循环结束。" << std::endl;
}