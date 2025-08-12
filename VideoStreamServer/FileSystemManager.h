#pragma once
#include <vector>
#include <string>

class FileSystemManager
{
public:
    static std::vector<std::string> get_video_files(const std::string& path = "videos");
};