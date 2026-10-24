#include "stream_reassembler.hh"

#include <algorithm>
#include <cstring>

using namespace std;

// The reassembler keeps every byte that is currently inside the flow-control
// window in a fixed-size ring buffer. The window is
//   [bytes_read(), bytes_read() + capacity)
// so the absolute index `i` of a live byte maps to `i % capacity` without any
// collisions. A parallel bitmap records which slots already hold data, which
// makes duplicate/overlapping segments cheap to skip and makes the
// "write everything that is contiguous" step a simple linear scan.

StreamReassembler::StreamReassembler(const size_t capacity)
    : _output(capacity)
    , _capacity(capacity)
    , _buffer(capacity)
    , _received(capacity, 0)
    , _unassembled_bytes(0)
    , _eof(false)
    , _eof_index(0) {}

size_t StreamReassembler::first_unassembled() const { return _output.bytes_written(); }

size_t StreamReassembler::slot(const size_t index) const { return index % _capacity; }

//! \details This function accepts a substring (aka a segment) of bytes,
//! possibly out-of-order, from the logical stream, and assembles any newly
//! contiguous substrings and writes them into the output stream in order.
void StreamReassembler::push_substring(const string &data, const size_t index, const bool eof) {
    // 先检查是否遇到 EOF，如果遇到则记录 EOF 的位置
    if (eof) {
        _eof = true;
        _eof_index = index + data.size();
    }
    // 如果 buffer 的容量大于 0，则尝试将数据存入 buffer 中
    if (_capacity > 0) {
        // 已按序交给 _output 的字节数就是 first_unassembled()，即下一个待组装的字节的绝对索引
        const size_t fu = first_unassembled();
        // Bytes may only be stored inside the flow-control window.
        // 只有当数据索引在流控窗口内时，才会被存储到 buffer 中
        // bytes_read()是已被上层读走的有序字节数，capacity 是 buffer的容量，window_end 是流控窗口的结束索引
        const size_t window_end = _output.bytes_read() + _capacity;

        const size_t start = max(index, fu);  // 数据的起始索引是 index，但不能小于 first_unassembled()
        const size_t end =
            min(index + data.size(), window_end);  // 数据结束索引是 index + data.size()，但不能超过流控窗口的结束索引

        if (start < end) {
            const size_t len = end - start;
            // `src` 指向 data 中对应于 start 的字节
            const char *src = data.data() + (start - index);

            size_t consumed = 0;
            // 快路径：如果 start == first_unassembled() 且 _unassembled_bytes == 0，则说明 buffer
            // 中没有未组装的字节，可以直接将数据写入输出流
            if (start == fu && _unassembled_bytes == 0) {
                // In-order streaming fast path: nothing is buffered, so the new
                // bytes can go straight into the output stream.
                consumed = _output.write(src, len);
            }
            // 如果在还有数据需要存储的情况下，尝试将数据存入 buffer 中
            const size_t store_start = start + consumed;
            if (store_start < end) {
                // `src` points at the byte for `start`; advance to `store_start`.
                store_range(store_start, end, src + (store_start - start));
            }
            // 如果在存储完数据后，仍然有数据可以写入输出流，则调用 flush() 将连续的字节写入输出流
            flush();
        }
    }
    // 如果已经遇到 EOF 且所有字节都已写入输出流，则结束输出流
    if (_eof && first_unassembled() >= _eof_index) {
        _output.end_input();
    }
}

//! \returns true if any slot in [start, end) already holds received data.
bool StreamReassembler::any_received(const size_t start, const size_t end) const {
    size_t pos = slot(start);
    size_t remaining = end - start;  // 还需要检查的字节数
    while (remaining > 0) {
        // 待检查的字节可能跨越 ring buffer 的末尾，因此每次检查一段连续的字节
        const size_t len = min(remaining, _capacity - pos);
        // 从 ring buffer 的 pos 位置开始检查 len 个字节，如果有任何一个字节已经收到，则返回 true
        if (memchr(_received.data() + pos, 1, len) != nullptr) {
            return true;
        }
        remaining -= len;
        // 回到 ring buffer 的开头继续检查
        pos = 0;
    }
    return false;
}

//! \details Store the bytes of `src` in [start, end).
//! Bytes that already arrived are skipped, so duplicates are counted once.
void StreamReassembler::store_range(const size_t start, const size_t end, const char *src) {
    const size_t len = end - start;

    // 快路径：如果 start 到 end 的范围内没有任何字节已经收到，则可以直接将数据存入 ring buffer 中
    if (!any_received(start, end)) {
        // Nothing overlaps: copy the range in at most two memcpys.
        const size_t pos = slot(start);
        const size_t first_len = min(len, _capacity - pos);
        // 拷贝数据到 ring buffer 中，并将对应的 bitmap 位置标记为已收到
        memcpy(_buffer.data() + pos, src, first_len);
        memset(_received.data() + pos, 1, first_len);
        // 如果数据跨越了 ring buffer 的末尾，则需要将剩余的数据拷贝到 ring buffer 的开头，并将对应的 bitmap
        // 位置标记为已收到
        if (first_len < len) {
            memcpy(_buffer.data(), src + first_len, len - first_len);
            memset(_received.data(), 1, len - first_len);
        }
        _unassembled_bytes += len;
        return;
    }

    // 一般路径：如果 start 到 end
    // 的范围内有任何字节已经收到，则需要逐个检查每个字节是否已经收到，如果没有收到，则将其存入 ring buffer
    // 中，并将对应的 bitmap 位置标记为已收到
    size_t pos = slot(start);
    for (size_t i = start; i < end; ++i) {
        if (_received[pos] == 0) {
            _buffer[pos] = src[i - start];
            _received[pos] = 1;
            ++_unassembled_bytes;
        }
        if (++pos == _capacity) {
            pos = 0;
        }
    }
}

//! \details Write every byte that is contiguous with the output stream, then
//! clear the corresponding bitmap slots.
void StreamReassembler::flush() {
    size_t fu = first_unassembled();  // 未有序组装的字节的绝对索引
    const size_t window_end = _output.bytes_read() + _capacity;

    // 留空窗口内还有未组装的字节，且这些字节已经连续到输出流中，则将这些字节写入输出流，并清除对应的 bitmap 位
    while (fu < window_end) {
        const size_t pos = slot(fu);  // 起始位置在 ring buffer 中的索引
        // 如果当前字节没有收到，则退出循环
        if (_received[pos] == 0) {
            break;
        }

        // Find the end of the contiguous run inside this ring section.
        // 当前连续段的长度不能超过 ring buffer 的末尾，因此需要计算出当前连续段的最大长度
        const size_t max_len = min(window_end - fu, _capacity - pos);
        const uint8_t *const base = _received.data() + pos;
        const void *const zero = memchr(base, 0, max_len);
        const size_t run_len =
            zero != nullptr ? static_cast<size_t>(static_cast<const uint8_t *>(zero) - base) : max_len;

        // The run is contiguous both in `_buffer` and in the output window, so it
        // can be handed to the output stream without a temporary string copy.
        const size_t written = _output.write(_buffer.data() + pos, run_len);
        memset(_received.data() + pos, 0, written);
        _unassembled_bytes -= written;
        fu += written;

        if (written < run_len) {
            break;  // output stream is full; the rest stays buffered
        }
    }
}

size_t StreamReassembler::unassembled_bytes() const { return _unassembled_bytes; }

bool StreamReassembler::empty() const { return _unassembled_bytes == 0; }

size_t StreamReassembler::ack_index() const { return first_unassembled(); }