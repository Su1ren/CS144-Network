#ifndef SPONGE_LIBSPONGE_STREAM_REASSEMBLER_HH
#define SPONGE_LIBSPONGE_STREAM_REASSEMBLER_HH

#include "byte_stream.hh"

#include <cstdint>
#include <string>
#include <vector>

// clang-format off
//! \file
//! \brief The stream model of the Lab 1 handout (§3.1), written in symbols.
//!
//! The stream is one infinite array of bytes, each byte having its own
//! *absolute index* (the first byte ever sent has index 0).  At any instant four
//! cursors cut that index line into regions:
//!
//!   0              R                        W                          U
//!   |--------------|------------------------|--------------------------|----> +infinity
//!   | already read |       in _output       |   in _buffer (auxiliary) |  discarded
//!   |    (blue)    |        (green)         |          (red)           |
//!
//!   marker               symbol   expression
//!   ------------------   ------   -------------------------------------------------------
//!   stream start         0        index of the first byte ever sent
//!   first unread         R        stream_out().bytes_read()
//!   first unassembled    W        stream_out().bytes_written() == first_unassembled()
//!   first unacceptable   U        stream_out().bytes_read() + _capacity == window_end
//!
//! The handout's "width: capacity" bracket spans exactly [R, U):
//!
//!   (W - R)   +   (U - W)   =   U - R   =   _capacity
//!    green         red
//!
//! so the bytes already assembled into the ByteStream (green) and the bytes still
//! held in auxiliary storage (red) *share* one budget of `capacity` bytes, while
//! the bytes already read by the application (blue) are free.  Consequences:
//!
//!   * A byte with absolute index i is admissible iff W <= i < U; everything at
//!     i >= U is silently discarded.
//!   * _buffer never holds a byte of [R, W): those bytes already live in _output.
//!     Its live range is the red region [W, U).
//!   * [R, U) has length exactly _capacity, so slot(i) = i % _capacity is
//!     injective on it: two live indices can never share a ring slot, even though
//!     the ring is only `_capacity` bytes long.
//!
//! A pushed segment (data, index), with p = index and n = data.size(), covers
//! [p, p + n).  Defining
//!
//!   a = max(p, W)      // first admissible byte of the segment
//!   b = min(p + n, U)  // one past its last admissible byte
//!
//! the accepted slice is data[a - p, b - p).  If a < b, then
//!
//!   * [a, a + written) may go straight into _output (only when a == W and no
//!     buffered bytes are pending; `written` is what _output.write() accepted);
//!   * the remainder [a + written, b) is stored in the ring, with
//!     _received[slot(i)] = 1 for every i it covers.  Re-storing a slot that is
//!     already set is a no-op, so duplicates are counted only once.
//!
//! Bookkeeping symbols:
//!
//!   _unassembled_bytes  =  |{ i in [W, U) : _received[slot(i)] == 1 }|  (= |red|)
//!   empty()             <=>  _unassembled_bytes == 0
//!   ack_index()         =  W
//!   _eof_index          =  p + n of the segment pushed with eof == true
//!   input ends once     W >= _eof_index
//!
//! Invariants maintained after every public call (for _capacity > 0):
//!
//!   R <= W <= U,   U - R == _capacity,
//!   _received[slot(W)] == 0   unless   W == U
//!
//! The last clause says: whenever the byte at W is known and _output still has
//! room, it has already been written to _output -- nothing contiguous may linger
//! in the ring buffer.
// clang-format on

//! \brief A class that assembles a series of excerpts from a byte stream (possibly out of order,
//! possibly overlapping) into an in-order byte stream.
//!
//! \see The symbolic stream model at the top of this file.
class StreamReassembler {
  private:
    ByteStream _output;              //!< The reassembled in-order byte stream: owns [0, W), readable part [R, W)
    size_t _capacity;                //!< The maximum number of bytes: the width of the window [R, U)
    std::vector<char> _buffer;       //!< Ring buffer over [W, U) (size == capacity): slot(i) holds byte i
    std::vector<uint8_t> _received;  //!< Bitmap: _received[slot(i)] == 1 iff byte i is in the red region [W, U)
    size_t _unassembled_bytes;       //!< Number of received but not yet assembled bytes, i.e. |red|
    bool _eof;                       //!< Whether an EOF has been seen
    size_t _eof_index;               //!< Absolute index just past the last byte in the stream

    size_t first_unassembled() const;       // !< W: absolute index of the first unassembled byte in the stream
    size_t slot(const size_t index) const;  // !< Ring buffer slot of `index`; injective on [R, U)
    bool any_received(const size_t start, const size_t end)
        const;  // !< Returns true if any slot in [start, end) already holds received data
    void store_range(
        const size_t start,
        const size_t end,
        const char *
            src);  // !< Store the bytes of `src` in [start, end). Bytes that already arrived are skipped, so duplicates are counted once.
    void
    flush();  // !< Write every byte that is contiguous with the output stream, then remove those bytes from the buffer and bitmap. If the EOF has been seen and all bytes have been written to the output, end the input.

  public:
    //! \brief Construct a `StreamReassembler` that will store up to `capacity` bytes.
    //! \note This capacity limits both the bytes that have been reassembled,
    //! and those that have not yet been reassembled.
    StreamReassembler(const size_t capacity);

    //! \brief Receive a substring and write any newly contiguous bytes into the stream.
    //!
    //! The StreamReassembler will stay within the memory limits of the `capacity`.
    //! Bytes that would exceed the capacity are silently discarded.
    //!
    //! \param data the substring
    //! \param index indicates the index (place in sequence) of the first byte in `data`
    //! \param eof the last byte of `data` will be the last byte in the entire stream
    void push_substring(const std::string &data, const uint64_t index, const bool eof);

    //! \name Access the reassembled byte stream
    //!@{
    const ByteStream &stream_out() const { return _output; }
    ByteStream &stream_out() { return _output; }
    //!@}

    //! The number of bytes in the substrings stored but not yet reassembled
    //!
    //! \note If the byte at a particular index has been pushed more than once, it
    //! should only be counted once for the purpose of this function.
    size_t unassembled_bytes() const;

    //! \brief Is the internal state empty (other than the output stream)?
    //! \returns `true` if no substrings are waiting to be assembled
    bool empty() const;

    //! The acknowledge index of the stream, i.e., the index of the next interested substring
    size_t ack_index() const;
};

#endif  // SPONGE_LIBSPONGE_STREAM_REASSEMBLER_HH
