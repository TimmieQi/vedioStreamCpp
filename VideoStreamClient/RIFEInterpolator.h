#pragma once

#include <memory>
#include <string>

// 前向声明FFmpeg结构体
struct AVFrame;

class RIFEInterpolator {
public:
    RIFEInterpolator(); // 构造函数变轻量
    ~RIFEInterpolator();

    // 按需初始化，返回是否成功，并通过引用传出错误信息
    bool initialize(const std::string& model_path, std::string& error_message);

    bool is_initialized() const;
    AVFrame* interpolate(const AVFrame* prev, const AVFrame* next, double factor);

    // 允许移动，但禁止拷贝
    RIFEInterpolator(RIFEInterpolator&&) noexcept;
    RIFEInterpolator& operator=(RIFEInterpolator&&) noexcept;
    RIFEInterpolator(const RIFEInterpolator&) = delete;
    RIFEInterpolator& operator=(const RIFEInterpolator&) = delete;

private:
    // Pimpl设计模式：将所有实现细节隐藏起来
    struct Impl;
    std::unique_ptr<Impl> pimpl;
};