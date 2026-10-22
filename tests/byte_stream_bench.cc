#include "byte_stream.hh"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Consumes the loop results so the optimizer cannot delete the measured work.
volatile size_t g_sink = 0;

// Baseline: the std::deque<char>-based implementation this lab started from.
// It is kept here so both implementations can be measured in one binary.
class DequeByteStream {
  public:
    explicit DequeByteStream(const size_t capacity)
        : _capacity(capacity), _buffer(), _total_written(0), _total_read(0) {}

    size_t write(const std::string &data) {
        const size_t n = std::min(data.size(), remaining_capacity());
        _buffer.insert(_buffer.end(), data.begin(), data.begin() + n);
        _total_written += n;
        return n;
    }

    std::string peek_output(const size_t len) const {
        const size_t n = std::min(len, _buffer.size());
        return std::string(_buffer.begin(), _buffer.begin() + n);
    }

    void pop_output(const size_t len) {
        const size_t n = std::min(len, _buffer.size());
        _buffer.erase(_buffer.begin(), _buffer.begin() + n);
        _total_read += n;
    }

    std::string read(const size_t len) {
        std::string result = peek_output(len);
        pop_output(len);
        return result;
    }

    size_t remaining_capacity() const { return _capacity - _buffer.size(); }
    size_t buffer_size() const { return _buffer.size(); }
    size_t bytes_written() const { return _total_written; }
    size_t bytes_read() const { return _total_read; }

  private:
    size_t _capacity;
    std::deque<char> _buffer;
    size_t _total_written{0};
    size_t _total_read{0};
};

using Clock = std::chrono::steady_clock;

template <typename Fn>
double time_micros(Fn &&fn) {
    const auto start = Clock::now();
    fn();
    const auto stop = Clock::now();
    return std::chrono::duration<double, std::micro>(stop - start).count();
}

enum class Op { write, peek, pop, stream };

const char *op_name(const Op op) {
    switch (op) {
        case Op::write:
            return "write";
        case Op::peek:
            return "peek";
        case Op::pop:
            return "pop";
        case Op::stream:
            return "read+write";
    }
    return "?";
}

// Each measurement moves `total` bytes through the stream in `chunk`-sized calls.
template <typename Stream>
double run_once(const Op op, const size_t chunk, const size_t total, const size_t capacity) {
    const size_t steps = total / chunk;
    const std::string payload(chunk, 'x');

    switch (op) {
        case Op::write: {
            // Capacity >= total so the ring never wraps and no write is clamped.
            Stream stream(total + chunk);
            return time_micros([&] {
                size_t written = 0;
                for (size_t i = 0; i < steps; ++i) {
                    written += stream.write(payload);
                }
                g_sink += written;
                if (written != total) {
                    throw std::runtime_error("write benchmark lost bytes");
                }
            });
        }
        case Op::peek: {
            Stream stream(total + chunk);
            stream.write(std::string(total, 'x'));
            return time_micros([&] {
                size_t peeked = 0;
                for (size_t i = 0; i < steps; ++i) {
                    const std::string out = stream.peek_output(chunk);
                    peeked += out.size();
                    g_sink += static_cast<unsigned char>(out.front());
                }
                if (peeked != total) {
                    throw std::runtime_error("peek benchmark lost bytes");
                }
            });
        }
        case Op::pop: {
            Stream stream(total + chunk);
            stream.write(std::string(total, 'x'));
            return time_micros([&] {
                for (size_t i = 0; i < steps; ++i) {
                    stream.pop_output(chunk);
                }
                g_sink += stream.buffer_size();
                if (stream.buffer_size() != 0) {
                    throw std::runtime_error("pop benchmark left bytes behind");
                }
            });
        }
        case Op::stream: {
            // Steady state: the buffer starts full, then every iteration reads and
            // writes the same amount, so `total` bytes are both read and written.
            Stream stream(capacity);
            stream.write(std::string(capacity, 'x'));
            return time_micros([&] {
                size_t consumed = 0;
                while (consumed < total) {
                    const std::string out = stream.read(chunk);
                    consumed += out.size();
                    const size_t written = stream.write(payload);
                    if (written != out.size()) {
                        throw std::runtime_error("stream benchmark lost synchronization");
                    }
                }
                g_sink += consumed;
            });
        }
    }

    throw std::runtime_error("unknown benchmark operation");
}

template <typename Stream>
double best_micros(const Op op, const size_t chunk, const size_t total, const size_t capacity, const size_t reps) {
    double best = 1e100;
    for (size_t r = 0; r < reps; ++r) {
        best = std::min(best, run_once<Stream>(op, chunk, total, capacity));
    }
    return best;
}

std::string make_pattern(const size_t n) {
    std::string pattern(n, 0);
    for (size_t i = 0; i < n; ++i) {
        pattern[i] = static_cast<char>('a' + (i % 26));
    }
    return pattern;
}

// Both implementations must agree on semantics, including wrap-around behavior.
template <typename Stream>
void verify(const char *name) {
    const std::string pattern = make_pattern(1300);
    Stream stream(1000);

    const auto expect = [&](const bool condition, const char *what) {
        if (!condition) {
            throw std::runtime_error(std::string(name) + " failed: " + what);
        }
    };

    expect(stream.write(pattern.substr(0, 1000)) == 1000, "write into an empty buffer");
    expect(stream.write(pattern.substr(1000, 300)) == 0, "write past capacity must be rejected");
    expect(stream.peek_output(1000) == pattern.substr(0, 1000), "peek before the wrap");
    expect(stream.read(300) == pattern.substr(0, 300), "read before the wrap");
    expect(stream.buffer_size() == 700, "buffer size after the read");
    expect(stream.write(pattern.substr(1000, 500)) == 300, "write must be clamped at capacity");
    expect(stream.peek_output(1000) == pattern.substr(300, 1000), "peek across the wrap");
    expect(stream.read(1000) == pattern.substr(300, 1000), "read across the wrap");
    expect(stream.buffer_size() == 0, "buffer must be empty at the end");
    expect(stream.bytes_written() == 1300, "bytes-written accounting");
    expect(stream.bytes_read() == 1300, "bytes-read accounting");
}

}  // namespace

int main(int argc, char **argv) {
    try {
        size_t total = 512 * 1024;
        size_t reps = 3;
        size_t capacity = 64 * 1024;
        if (argc > 1) {
            total = std::stoull(argv[1]);
        }
        if (argc > 2) {
            reps = std::stoull(argv[2]);
        }
        if (argc > 3) {
            capacity = std::stoull(argv[3]);
        }

        verify<DequeByteStream>("deque baseline");
        verify<ByteStream>("vector ring buffer");

        std::vector<size_t> chunks;
        for (const size_t chunk : {size_t{64}, size_t{256}, size_t{1024}, size_t{4096}, size_t{16384}, size_t{65536}}) {
            if (chunk <= std::min(total, capacity)) {
                chunks.push_back(chunk);
            }
        }

        std::cout << "# ByteStream benchmark (total = " << total << " B per measurement, min of " << reps
                  << " runs, streaming capacity = " << capacity << " B)\n\n";
        std::cout << "| chunk | op | deque ns/B | vector ns/B | speedup |\n";
        std::cout << "|---:|---|---:|---:|---:|\n";

        for (const size_t chunk : chunks) {
            for (const Op op : {Op::write, Op::peek, Op::pop, Op::stream}) {
                const double deque_us = best_micros<DequeByteStream>(op, chunk, total, capacity, reps);
                const double vector_us = best_micros<ByteStream>(op, chunk, total, capacity, reps);
                const double deque_ns = deque_us * 1000.0 / static_cast<double>(total);
                const double vector_ns = vector_us * 1000.0 / static_cast<double>(total);
                std::cout << "| " << chunk << " B | " << op_name(op) << " | " << std::fixed << std::setprecision(3)
                          << deque_ns << " | " << vector_ns << " | " << std::setprecision(2) << deque_us / vector_us
                          << "x |\n";
            }
        }

        std::cout << "\n# Checksum: " << g_sink << "\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}
