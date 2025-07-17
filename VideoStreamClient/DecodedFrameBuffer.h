#pragma once

#include <deque>
#include <mutex>
#include <cstdint>
#include <memory>
#include <vector>

// 前向声明，避免在头文件中引入重量级的 FFmpeg 或 OpenCV 头文件
struct AVFrame;
class QImage; // 如果我们决定直接存QImage

// 定义解码后的帧的数据结构
struct DecodedFrame {
    DecodedFrame(AVFrame* fr);
    ~DecodedFrame();
    std::unique_ptr<AVFrame, void(*)(AVFrame*)> frame;
};

// 存储解码后的视频帧，并根据主时钟提供正确的帧
class DecodedFrameBuffer
{
public:
    DecodedFrameBuffer();
    ~DecodedFrameBuffer(); 

    void reset();
    void add_frame(std::unique_ptr<DecodedFrame> frame);
    std::unique_ptr<DecodedFrame> get_frame(int64_t target_pts_ms);

private:
    std::mutex mtx_;
    // 使用 deque 作为底层容器
    std::deque<std::unique_ptr<DecodedFrame>> queue_;
    int64_t last_played_pts_;
    size_t buffer_size_ms_;
};