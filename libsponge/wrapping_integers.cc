#include "wrapping_integers.hh"

// Dummy implementation of a 32-bit wrapping integer

// For Lab 2, please replace with a real implementation that passes the
// automated checks run by `make check_lab2`.

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

//! Transform an "absolute" 64-bit sequence number (zero-indexed) into a WrappingInt32
//! \param n The input absolute 64-bit sequence number
//! \param isn The initial sequence number
WrappingInt32 wrap(uint64_t n, WrappingInt32 isn) {
    // 绝对数据序列号 n 加上 ISN 的基准，最后对 2^32 取模，得到 TCP 流内的相对序列号
    uint64_t raw_value = (isn.raw_value() + n) % (1ul << 32);
    return WrappingInt32{static_cast<uint32_t>(raw_value)};
}

//! Transform a WrappingInt32 into an "absolute" 64-bit sequence number (zero-indexed)
//! \param n The relative sequence number
//! \param isn The initial sequence number
//! \param checkpoint A recent absolute 64-bit sequence number
//! \returns the 64-bit sequence number that wraps to `n` and is closest to `checkpoint`
//!
//! \note Each of the two streams of the TCP connection has its own ISN. One stream
//! runs from the local TCPSender to the remote TCPReceiver and has one ISN,
//! and the other stream runs from the remote TCPSender to the local TCPReceiver and
//! has a different ISN.
uint64_t unwrap(WrappingInt32 n, WrappingInt32 isn, uint64_t checkpoint) {
    static constexpr uint64_t MOD = 1ULL << 32;
    static constexpr int64_t HALF = 1LL << 31;

    // 相对 ISN 的偏移量（模 2^32）：seqno n 与 ISN 相差多少个序号
    const uint32_t offset = n.raw_value() - isn.raw_value();

    // 与 checkpoint 处于同一个 2^32 区块的基址
    const uint64_t base = checkpoint & ~(MOD - 1);

    // 先构造落在 [base, base + 2^32) 内的候选值
    uint64_t candidate = base + offset;

    // candidate 与 checkpoint 的有符号差必在 (-2^32, 2^32) 内
    const int64_t diff = static_cast<int64_t>(candidate - checkpoint);

    // 选择最近的候选值。注意绝对序号不能为负：
    // candidate < 2^32 时不能再减 2^32，否则无符号下溢成接近 2^64 的值
    if (diff > HALF) {
        if (candidate >= MOD) {
            candidate -= MOD;
        }
    } else if (diff < -HALF) {
        candidate += MOD;
    }

    return candidate;
}
