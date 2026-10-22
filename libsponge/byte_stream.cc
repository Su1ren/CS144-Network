#include "byte_stream.hh"

#include <algorithm>

// Dummy implementation of a flow-controlled in-memory byte stream.

// For Lab 0, please replace with a real implementation that passes the
// automated checks run by `make check_lab0`.

// You will need to add private members to the class declaration in `byte_stream.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

ByteStream::ByteStream(const size_t capacity)
    : _capacity(capacity), _buffer(), _total_written(0), _total_read(0), _error(false) {
    // DUMMY_CODE(capacity);
}

size_t ByteStream::write(const string &data) {
    size_t bytes_to_write = min(data.size(), remaining_capacity());
    _buffer.insert(_buffer.end(), data.begin(), data.begin() + bytes_to_write);
    _total_written += bytes_to_write;
    return bytes_to_write;
}

//! \param[in] len bytes will be copied from the output side of the buffer
string ByteStream::peek_output(const size_t len) const {
    // 检查是否有足够的字节可供读取
    size_t bytes_to_read = min(len, _buffer.size());
    // 从缓冲区中获取指定数量的字节
    string result(_buffer.begin(), _buffer.begin() + bytes_to_read);
    return result;
}

//! \param[in] len bytes will be removed from the output side of the buffer
void ByteStream::pop_output(const size_t len) {
    // 检查是否有足够的字节可供移除
    size_t bytes_to_pop = min(len, _buffer.size());
    // 从缓冲区中移除指定数量的字节
    _buffer.erase(_buffer.begin(), _buffer.begin() + bytes_to_pop);
    // 更新已读取的字节数
    _total_read += bytes_to_pop;
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

size_t ByteStream::buffer_size() const { return _buffer.size(); }

bool ByteStream::buffer_empty() const { return buffer_size() == 0; }

bool ByteStream::eof() const { return buffer_empty() && input_ended(); }

size_t ByteStream::bytes_written() const { return _total_written; }

size_t ByteStream::bytes_read() const { return _total_read; }

size_t ByteStream::remaining_capacity() const { return _capacity - _buffer.size(); }