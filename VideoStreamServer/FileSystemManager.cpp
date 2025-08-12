#include "FileSystemManager.h"
#include <filesystem> // C++17 文件系统库
#include <iostream>

namespace fs = std::filesystem;

std::vector<std::string> FileSystemManager::get_video_files(const std::string& path)
{
    if (!fs::exists(path)) {
        std::cout << "[文件系统] 目录 " << path << " 不存在，正在创建..." << std::endl;
        fs::create_directories(path);
        return {};
    }

    std::vector<std::string> files;
    const std::vector<std::string> supported_formats = { ".mp4", ".mkv", ".avi", ".mov" };

    for (const auto& entry : fs::directory_iterator(path)) {
        if (entry.is_regular_file()) {
            std::string extension = entry.path().extension().string();
            // 转换为小写进行比较
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

            for (const auto& format : supported_formats) {
                if (extension == format) {
                    files.push_back(entry.path().filename().u8string());
                    break;
                }
            }
        }
    }
    return files;
}