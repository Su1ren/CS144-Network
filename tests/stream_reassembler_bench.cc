#include "stream_reassembler.hh"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Count every heap allocation made by the reassembler under test.
namespace {
size_t g_alloc_count = 0;
size_t g_alloc_bytes = 0;
}  // namespace

void *operator new(std::size_t size) {
    const std::size_t bytes = size == 0 ? 1 : size;
    ++g_alloc_count;
    g_alloc_bytes += bytes;
    void *const p = std::malloc(bytes);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void *operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace {

using Clock = std::chrono::steady_clock;

void reset_alloc_counters() {
    g_alloc_count = 0;
    g_alloc_bytes = 0;
}

size_t alloc_count() { return g_alloc_count; }
size_t alloc_bytes() { return g_alloc_bytes; }

template <typename Fn>
double time_micros(Fn &&fn) {
    const auto start = Clock::now();
    fn();
    const auto stop = Clock::now();
    return std::chrono::duration<double, std::micro>(stop - start).count();
}

// Baseline: the std::map<start, string>-based implementation used before the
// ring-buffer rewrite. It is kept here so both implementations can be measured
// in the same binary under identical workloads.
class MapStreamReassembler {
  public:
    explicit MapStreamReassembler(const size_t capacity)
        : _segments(), _unassembled_bytes(0), _output(capacity), _capacity(capacity), _eof(false), _eof_index(0) {}

    void push_substring(const std::string &data, const size_t index, const bool eof) {
        if (eof) {
            _eof = true;
            _eof_index = index + data.size();
        }

        const size_t fu = first_unassembled();
        size_t first = index;
        std::string new_data = data;
        if (index < fu) {
            const size_t overlap = std::min(fu - index, data.size());
            first += overlap;
            new_data = data.substr(overlap);
        }

        const size_t window_end = _output.bytes_read() + _capacity;
        if (first >= window_end) {
            new_data.clear();
        } else if (first + new_data.size() > window_end) {
            new_data.resize(window_end - first);
        }

        if (!new_data.empty()) {
            merge(first, std::move(new_data));
            flush();
        }

        if (_eof && first_unassembled() >= _eof_index) {
            _output.end_input();
        }
    }

    const ByteStream &stream_out() const { return _output; }
    ByteStream &stream_out() { return _output; }
    size_t unassembled_bytes() const { return _unassembled_bytes; }
    bool empty() const { return _unassembled_bytes == 0; }
    size_t ack_index() const { return first_unassembled(); }

  private:
    size_t first_unassembled() const { return _output.bytes_written(); }

    void merge(size_t first, std::string data) {
        auto it = _segments.lower_bound(first);
        if (it != _segments.begin()) {
            --it;
            if (it->first + it->second.size() <= first) {
                ++it;
            }
        }

        while (it != _segments.end() && it->first < first + data.size()) {
            const size_t seg_start = it->first;
            const size_t seg_end = it->first + it->second.size();
            const size_t new_end = first + data.size();

            if (seg_start <= first) {
                if (seg_end > new_end) {
                    data = it->second;
                } else {
                    data = it->second + data.substr(seg_end - first);
                }
                first = seg_start;
            } else if (seg_end > new_end) {
                data += it->second.substr(new_end - seg_start);
            }

            _unassembled_bytes -= it->second.size();
            it = _segments.erase(it);
        }

        _unassembled_bytes += data.size();
        _segments.emplace(first, std::move(data));
    }

    void flush() {
        size_t next_index = first_unassembled();
        auto it = _segments.begin();
        while (it != _segments.end() && it->first == next_index) {
            std::string data = it->second;
            const size_t seg_size = data.size();
            const size_t written = _output.write(data);
            next_index += written;
            _unassembled_bytes -= written;
            it = _segments.erase(it);

            if (written < seg_size) {
                _segments.emplace(next_index, data.substr(written));
                break;
            }
        }
    }

    std::map<size_t, std::string> _segments;
    size_t _unassembled_bytes;
    ByteStream _output;
    size_t _capacity;
    bool _eof;
    size_t _eof_index;
};

// Baseline: the ring-buffer implementation with a byte-per-slot bitmap. It is
// identical to the current implementation except for the bitmap representation,
// so the difference isolates the cost of the bitmap itself.
class ByteRingReassembler {
  public:
    explicit ByteRingReassembler(const size_t capacity)
        : _output(capacity)
        , _capacity(capacity)
        , _buffer(capacity)
        , _received(capacity, 0)
        , _unassembled_bytes(0)
        , _eof(false)
        , _eof_index(0) {}

    void push_substring(const std::string &data, const size_t index, const bool eof) {
        if (eof) {
            _eof = true;
            _eof_index = index + data.size();
        }

        if (_capacity > 0) {
            const size_t fu = first_unassembled();
            const size_t window_end = _output.bytes_read() + _capacity;
            const size_t start = std::max(index, fu);
            const size_t end = std::min(index + data.size(), window_end);

            if (start < end) {
                const size_t len = end - start;
                const char *src = data.data() + (start - index);

                size_t consumed = 0;
                if (start == fu && _unassembled_bytes == 0) {
                    consumed = _output.write(src, len);
                }
                const size_t store_start = start + consumed;
                if (store_start < end) {
                    store_range(store_start, end, src + (store_start - start));
                }
                flush();
            }
        }

        if (_eof && first_unassembled() >= _eof_index) {
            _output.end_input();
        }
    }

    const ByteStream &stream_out() const { return _output; }
    ByteStream &stream_out() { return _output; }
    size_t unassembled_bytes() const { return _unassembled_bytes; }
    bool empty() const { return _unassembled_bytes == 0; }
    size_t ack_index() const { return first_unassembled(); }

  private:
    size_t first_unassembled() const { return _output.bytes_written(); }
    size_t slot(const size_t index) const { return index % _capacity; }

    bool any_received(const size_t start, const size_t end) const {
        size_t pos = slot(start);
        size_t remaining = end - start;
        while (remaining > 0) {
            const size_t len = std::min(remaining, _capacity - pos);
            if (memchr(_received.data() + pos, 1, len) != nullptr) {
                return true;
            }
            remaining -= len;
            pos = 0;
        }
        return false;
    }

    void store_range(const size_t start, const size_t end, const char *src) {
        const size_t len = end - start;

        if (!any_received(start, end)) {
            const size_t pos = slot(start);
            const size_t first_len = std::min(len, _capacity - pos);
            memcpy(_buffer.data() + pos, src, first_len);
            memset(_received.data() + pos, 1, first_len);
            if (first_len < len) {
                memcpy(_buffer.data(), src + first_len, len - first_len);
                memset(_received.data(), 1, len - first_len);
            }
            _unassembled_bytes += len;
            return;
        }

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

    void flush() {
        size_t fu = first_unassembled();
        const size_t window_end = _output.bytes_read() + _capacity;

        while (fu < window_end) {
            const size_t pos = slot(fu);
            if (_received[pos] == 0) {
                break;
            }

            const size_t max_len = std::min(window_end - fu, _capacity - pos);
            const uint8_t *const base = _received.data() + pos;
            const void *const zero = memchr(base, 0, max_len);
            const size_t run_len =
                zero != nullptr ? static_cast<size_t>(static_cast<const uint8_t *>(zero) - base) : max_len;

            const size_t written = _output.write(_buffer.data() + pos, run_len);
            memset(_received.data() + pos, 0, written);
            _unassembled_bytes -= written;
            fu += written;

            if (written < run_len) {
                break;
            }
        }
    }

    ByteStream _output;
    size_t _capacity;
    std::vector<char> _buffer;
    std::vector<uint8_t> _received;
    size_t _unassembled_bytes;
    bool _eof;
    size_t _eof_index;
};

enum class Workload { sequential, shuffled, overlap, retransmit, reverse, streaming };

const char *workload_name(const Workload w) {
    switch (w) {
        case Workload::sequential:
            return "sequential";
        case Workload::shuffled:
            return "shuffled";
        case Workload::overlap:
            return "overlap";
        case Workload::retransmit:
            return "retransmit";
        case Workload::reverse:
            return "reverse";
        case Workload::streaming:
            return "streaming";
    }
    return "?";
}

Workload parse_workload(const std::string &name) {
    if (name == "sequential") {
        return Workload::sequential;
    }
    if (name == "shuffled") {
        return Workload::shuffled;
    }
    if (name == "overlap") {
        return Workload::overlap;
    }
    if (name == "retransmit") {
        return Workload::retransmit;
    }
    if (name == "reverse") {
        return Workload::reverse;
    }
    if (name == "streaming") {
        return Workload::streaming;
    }
    throw std::runtime_error("unknown workload: " + name);
}

struct Segment {
    size_t index;
    std::string data;
};

std::string make_pattern(const size_t n) {
    std::string pattern(n, 0);
    for (size_t i = 0; i < n; ++i) {
        pattern[i] = static_cast<char>('a' + ((i * 131 + 7) % 26));
    }
    return pattern;
}

std::vector<Segment> make_segments(const Workload w, const std::string &pattern, const size_t chunk) {
    std::vector<Segment> segments;
    const size_t total = pattern.size();

    if (w == Workload::overlap) {
        // Each segment also covers half a chunk of the previous one, like a
        // retransmission with overlap.
        const size_t back = chunk / 2;
        for (size_t off = 0; off < total; off += chunk) {
            const size_t len = std::min(chunk, total - off);
            const size_t start = off >= back ? off - back : 0;
            segments.push_back({start, pattern.substr(start, off + len - start)});
        }
    } else {
        for (size_t off = 0; off < total; off += chunk) {
            const size_t len = std::min(chunk, total - off);
            segments.push_back({off, pattern.substr(off, len)});
        }
    }
    return segments;
}

template <typename Reassembler>
void push_all(Reassembler &r, const std::vector<Segment> &segments, const Workload w, const size_t total) {
    auto push = [&](const Segment &s) { r.push_substring(s.data, s.index, s.index + s.data.size() == total); };

    if (w == Workload::reverse) {
        for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
            push(*it);
        }
    } else if (w == Workload::retransmit) {
        for (const auto &s : segments) {
            push(s);
        }
        for (const auto &s : segments) {
            push(s);  // duplicate everything: retransmission
        }
    } else {
        for (const auto &s : segments) {
            push(s);
        }
    }
}

struct Measurement {
    double us;
    size_t allocs;
    size_t alloc_bytes;
};

// Steady-state streaming: a small window (capacity) is drained while data keeps
// arriving, which is what a real TCP connection looks like. Construction is
// amortized over the whole stream, so profiler counts reflect the push/read
// loop rather than the initial buffer allocation.
template <typename Reassembler>
Measurement run_streaming(const std::vector<Segment> &segments,
                          const std::string &pattern,
                          const size_t reps,
                          const size_t capacity) {
    Measurement best{1e100, 0, 0};
    for (size_t rep = 0; rep < reps; ++rep) {
        Reassembler r(capacity);

        reset_alloc_counters();
        size_t received = 0;
        const double us = time_micros([&] {
            for (const auto &s : segments) {
                r.push_substring(s.data, s.index, s.index + s.data.size() == pattern.size());
                if (r.stream_out().buffer_size() + s.data.size() >= capacity) {
                    received += r.stream_out().buffer_size();
                    r.stream_out().pop_output(r.stream_out().buffer_size());
                }
            }
            received += r.stream_out().buffer_size();
            r.stream_out().pop_output(r.stream_out().buffer_size());
        });
        if (us < best.us) {
            best = {us, alloc_count(), alloc_bytes()};
        }

        if (received != pattern.size() || r.stream_out().bytes_written() != pattern.size() ||
            r.unassembled_bytes() != 0) {
            throw std::runtime_error("streaming benchmark accounting mismatch");
        }
    }
    return best;
}

template <typename Reassembler>
Measurement run_once(const Workload w,
                     const std::vector<Segment> &segments,
                     const std::string &pattern,
                     const size_t reps) {
    Measurement best{1e100, 0, 0};
    for (size_t rep = 0; rep < reps; ++rep) {
        Reassembler r(pattern.size());  // capacity == stream length: nothing is dropped

        reset_alloc_counters();
        const double us = time_micros([&] { push_all(r, segments, w, pattern.size()); });
        const size_t allocs = alloc_count();
        const size_t alloc_bytes_used = alloc_bytes();
        if (us < best.us) {
            best = {us, allocs, alloc_bytes_used};
        }

        // Verify correctness outside the timed region.
        const std::string out = r.stream_out().read(r.stream_out().buffer_size());
        if (out.size() != pattern.size() || out != pattern) {
            throw std::runtime_error("reassembled data does not match the input");
        }
        if (r.unassembled_bytes() != 0 || r.stream_out().bytes_written() != pattern.size()) {
            throw std::runtime_error("reassembler accounting mismatch");
        }
    }
    return best;
}

}  // namespace

int main(int argc, char **argv) {
    try {
        size_t total = 256 * 1024;
        size_t reps = 3;
        if (argc > 1) {
            total = std::stoull(argv[1]);
        }
        if (argc > 2) {
            reps = std::stoull(argv[2]);
        }
        const size_t reverse_total = std::min<size_t>(total, 64 * 1024);

        if (argc >= 5) {
            // Single-config mode, used for perf / cachegrind runs:
            //   stream_reassembler_bench <total> <reps> <workload> <chunk> [map|byte|bits] [capacity]
            // `streaming` uses a small flow-control window and drains it as data
            // arrives, so profiler counts reflect the steady state.
            const Workload w = parse_workload(argv[3]);
            const size_t chunk = std::stoull(argv[4]);
            const std::string impl = argc > 5 ? argv[5] : "bits";
            const size_t data_total = (w == Workload::reverse) ? reverse_total : total;
            const size_t capacity = (w == Workload::streaming)
                                        ? (argc > 6 ? std::stoull(argv[6]) : std::min<size_t>(64 * 1024, data_total))
                                        : data_total;
            const std::string pattern = make_pattern(data_total);
            auto segments = make_segments(w, pattern, chunk);
            if (w == Workload::shuffled || w == Workload::retransmit) {
                std::mt19937 gen(20241024);
                std::shuffle(segments.begin(), segments.end(), gen);
            }

            Measurement m{0, 0, 0};
            if (w == Workload::streaming) {
                if (impl == "map") {
                    m = run_streaming<MapStreamReassembler>(segments, pattern, reps, capacity);
                } else if (impl == "byte") {
                    m = run_streaming<ByteRingReassembler>(segments, pattern, reps, capacity);
                } else if (impl == "bits") {
                    m = run_streaming<StreamReassembler>(segments, pattern, reps, capacity);
                } else {
                    throw std::runtime_error("unknown impl: " + impl);
                }
            } else if (impl == "map") {
                m = run_once<MapStreamReassembler>(w, segments, pattern, reps);
            } else if (impl == "byte") {
                m = run_once<ByteRingReassembler>(w, segments, pattern, reps);
            } else if (impl == "bits") {
                m = run_once<StreamReassembler>(w, segments, pattern, reps);
            } else {
                throw std::runtime_error("unknown impl: " + impl);
            }

            std::cout << "workload=" << workload_name(w) << " chunk=" << chunk << " impl=" << impl
                      << " total=" << data_total << " capacity=" << capacity << " reps=" << reps
                      << " ns_per_byte=" << std::fixed << std::setprecision(3)
                      << m.us * 1000.0 / static_cast<double>(data_total) << " allocs_per_run=" << m.allocs
                      << " alloc_bytes_per_run=" << m.alloc_bytes << "\n";
            return EXIT_SUCCESS;
        }

        std::cout << "# StreamReassembler benchmark (sequential/shuffled/overlap/retransmit: total = " << total
                  << " B; reverse: total = " << reverse_total << " B; min of " << reps << " runs)\n";
        std::cout << "# bitmap memory at capacity C: byte = C B, bitset = ceil(C/64)*8 B (8x smaller)\n\n";
        std::cout << "| workload | chunk | map ns/B | ring(byte) ns/B | ring(bitset) ns/B | bitset vs byte |\n";
        std::cout << "|---|---:|---:|---:|---:|---:|\n";

        for (const Workload w :
             {Workload::sequential, Workload::shuffled, Workload::overlap, Workload::retransmit, Workload::reverse}) {
            const size_t data_total = (w == Workload::reverse) ? reverse_total : total;
            const std::string pattern = make_pattern(data_total);

            for (const size_t chunk :
                 {size_t{64}, size_t{256}, size_t{1024}, size_t{4096}, size_t{16384}, size_t{65536}}) {
                if (chunk > data_total) {
                    continue;
                }
                auto segments = make_segments(w, pattern, chunk);
                if (w == Workload::shuffled || w == Workload::retransmit) {
                    std::mt19937 gen(20241024);
                    std::shuffle(segments.begin(), segments.end(), gen);
                }

                const Measurement map_m = run_once<MapStreamReassembler>(w, segments, pattern, reps);
                const Measurement byte_m = run_once<ByteRingReassembler>(w, segments, pattern, reps);
                const Measurement bits_m = run_once<StreamReassembler>(w, segments, pattern, reps);
                const double map_ns = map_m.us * 1000.0 / static_cast<double>(data_total);
                const double byte_ns = byte_m.us * 1000.0 / static_cast<double>(data_total);
                const double bits_ns = bits_m.us * 1000.0 / static_cast<double>(data_total);

                std::cout << "| " << workload_name(w) << " | " << chunk << " B | " << std::fixed << std::setprecision(3)
                          << map_ns << " | " << byte_ns << " | " << bits_ns << " | " << std::setprecision(2)
                          << byte_m.us / bits_m.us << "x |\n";
            }
        }

        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}
