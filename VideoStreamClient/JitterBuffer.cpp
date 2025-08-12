#include "JitterBuffer.h"

// 注意：模板类的成员函数实现通常也放在头文件中，但对于非模板化的构造函数等可以放在cpp
JitterBuffer::JitterBuffer(size_t max_size)
    : max_size_(max_size)
{
    reset();
}

void JitterBuffer::reset()
{
    std::lock_guard<std::mutex> lock(mtx_);
    // 清空 priority_queue
    std::priority_queue<MediaPacket, std::vector<MediaPacket>, std::greater<MediaPacket>> empty_queue;
    buffer_.swap(empty_queue);

    expected_seq_ = -1;
}

void JitterBuffer::add_packet(std::unique_ptr<MediaPacket> packet)
{
    if (!packet) return;

    std::lock_guard<std::mutex> lock(mtx_);

    // 初始化期望序列号
    if (expected_seq_ == -1) {
        expected_seq_ = packet->seq;
    }

    // 只添加在预期范围内的包，防止缓冲区被过时的包填满
    if (packet->seq >= expected_seq_ && buffer_.size() < max_size_) {
        buffer_.push(std::move(*packet));
    }
}

std::unique_ptr<MediaPacket> JitterBuffer::get_packet()
{
    std::lock_guard<std::mutex> lock(mtx_);

    if (buffer_.empty()) {
        return nullptr; // 返回空指针表示没有包
    }

    // 查看堆顶的包（序列号最小的）
    const MediaPacket& top_packet = buffer_.top();

    if (top_packet.seq == expected_seq_) {
        // 序列号匹配，正常出队
        // 不能直接返回 top_packet 的引用，因为它马上要被 pop 掉
        // 需要创建一个副本
        auto packet_to_return = std::make_unique<MediaPacket>(std::move(const_cast<MediaPacket&>(buffer_.top())));
        buffer_.pop();
        expected_seq_++; // 推进期望序列号
        return packet_to_return;
    }
    else if (top_packet.seq < expected_seq_) {
        // 收到过时的包，直接丢弃并尝试获取下一个
        buffer_.pop();
        return get_packet(); // 递归获取
    }
    else { // top_packet.seq > expected_seq_，意味着发生了丢包
        // 我们不从缓冲区拿走数据，因为那个是未来的包。
        // 返回 nullptr，让调用者知道当前期望的包丢失了。
        // （在音频播放器中，调用者可以播放静音来填补）
        // 同时推进期望序列号，模拟播放一个丢失的包
        expected_seq_++;
        return nullptr;
    }
}