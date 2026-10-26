#include "tcp_receiver.hh"

// Dummy implementation of a TCP receiver

// For Lab 2, please replace with a real implementation that passes the
// automated checks run by `make check_lab2`.

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

void TCPReceiver::segment_received(const TCPSegment &seg) {
    // 如果收到 SYN，则记录 ISN
    if (seg.header().syn && !_isn.has_value()) {
        _isn = seg.header().seqno;
    }

    // 没有 SYN 之前无法 unwrap，暂不处理
    if (!_isn.has_value()) {
        return;
    }

    // 纯 ACK（不占序列号）无需交给 reassembler
    if (seg.length_in_sequence_space() == 0) {
        return;
    }

    // 根据 seqno 和 ISN 计算绝对序号，消除 wrap-around 的影响
    // 计算 seqno 对应的绝对序号；checkpoint 用最近已重组的字节数
    const uint64_t abs_seqno = unwrap(seg.header().seqno, _isn.value(), _reassembler.stream_out().bytes_written());

    // 转换绝对序号为从 0 开始的 stream index：
    // - 带 SYN 的段，seqno 指向 SYN（绝对序号 0），payload 从 stream index 0 开始
    // - 不带 SYN 的段，seqno 指向第一个 payload 字节（绝对序号 = index + 1）
    if (!seg.header().syn && abs_seqno == 0) {
        return;  // 非法：数据/FIN 落在 SYN 占用的序号上
    }
    const uint64_t stream_index = abs_seqno - (seg.header().syn ? 0 : 1);

    // FIN 表示 payload 之后流结束，push_substring 的 eof 恰好对应 FIN 的位置
    _reassembler.push_substring(seg.payload().copy(), stream_index, seg.header().fin);
}

optional<WrappingInt32> TCPReceiver::ackno() const {
    // 如果没有收到 SYN，则返回空
    if (!_isn.has_value()) {
        return nullopt;
    }

    // 绝对序号：SYN 占 1 个序号，数据每个字节占 1 个，FIN 占 1 个
    uint64_t abs_ackno = _reassembler.stream_out().bytes_written() + 1;
    if (_reassembler.stream_out().input_ended()) {
        ++abs_ackno;  // FIN 已被重组，下一个期望序号要越过 FIN
    }
    return wrap(abs_ackno, _isn.value());
}

size_t TCPReceiver::window_size() const {
    // first unassembled 到 first unacceptable 的距离：
    // 窗口左沿 = bytes_written，右沿 = bytes_read + capacity
    return _capacity - _reassembler.stream_out().buffer_size();
}
