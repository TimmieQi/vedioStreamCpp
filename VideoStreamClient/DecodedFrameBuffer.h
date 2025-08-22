#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <vector>

// 前向声明，避免在头文件中包含FFmpeg和OpenCV的头文件
struct AVFrame;
namespace cv {
    class Mat;
}

// 封装AVFrame的智能指针，确保自动释放
class DecodedFrame {
public:
    // 使用自定义删除器的unique_ptr来管理AVFrame
    std::unique_ptr<AVFrame, void(*)(AVFrame*)> frame;

    explicit DecodedFrame(AVFrame* fr);
    ~DecodedFrame();

    // 禁用拷贝构造和拷贝赋值
    DecodedFrame(const DecodedFrame&) = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;
};

class DecodedFrameBuffer
{
public:
    DecodedFrameBuffer();
    ~DecodedFrameBuffer();

    void reset();
    void add_frame(std::unique_ptr<DecodedFrame> frame);
    std::unique_ptr<DecodedFrame> get_frame(int64_t target_pts_ms);
    std::unique_ptr<DecodedFrame> get_interpolated_frame(int64_t target_pts_ms);
    void set_buffer_duration(int ms);

    // 【新增】获取当前缓冲区内帧的时间跨度
    int64_t get_current_duration_ms() const;

    // 必须公开，以便渲染逻辑调用
    void get_interpolation_frames(int64_t target_pts_ms, const AVFrame*& out_prev, const AVFrame*& out_next, double& out_factor);

private:
    // 辅助函数
    AVFrame* interpolate(const AVFrame* prev, const AVFrame* next, double factor);
    bool avframe_to_mat_gray(const AVFrame* av_frame, cv::Mat& out_mat);
    AVFrame* mat_to_avframe(const cv::Mat& mat, int width, int height);

    std::deque<std::unique_ptr<DecodedFrame>> queue_;
    mutable std::mutex mtx_; // 【修改】设为 mutable 以便在 const 函数中加锁
    int64_t last_played_pts_;
    int buffer_size_ms_; // 缓冲时长（毫秒）
};

