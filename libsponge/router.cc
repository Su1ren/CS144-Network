#include "router.hh"

#include <algorithm>

using namespace std;

//! \param[in] route_prefix The "up-to-32-bit" IPv4 address prefix to match the datagram's destination address against
//! \param[in] prefix_length For this route to be applicable, how many high-order (most-significant) bits of the route_prefix will need to match the corresponding bits of the datagram's destination address?
//! \param[in] next_hop The IP address of the next hop. Will be empty if the network is directly attached to the router (in which case, the next hop address should be the datagram's final destination).
//! \param[in] interface_num The index of the interface to send the datagram out on.
void Router::add_route(const uint32_t route_prefix,
                       const uint8_t prefix_length,
                       const optional<Address> next_hop,
                       const size_t interface_num) {
    // Storing the rule is all this method has to do; the routing table decides
    // how to index it. Re-adding an identical prefix replaces the old entry.
    _routing_table->insert({route_prefix, prefix_length, next_hop, interface_num});
}

//! \param[in] dgram The datagram to be routed
void Router::route_one_datagram(InternetDatagram &dgram) {
    const auto best_route = _routing_table->lookup(dgram.header().dst);
    if (not best_route.has_value()) {
        return;  // no route to the destination: drop the datagram
    }

    // Decrement the TTL. A datagram whose TTL is already zero, or that reaches
    // zero after the decrement, is dropped.
    if (dgram.header().ttl <= 1) {
        return;
    }
    --dgram.header().ttl;

    // For a directly attached network the next hop is the final destination;
    // otherwise it is the next router along the path.
    const Address next_hop = best_route->next_hop.has_value() ? best_route->next_hop.value()
                                                              : Address::from_ipv4_numeric(dgram.header().dst);
    interface(best_route->interface_num).send_datagram(dgram, next_hop);
}

void Router::route() {
    // Go through all the interfaces, and route every incoming datagram to its proper outgoing interface.
    for (auto &interface : _interfaces) {
        auto &queue = interface.datagrams_out();
        while (not queue.empty()) {
            route_one_datagram(queue.front());
            queue.pop();
        }
    }
}

void TrieRoutingTable::insert_at(std::unique_ptr<Node> &slot, const RouteEntry &entry, size_t &node_count) {
    // Empty slot: this prefix is the only one on this path.
    if (not slot) {
        slot = std::make_unique<Node>();
        ++node_count;
        slot->key = entry.route_prefix & routing_detail::prefix_mask(entry.prefix_length);
        slot->depth = entry.prefix_length;
        slot->has_route = true;
        slot->entry = entry;
        return;
    }

    Node &node = *slot;
    const uint32_t prefix = entry.route_prefix;
    const uint8_t length = entry.prefix_length;

    // Number of leading bits shared by this node's prefix and the new prefix,
    // limited to the common length of both prefixes.
    const unsigned shared = std::min(routing_detail::first_differing_bit(node.key, prefix),
                                     static_cast<unsigned>(std::min(node.depth, length)));

    if (shared == node.depth) {
        if (length == node.depth) {
            // Same prefix: replace the old rule.
            node.has_route = true;
            node.entry = entry;
            return;
        }
        // The new prefix extends this node's path: keep descending.
        insert_at(node.child[routing_detail::bit_at(prefix, node.depth) ? 1 : 0], entry, node_count);
        return;
    }

    if (shared == length) {
        // The new prefix is an ancestor of this node: insert it above.
        auto ancestor = std::make_unique<Node>();
        ++node_count;
        ancestor->key = prefix & routing_detail::prefix_mask(length);
        ancestor->depth = length;
        ancestor->has_route = true;
        ancestor->entry = entry;
        ancestor->child[routing_detail::bit_at(node.key, length) ? 1 : 0] = std::move(slot);
        slot = std::move(ancestor);
        return;
    }

    // The two prefixes diverge at bit `shared`: create a branch node that holds
    // the existing subtree and the new leaf as its two children.
    auto branch = std::make_unique<Node>();
    ++node_count;
    branch->key = prefix & routing_detail::prefix_mask(static_cast<uint8_t>(shared));
    branch->depth = static_cast<uint8_t>(shared);

    auto leaf = std::make_unique<Node>();
    ++node_count;
    leaf->key = prefix & routing_detail::prefix_mask(length);
    leaf->depth = length;
    leaf->has_route = true;
    leaf->entry = entry;

    branch->child[routing_detail::bit_at(node.key, shared) ? 1 : 0] = std::move(slot);
    branch->child[routing_detail::bit_at(prefix, shared) ? 1 : 0] = std::move(leaf);
    slot = std::move(branch);
}

std::optional<RouteEntry> TrieRoutingTable::lookup(uint32_t dest_addr) const {
    // The root is the default route (prefix length zero), if it was installed.
    const Node *node = _root.get();
    std::optional<RouteEntry> best_route = node->has_route ? std::optional<RouteEntry>(node->entry) : std::nullopt;

    while (node->depth < 32) {
        const Node *child = node->child[routing_detail::bit_at(dest_addr, node->depth) ? 1 : 0].get();
        if (child == nullptr) {
            break;
        }
        // A compressed edge must still match the bits it skips over.
        if (not routing_detail::same_prefix(child->key, dest_addr, child->depth)) {
            break;
        }
        // The longest match may be an internal node, so remember every hit.
        if (child->has_route) {
            best_route = child->entry;
        }
        node = child;
    }
    return best_route;
}
