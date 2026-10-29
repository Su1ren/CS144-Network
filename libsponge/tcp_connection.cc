#include "tcp_connection.hh"

#include "tcp_segment.hh"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>

// Dummy implementation of a TCP connection

// For Lab 4, please replace with a real implementation that passes the
// automated checks run by `make check`.

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

size_t TCPConnection::remaining_outbound_capacity() const { return _sender.stream_in().remaining_capacity(); }

size_t TCPConnection::bytes_in_flight() const { return _sender.bytes_in_flight(); }

size_t TCPConnection::unassembled_bytes() const { return _receiver.unassembled_bytes(); }

size_t TCPConnection::time_since_last_segment_received() const { return _time_since_last_segment_received; }

void TCPConnection::segment_received(const TCPSegment &seg) {
    if (!active()) {
        return;
    }
    // 如果是监听状态
    if (_sender.next_seqno_absolute() == 0 && !_receiver.ackno().has_value()) {
        // 忽略 RST、纯 ACK 以及不带 SYN 的段
        if (seg.header().rst || !seg.header().syn) {
            return;
        }
        if (seg.header().ack && seg.length_in_sequence_space() == 0) {
            return;
        }
    } else if (seg.header().rst) {
        // 非监听状态
        // 收到 RST 标志
        // 设置双向 ByteStream 都设为 error
        _sender.stream_in().set_error();
        _receiver.stream_out().set_error();
        // 连接结束
        shutdown();
        return;
    }
    // 重置计时
    _time_since_last_segment_received = 0;
    _receiver.segment_received(seg);
    // 处理 ACK
    if (seg.header().ack && _sender.next_seqno_absolute() > 0) {
        _sender.ack_received(seg.header().ackno, seg.header().win);
    }
    // 如果接收端已经关闭
    if (_receiver.stream_out().input_ended() && !_sender.stream_in().eof()) {
        _linger_after_streams_finish = false;
    }
    // 如果不是纯 ACK，回发数据
    if (seg.length_in_sequence_space() > 0) {
        _sender.fill_window();
        // 如果发送窗口为空，则发送一个空段
        if (_sender.segments_out().empty()) {
            _sender.send_empty_segment();
        }
    } else if (_receiver.ackno().has_value() && seg.header().seqno != _receiver.ackno().value() &&
               _receiver.window_size() > 0) {
        // 零窗口探测（或过期的重复纯 ACK）：接收窗口已重新打开时，回 ACK 通告新窗口
        _sender.send_empty_segment();
    }
    _flush_sender();
}

bool TCPConnection::active() const {
    if (!_active)
        return false;
    // 接收端输入 eof
    // 发送端没有字节等待确认
    bool done = _receiver.stream_out().input_ended() && _sender.stream_in().eof() && _sender.bytes_in_flight() == 0;
    if (!done)
        return true;
    // 是否还有待处理的段
    if (!_linger_after_streams_finish) {
        // 被动关闭
        return false;
    }
    // 等待过久，主动关闭
    return _time_since_last_segment_received < 10 * _cfg.rt_timeout;
}

size_t TCPConnection::write(const string &data) {
    // 数据写入字节流
    auto n = _sender.stream_in().write(data);
    // 填充发送窗口
    _sender.fill_window();
    // 刷新发送队列
    _flush_sender();
    return n;
}

//! \param[in] ms_since_last_tick number of milliseconds since the last call to this method
void TCPConnection::tick(const size_t ms_since_last_tick) {
    _time_since_last_segment_received += ms_since_last_tick;
    if (!_active)
        return;

    // Sender's timer logic
    _sender.tick(ms_since_last_tick);
    // 检查连续重传次数
    if (_sender.consecutive_retransmissions() > _cfg.MAX_RETX_ATTEMPTS) {
        // 超过重传最大次数
        // 清空发送队列
        std::queue<TCPSegment> empty;
        _segments_out.swap(empty);
        // 发送一个 RST 段
        _sender.send_empty_segment();
        auto seg = _sender.segments_out().front();
        seg.header().rst = true;
        _segments_out.push(seg);
        // 设置错误标志
        _sender.stream_in().set_error();
        _receiver.stream_out().set_error();
        _active = false;
        _linger_after_streams_finish = false;
        return;
    }
    _flush_sender();
}

void TCPConnection::end_input_stream() {
    // 关闭发送端字节流
    _sender.stream_in().end_input();
    // 发送一个 FIN 段
    _sender.fill_window();
    // 清空发送队列
    _flush_sender();
}

void TCPConnection::connect() {
    // 发送端发送一个 SYN，发起第一次握手
    _sender.fill_window();
    _flush_sender();
}

void TCPConnection::shutdown() {
    _linger_after_streams_finish = false;
    // 标记连接已经关闭
    _active = false;
    // 丢弃所有待发送的段
    std::queue<TCPSegment> empty;
    _segments_out.swap(empty);
    // 此后收到所有段直接返回
}

void TCPConnection::_flush_sender() {
    // 从 _sender 的发送段中取出所有段并加入到 _segments_out 中
    while (!_sender.segments_out().empty()) {
        auto seg = _sender.segments_out().front();
        // 如果接收端接受过了信号，则对应填写 ackno
        if (_receiver.ackno().has_value()) {
            seg.header().ack = true;
            seg.header().ackno = _receiver.ackno().value();
        }
        // 设置 window 大小；超过 uint16_t 可表示范围时通告最大值
        constexpr size_t max_win = std::numeric_limits<uint16_t>::max();
        seg.header().win = static_cast<uint16_t>(std::min(_receiver.window_size(), max_win));
        _segments_out.push(seg);
        _sender.segments_out().pop();
    }
}

TCPConnection::~TCPConnection() {
    try {
        if (active()) {
            cerr << "Warning: Unclean shutdown of TCPConnection\n";

            // 丢弃两端待发送的段，避免 RST 混在旧数据后面
            std::queue<TCPSegment> empty_out;
            _segments_out.swap(empty_out);
            std::queue<TCPSegment> empty_sender;
            _sender.segments_out().swap(empty_sender);

            // 借助 sender 取得正确的 seqno，再构造 RST
            _sender.send_empty_segment();
            auto seg = _sender.segments_out().front();
            _sender.segments_out().pop();
            seg.header().rst = true;

            // LISTEN 状态下还没有 ackno，不能直接 value()
            if (_receiver.ackno().has_value()) {
                seg.header().ack = true;
                seg.header().ackno = _receiver.ackno().value();
            }
            constexpr size_t max_win = std::numeric_limits<uint16_t>::max();
            seg.header().win = static_cast<uint16_t>(std::min(_receiver.window_size(), max_win));
            _segments_out.push(seg);

            // 双向字节流进入 error 状态
            _sender.stream_in().set_error();
            _receiver.stream_out().set_error();
            _active = false;
            _linger_after_streams_finish = false;
        }
    } catch (const exception &e) {
        std::cerr << "Exception destructing TCP FSM: " << e.what() << std::endl;
    }
}
