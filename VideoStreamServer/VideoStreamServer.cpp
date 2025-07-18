#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <boost/asio.hpp>
#include "shared_config.h"
#include "FileSystemManager.h"
#include "AdaptiveStreamController.h"
#include "StreamerManager.h"
#include "ControlChannelServer.h"

void run_server()
{
    try {
        // 创建 Asio 的核心 I/O 上下文
        boost::asio::io_context io_context;

        // 1. 创建自适应码率控制器
        auto controller = std::make_shared<AdaptiveStreamController>();

        // 2. 创建推流管理器
        auto streamer_manager = std::make_shared<StreamerManager>(io_context, controller);

        // 3. 创建并启动控制通道服务
        ControlChannelServer control_server(io_context, AppConfig::CONTROL_PORT, streamer_manager);

        std::cout << "[服务端] 服务器已在端口 " << AppConfig::CONTROL_PORT << " 上启动 (C++版)。按 Ctrl+C 关闭。\n";

        // 4. 运行 io_context，这将阻塞并处理所有异步事件
        io_context.run();

    }
    catch (const std::exception& e) {
        std::cerr << "严重错误: " << e.what() << std::endl;
    }
}

int main()
{
    run_server();
    return 0;
}