#ifndef SPONGE_LIBSPONGE_TCP_SENDER_HH
#define SPONGE_LIBSPONGE_TCP_SENDER_HH

#include "byte_stream.hh"
#include "tcp_config.hh"
#include "tcp_segment.hh"
#include "wrapping_integers.hh"

#include <deque>
#include <queue>

//! \brief The retransmission timer for a TCPSender.
//!
//! Tracks the amount of time elapsed since it was started, using only
//! the time reported by tick() (never the operating system clock, which
//! keeps the sender deterministic and testable). The timer "expires"
//! once the elapsed time reaches the current timeout. The TCPSender
//! uses it to decide when the earliest outstanding segment must be
//! retransmitted.
class RetransmissionTimer {
  public:
    //! \brief Construct a timer with the given initial timeout
    //! \param[in] timeout number of milliseconds to wait before expiring
    RetransmissionTimer(unsigned int timeout) : _timeout(timeout) {}

    //! \brief Restart the timer from zero, so that it expires after
    //! timeout milliseconds
    void reset() { _elapsed_time = 0; }

    //! \brief Advance the timer by the given number of milliseconds
    void tick(size_t ms_since_last_tick) { _elapsed_time += ms_since_last_tick; }

    //! \brief Whether the timer has reached its timeout and expired
    bool is_expired() const { return _elapsed_time >= _timeout; }

    //! \brief Change the timeout, e.g. to implement exponential backoff
    void set_timeout(unsigned int timeout) { _timeout = timeout; }

    //! \brief Whether the timer is currently running (i.e. has been started and not yet reset)
    bool is_running() const { return _running; }

    //! \brief Start the timer, if it is not already running
    void start() { _running = true; }

    //! \brief Stop the timer, if it is running
    void stop() { _running = false; }

  private:
    //! the current amount of time to wait before expiring, in milliseconds
    unsigned int _timeout;

    //! milliseconds elapsed since the last reset
    size_t _elapsed_time{0};

    //! Whether the timer is currently running (i.e. has been started and not yet reset)
    bool _running{false};
};

//! \brief The "sender" part of a TCP implementation.

//! Accepts a ByteStream, divides it up into segments and sends the
//! segments, keeps track of which segments are still in-flight,
//! maintains the Retransmission Timer, and retransmits in-flight
//! segments if the retransmission timer expires.
class TCPSender {
  private:
    //! our initial sequence number, the number for our SYN.
    WrappingInt32 _isn;

    //! outbound queue of segments that the TCPSender wants sent
    std::queue<TCPSegment> _segments_out{};

    //! retransmission timer for the connection
    unsigned int _initial_retransmission_timeout;

    //! outgoing stream of bytes that have not yet been sent
    ByteStream _stream;

    //! a segment that has been sent but not yet fully acknowledged
    struct OutstandingSegment {
        //! copy of the segment (its payload is a reference-counted Buffer)
        TCPSegment segment;

        //! absolute sequence number of the segment's first sequence number
        uint64_t abs_seqno;
    };

    //! the (absolute) sequence number for the next byte to be sent
    uint64_t _next_seqno{0};

    //! the outstanding segments that have been sent but not yet acknowledged
    std::deque<OutstandingSegment> _outstanding_segments;

    //! current retransmission timeout, which may be larger than the initial value due to exponential backoff
    unsigned int _current_retransmission_timeout{0};

    //! current number of consecutive retransmissions that have occurred in a row
    unsigned int _consecutive_retransmissions{0};

    //! total bytes in flight, i.e. the number of bytes that have been sent but not yet acknowledged
    size_t _bytes_in_flight{0};

    //! the retransmission timer for the connection
    RetransmissionTimer _retransmission_timer{_initial_retransmission_timeout};

    //! current window size, as advertised by the remote receiver
    uint16_t _window_size{1};

    //! whether the SYN flag has already been sent
    bool _syn_sent{false};

    //! whether the FIN flag has already been sent
    bool _fin_sent{false};

    //! the largest absolute ackno received so far
    uint64_t _last_ackno{0};

    //! push a segment onto the outbound queue and, if it occupies sequence
    //! space, track it as outstanding and start the retransmission timer
    void _send_segment(TCPSegment &seg);

  public:
    //! Initialize a TCPSender
    TCPSender(const size_t capacity = TCPConfig::DEFAULT_CAPACITY,
              const uint16_t retx_timeout = TCPConfig::TIMEOUT_DFLT,
              const std::optional<WrappingInt32> fixed_isn = {});

    //! \name "Input" interface for the writer
    //!@{
    ByteStream &stream_in() { return _stream; }
    const ByteStream &stream_in() const { return _stream; }
    //!@}

    //! \name Methods that can cause the TCPSender to send a segment
    //!@{

    //! \brief A new acknowledgment was received
    void ack_received(const WrappingInt32 ackno, const uint16_t window_size);

    //! \brief Generate an empty-payload segment (useful for creating empty ACK segments)
    void send_empty_segment();

    //! \brief create and send segments to fill as much of the window as possible
    void fill_window();

    //! \brief Notifies the TCPSender of the passage of time
    void tick(const size_t ms_since_last_tick);
    //!@}

    //! \name Accessors
    //!@{

    //! \brief How many sequence numbers are occupied by segments sent but not yet acknowledged?
    //! \note count is in "sequence space," i.e. SYN and FIN each count for one byte
    //! (see TCPSegment::length_in_sequence_space())
    size_t bytes_in_flight() const;

    //! \brief Number of consecutive retransmissions that have occurred in a row
    unsigned int consecutive_retransmissions() const;

    //! \brief TCPSegments that the TCPSender has enqueued for transmission.
    //! \note These must be dequeued and sent by the TCPConnection,
    //! which will need to fill in the fields that are set by the TCPReceiver
    //! (ackno and window size) before sending.
    std::queue<TCPSegment> &segments_out() { return _segments_out; }
    //!@}

    //! \name What is the next sequence number? (used for testing)
    //!@{

    //! \brief absolute seqno for the next byte to be sent
    uint64_t next_seqno_absolute() const { return _next_seqno; }

    //! \brief relative seqno for the next byte to be sent
    WrappingInt32 next_seqno() const { return wrap(_next_seqno, _isn); }
    //!@}
};

#endif  // SPONGE_LIBSPONGE_TCP_SENDER_HH
