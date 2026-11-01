// Benchmark and cross-check for the three longest-prefix-match routing tables.
//
// The lab only ships a nine-route simulated network, where a linear scan is
// perfectly adequate. This program builds much larger synthetic tables, verifies
// that VectorRoutingTable, TrieRoutingTable and HashRoutingTable return exactly
// the same answer for every query, and then compares their lookup cost.
//
// Usage:
//   ./apps/router_benchmark                 # 1k / 10k / 100k prefixes
//   ./apps/router_benchmark 1000 50000      # explicit prefix counts

#include "router.hh"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

using namespace std;
using namespace std::chrono;

namespace {

struct PrefixSpec {
    uint32_t prefix{};
    uint8_t length{};
};

//! Build a synthetic routing table with a realistic prefix-length mix:
//! a default route, a few short prefixes, many /17-/24 and some /25-/32.
vector<PrefixSpec> generate_prefixes(size_t count, mt19937 &rng) {
    vector<PrefixSpec> prefixes;
    prefixes.reserve(count + 1);
    prefixes.push_back({0, 0});  // default route 0.0.0.0/0

    uniform_int_distribution<int> class_roll(0, 99);
    uniform_int_distribution<uint32_t> any_address(0, 0xFFFFFFFFu);
    unordered_set<uint64_t> seen{0};

    while (prefixes.size() < count + 1) {
        uint8_t length = 0;
        const int roll = class_roll(rng);
        if (roll < 10) {
            length = static_cast<uint8_t>(8 + rng() % 9);  // /8  - /16
        } else if (roll < 65) {
            length = static_cast<uint8_t>(17 + rng() % 8);  // /17 - /24
        } else {
            length = static_cast<uint8_t>(25 + rng() % 8);  // /25 - /32
        }

        const uint32_t prefix = any_address(rng) & routing_detail::prefix_mask(length);
        const uint64_t key = (static_cast<uint64_t>(length) << 32) | prefix;
        if (seen.insert(key).second) {
            prefixes.push_back({prefix, length});
        }
    }
    return prefixes;
}

//! Half of the queries are guaranteed hits inside a known prefix, half are random.
vector<uint32_t> generate_queries(const vector<PrefixSpec> &prefixes, size_t count, mt19937 &rng) {
    vector<uint32_t> queries;
    queries.reserve(count);
    uniform_int_distribution<uint32_t> any_address(0, 0xFFFFFFFFu);
    uniform_int_distribution<size_t> pick(0, prefixes.size() - 1);

    for (size_t i = 0; i < count; ++i) {
        if ((i % 2) == 0) {
            const PrefixSpec &spec = prefixes[pick(rng)];
            const uint32_t host_mask = ~routing_detail::prefix_mask(spec.length);
            queries.push_back(spec.prefix | (any_address(rng) & host_mask));
        } else {
            queries.push_back(any_address(rng));
        }
    }
    return queries;
}

//! A compact fingerprint of a lookup result, used both for the equivalence
//! check and to keep the timed loop's work from being optimized away.
uint64_t fingerprint(const optional<RouteEntry> &result) {
    if (not result.has_value()) {
        return 0xFFFFFFFFFFFFFFFFull;
    }
    return (static_cast<uint64_t>(result->prefix_length) << 40) ^ (static_cast<uint64_t>(result->route_prefix) << 8) ^
           static_cast<uint64_t>(result->interface_num);
}

struct BuiltTable {
    string name;
    string detail;
    unique_ptr<RoutingTable> table;
    double build_ms{};
};

template <typename TableType>
BuiltTable build_table(const string &name, const vector<PrefixSpec> &prefixes) {
    auto table = make_unique<TableType>();
    const auto start = steady_clock::now();
    for (const auto &spec : prefixes) {
        table->insert(RouteEntry{spec.prefix, spec.length, nullopt, 0});
    }
    const auto elapsed_ms = duration<double, milli>(steady_clock::now() - start).count();

    string detail;
    if constexpr (is_same_v<TableType, TrieRoutingTable>) {
        detail = "nodes=" + to_string(table->node_count());
    } else {
        detail = "entries=" + to_string(table->size());
    }
    return {name, detail, std::move(table), elapsed_ms};
}

//! \returns the best (minimum) average nanoseconds per lookup over `repeats` runs.
double measure_lookup(const RoutingTable &table, const vector<uint32_t> &queries, int repeats) {
    double best_ns_per_lookup = 0.0;
    uint64_t sink = 0;

    for (int run = 0; run < repeats; ++run) {
        const auto start = steady_clock::now();
        for (const uint32_t query : queries) {
            sink ^= fingerprint(table.lookup(query)) + 0x9E3779B97F4A7C15ull + (sink << 6) + (sink >> 2);
        }
        const auto elapsed_ns = duration<double, nano>(steady_clock::now() - start).count();
        const double ns_per_lookup = elapsed_ns / static_cast<double>(queries.size());
        if (run == 0 || ns_per_lookup < best_ns_per_lookup) {
            best_ns_per_lookup = ns_per_lookup;
        }
    }

    if (sink == 0xDEADBEEF) {
        cerr << "unreachable\n";  // keeps `sink` observable
    }
    return best_ns_per_lookup;
}

//! Verify that all three tables agree on every query.
void check_equivalence(const vector<BuiltTable> &tables, const vector<uint32_t> &queries) {
    const auto &reference = tables.front();
    for (size_t i = 0; i < queries.size(); ++i) {
        const uint64_t expected = fingerprint(reference.table->lookup(queries[i]));
        for (size_t t = 1; t < tables.size(); ++t) {
            const uint64_t actual = fingerprint(tables[t].table->lookup(queries[i]));
            if (actual != expected) {
                cerr << "MISMATCH at query " << i << " (0x" << hex << queries[i] << dec << "): ";
                cerr << reference.name << "=" << expected << " " << tables[t].name << "=" << actual << "\n";
                exit(EXIT_FAILURE);
            }
        }
    }
}

void run_size(size_t prefix_count, int repeats) {
    mt19937 rng(0xC5E144u + static_cast<unsigned>(prefix_count));
    const auto prefixes = generate_prefixes(prefix_count, rng);

    // Keep total work bounded: the linear scan is O(N) per query.
    const size_t query_count = max<size_t>(2000, min<size_t>(200000, 20000000 / prefixes.size()));
    const auto queries = generate_queries(prefixes, query_count, rng);

    vector<BuiltTable> tables;
    tables.push_back(build_table<VectorRoutingTable>("vector (linear)", prefixes));
    tables.push_back(build_table<TrieRoutingTable>("patricia-trie", prefixes));
    tables.push_back(build_table<HashRoutingTable>("bucketed-hash", prefixes));

    check_equivalence(tables, queries);

    cout << "\nprefixes: " << prefixes.size() << ", queries per run: " << queries.size() << "\n";
    cout << left << setw(18) << "implementation" << right << setw(14) << "structure" << setw(14) << "build (ms)"
         << setw(16) << "lookup (ns)" << setw(14) << "Mlookup/s" << "\n";
    cout << string(76, '-') << "\n";

    for (const auto &entry : tables) {
        const double ns = measure_lookup(*entry.table, queries, repeats);
        cout << left << setw(18) << entry.name << right << setw(14) << entry.detail << setw(14) << fixed
             << setprecision(2) << entry.build_ms << setw(16) << setprecision(1) << ns << setw(14) << setprecision(2)
             << (1000.0 / ns) << "\n";
    }
}

}  // namespace

int main(int argc, char **argv) {
    vector<size_t> sizes{1000, 10000, 100000};
    if (argc > 1) {
        sizes.clear();
        for (int i = 1; i < argc; ++i) {
            const long value = strtol(argv[i], nullptr, 10);
            if (value <= 0) {
                cerr << "usage: " << argv[0] << " [prefix_count ...]\n";
                return EXIT_FAILURE;
            }
            sizes.push_back(static_cast<size_t>(value));
        }
    }

    cout << "Longest-prefix-match routing table benchmark\n";
    cout << "Times are the best of several runs; all three tables are verified to\n";
    cout << "return identical results for every query before timing.\n";

    for (const size_t size : sizes) {
        run_size(size, size > 50000 ? 2 : 5);
    }

    return EXIT_SUCCESS;
}
