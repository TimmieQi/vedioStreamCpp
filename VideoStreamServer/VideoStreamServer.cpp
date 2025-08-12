#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include "shared_config.h"
#include "AdaptiveStreamController.h"
#include "StreamerManager.h"
#include "QuicServer.h" // 包含我们新创建的 QuicServer

void run_server()
{
    const std::string CERT_HASH = "64eb794f4385406ad074428e2a667e3e8f8c279a";

    if (CERT_HASH == "在此处粘贴你生成的证书指纹") {
        std::cerr << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
        std::cerr << "!!! 错误: 请在 VideoStreamServer.cpp 中设置你的证书指纹。 !!!" << std::endl;
        std::cerr << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
        std::cout << "按 Enter 键退出..." << std::endl;
        std::cin.get();
        return;
    }

    try {
        // 1. 创建自适应码率控制器 (逻辑不变)
        auto controller = std::make_shared<AdaptiveStreamController>();

        // 2. 创建推流管理器 (不再需要 io_context)
        auto streamer_manager = std::make_shared<StreamerManager>(controller);

        // 3. 创建并启动我们的 QUIC 服务器，取代了 boost::asio 和 ControlChannelServer
        auto quic_server = std::make_unique<QuicServer>(streamer_manager);

        // 启动服务器，监听在 CONTROL_PORT (可以改为任何你想要的端口)
        if (!quic_server->Start(CERT_HASH, AppConfig::CONTROL_PORT)) {
            std::cerr << "[服务端] 致命错误: 无法启动 QUIC 服务器。" << std::endl;
            return;
        }

        std::cout << "[服务端] 服务器已成功启动 (QUIC 版)。按 Enter 键关闭。" << std::endl;

        // 阻塞主线程，直到用户按下 Enter
        std::cin.get();

        // 4. 用户按下 Enter 后，优雅地停止服务器
        std::cout << "[服务端] 正在关闭服务器..." << std::endl;
        quic_server->Stop();
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