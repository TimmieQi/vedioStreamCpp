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
AVFrame* DecodedFrameBuffer::interpolate(const AVFrame* prev, const AVFrame* next, double factor) {
    if (prev->format != AV_PIX_FMT_YUV420P || next->format != AV_PIX_FMT_YUV420P) {
        return nullptr;
    }

    // 1. 【性能优化】直接在降维后的图像上完成所有操作
    const double scale_factor = 0.5; // 在一半的尺寸上操作
    cv::Size small_size(prev->width * scale_factor, prev->height * scale_factor);
    cv::Size small_size_uv(prev->width / 2 * scale_factor, prev->height / 2 * scale_factor);

    cv::Mat prev_y, prev_u, prev_v;
    cv::Mat next_y, next_u, next_v;
    avframe_to_mats_yuv(prev, prev_y, prev_u, prev_v);
    avframe_to_mats_yuv(next, next_y, next_u, next_v);

    cv::Mat prev_y_small, prev_u_small, prev_v_small;
    cv::Mat next_y_small, next_u_small, next_v_small;

    // 2. 将所有通道降维
    cv::resize(prev_y, prev_y_small, small_size, 0, 0, cv::INTER_LINEAR);
    cv::resize(prev_u, prev_u_small, small_size_uv, 0, 0, cv::INTER_LINEAR);
    cv::resize(prev_v, prev_v_small, small_size_uv, 0, 0, cv::INTER_LINEAR);
    cv::resize(next_y, next_y_small, small_size, 0, 0, cv::INTER_LINEAR);
    cv::resize(next_u, next_u_small, small_size_uv, 0, 0, cv::INTER_LINEAR);
    cv::resize(next_v, next_v_small, small_size_uv, 0, 0, cv::INTER_LINEAR);

    // 3. 在降维后的Y通道上计算光流
    cv::Mat flow_small;
    cv::calcOpticalFlowFarneback(prev_y_small, next_y_small, flow_small, 0.5, 3, 10, 3, 5, 1.1, 0);

    // 4. 在降维尺寸上创建映射图
    cv::Mat map1(small_size, CV_32FC2);
    cv::Mat map2(small_size, CV_32FC2);
    for (int y = 0; y < small_size.height; ++y) {
        for (int x = 0; x < small_size.width; ++x) {
            cv::Point2f flow_at_point = flow_small.at<cv::Point2f>(y, x);
            // 注意：这里的运动矢量已经是小尺寸上的了，不需要再缩放
            map1.at<cv::Point2f>(y, x) = cv::Point2f(x + flow_at_point.x * factor, y + flow_at_point.y * factor);
            map2.at<cv::Point2f>(y, x) = cv::Point2f(x - flow_at_point.x * (1.0 - factor), y - flow_at_point.y * (1.0 - factor));
        }
    }

    // 5. 在降维尺寸上扭曲和融合所有通道
    cv::Mat warped_prev_y, warped_next_y, final_y_small;
    cv::remap(prev_y_small, warped_prev_y, map1, cv::Mat(), cv::INTER_LINEAR);
    cv::remap(next_y_small, warped_next_y, map2, cv::Mat(), cv::INTER_LINEAR);
    cv::addWeighted(warped_prev_y, 1.0 - factor, warped_next_y, factor, 0.0, final_y_small);

    // 为U/V通道创建映射图
    cv::Mat map1_uv, map2_uv;
    cv::resize(map1, map1_uv, small_size_uv, 0, 0, cv::INTER_LINEAR);
    cv::resize(map2, map2_uv, small_size_uv, 0, 0, cv::INTER_LINEAR);
    map1_uv *= 0.5; // 映射图坐标也要缩放
    map2_uv *= 0.5;

    cv::Mat warped_prev_u, warped_next_u, final_u_small;
    cv::remap(prev_u_small, warped_prev_u, map1_uv, cv::Mat(), cv::INTER_LINEAR);
    cv::remap(next_u_small, warped_next_u, map2_uv, cv::Mat(), cv::INTER_LINEAR);
    cv::addWeighted(warped_prev_u, 1.0 - factor, warped_next_u, factor, 0.0, final_u_small);

    cv::Mat warped_prev_v, warped_next_v, final_v_small;
    cv::remap(prev_v_small, warped_prev_v, map1_uv, cv::Mat(), cv::INTER_LINEAR);
    cv::remap(next_v_small, warped_next_v, map2_uv, cv::Mat(), cv::INTER_LINEAR);
    cv::addWeighted(warped_prev_v, 1.0 - factor, warped_next_v, factor, 0.0, final_v_small);

    // 6. 【性能优化】只在最后将融合后的结果升维
    cv::Mat final_y, final_u, final_v;
    cv::resize(final_y_small, final_y, prev_y.size(), 0, 0, cv::INTER_LINEAR);
    cv::resize(final_u_small, final_u, prev_u.size(), 0, 0, cv::INTER_LINEAR);
    cv::resize(final_v_small, final_v, prev_v.size(), 0, 0, cv::INTER_LINEAR);

    // 7. 将最终的 YUV Mat 转换回 AVFrame 并返回
    return mats_yuv_to_avframe(final_y, final_u, final_v, prev->width, prev->height);
}
