#ifndef SPONGE_LIBSPONGE_ROUTER_HH
#define SPONGE_LIBSPONGE_ROUTER_HH

#include "network_interface.hh"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

//! \brief A wrapper for NetworkInterface that makes the host-side
//! interface asynchronous: instead of returning received datagrams
//! immediately (from the `recv_frame` method), it stores them for
//! later retrieval. Otherwise, behaves identically to the underlying
//! implementation of NetworkInterface.
class AsyncNetworkInterface : public NetworkInterface {
    std::queue<InternetDatagram> _datagrams_out{};

  public:
    using NetworkInterface::NetworkInterface;

    //! Construct from a NetworkInterface
    AsyncNetworkInterface(NetworkInterface &&interface) : NetworkInterface(interface) {}

    //! \brief Receives and Ethernet frame and responds appropriately.

    //! - If type is IPv4, pushes to the `datagrams_out` queue for later retrieval by the owner.
    //! - If type is ARP request, learn a mapping from the "sender" fields, and send an ARP reply.
    //! - If type is ARP reply, learn a mapping from the "target" fields.
    //!
    //! \param[in] frame the incoming Ethernet frame
    void recv_frame(const EthernetFrame &frame) {
        auto optional_dgram = NetworkInterface::recv_frame(frame);
        if (optional_dgram.has_value()) {
            _datagrams_out.push(std::move(optional_dgram.value()));
        }
    };

    //! Access queue of Internet datagrams that have been received
    std::queue<InternetDatagram> &datagrams_out() { return _datagrams_out; }
};

//! \brief A single forwarding rule: match a prefix, then forward to a next hop
//! out of a particular interface.
struct RouteEntry {
    uint32_t route_prefix{};            //!< network prefix (only the top prefix_length bits matter)
    uint8_t prefix_length{};            //!< number of significant bits, 0..32
    std::optional<Address> next_hop{};  //!< empty if the network is directly attached
    size_t interface_num{};             //!< index of the outbound interface
};

//! \brief Bit helpers shared by the routing-table implementations.
namespace routing_detail {

//! \returns a mask whose top `prefix_length` bits are 1 (0 <= prefix_length <= 32).
//! This avoids the undefined behavior of shifting a 32-bit value by 32.
inline uint32_t prefix_mask(uint8_t prefix_length) {
    return prefix_length == 0 ? 0u : 0xFFFFFFFFu << (32 - prefix_length);
}

//! \returns true if the top `prefix_length` bits of a and b are identical.
inline bool same_prefix(uint32_t a, uint32_t b, uint8_t prefix_length) {
    const uint32_t mask = prefix_mask(prefix_length);
    return (a & mask) == (b & mask);
}

//! \returns the index of the first (most significant) bit where a and b differ,
//! or 32 if the two values are identical. Index 0 is the most significant bit.
inline unsigned first_differing_bit(uint32_t a, uint32_t b) {
    uint32_t diff = a ^ b;
    if (diff == 0) {
        return 32;
    }
    unsigned index = 0;
    while ((diff & 0x80000000u) == 0) {
        diff <<= 1;
        ++index;
    }
    return index;
}

//! \returns the value of bit `index` (0 = most significant) of addr; requires index < 32.
inline bool bit_at(uint32_t addr, unsigned index) { return ((addr >> (31 - index)) & 1u) != 0; }

}  // namespace routing_detail

//! \brief Abstract longest-prefix-match routing table.
class RoutingTable {
  public:
    virtual ~RoutingTable() = default;

    //! Add a route. Adding an identical prefix again replaces the old entry.
    virtual void insert(const RouteEntry &entry) = 0;

    //! \returns the most specific matching route, or std::nullopt if none matches.
    virtual std::optional<RouteEntry> lookup(uint32_t dest_addr) const = 0;
};

//! \brief Baseline implementation: linear scan, O(N) per lookup.
class VectorRoutingTable : public RoutingTable {
    std::vector<RouteEntry> _entries{};

  public:
    //! Appending keeps insertion O(1); `lookup` lets a later duplicate win,
    //! which matches the replace-on-reinsert semantics of the other tables.
    void insert(const RouteEntry &entry) override { _entries.push_back(entry); }

    std::optional<RouteEntry> lookup(uint32_t dest_addr) const override {
        std::optional<RouteEntry> best_route = std::nullopt;
        for (const auto &route : _entries) {
            if (!routing_detail::same_prefix(dest_addr, route.route_prefix, route.prefix_length)) {
                continue;
            }
            if (!best_route.has_value() || route.prefix_length >= best_route->prefix_length) {
                best_route = route;
            }
        }
        return best_route;
    }

    size_t size() const { return _entries.size(); }
};

//! \brief Path-compressed binary trie (Patricia trie).
//!
//! Every node represents one prefix; single-child chains are skipped, so the
//! number of nodes is O(N) and a lookup visits at most one node per stored
//! prefix rather than one node per bit. A lookup keeps track of the deepest
//! node that carries a route, because the longest match may be an internal
//! node (for example a /8 covering the path down to a /24).
class TrieRoutingTable : public RoutingTable {
    struct Node {
        uint32_t key{};    //!< the prefix; only the top `depth` bits are significant
        uint8_t depth{};   //!< number of bits represented by this node
        bool has_route{};  //!< whether a forwarding rule is stored here
        RouteEntry entry{};
        std::unique_ptr<Node> child[2]{};
    };

    std::unique_ptr<Node> _root{std::make_unique<Node>()};  //!< depth 0 = the default route
    size_t _node_count{1};

    static void insert_at(std::unique_ptr<Node> &slot, const RouteEntry &entry, size_t &node_count);

  public:
    void insert(const RouteEntry &entry) override { insert_at(_root, entry, _node_count); }

    std::optional<RouteEntry> lookup(uint32_t dest_addr) const override;

    //! Number of nodes currently allocated (useful for memory comparisons).
    size_t node_count() const { return _node_count; }
};

//! \brief One hash table per prefix length plus a bitmap of the non-empty lengths.
//!
//! A lookup probes the buckets from /32 down to /0 and stops at the first hit,
//! so it performs at most 33 hash lookups regardless of the table size.
class HashRoutingTable : public RoutingTable {
    std::array<std::unordered_map<uint32_t, RouteEntry>, 33> _buckets{};
    uint64_t _used_lengths{0};

  public:
    void insert(const RouteEntry &entry) override {
        const uint32_t masked = entry.route_prefix & routing_detail::prefix_mask(entry.prefix_length);
        _buckets[entry.prefix_length][masked] = entry;
        _used_lengths |= (1ULL << entry.prefix_length);
    }

    std::optional<RouteEntry> lookup(uint32_t dest_addr) const override {
        unsigned length = 32;
        while (true) {
            if ((_used_lengths & (1ULL << length)) != 0) {
                const auto it = _buckets[length].find(dest_addr & routing_detail::prefix_mask(length));
                if (it != _buckets[length].end()) {
                    return it->second;
                }
            }
            if (length == 0) {
                return std::nullopt;
            }
            --length;
        }
    }

    size_t size() const {
        size_t total = 0;
        for (const auto &bucket : _buckets) {
            total += bucket.size();
        }
        return total;
    }
};

//! \brief A router that has multiple network interfaces and
//! performs longest-prefix-match routing between them.
class Router {
    //! The router's collection of network interfaces
    std::vector<AsyncNetworkInterface> _interfaces{};

    //! The forwarding table. The path-compressed trie is the production choice;
    //! swap this single line to compare against VectorRoutingTable or
    //! HashRoutingTable (see apps/router_benchmark.cc).
    std::unique_ptr<RoutingTable> _routing_table{std::make_unique<TrieRoutingTable>()};

    //! Send a single datagram from the appropriate outbound interface to the next hop,
    //! as specified by the route with the longest prefix_length that matches the
    //! datagram's destination address.
    void route_one_datagram(InternetDatagram &dgram);

  public:
    //! Add an interface to the router
    //! \param[in] interface an already-constructed network interface
    //! \returns The index of the interface after it has been added to the router
    size_t add_interface(AsyncNetworkInterface &&interface) {
        _interfaces.push_back(std::move(interface));
        return _interfaces.size() - 1;
    }

    //! Access an interface by index
    AsyncNetworkInterface &interface(const size_t N) { return _interfaces.at(N); }

    //! Add a route (a forwarding rule)
    void add_route(const uint32_t route_prefix,
                   const uint8_t prefix_length,
                   const std::optional<Address> next_hop,
                   const size_t interface_num);

    //! Route packets between the interfaces
    void route();
};

#endif  // SPONGE_LIBSPONGE_ROUTER_HH
