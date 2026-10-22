#include "byte_stream.hh"

#include <algorithm>
#include <cstring>

using namespace std;

// A flow-controlled in-memory byte stream backed by a fixed-size
// `std::vector<char>` used as a ring buffer. Compared with the previous
// `std::deque<char>` version, the bytes live in a single contiguous allocation:
// `write()` and `peek_output()` are plain `memcpy`s instead of per-element
// iterator loops, and the stream allocates memory only once (at construction).

ByteStream::ByteStream(const size_t capacity)
    : _capacity(capacity), _buffer(capacity), _head(0), _size(0), _total_written(0), _total_read(0) {}

size_t ByteStream::write(const string &data) {
    const size_t n = min(data.size(), remaining_capacity());
    if (n == 0) {
        return 0;
    }

    const size_t tail = (_head + _size) % _capacity;  // ring buffer 中空闲的起始位置
    const size_t first = min(n, _capacity - tail);    // 第一段拷贝的长度，可能会 wrap-around

    memcpy(_buffer.data() + tail, data.data(), first);       // 拷贝第一段
    memcpy(_buffer.data(), data.data() + first, n - first);  // 如果数据超过了尾部，拷贝剩余的部分到 ring buffer 的开头

    _size += n;  // 更新 ring buffer 中的有效数据长度
    _total_written += n;
    return n;
}

//! \param[in] len bytes will be copied from the output side of the buffer
string ByteStream::peek_output(const size_t len) const {
    const size_t n = min(len, _size);
    if (n == 0) {
        return {};
    }

    // The buffered bytes may wrap around the end of the ring: copy at most two runs.
    const size_t first = min(n, _capacity - _head);
    string result(_buffer.data() + _head, first);
    result.append(_buffer.data(), n - first);
    return result;
}

//! \param[in] len bytes will be removed from the output side of the buffer
void ByteStream::pop_output(const size_t len) {
    const size_t n = min(len, _size);
    _size -= n;
    _total_read += n;
    _head = (_size == 0) ? 0 : (_head + n) % _capacity;
}

//! Read (i.e., copy and then pop) the next "len" bytes of the stream
//! \param[in] len bytes will be popped and returned
//! \returns a string
std::string ByteStream::read(const size_t len) {
    std::string result = peek_output(len);
    pop_output(len);
    return result;
}

void ByteStream::end_input() { _input_ended = true; }

bool ByteStream::input_ended() const { return _input_ended; }

size_t ByteStream::buffer_size() const { return _size; }

bool ByteStream::buffer_empty() const { return buffer_size() == 0; }

bool ByteStream::eof() const { return buffer_empty() && input_ended(); }

size_t ByteStream::bytes_written() const { return _total_written; }

size_t ByteStream::bytes_read() const { return _total_read; }

size_t ByteStream::remaining_capacity() const { return _capacity - _size; }