//! Microbenchmark co two ARP-cache layouts under identical workloads:
//!
//!   A = std::unordered_map<ip, {mac, expires_at}>  (the implementation used
//!       in this lab: O(1) lookup, tick() scans the whole table, O(n))
//!   B = std::list ordered by expiry time + std::unordered_map<ip, iterator>
//!       (O(1) lookup, refresh splices to the tail, tick() pops from the
//!       front, O(k) amortized where k = entries actually expiring)
//!
//! Both caches use the same fixed 30 s TTL and a deterministic integer clock,
//! so the only difference under test is the data-structure cost.  Correctness
//! is checked outside the timed region: the two caches must agree on membership
//! and learned Ethernet addresses after an identical event sequence.
//!
//! Usage:
//!   arp_cache_bench [n] [reps] [ticks]
//! Defaults are small enough for a Debug `ctest` run; pass larger values in a
//! Release build for the numbers recorded in writeups/lab5.md.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <list>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>

// Count every heap allocation made by the cache under test (same pattern as
// the other lab benchmarks).
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
using EthernetAddress = std::array<uint8_t, 6>;

constexpr size_t ARP_MAPPING_TTL_MS = 30 * 1000;

// Consumes the loop results so the optimizer cannot delete the measured work.
volatile size_t g_sink = 0;

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

uint32_t ip_at(const size_t index) {
    return 0xC0A80000U + static_cast<uint32_t>(index) + 1U;  // 192.168.0.1, .2, ...
}

EthernetAddress mac_at(const size_t index) {
    return {0x02, 0x00, 0x00, 0x00, static_cast<uint8_t>((index >> 8) & 0xff), static_cast<uint8_t>(index & 0xff)};
}

//! Plan A (used by NetworkInterface): absolute expiry inside the value,
//! tick() scans the whole map.  Lookup/learn/refresh O(1).
class ArpCacheScan {
  public:
    void learn(const uint32_t ip, const EthernetAddress &mac, const size_t now) {
        _entries[ip] = Entry{mac, now + ARP_MAPPING_TTL_MS};
    }

    bool lookup(const uint32_t ip, EthernetAddress &mac) const {
        const auto it = _entries.find(ip);
        if (it == _entries.end()) {
            return false;
        }
        mac = it->second.mac;
        return true;
    }

    size_t tick(const size_t now) {
        size_t expired = 0;
        for (auto it = _entries.begin(); it != _entries.end();) {
            if (it->second.expires_at <= now) {
                it = _entries.erase(it);
                ++expired;
            } else {
                ++it;
            }
        }
        return expired;
    }

    size_t size() const { return _entries.size(); }

  private:
    struct Entry {
        EthernetAddress mac{};
        size_t expires_at{};
    };

    std::unordered_map<uint32_t, Entry> _entries{};
};

//! Plan B (comparison baseline): the TTL is fixed, so the order in which
//! mappings were last learned is also the order in which they expire.  A list
//! kept in that order plus an index to its nodes gives O(1) learn/refresh
//! (splice, no allocation) and O(k) amortized expiry.
class ArpCacheLru {
  public:
    void learn(const uint32_t ip, const EthernetAddress &mac, const size_t now) {
        const auto it = _index.find(ip);
        if (it != _index.end()) {
            const auto node = it->second;
            _lru.splice(_lru.end(), _lru, node);
            node->mac = mac;
            node->expires_at = now + ARP_MAPPING_TTL_MS;
            return;
        }
        _lru.push_back(Node{ip, mac, now + ARP_MAPPING_TTL_MS});
        _index.emplace(ip, std::prev(_lru.end()));
    }

    bool lookup(const uint32_t ip, EthernetAddress &mac) const {
        const auto it = _index.find(ip);
        if (it == _index.end()) {
            return false;
        }
        mac = it->second->mac;
        return true;
    }

    size_t tick(const size_t now) {
        size_t expired = 0;
        while (not _lru.empty() && _lru.front().expires_at <= now) {
            _index.erase(_lru.front().ip);
            _lru.pop_front();
            ++expired;
        }
        return expired;
    }

    size_t size() const { return _lru.size(); }

  private:
    struct Node {
        uint32_t ip{};
        EthernetAddress mac{};
        size_t expires_at{};
    };

    std::list<Node> _lru{};
    std::unordered_map<uint32_t, std::list<Node>::iterator> _index{};
};

struct Measurement {
    double best_us{};
    size_t ops{};
    size_t allocs{};
    size_t alloc_bytes{};
};

//! n stable mappings, then `ticks` calls to tick(); no entry may expire.
template <typename Cache>
Measurement run_tick_idle(const size_t n, const size_t reps, const size_t ticks) {
    const size_t tick_count = std::min(ticks, ARP_MAPPING_TTL_MS - 4 * 1000);
    Measurement best{};

    for (size_t rep = 0; rep < reps; ++rep) {
        Cache cache;
        const size_t now = 1000;
        for (size_t i = 0; i < n; ++i) {
            cache.learn(ip_at(i), mac_at(i), now);
        }
        if (cache.size() != n) {
            throw std::runtime_error("tick-idle setup failed");
        }

        reset_alloc_counters();
        const double us = time_micros([&] {
            for (size_t t = 0; t < tick_count; ++t) {
                g_sink += cache.tick(now + 1 + t);
            }
        });
        const size_t allocs = alloc_count();
        const size_t bytes = alloc_bytes();

        if (cache.size() != n) {
            throw std::runtime_error("tick-idle run dropped live entries");
        }
        if (rep == 0 or us < best.best_us) {
            best = Measurement{us, tick_count, allocs, bytes};
        }
    }
    return best;
}

//! n mappings all expiring at once; one tick() has to remove all of them.
//! Report ns per expired entry so the two layouts can be compared directly.
template <typename Cache>
Measurement run_expiry_burst(const size_t n, const size_t reps) {
    Measurement best{};

    for (size_t rep = 0; rep < reps; ++rep) {
        Cache cache;
        const size_t now = 1000;
        for (size_t i = 0; i < n; ++i) {
            cache.learn(ip_at(i), mac_at(i), now);
        }

        reset_alloc_counters();
        const double us = time_micros([&] { g_sink += cache.tick(now + ARP_MAPPING_TTL_MS); });
        const size_t allocs = alloc_count();
        const size_t bytes = alloc_bytes();

        if (cache.size() != 0) {
            throw std::runtime_error("expiry-burst did not remove every entry");
        }
        if (rep == 0 or us < best.best_us) {
            best = Measurement{us, n, allocs, bytes};
        }
    }
    return best;
}

//! Refresh every mapping repeatedly: the cache stays full, nothing expires.
template <typename Cache>
Measurement run_learn_churn(const size_t n, const size_t reps, const size_t rounds) {
    Measurement best{};

    for (size_t rep = 0; rep < reps; ++rep) {
        Cache cache;
        size_t now = 1000;
        for (size_t i = 0; i < n; ++i) {
            cache.learn(ip_at(i), mac_at(i), now);
        }

        reset_alloc_counters();
        const double us = time_micros([&] {
            for (size_t r = 0; r < rounds; ++r) {
                now += 10;
                for (size_t i = 0; i < n; ++i) {
                    cache.learn(ip_at(i), mac_at(i), now);
                }
            }
        });
        const size_t allocs = alloc_count();
        const size_t bytes = alloc_bytes();

        if (cache.size() != n) {
            throw std::runtime_error("learn-churn changed the cache size");
        }
        if (rep == 0 or us < best.best_us) {
            best = Measurement{us, rounds * n, allocs, bytes};
        }
    }
    return best;
}

//! Lab-like steady state: up to 8 neighbours, one tick per simulated
//! millisecond, one lookup per neighbour per tick, and every mapping refreshed
//! every 10 s (as real traffic confirmation would do).
template <typename Cache>
Measurement run_realistic(const size_t n, const size_t reps, const size_t ticks) {
    const size_t neighbors = std::min<size_t>(n, 8);
    Measurement best{};

    for (size_t rep = 0; rep < reps; ++rep) {
        Cache cache;
        size_t now = 0;
        for (size_t i = 0; i < neighbors; ++i) {
            cache.learn(ip_at(i), mac_at(i), now);
        }

        reset_alloc_counters();
        const double us = time_micros([&] {
            for (size_t t = 0; t < ticks; ++t) {
                now += 1;
                if (now % 10'000 == 0) {
                    for (size_t i = 0; i < neighbors; ++i) {
                        cache.learn(ip_at(i), mac_at(i), now);
                    }
                }
                for (size_t i = 0; i < neighbors; ++i) {
                    EthernetAddress mac{};
                    g_sink += (cache.lookup(ip_at(i), mac) ? 1U : 0U);
                }
                g_sink += cache.tick(now);
            }
        });
        const size_t allocs = alloc_count();
        const size_t bytes = alloc_bytes();

        if (cache.size() != neighbors) {
            throw std::runtime_error("realistic run lost live mappings");
        }
        if (rep == 0 or us < best.best_us) {
            best = Measurement{us, ticks, allocs, bytes};
        }
    }
    return best;
}

//! Deterministic event sequence; both caches must stay byte-for-byte agreeing.
void verify_equivalence(const size_t n) {
    ArpCacheScan a;
    ArpCacheLru b;
    size_t now = 0;

    for (size_t step = 0; step < 200; ++step) {
        now += 1000;  // 1 s per step
        for (size_t i = 0; i < n; ++i) {
            // Three quarters of the entries are refreshed every 7 s; the rest
            // are left to expire so the 30 s TTL is exercised.
            if ((i % 4 != 3) and ((step + i) % 7 == 0)) {
                a.learn(ip_at(i), mac_at(i), now);
                b.learn(ip_at(i), mac_at(i), now);
            }
        }

        const size_t expired_a = a.tick(now);
        const size_t expired_b = b.tick(now);
        if (expired_a != expired_b or a.size() != b.size()) {
            throw std::runtime_error("A/B caches diverged (size or expiry count)");
        }

        for (size_t i = 0; i < n; ++i) {
            EthernetAddress mac_a{};
            EthernetAddress mac_b{};
            const bool found_a = a.lookup(ip_at(i), mac_a);
            const bool found_b = b.lookup(ip_at(i), mac_b);
            if (found_a != found_b or (found_a and not(mac_a == mac_b))) {
                throw std::runtime_error("A/B caches diverged (lookup)");
            }
        }
    }
}

void print_row(const std::string &workload, const char impl, const Measurement &m) {
    std::cout << std::left << std::setw(26) << workload << std::setw(3) << impl << std::right << std::fixed
              << std::setprecision(1) << std::setw(12) << (m.best_us * 1000.0 / static_cast<double>(m.ops))
              << std::setw(12) << m.ops << std::setw(12) << m.allocs << std::setw(12) << m.alloc_bytes << "\n";
}

}  // namespace

int main(int argc, char **argv) {
    try {
        size_t n = 64;
        size_t reps = 2;
        size_t ticks = 2000;
        if (argc > 1) {
            n = std::stoull(argv[1]);
        }
        if (argc > 2) {
            reps = std::stoull(argv[2]);
        }
        if (argc > 3) {
            ticks = std::stoull(argv[3]);
        }
        if (n == 0 or reps == 0) {
            throw std::runtime_error("n and reps must be positive");
        }

        verify_equivalence(n);
        std::cout << "A/B equivalence check: OK (200 s of events, " << n << " addresses)\n";

        std::cout << "ARP cache microbenchmark: A = unordered_map + full scan, "
                     "B = expiry-ordered list + index\n";
        std::cout << "n=" << n << " reps=" << reps << " ticks=" << ticks << " TTL=" << ARP_MAPPING_TTL_MS
                  << " ms; times are min of " << reps << " runs\n";
        std::cout << std::left << std::setw(26) << "workload" << std::setw(3) << "impl" << std::right << std::setw(12)
                  << "ns/op" << std::setw(12) << "ops" << std::setw(12) << "allocs" << std::setw(12) << "bytes" << "\n";

        const std::string tag = "/" + std::to_string(n);
        print_row("tick-idle" + tag, 'A', run_tick_idle<ArpCacheScan>(n, reps, ticks));
        print_row("tick-idle" + tag, 'B', run_tick_idle<ArpCacheLru>(n, reps, ticks));

        print_row("expiry-burst" + tag, 'A', run_expiry_burst<ArpCacheScan>(n, reps));
        print_row("expiry-burst" + tag, 'B', run_expiry_burst<ArpCacheLru>(n, reps));

        const size_t rounds = std::min<size_t>(ticks, 200'000);
        print_row("learn-churn" + tag, 'A', run_learn_churn<ArpCacheScan>(n, reps, rounds));
        print_row("learn-churn" + tag, 'B', run_learn_churn<ArpCacheLru>(n, reps, rounds));

        print_row("realistic(<=8 peers)", 'A', run_realistic<ArpCacheScan>(n, reps, ticks));
        print_row("realistic(<=8 peers)", 'B', run_realistic<ArpCacheLru>(n, reps, ticks));

        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
