#pragma once

#include <memory>
#include <string>

// 前向声明FFmpeg结构体
struct AVFrame;

class FSRCNNUpscaler {
public:
    FSRCNNUpscaler();
    ~FSRCNNUpscaler();

    // 初始化模型，返回是否成功，并通过引用传出错误信息
    bool initialize(const std::string& model_path, std::string& error_message);

    bool is_initialized() const;
    AVFrame* upscale(const AVFrame* input_frame);

    // 允许移动，但禁止拷贝
    FSRCNNUpscaler(FSRCNNUpscaler&&) noexcept;
    FSRCNNUpscaler& operator=(FSRCNNUpscaler&&) noexcept;
    FSRCNNUpscaler(const FSRCNNUpscaler&) = delete;
    FSRCNNUpscaler& operator=(const FSRCNNUpscaler&) = delete;

private:
    // Pimpl设计模式：将所有实现细节隐藏起来
    struct Impl;
    std::unique_ptr<Impl> pimpl;
};