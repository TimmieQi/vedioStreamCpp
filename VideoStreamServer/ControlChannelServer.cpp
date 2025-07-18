#include "ControlChannelServer.h"
#include "StreamerManager.h"
#include "FileSystemManager.h"
#include "AdaptiveStreamController.h"
#include <iostream>

ControlChannelServer::ControlChannelServer(boost::asio::io_context& io_context, unsigned short port, std::shared_ptr<StreamerManager> manager)
    : m_socket(io_context, udp::endpoint(udp::v4(), port)),
      m_recv_buffer(1024),
      m_streamer_manager(manager)
{
    m_video_files = FileSystemManager::get_video_files();
    start_receive();
}

void ControlChannelServer::start_receive()
{
    m_socket.async_receive_from(
        boost::asio::buffer(m_recv_buffer), m_remote_endpoint,
        [this](const boost::system::error_code& error, std::size_t bytes_transferred) {
            handle_receive(error, bytes_transferred);
        });
}

void ControlChannelServer::handle_receive(const boost::system::error_code& error, std::size_t bytes_transferred)
{
    if (!error && bytes_transferred > 0) {
        try {
            std::string received_data(m_recv_buffer.begin(), m_recv_buffer.begin() + bytes_transferred);
            nlohmann::json command = nlohmann::json::parse(received_data);
            std::cout << "[服务端-控制] 收到来自 " << m_remote_endpoint << " 的命令: " << command.dump() << std::endl;
            handle_command(command);
        }
        catch (const std::exception& e) {
            std::cerr << "[服务端-控制] 解析命令错误: " << e.what() << std::endl;
        }
    }
    
    // 无论成功与否，都继续下一次接收
    start_receive();
}

void ControlChannelServer::handle_command(const nlohmann::json& command_json)
{
    std::string command_str = command_json.value("command", "");
    std::cerr << "[服务端-控制] 开始匹配命令 "  << std::endl;
    if (command_str == "get_list") {
        nlohmann::json file_list_json = m_video_files;
        file_list_json.push_back("camera");
        send_response(file_list_json);
    }
    else if (command_str == "play") {
        std::string source = command_json.value("source", "");
        if (!source.empty()) {
            nlohmann::json play_info = m_streamer_manager->start_stream(source, m_remote_endpoint);
            if (!play_info.is_null()) {
                send_response(play_info);
            }
        }
    }
    else if (command_str == "seek") {
        double time = command_json.value("time", -1.0);
        if (time >= 0) {
            m_streamer_manager->seek_stream(time);
        }
    }
    else if (command_str == "heartbeat") {
        double loss_rate = command_json.value("loss_rate", 0.0);
        m_streamer_manager->get_controller()->update_strategy(loss_rate);
    }
    // "stop" 命令由 StreamerManager 的析构或新的 start_stream 调用隐式处理
}


void ControlChannelServer::send_response(const nlohmann::json& response)
{
    auto response_str = std::make_shared<std::string>(response.dump());
    m_socket.async_send_to(
        boost::asio::buffer(*response_str), m_remote_endpoint,
        [response_str](const boost::system::error_code&, std::size_t) {
            // 发送完成后的回调，这里可以什么都不做
        });
}