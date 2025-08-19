#include "DecodedFrameBuffer.h"
#include <algorithm>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

DecodedFrame::DecodedFrame(AVFrame* fr)
    : frame(fr, [](AVFrame* f) { av_frame_free(&f); })
{
}

DecodedFrame::~DecodedFrame() = default;

DecodedFrameBuffer::DecodedFrameBuffer() : buffer_size_ms_(200)
{
    reset();
}

DecodedFrameBuffer::~DecodedFrameBuffer() = default;

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

void DecodedFrameBuffer::set_buffer_duration(int ms)
{
    std::lock_guard<std::mutex> lock(mtx_);
    buffer_size_ms_ = ms;
}

// 【重要修改】恢复为一个简单、正确的实现
std::unique_ptr<DecodedFrame> DecodedFrameBuffer::get_frame(int64_t target_pts_ms)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (queue_.empty()) {
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

    if (best_frame_it == queue_.end()) {
        return nullptr;
    }

    last_played_pts_ = (*best_frame_it)->frame->pts;
    auto best_frame = std::move(*best_frame_it);

    // 清理所有比刚取出的帧更旧的帧（包括它自己，因为它已经被 move 了）
    auto new_end_it = std::remove_if(queue_.begin(), queue_.end(),
        [this](const auto& frm) {
            return !frm || frm->frame->pts <= this->last_played_pts_;
        });
    queue_.erase(new_end_it, queue_.end());

    return best_frame;
}

std::unique_ptr<DecodedFrame> DecodedFrameBuffer::get_interpolated_frame(int64_t target_pts_ms)
{
    const AVFrame* prev_frame = nullptr;
    const AVFrame* next_frame = nullptr;
    double factor = 0.0;

    get_interpolation_frames(target_pts_ms, prev_frame, next_frame, factor);

    if (!prev_frame || !next_frame) {
        return nullptr;
    }

    AVFrame* interpolated_frame_raw = interpolate(prev_frame, next_frame, factor);
    if (!interpolated_frame_raw) return nullptr;

    auto interpolated_frame_wrapper = std::make_unique<DecodedFrame>(interpolated_frame_raw);
    interpolated_frame_wrapper->frame->pts = target_pts_ms;
    return interpolated_frame_wrapper;
}

void DecodedFrameBuffer::get_interpolation_frames(int64_t target_pts_ms, const AVFrame*& out_prev, const AVFrame*& out_next, double& out_factor)
{
    std::lock_guard<std::mutex> lock(mtx_);
    out_prev = nullptr;
    out_next = nullptr;
    out_factor = 0.0;

    if (queue_.size() < 2) return;

    auto it = queue_.begin();
    while (it != queue_.end() && (*it)->frame->pts < target_pts_ms) {
        ++it;
    }

    if (it == queue_.begin() || it == queue_.end()) return;

    out_next = (*it)->frame.get();
    out_prev = (*(it - 1))->frame.get();

    double factor_calc = static_cast<double>(target_pts_ms - out_prev->pts) / static_cast<double>(out_next->pts - out_prev->pts);
    if (factor_calc < 0.0 || factor_calc > 1.0) {
        out_prev = nullptr;
        out_next = nullptr;
        return;
    }
    out_factor = factor_calc;
}

AVFrame* DecodedFrameBuffer::interpolate(const AVFrame* prev, const AVFrame* next, double factor) {
    if (prev->format != AV_PIX_FMT_YUV420P || next->format != AV_PIX_FMT_YUV420P) {
        return nullptr;
    }
    AVFrame* interpolated_frame = av_frame_alloc();
    if (!interpolated_frame) return nullptr;
    interpolated_frame->width = prev->width;
    interpolated_frame->height = prev->height;
    interpolated_frame->format = AV_PIX_FMT_YUV420P;
    if (av_frame_get_buffer(interpolated_frame, 32) < 0) {
        av_frame_free(&interpolated_frame);
        return nullptr;
    }
    for (int y = 1; y < prev->height - 1; ++y) {
        for (int x = 1; x < prev->width - 1; ++x) {
            int gx_prev = std::abs(prev->data[0][y * prev->linesize[0] + x + 1] - prev->data[0][y * prev->linesize[0] + x - 1]);
            int gy_prev = std::abs(prev->data[0][(y + 1) * prev->linesize[0] + x] - prev->data[0][(y - 1) * prev->linesize[0] + x]);
            int gx_next = std::abs(next->data[0][y * next->linesize[0] + x + 1] - next->data[0][y * next->linesize[0] + x - 1]);
            int gy_next = std::abs(next->data[0][(y + 1) * next->linesize[0] + x] - next->data[0][(y - 1) * next->linesize[0] + x]);
            int gradient_prev = std::max(gx_prev, gy_prev);
            int gradient_next = std::max(gx_next, gy_next);
            int prev_value = prev->data[0][y * prev->linesize[0] + x];
            int next_value = next->data[0][y * next->linesize[0] + x];
            if (gradient_prev > 20 || gradient_next > 20) {
                double edge_factor = factor * 0.7;
                interpolated_frame->data[0][y * interpolated_frame->linesize[0] + x] = static_cast<uint8_t>(prev_value + edge_factor * (next_value - prev_value));
            }
            else {
                interpolated_frame->data[0][y * interpolated_frame->linesize[0] + x] = static_cast<uint8_t>(prev_value + factor * (next_value - prev_value));
            }
        }
    }
    for (int y = 0; y < prev->height / 2; ++y) {
        for (int x = 0; x < prev->width / 2; ++x) {
            int prev_value = prev->data[1][y * prev->linesize[1] + x];
            int next_value = next->data[1][y * next->linesize[1] + x];
            interpolated_frame->data[1][y * interpolated_frame->linesize[1] + x] = static_cast<uint8_t>(prev_value + factor * (next_value - prev_value));
        }
    }
    for (int y = 0; y < prev->height / 2; ++y) {
        for (int x = 0; x < prev->width / 2; ++x) {
            int prev_value = prev->data[2][y * prev->linesize[2] + x];
            int next_value = next->data[2][y * next->linesize[2] + x];
            interpolated_frame->data[2][y * interpolated_frame->linesize[2] + x] = static_cast<uint8_t>(prev_value + factor * (next_value - prev_value));
        }
    }
    return interpolated_frame;
}

bool DecodedFrameBuffer::avframe_to_mat_gray(const AVFrame* av_frame, cv::Mat& out_mat) { return false; }
AVFrame* DecodedFrameBuffer::mat_to_avframe(const cv::Mat& mat, int width, int height) { return nullptr; }

// 【新增】获取当前缓冲区内帧的时间跨度的实现
int64_t DecodedFrameBuffer::get_current_duration_ms() const
{
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mtx_));
    if (queue_.size() < 2) {
        return 0;
    }
    // 返回最后一帧和第一帧的时间戳之差
    return queue_.back()->frame->pts - queue_.front()->frame->pts;
}