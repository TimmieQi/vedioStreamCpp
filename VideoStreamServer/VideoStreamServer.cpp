#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <fstream>
#include "shared_config.h"
#include "AdaptiveStreamController.h"
#include "StreamerManager.h"
#include "QuicServer.h"
#include "nlohmann/json.hpp"

// 【修改】函数现在返回一个包含所有配置的json对象
nlohmann::json load_config() {
    std::ifstream config_file("config.json");
    if (!config_file.is_open()) {
        std::cerr << "!!! 错误: 无法打开 config.json 文件。 !!!" << std::endl;
        return nullptr;
    }
    try {
        nlohmann::json config_json;
        config_file >> config_json;
        return config_json;
    }
    catch (const nlohmann::json::parse_error& e) {
        std::cerr << "!!! 错误: 解析 config.json 文件失败: " << e.what() << " !!!" << std::endl;
        return nullptr;
    }
}

void run_server()
{
    auto config = load_config();
    if (config.is_null() || !config.contains("certificate_fingerprint") || !config.contains("server_port")) {
        std::cerr << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
        std::cerr << "!!! 错误: config.json 文件无效或缺少必要的字段。 !!!" << std::endl;
        std::cerr << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
        std::cout << "按 Enter 键退出..." << std::endl;
        std::cin.get();
        return;
    }

    const std::string CERT_HASH = config["certificate_fingerprint"];
    const uint16_t SERVER_PORT = static_cast<uint16_t>(config["server_port"].get<int>());

    if (CERT_HASH.empty() || CERT_HASH == "your_certificate_fingerprint_here") {
        std::cerr << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
        std::cerr << "!!! 错误: 请在 config.json 文件中设置您的'certificate_fingerprint'。 !!!" << std::endl;
        std::cerr << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
        std::cout << "按 Enter 键退出..." << std::endl;
        std::cin.get();
        return;
    }

    try {
        auto controller = std::make_shared<AdaptiveStreamController>();
        auto streamer_manager = std::make_shared<StreamerManager>(controller);
        auto quic_server = std::make_unique<QuicServer>(streamer_manager);

        // 【核心修改】使用从配置文件加载的指纹和端口
        if (!quic_server->Start(CERT_HASH, SERVER_PORT)) {
            std::cerr << "[服务端] 致命错误: 无法启动 QUIC 服务器。" << std::endl;
            return;
        }

        std::cout << "[服务端] 服务器已在 QUIC 端口 " << SERVER_PORT << " 上成功启动并监听。" << std::endl;
        std::cout << "按 Enter 键关闭。" << std::endl;
        std::cin.get();

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