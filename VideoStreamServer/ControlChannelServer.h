#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <string>
#include "nlohmann/json.hpp"

// 前向声明
class StreamerManager;

using boost::asio::ip::udp;

class ControlChannelServer
{
public:
    ControlChannelServer(boost::asio::io_context& io_context, unsigned short port, std::shared_ptr<StreamerManager> manager);

private:
    void start_receive();
    void handle_receive(const boost::system::error_code& error, std::size_t bytes_transferred);
    void handle_command(const nlohmann::json& command);
    void send_response(const nlohmann::json& response);

    udp::socket m_socket;
    udp::endpoint m_remote_endpoint;
    std::vector<char> m_recv_buffer;
    std::shared_ptr<StreamerManager> m_streamer_manager;
    std::vector<std::string> m_video_files;
};