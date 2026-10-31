// net_order.h - IPv4 addresses and ports in network byte order, the way the
// platform layer (os.h) and a sockaddr_in hold them: a uint32_t or uint16_t
// whose bytes in memory are the bytes on the wire. Such a value is only ever
// copied whole; its octets and its number come out through these helpers,
// which work byte by byte and so give the same answer on any host. Neither
// the host's htons (a swap on a little-endian host, nothing on a big-endian
// one) nor the host's byte order enters into it.
//
// The guest's own view is different: the x86 guest reads the same four bytes
// as a little-endian integer (in_addr 127.0.0.1 is 0x0100007f in a guest
// register). Guest memory and a host network-order value hold the same bytes,
// so copying between them is a memcpy; turning one into the other's integer
// is what the helpers below are for.
#ifndef RECOMP_NET_ORDER_H
#define RECOMP_NET_ORDER_H

#include <stdint.h>
#include <string.h>

// The address a.b.c.d in network order.
static inline uint32_t net_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint8_t q[4] = {a, b, c, d};
    uint32_t v;
    memcpy(&v, q, 4);
    return v;
}
// Octet i (0 = the first, "a" in a.b.c.d) of a network-order address.
static inline uint8_t net_ip_octet(uint32_t net, int i) {
    uint8_t q[4];
    memcpy(q, &net, 4);
    return q[i & 3];
}
// A network-order port's number, and a port number in network order.
static inline uint16_t net_port_number(uint16_t net) {
    uint8_t b[2];
    memcpy(b, &net, 2);
    return (uint16_t)(b[0] << 8 | b[1]);
}
static inline uint16_t net_port(uint16_t number) {
    const uint8_t b[2] = {(uint8_t)(number >> 8), (uint8_t)number};
    uint16_t v;
    memcpy(&v, b, 2);
    return v;
}

#endif
