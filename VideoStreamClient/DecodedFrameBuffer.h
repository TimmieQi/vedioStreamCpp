#pragma once

#include <deque>
#include <mutex>
#include <cstdint>
#include <memory>
#include <vector>
#include <opencv2/opencv.hpp> // 包含OpenCV主头文件

// 前向声明 FFmpeg 结构体
struct AVFrame;

// 解码后帧的数据结构
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
    std::unique_ptr<DecodedFrame> get_interpolated_frame(int64_t target_pts_ms);

private:
    // 使用OpenCV光流法进行插值的函数
    AVFrame* interpolate(const AVFrame* prev, const AVFrame* next, double factor);

    // 辅助函数：AVFrame -> cv::Mat (仅Y分量/灰度图)
    bool avframe_to_mat_gray(const AVFrame* av_frame, cv::Mat& out_mat);
    // 辅助函数：cv::Mat -> AVFrame
    AVFrame* mat_to_avframe(const cv::Mat& mat, int width, int height);


    std::mutex mtx_;
    std::deque<std::unique_ptr<DecodedFrame>> queue_;
    int64_t last_played_pts_;
    size_t buffer_size_ms_;
};
