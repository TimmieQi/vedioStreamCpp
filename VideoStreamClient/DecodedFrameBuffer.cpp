#include "DecodedFrameBuffer.h"
#include <algorithm> // for std::sort and std::find_if

// FFmpeg的头文件，因为.h中只是前向声明，.cpp中需要完整定义
extern "C" {
#include <libavutil/frame.h>
}

// 实现 DecodedFrame 的构造函数
DecodedFrame::DecodedFrame(AVFrame* fr)
// 在这里定义 lambda 删除器
    : frame(fr, [](AVFrame* f) { av_frame_free(&f); })
{
}

// 实现 DecodedFrame 的析构函数
// 即使函数体是空的，这个定义也是必须的。
// 它告诉编译器在何处实例化 unique_ptr 的析构逻辑，
// 而在这里，编译器已经看到了 av_frame_free 的完整声明。
DecodedFrame::~DecodedFrame() = default; // 使用 C++11 的 default 关键字

DecodedFrameBuffer::DecodedFrameBuffer()
    : buffer_size_ms_(500)
{
    reset();
}

// 实现 DecodedFrameBuffer 的析构函数
DecodedFrameBuffer::~DecodedFrameBuffer() = default;


void DecodedFrameBuffer::reset()
{
    std::lock_guard<std::mutex> lock(mtx_);
    // 当deque被清空时，它所包含的 unique_ptr 会被析构，
    // unique_ptr 的自定义删除器会自动调用 av_frame_free，从而释放所有 AVFrame
    queue_.clear();
    last_played_pts_ = -1;
}

void DecodedFrameBuffer::add_frame(std::unique_ptr<DecodedFrame> frame)
{
    if (!frame || !frame->frame) return;

    std::lock_guard<std::mutex> lock(mtx_);
    queue_.push_back(std::move(frame));

    // 保持队列按PTS排序
    // a 和 b 都是 std::unique_ptr<DecodedFrame>
    std::sort(queue_.begin(), queue_.end(), [](const auto& a, const auto& b) {
        // 通过 unique_ptr 访问 DecodedFrame，再访问其内部的 AVFrame 的 pts
        return a->frame->pts < b->frame->pts;
        });
}

std::unique_ptr<DecodedFrame> DecodedFrameBuffer::get_frame(int64_t target_pts_ms)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (queue_.empty() || target_pts_ms < 0) {
        return nullptr;
    }

    // 寻找最后一个 PTS <= target_pts_ms 的帧
    std::unique_ptr<DecodedFrame> best_frame = nullptr;
    auto best_frame_it = queue_.end();

    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if ((*it)->frame->pts <= target_pts_ms) {
            best_frame_it = it;
        }
        else {
            // 因为队列是排序的，一旦找到一个PTS更大的，后面的就不用看了
            break;
        }
    }

    // 如果找到了合适的帧
    if (best_frame_it != queue_.end()) {
        // 取出帧
        best_frame = std::move(*best_frame_it);
        last_played_pts_ = best_frame->frame->pts;

        // 删除从开头到 best_frame_it (包含) 的所有帧
        queue_.erase(queue_.begin(), best_frame_it + 1);

        return best_frame;
    }

    // 没有找到PTS小于等于目标的帧 (可能所有帧都太新了)
    return nullptr;
}