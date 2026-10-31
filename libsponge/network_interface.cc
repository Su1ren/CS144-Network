#include "network_interface.hh"

#include "arp_message.hh"
#include "ethernet_frame.hh"

#include <iostream>

// Dummy implementation of a network interface
// Translates from {IP datagram, next hop address} to link-layer frame, and from link-layer frame to IP datagram

// For Lab 5, please replace with a real implementation that passes the
// automated checks run by `make check_lab5`.

// You will need to add private members to the class declaration in `network_interface.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&.../* unused */) {}

using namespace std;

//! \param[in] ethernet_address Ethernet (what ARP calls "hardware") address of the interface
//! \param[in] ip_address IP (what ARP calls "protocol") address of the interface
NetworkInterface::NetworkInterface(const EthernetAddress &ethernet_address, const Address &ip_address)
    : _ethernet_address(ethernet_address), _ip_address(ip_address) {
    cerr << "DEBUG: Network interface has Ethernet address " << to_string(_ethernet_address) << " and IP address "
         << ip_address.ip() << "\n";
}

//! \param[in] dgram the IPv4 datagram to be sent
//! \param[in] next_hop the IP address of the interface to send it to (typically a router or default gateway, but may also be another host if directly connected to the same network as the destination)
//! (Note: the Address type can be converted to a uint32_t (raw 32-bit IP address) with the Address::ipv4_numeric() method.)
void NetworkInterface::send_datagram(const InternetDatagram &dgram, const Address &next_hop) {
    // convert IP address of next hop to raw 32-bit representation (used in ARP header)
    const uint32_t next_hop_ip = next_hop.ipv4_numeric();

    // 检查 ARP 缓存中是否有 next_hop_ip 对应的以太网地址
    auto it = _arp_cache_map.find(next_hop_ip);
    if (it != _arp_cache_map.end()) {
        // 如果有，读取以太网地址，封装成以太网帧，并加入发送队列
        EthernetFrame frame;
        frame.header().src = _ethernet_address;
        frame.header().dst = it->second.ethernet_address;
        frame.header().type = EthernetHeader::TYPE_IPv4;
        frame.payload() = dgram.serialize();
        _frames_out.push(frame);
    } else {
        // 如果没有，广播 ARP 请求，并且将该数据包加入等待队列，等待 ARP 回复
        if (!_last_arp_request_time_map.count(next_hop_ip) ||
            _last_tick_time - _last_arp_request_time_map[next_hop_ip] >= 5000) {
            ARPMessage arp_request;
            arp_request.opcode = ARPMessage::OPCODE_REQUEST;
            arp_request.sender_ethernet_address = _ethernet_address;
            arp_request.sender_ip_address = _ip_address.ipv4_numeric();
            arp_request.target_ethernet_address = {};  // 空的以太网地址
            arp_request.target_ip_address = next_hop_ip;

            // 将数据包加入等待队列
            _waiting_datagrams_map[next_hop_ip].push(dgram);

            // 先发送 ARP 请求
            // 检查发送间隔，如果上次发送 ARP 请求的时间不超过 5 秒则不发送 ARP 请求
            EthernetFrame arp_frame;
            arp_frame.header().src = _ethernet_address;
            arp_frame.header().dst = ETHERNET_BROADCAST;
            arp_frame.header().type = EthernetHeader::TYPE_ARP;
            arp_frame.payload() = arp_request.serialize();

            _last_arp_request_time_map[next_hop_ip] = _last_tick_time;
            _frames_out.push(arp_frame);
        }
    }
}

//! \param[in] frame the incoming Ethernet frame
optional<InternetDatagram> NetworkInterface::recv_frame(const EthernetFrame &frame) {
    // 接收到来的以太网帧，检查是否是发给自己的
    if (frame.header().dst != _ethernet_address && frame.header().dst != ETHERNET_BROADCAST) {
        return {};
    }
    // 确定是发给自己的，根据以太网帧的类型字段，判断是 IPv4 数据报还是 ARP 广播请求/应答
    if (frame.header().type == EthernetHeader::TYPE_IPv4) {
        // 解析以太网帧的 payload，得到 IPv4 数据报
        InternetDatagram dgram;
        if (dgram.parse(frame.payload()) == ParseResult::NoError) {
            return dgram;
        }
    } else if (frame.header().type == EthernetHeader::TYPE_ARP) {
        // 解析以太网帧的 payload，得到 ARP 报文
        ARPMessage arp_msg;
        if (arp_msg.parse(frame.payload()) == ParseResult::NoError) {
            // 将 ARP 报文的发送者 IP 地址和以太网地址加入 ARP 缓存
            learn_arp_mapping(arp_msg.sender_ip_address, arp_msg.sender_ethernet_address);
            // 如果是 ARP 请求，且目标 IP 地址是自己的 IP 地址，则回复 ARP 应答
            if (arp_msg.opcode == ARPMessage::OPCODE_REQUEST &&
                arp_msg.target_ip_address == _ip_address.ipv4_numeric()) {
                ARPMessage reply;
                reply.opcode = ARPMessage::OPCODE_REPLY;
                reply.sender_ethernet_address = _ethernet_address;
                reply.sender_ip_address = _ip_address.ipv4_numeric();
                reply.target_ethernet_address = arp_msg.sender_ethernet_address;
                reply.target_ip_address = arp_msg.sender_ip_address;

                EthernetFrame reply_frame;
                reply_frame.header().dst = arp_msg.sender_ethernet_address;
                reply_frame.header().src = _ethernet_address;
                reply_frame.header().type = EthernetHeader::TYPE_ARP;
                reply_frame.payload() = reply.serialize();

                _frames_out.push(reply_frame);
            }
        }
    }
    return {};
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void NetworkInterface::tick(const size_t ms_since_last_tick) {
    _last_tick_time += ms_since_last_tick;
    // 负责全部计时功能
    // 1. 维护 ARP 缓存的有效性
    // 2. 抑制 ARP 广播的频率
    // 检查所有 ARP 缓存条目，删除过期的条目
    for (auto it = _arp_cache_map.begin(); it != _arp_cache_map.end();) {
        if (it->second.expires_at <= _last_tick_time) {
            it = _arp_cache_map.erase(it);
        } else {
            ++it;
        }
    }
}

void NetworkInterface::learn_arp_mapping(uint32_t ip, const EthernetAddress &ethernet_address) {
    // 将 IP 地址和以太网地址加入 ARP 缓存，并设置过期时间为 30 秒后
    _arp_cache_map[ip] = {ethernet_address, _last_tick_time + 30000};

    // 检查等待发送的数据报队列中是否有对应的 IP 地址，如果有，则发送所有等待的数据报
    for (auto it = _waiting_datagrams_map.begin(); it != _waiting_datagrams_map.end();) {
        uint32_t next_hop_ip = it->first;
        auto arp_it = _arp_cache_map.find(next_hop_ip);
        if (arp_it != _arp_cache_map.end()) {
            // 如果 ARP 缓存中有对应的以太网地址，则发送所有等待的数据报
            while (!it->second.empty()) {
                InternetDatagram dgram = it->second.front();
                it->second.pop();
                // 将 next_hop_it 转为 Address 类型
                Address next_hop_address(Address::from_ipv4_numeric(next_hop_ip));
                send_datagram(dgram, next_hop_address);
            }
            // 删除等待队列中的条目
            it = _waiting_datagrams_map.erase(it);
        } else {
            ++it;
        }
    }
}