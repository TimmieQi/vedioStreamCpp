#pragma once
#include "IStreamer.h"
#include <string>
#include <boost/asio.hpp>

class AdaptiveStreamController;
using boost::asio::ip::udp;

class FileStreamer : public IStreamer, public std::enable_shared_from_this<FileStreamer>
{
public:
    FileStreamer(
        boost::asio::io_context& io_context,
        std::shared_ptr<AdaptiveStreamController> controller,
        const std::string& video_path,
        udp::endpoint client_endpoint
    );

    void start() override;
    void stop() override;
    void seek(double time_sec) override;

private:
    void stream_loop();

    // 共享的控制块
    std::shared_ptr<StreamControlBlock> m_control_block;

    // 网络
    udp::socket m_video_socket;
    udp::socket m_audio_socket;
    udp::endpoint m_client_endpoint;

    // 逻辑
    std::shared_ptr<AdaptiveStreamController> m_controller;
    std::string m_video_path;
};