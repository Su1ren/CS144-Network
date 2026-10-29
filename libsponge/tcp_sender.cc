#include "tcp_sender.hh"

#include "tcp_config.hh"

#include <algorithm>
#include <random>
#include <string>

// Dummy implementation of a TCP sender

// For Lab 3, please replace with a real implementation that passes the
// automated checks run by `make check_lab3`.

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

//! \param[in] capacity the capacity of the outgoing byte stream
//! \param[in] retx_timeout the initial amount of time to wait before retransmitting the oldest outstanding segment
//! \param[in] fixed_isn the Initial Sequence Number to use, if set (otherwise uses a random ISN)
TCPSender::TCPSender(const size_t capacity, const uint16_t retx_timeout, const std::optional<WrappingInt32> fixed_isn)
    : _isn(fixed_isn.value_or(WrappingInt32{random_device()()}))
    , _segments_out{}
    , _initial_retransmission_timeout{retx_timeout}
    , _stream(capacity)
    , _next_seqno{0}
    , _outstanding_segments{}
    , _current_retransmission_timeout{retx_timeout}
    , _consecutive_retransmissions{0}
    , _bytes_in_flight{0}
    , _retransmission_timer{retx_timeout}
    , _window_size{1}
    , _syn_sent{false}
    , _fin_sent{false}
    , _last_ackno{0} {}

size_t TCPSender::bytes_in_flight() const { return _bytes_in_flight; }

void TCPSender::_send_segment(TCPSegment &seg) {
    const size_t length = seg.length_in_sequence_space();
    if (length == 0) {
        // 不占序列号的段（例如空 ACK）不需要记录为 outstanding，也永不重传
        _segments_out.push(seg);
        return;
    }

    // 记录占序列号的段为 outstanding，并更新相关状态
    _outstanding_segments.push_back({seg, _next_seqno});
    _next_seqno += length;
    _bytes_in_flight += length;
    _segments_out.push(seg);

    // 发送占序列号的段时，若重传定时器未运行则启动它
    if (!_retransmission_timer.is_running()) {
        _retransmission_timer.set_timeout(_current_retransmission_timeout);
        _retransmission_timer.reset();
        _retransmission_timer.start();
    }
}

void TCPSender::fill_window() {
    // SYN 占一个序列号，必须先于所有数据和 FIN 发送
    if (!_syn_sent) {
        _syn_sent = true;
        TCPSegment syn_seg;
        syn_seg.header().syn = true;
        syn_seg.header().seqno = next_seqno();
        _send_segment(syn_seg);
    }

    // 窗口为 0 时按 1 字节处理：发送单字节探测段，促使接收方通告新窗口
    const size_t window = _window_size == 0 ? 1 : _window_size;

    // 窗口中剩余可用的序列号数量。接收方收缩窗口时 _bytes_in_flight 可能
    // 更大，先比较再相减，避免 size_t 下溢成巨大的值。
    // 如果 _bytes_in_flight >= window，则即使探测段也无法发送，available 会变为 0，下面的 while
    // 循环不会执行。避免造成更多被丢弃的 segment。
    size_t available = window > _bytes_in_flight ? window - _bytes_in_flight : 0;

    while (available > 0) {
        const size_t max_payload = min(available, TCPConfig::MAX_PAYLOAD_SIZE);
        string data = _stream.read(max_payload);

        // 检查是否需要发送 FIN：
        // read() 之后 EOF 才可能变为 true；FIN 占 1 个序列号，必须放得下
        const bool send_fin = _stream.eof() && !_fin_sent && data.size() + 1 <= available;

        // 没有数据可发，也不需要或放不下 FIN：退出，避免无进展死循环
        if (data.empty() && !send_fin) {
            break;
        }

        // 可以发送 segment 了：构造并发送
        TCPSegment seg;
        seg.header().seqno = next_seqno();
        seg.payload() = Buffer(std::move(data));
        if (send_fin) {
            seg.header().fin = true;
            _fin_sent = true;
        }

        _send_segment(seg);
        available -= seg.length_in_sequence_space();
    }
}

//! \param ackno The remote receiver's ackno (acknowledgment number)
//! \param window_size The remote receiver's advertised window size
void TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    const uint64_t abs_ackno = unwrap(ackno, _isn, _next_seqno);

    // 确认了尚未发送的数据：无效 ACK，直接忽略
    if (abs_ackno > _next_seqno) {
        return;
    }

    _window_size = window_size;

    // 移除所有已被完全确认的段（段尾序号 <= ackno）
    while (!_outstanding_segments.empty()) {
        const auto &outstanding_segment = _outstanding_segments.front();
        if (outstanding_segment.abs_seqno + outstanding_segment.segment.length_in_sequence_space() <= abs_ackno) {
            _bytes_in_flight -= outstanding_segment.segment.length_in_sequence_space();
            _outstanding_segments.pop_front();
        } else {
            break;
        }
    }

    // 只有确认了新数据（ackno 比之前更大）才执行 PDF 规则 7：
    // 复位 RTO、清零连续重传计数、按需重启定时器。
    // 重复 ACK 不能重置定时器，否则重传会被无限推迟。
    if (abs_ackno > _last_ackno) {
        _last_ackno = abs_ackno;
        _consecutive_retransmissions = 0;
        _current_retransmission_timeout = _initial_retransmission_timeout;
        _retransmission_timer.set_timeout(_current_retransmission_timeout);
        if (_outstanding_segments.empty()) {
            _retransmission_timer.stop();
        } else {
            _retransmission_timer.reset();
            _retransmission_timer.start();
        }
    } else if (_outstanding_segments.empty()) {
        _retransmission_timer.stop();
    }

    // 尝试继续发送。这里必须无条件调用 fill_window()：
    // 当窗口为 0 且 bytes_in_flight 为 0 时，fill_window() 会发送
    // 1 字节探测段（或 FIN），否则待发数据/FIN 可能永远发不出去。
    fill_window();
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void TCPSender::tick(const size_t ms_since_last_tick) {
    // 定时器只在有 outstanding 数据时运行
    if (!_retransmission_timer.is_running()) {
        return;
    }

    _retransmission_timer.tick(ms_since_last_tick);
    if (!_retransmission_timer.is_expired()) {
        return;
    }

    if (_outstanding_segments.empty()) {
        _retransmission_timer.stop();
        return;
    }

    // 重传最早（序号最低）的未确认段
    _segments_out.push(_outstanding_segments.front().segment);

    // 窗口非零时才累计连续重传次数并做指数退避；
    // 零窗口探测段重传时不 backoff。
    if (_window_size > 0) {
        _consecutive_retransmissions++;
        _current_retransmission_timeout *= 2;
        _retransmission_timer.set_timeout(_current_retransmission_timeout);
    }

    // 无论窗口是否为零，都要按当前 RTO 重新开始计时
    _retransmission_timer.reset();
}

unsigned int TCPSender::consecutive_retransmissions() const { return _consecutive_retransmissions; }

void TCPSender::send_empty_segment() {
    TCPSegment seg;
    seg.header().seqno = next_seqno();
    _segments_out.push(seg);
}
