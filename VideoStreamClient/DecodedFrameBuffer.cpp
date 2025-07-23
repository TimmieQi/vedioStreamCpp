#include "DecodedFrameBuffer.h"
#include <algorithm>
#include <vector>

// FFmpeg 头文件
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// DecodedFrame 构造函数和析构函数 (不变)
DecodedFrame::DecodedFrame(AVFrame* fr)
    : frame(fr, [](AVFrame* f) { av_frame_free(&f); })
{
}

DecodedFrame::~DecodedFrame() = default;

// DecodedFrameBuffer 构造函数和析构函数 (不变)
DecodedFrameBuffer::DecodedFrameBuffer() : buffer_size_ms_(1000) { reset(); }
DecodedFrameBuffer::~DecodedFrameBuffer() = default;

// reset 和 add_frame 方法 (不变)
void DecodedFrameBuffer::reset()
{
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.clear();
    last_played_pts_ = -1;
}

void DecodedFrameBuffer::add_frame(std::unique_ptr<DecodedFrame> frame)
{
    if (!frame || !frame->frame) return;
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.push_back(std::move(frame));
    std::sort(queue_.begin(), queue_.end(), [](const auto& a, const auto& b) {
        return a->frame->pts < b->frame->pts;
        });
}

// get_frame 方法 (不变)
std::unique_ptr<DecodedFrame> DecodedFrameBuffer::get_frame(int64_t target_pts_ms)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (queue_.empty() || target_pts_ms < 0) {
        return nullptr;
    }
    auto best_frame_it = queue_.end();
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if ((*it)->frame->pts <= target_pts_ms) {
            best_frame_it = it;
        }
        else {
            break;
        }
    }
    if (best_frame_it != queue_.end()) {
        auto best_frame = std::move(*best_frame_it);
        last_played_pts_ = best_frame->frame->pts;
        queue_.erase(queue_.begin(), best_frame_it + 1);
        return best_frame;
    }
    return nullptr;
}

// get_interpolated_frame 方法 (逻辑不变, 调用新的interpolate)
std::unique_ptr<DecodedFrame> DecodedFrameBuffer::get_interpolated_frame(int64_t target_pts_ms)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (queue_.size() < 2) return nullptr;

    auto it = queue_.begin();
    while (it != queue_.end() && (*it)->frame->pts < target_pts_ms) {
        ++it;
    }

    if (it == queue_.begin() || it == queue_.end()) return nullptr;

    const AVFrame* next_frame = (*it)->frame.get();
    const AVFrame* prev_frame = (*(it - 1))->frame.get();

    double factor = static_cast<double>(target_pts_ms - prev_frame->pts) / static_cast<double>(next_frame->pts - prev_frame->pts);
    if (factor < 0.0 || factor > 1.0) return nullptr;

    AVFrame* interpolated_frame_raw = interpolate(prev_frame, next_frame, factor);
    if (!interpolated_frame_raw) return nullptr;

    auto interpolated_frame_wrapper = std::make_unique<DecodedFrame>(interpolated_frame_raw);
    interpolated_frame_wrapper->frame->pts = target_pts_ms;
    return interpolated_frame_wrapper;
}

// =================================================================
// ==              核心修改：优化后的OpenCV插值实现               ==
// =================================================================

// 辅助函数：将 AVFrame 的 YUV 数据包装成三个 cv::Mat，无数据拷贝
void avframe_to_mats_yuv(const AVFrame* frame, cv::Mat& y, cv::Mat& u, cv::Mat& v) {
    y = cv::Mat(frame->height, frame->width, CV_8UC1, frame->data[0], frame->linesize[0]);
    u = cv::Mat(frame->height / 2, frame->width / 2, CV_8UC1, frame->data[1], frame->linesize[1]);
    v = cv::Mat(frame->height / 2, frame->width / 2, CV_8UC1, frame->data[2], frame->linesize[2]);
}

// 辅助函数：将三个 YUV cv::Mat 合并成一个新的 AVFrame
AVFrame* mats_yuv_to_avframe(const cv::Mat& y, const cv::Mat& u, const cv::Mat& v, int width, int height) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;

    frame->width = width;
    frame->height = height;
    frame->format = AV_PIX_FMT_YUV420P;

    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }

    // 将 Mat 数据拷贝到 AVFrame
    for (int i = 0; i < height; i++) {
        memcpy(frame->data[0] + i * frame->linesize[0], y.data + i * y.step, width);
    }
    for (int i = 0; i < height / 2; i++) {
        memcpy(frame->data[1] + i * frame->linesize[1], u.data + i * u.step, width / 2);
        memcpy(frame->data[2] + i * frame->linesize[2], v.data + i * v.step, width / 2);
    }

    return frame;
}

// 【核心实现】使用光流法进行插值 (最终优化版)
// 【核心实现】使用简单的线性插值进行补帧
// 【核心实现】使用光流法进行插值 (最终优化版)
AVFrame* DecodedFrameBuffer::interpolate(const AVFrame* prev, const AVFrame* next, double factor) {
    if (prev->format != AV_PIX_FMT_YUV420P || next->format != AV_PIX_FMT_YUV420P) {
        return nullptr;
    }

    // 分配新的AVFrame
    AVFrame* interpolated_frame = av_frame_alloc();
    if (!interpolated_frame) {
        return nullptr;
    }

    interpolated_frame->width = prev->width;
    interpolated_frame->height = prev->height;
    interpolated_frame->format = AV_PIX_FMT_YUV420P;

    if (av_frame_get_buffer(interpolated_frame, 32) < 0) {
        av_frame_free(&interpolated_frame);
        return nullptr;
    }

    // 对Y通道进行带边缘检测的插值
    for (int y = 1; y < prev->height - 1; ++y) {
        for (int x = 1; x < prev->width - 1; ++x) {
            // 计算梯度来检测边缘
            int gx_prev = std::abs(prev->data[0][y * prev->linesize[0] + x + 1] - prev->data[0][y * prev->linesize[0] + x - 1]);
            int gy_prev = std::abs(prev->data[0][(y + 1) * prev->linesize[0] + x] - prev->data[0][(y - 1) * prev->linesize[0] + x]);

            int gx_next = std::abs(next->data[0][y * next->linesize[0] + x + 1] - next->data[0][y * next->linesize[0] + x - 1]);
            int gy_next = std::abs(next->data[0][(y + 1) * next->linesize[0] + x] - next->data[0][(y - 1) * next->linesize[0] + x]);

            int gradient_prev = std::max(gx_prev, gy_prev);
            int gradient_next = std::max(gx_next, gy_next);

            int prev_value = prev->data[0][y * prev->linesize[0] + x];
            int next_value = next->data[0][y * next->linesize[0] + x];

            // 在边缘区域使用加权更保守的插值，减少模糊
            if (gradient_prev > 20 || gradient_next > 20) {
                double edge_factor = factor * 0.7; // 减少边缘处的插值强度
                interpolated_frame->data[0][y * interpolated_frame->linesize[0] + x] =
                    static_cast<uint8_t>(prev_value + edge_factor * (next_value - prev_value));
            }
            else {
                // 非边缘区域使用正常插值
                interpolated_frame->data[0][y * interpolated_frame->linesize[0] + x] =
                    static_cast<uint8_t>(prev_value + factor * (next_value - prev_value));
            }
        }
    }

    // 对U通道进行线性插值
    for (int y = 0; y < prev->height / 2; ++y) {
        for (int x = 0; x < prev->width / 2; ++x) {
            int prev_value = prev->data[1][y * prev->linesize[1] + x];
            int next_value = next->data[1][y * next->linesize[1] + x];
            interpolated_frame->data[1][y * interpolated_frame->linesize[1] + x] = static_cast<uint8_t>(prev_value + factor * (next_value - prev_value));
        }
    }

    // 对V通道进行线性插值
    for (int y = 0; y < prev->height / 2; ++y) {
        for (int x = 0; x < prev->width / 2; ++x) {
            int prev_value = prev->data[2][y * prev->linesize[2] + x];
            int next_value = next->data[2][y * next->linesize[2] + x];
            interpolated_frame->data[2][y * interpolated_frame->linesize[2] + x] = static_cast<uint8_t>(prev_value + factor * (next_value - prev_value));
        }
    }

    return interpolated_frame;
}