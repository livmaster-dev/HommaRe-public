// winsock.cpp - WSOCK32 / WS2_32 on the host's sockets, for a game that plays
// over a network (game.toml [game] sockets = true; RECOMP_SOCKETS=0 or 1
// overrides). Without it legacy_dlls.cpp's stand-ins fail every socket call.
//
// The host side is the platform layer's IPv4 sockets (os_sock_* in
// platform/os.h: BSD sockets on Apple, Linux and Android, Winsock 2 on
// Windows). A guest SOCKET is a small handle naming a host socket. Every host
// socket is non-blocking; a guest socket left blocking (the WinSock default)
// waits by retrying with the scheduler's sleep between tries, so the other
// guest threads run meanwhile. Addresses and ports are network order on both
// sides. Where POSIX and WinSock differ the guest gets WinSock's answer: a
// failed connect shows in select's exceptfds and SO_ERROR (not writefds), a
// datagram longer than the buffer is WSAEMSGSIZE, a broadcast without
// SO_BROADCAST is WSAEACCES.
//
// Beyond Windows' own stack, for LAN play where a broadcast would not arrive:
// a limited broadcast (255.255.255.255) goes out as the directed broadcast of
// every interface, because it would leave by one interface only and a virtual
// LAN adapter (ZeroTier, Hamachi, a tap VPN) is rarely that one; every
// broadcast also goes as a unicast copy to the hosts in RECOMP_NET_PEERS
// (routed VPNs that carry no broadcast) and, on Apple systems, to the Bonjour
// relay's peers (iOS refuses broadcasts); and a second copy of the game on
// one machine moves off the ports game.toml [net] movable_udp_ports names.
// RECOMP_TRACE_SOCKETS=1 logs the calls; failed calls are always logged.
#include "winsock.h"

#include "game_config.h"
#include "guest.h"
#include "imports.h"
#include "memory.h"
#include "win32.h"
#include "../platform/os.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#ifdef __APPLE__
#include <dns_sd.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#endif

void sched_sleep_for_host_call(uint32_t ms); // kernel32.cpp

#ifndef RECOMP_GAME_SOCKETS
#define RECOMP_GAME_SOCKETS 0
#endif
#ifndef RECOMP_NET_MOVABLE_UDP_PORTS
#define RECOMP_NET_MOVABLE_UDP_PORTS ""
#endif

namespace {

const uint32_t SOCKET_ERROR = 0xffffffffu, INVALID_SOCKET = 0xffffffffu;
const uint32_t LIMITED_BROADCAST = 0xffffffffu, LOOPBACK = 0x0100007fu; // network order
enum : uint32_t {
    WSAEINTR = 10004, WSAEBADF = 10009, WSAEACCES = 10013, WSAEFAULT = 10014, WSAEINVAL = 10022,
    WSAEMFILE = 10024, WSAEWOULDBLOCK = 10035, WSAEINPROGRESS = 10036, WSAEALREADY = 10037,
    WSAENOTSOCK = 10038, WSAEDESTADDRREQ = 10039, WSAEMSGSIZE = 10040, WSAEPROTOTYPE = 10041,
    WSAENOPROTOOPT = 10042, WSAEPROTONOSUPPORT = 10043, WSAEOPNOTSUPP = 10045, WSAEAFNOSUPPORT = 10047,
    WSAEADDRINUSE = 10048, WSAEADDRNOTAVAIL = 10049, WSAENETDOWN = 10050, WSAENETUNREACH = 10051,
    WSAECONNABORTED = 10053, WSAECONNRESET = 10054, WSAENOBUFS = 10055, WSAEISCONN = 10056,
    WSAENOTCONN = 10057, WSAESHUTDOWN = 10058, WSAETIMEDOUT = 10060, WSAECONNREFUSED = 10061,
    WSAEHOSTUNREACH = 10065, WSAHOST_NOT_FOUND = 11001,
};

uint32_t wsa_error(OsNetError e) {
    switch (e) {
    case OS_NET_OK: return 0;
    case OS_NET_WOULDBLOCK: return WSAEWOULDBLOCK;
    case OS_NET_INPROGRESS: return WSAEWOULDBLOCK; // a non-blocking connect under way
    case OS_NET_ALREADY: return WSAEALREADY;
    case OS_NET_ISCONN: return WSAEISCONN;
    case OS_NET_NOTCONN: return WSAENOTCONN;
    case OS_NET_ADDRINUSE: return WSAEADDRINUSE;
    case OS_NET_ADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case OS_NET_ACCES: return WSAEACCES;
    case OS_NET_CONNREFUSED: return WSAECONNREFUSED;
    case OS_NET_CONNRESET: return WSAECONNRESET; // EPIPE too: a send to a closed stream
    case OS_NET_CONNABORTED: return WSAECONNABORTED;
    case OS_NET_NETUNREACH: return WSAENETUNREACH;
    case OS_NET_HOSTUNREACH: return WSAEHOSTUNREACH;
    case OS_NET_NETDOWN: return WSAENETDOWN;
    case OS_NET_TIMEDOUT: return WSAETIMEDOUT;
    case OS_NET_MSGSIZE: return WSAEMSGSIZE;
    case OS_NET_NOBUFS: return WSAENOBUFS;
    case OS_NET_SHUTDOWN: return WSAESHUTDOWN;
    case OS_NET_NOTSOCK: return WSAENOTSOCK;
    case OS_NET_MFILE: return WSAEMFILE;
    case OS_NET_NOPROTOOPT: return WSAENOPROTOOPT;
    default: return WSAEINVAL;
    }
}
uint32_t last_wsa_error() {
    return wsa_error(os_net_error());
}

const char *wsa_error_name(uint32_t e) {
    switch (e) {
    case WSAEINTR: return "WSAEINTR";
    case WSAEACCES: return "WSAEACCES";
    case WSAEFAULT: return "WSAEFAULT";
    case WSAEINVAL: return "WSAEINVAL";
    case WSAEMFILE: return "WSAEMFILE";
    case WSAEALREADY: return "WSAEALREADY";
    case WSAENOTSOCK: return "WSAENOTSOCK";
    case WSAEMSGSIZE: return "WSAEMSGSIZE";
    case WSAEPROTOTYPE: return "WSAEPROTOTYPE";
    case WSAENOPROTOOPT: return "WSAENOPROTOOPT";
    case WSAEPROTONOSUPPORT: return "WSAEPROTONOSUPPORT";
    case WSAEOPNOTSUPP: return "WSAEOPNOTSUPP";
    case WSAEAFNOSUPPORT: return "WSAEAFNOSUPPORT";
    case WSAEADDRINUSE: return "WSAEADDRINUSE";
    case WSAEADDRNOTAVAIL: return "WSAEADDRNOTAVAIL";
    case WSAENETDOWN: return "WSAENETDOWN";
    case WSAENETUNREACH: return "WSAENETUNREACH";
    case WSAECONNABORTED: return "WSAECONNABORTED";
    case WSAECONNRESET: return "WSAECONNRESET";
    case WSAENOBUFS: return "WSAENOBUFS";
    case WSAEISCONN: return "WSAEISCONN";
    case WSAENOTCONN: return "WSAENOTCONN";
    case WSAESHUTDOWN: return "WSAESHUTDOWN";
    case WSAETIMEDOUT: return "WSAETIMEDOUT";
    case WSAECONNREFUSED: return "WSAECONNREFUSED";
    case WSAEHOSTUNREACH: return "WSAEHOSTUNREACH";
    default: return "?";
    }
}

bool trace() {
    static int on = -1;
    if (on < 0) {
        const char *v = recomp_env("TRACE_SOCKETS");
        on = v && *v && strcmp(v, "0") != 0;
    }
    return on;
}

struct Sock {
    OsSocket os = OS_SOCKET_INVALID;
    int type = 0;             // OS_SOCK_STREAM 1, OS_SOCK_DGRAM 2 (WinSock's numbers too)
    bool blocking = true;     // the guest's view (FIONBIO)
    uint32_t rcv_timeout = 0; // ms, SO_RCVTIMEO; 0 = forever
    uint32_t snd_timeout = 0;
    bool broadcast = false; // SO_BROADCAST, which the broadcast copies check themselves
    bool reuseaddr = false; // SO_REUSEADDR: the guest shares its port on purpose
    // A stream connect whose outcome nobody has seen yet, and a failed one
    // whose WinSock error SO_ERROR reports (once).
    bool connecting = false;
    bool connect_failed = false;
    uint32_t pending_error = 0;
};

std::map<uint32_t, Sock> &socks() {
    static std::map<uint32_t, Sock> m;
    return m;
}
uint32_t g_next_handle = 0x100;

Sock *find(uint32_t s) {
    auto it = socks().find(s);
    return it == socks().end() ? nullptr : &it->second;
}

uint32_t fail(X86 *c, uint32_t wsa) {
    set_last_error(wsa);
    set_eax(c, SOCKET_ERROR);
    return SOCKET_ERROR;
}

double now_ms() {
    return os_monotonic_ns() / 1e6;
}

// ------------------------------------------------------------ addresses --

uint16_t swap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

// The guest's sockaddr_in (16 bytes: family16, port, addr, zero[8]) at `a`:
// address and port in network order.
bool to_host(uint32_t a, uint32_t len, uint32_t *addr, uint16_t *port) {
    if (!a || len < 8 || !gm_valid(a, 8))
        return false;
    memcpy(port, g_mem + a + 2, 2);
    memcpy(addr, g_mem + a + 4, 4);
    return rd16(a) == 2; // AF_INET
}

void to_guest(uint32_t addr, uint16_t port, uint32_t a, uint32_t lenp) {
    if (!a)
        return;
    uint32_t cap = lenp ? rd32(lenp) : 16;
    uint8_t buf[16] = {};
    buf[0] = 2; // AF_INET
    memcpy(buf + 2, &port, 2);
    memcpy(buf + 4, &addr, 4);
    memcpy(g_mem + a, buf, cap < 16 ? cap : 16);
    if (lenp)
        wr32(lenp, 16);
}

std::string dotted(uint32_t a) {
    char s[20];
    snprintf(s, sizeof s, "%u.%u.%u.%u", a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff, a >> 24);
    return s;
}
std::string addr_str(uint32_t addr, uint16_t port) {
    return dotted(addr) + ":" + std::to_string(swap16(port));
}
// The first bytes of a guest buffer in hex, for the trace.
std::string hex_head(uint32_t buf, uint32_t len) {
    std::string out;
    char b[4];
    for (uint32_t i = 0; i < len && i < 24 && gm_valid(buf + i, 1); ++i) {
        snprintf(b, sizeof b, "%02x", g_mem[buf + i]);
        out += b;
    }
    return len > 24 ? out + "..." : out;
}

// The host's IPv4 interfaces that are up: {address, netmask, broadcast,
// far end, flags (OS_IF_*, which are WinSock's IFF_* values)}.
struct Iface {
    uint32_t addr, mask, bcast;
    uint32_t far_end; // the platform's directed broadcast, or a point-to-point link's peer
    unsigned flags;
};
std::vector<Iface> ifaces() {
    std::vector<Iface> out;
    OsNetInterface list[32];
    const int n = os_net_interfaces(list, 32);
    for (int i = 0; i < n; ++i) {
        if (!(list[i].flags & OS_IF_UP))
            continue;
        Iface f{};
        f.addr = list[i].addr;
        f.mask = list[i].netmask;
        f.bcast = f.addr | ~f.mask;
        f.far_end = list[i].broadcast;
        f.flags = list[i].flags;
        out.push_back(f);
    }
    // Real interfaces first, loopback last.
    std::stable_sort(out.begin(), out.end(), [](const Iface &a, const Iface &b) {
        return !(a.flags & OS_IF_LOOPBACK) && (b.flags & OS_IF_LOOPBACK);
    });
    return out;
}
// The same, re-read at most once a second, for the send path: a datagram must
// not cost an interface scan, but adapters come and go (a VPN connecting).
const std::vector<Iface> &ifaces_cached() {
    static std::vector<Iface> list;
    static double read_at = -1e9;
    if (now_ms() - read_at >= 1000) {
        list = ifaces();
        read_at = now_ms();
    }
    return list;
}

// ------------------------------------------------------ broadcast relay --
// iOS lets an app send and receive UDP broadcasts only with Apple's multicast
// entitlement, so a game's LAN discovery (a broadcast probe, unicast answers)
// would see no one. Every copy of the app on the network announces itself
// over Bonjour (_hommare._udp, named after the game, so each game sees its
// own), and a datagram the guest broadcasts also goes straight to each such
// peer, at the same port. Peers that can broadcast simply get it twice.
// RECOMP_LAN_RELAY=0 turns it off.

std::mutex g_relay_lock;
std::map<std::string, uint32_t> g_peers; // Bonjour name -> IPv4

#ifdef __APPLE__
std::string g_relay_self;
DNSServiceRef g_relay_conn = nullptr;

void relay_got_addr(DNSServiceRef, DNSServiceFlags flags, uint32_t, DNSServiceErrorType err, const char *,
                    const struct sockaddr *addr, uint32_t, void *ctx) {
    std::string *name = (std::string *)ctx;
    if (!err && addr && addr->sa_family == AF_INET && (flags & kDNSServiceFlagsAdd)) {
        const uint32_t a = ((const sockaddr_in *)addr)->sin_addr.s_addr;
        std::lock_guard<std::mutex> l(g_relay_lock);
        g_peers[*name] = a;
        if (trace())
            fprintf(stderr, "[sockets] relay peer %s at %s\n", name->c_str(), dotted(a).c_str());
    }
}

void relay_resolved(DNSServiceRef, DNSServiceFlags, uint32_t ifindex, DNSServiceErrorType err, const char *,
                    const char *host, uint16_t, uint16_t, const unsigned char *, void *ctx) {
    if (err)
        return;
    DNSServiceRef ref = g_relay_conn;
    DNSServiceGetAddrInfo(&ref, kDNSServiceFlagsShareConnection, ifindex, kDNSServiceProtocol_IPv4, host,
                          relay_got_addr, ctx);
}

void relay_browsed(DNSServiceRef, DNSServiceFlags flags, uint32_t ifindex, DNSServiceErrorType err,
                   const char *name, const char *type, const char *domain, void *) {
    if (err || name == g_relay_self)
        return;
    std::string prefix = std::string(RECOMP_GAME_ID) + " ";
    if (strncmp(name, prefix.c_str(), prefix.size()) != 0)
        return; // another game's
    if (!(flags & kDNSServiceFlagsAdd)) {
        std::lock_guard<std::mutex> l(g_relay_lock);
        g_peers.erase(name);
        return;
    }
    // Kept for the callbacks' lifetime: the app's (few peers come and go).
    std::string *keep = new std::string(name);
    DNSServiceRef ref = g_relay_conn;
    DNSServiceResolve(&ref, kDNSServiceFlagsShareConnection, ifindex, name, type, domain, relay_resolved, keep);
}
#endif

void relay_start() {
#ifdef __APPLE__
    static std::once_flag once;
    std::call_once(once, [] {
        const char *v = getenv("RECOMP_LAN_RELAY");
        if (v && *v == '0')
            return;
        if (DNSServiceCreateConnection(&g_relay_conn) != kDNSServiceErr_NoError)
            return;
        char self[64];
        snprintf(self, sizeof self, "%s %08x", RECOMP_GAME_ID, (unsigned)arc4random());
        g_relay_self = self;
        DNSServiceRef reg = g_relay_conn, browse = g_relay_conn;
        DNSServiceRegister(&reg, kDNSServiceFlagsShareConnection, 0, self, "_hommare._udp", nullptr, nullptr,
                           htons(9), 0, nullptr, nullptr, nullptr);
        DNSServiceBrowse(&browse, kDNSServiceFlagsShareConnection, 0, "_hommare._udp", nullptr, relay_browsed,
                         nullptr);
        std::thread([] {
            int fd = DNSServiceRefSockFD(g_relay_conn);
            for (;;) {
                struct pollfd p{fd, POLLIN, 0};
                if (poll(&p, 1, 1000) > 0 && DNSServiceProcessResult(g_relay_conn) != kDNSServiceErr_NoError)
                    return;
            }
        }).detach();
        if (trace())
            fprintf(stderr, "[sockets] relay: announced as \"%s\"\n", self);
    });
#endif
}

bool is_broadcast(uint32_t a) {
    if (a == LIMITED_BROADCAST)
        return true;
    // A /32 address (a tun's) is its own "broadcast": a unicast to it is none.
    for (const Iface &f : ifaces_cached())
        if (!(f.flags & OS_IF_LOOPBACK) && a == f.bcast && f.bcast != f.addr)
            return true;
    return false;
}

// RECOMP_NET_PEERS (addresses or host names, separated by commas, spaces or
// semicolons): hosts that get a unicast copy of every broadcast, for routed
// networks that carry none. Resolved once, on a host thread: a name can take
// DNS seconds, and the first broadcast must not wait for them.
struct PeerList {
    std::atomic<bool> ready{false};
    std::vector<uint32_t> addrs;
};
PeerList &net_peers() {
    static PeerList *list = [] {
        PeerList *p = new PeerList();
        const char *v = recomp_env("NET_PEERS");
        std::string text = v ? v : "";
        if (text.empty()) {
            p->ready = true;
            return p;
        }
        OsThread *t = os_thread_create(
            [](void *arg) -> void * {
                auto *job = (std::pair<PeerList *, std::string> *)arg;
                std::string word;
                std::vector<uint32_t> out;
                for (size_t i = 0; i <= job->second.size(); ++i) {
                    const char ch = i < job->second.size() ? job->second[i] : ',';
                    if (ch != ',' && ch != ' ' && ch != ';') {
                        word.push_back(ch);
                        continue;
                    }
                    if (word.empty())
                        continue;
                    uint32_t a[4];
                    if (os_net_resolve(word.c_str(), a, 4) > 0)
                        out.push_back(a[0]);
                    else
                        log_msg(1, "RECOMP_NET_PEERS: cannot resolve %s", word.c_str());
                    word.clear();
                }
                job->first->addrs = out;
                job->first->ready = true;
                delete job;
                return nullptr;
            },
            new std::pair<PeerList *, std::string>(p, text), 0);
        if (t)
            os_thread_detach(t);
        else
            p->ready = true;
        return p;
    }();
    return *list;
}

// ------------------------------------------------- two copies, one machine --
// Heroes V binds its game port (UDP 8888) and its LAN port (8889) without
// SO_REUSEADDR, so on Windows a second copy on the same machine cannot bind
// them. Sharing the port instead (SO_REUSEADDR / SO_REUSEPORT) is no answer:
// both copies would hear broadcasts, but a unicast datagram to a shared port
// reaches one of the sockets only (BSD: the last bound; Linux: one chosen by
// the sender's address), so one copy would hear its own messages and the
// other none. For the ports game.toml [net] movable_udp_ports lists (or
// RECOMP_NET_MOVABLE_UDP_PORTS, commas; empty for none), a bind that finds
// the port taken moves to port + k * MOVED_PORT_STRIDE instead: peers answer
// the port a datagram came from, and a broadcast to a movable port also goes
// to the moved ports at this machine's own address. The stride keeps a
// game's neighbouring ports out of each other's way.
const uint32_t MOVED_PORT_SPAN = 4, MOVED_PORT_STRIDE = 100;

bool movable_port(uint32_t port) {
    const char *env = recomp_env("NET_MOVABLE_UDP_PORTS");
    const char *list = env ? env : RECOMP_NET_MOVABLE_UDP_PORTS;
    for (const char *p = list; *p;) {
        char *end = nullptr;
        const unsigned long v = strtoul(p, &end, 10);
        if (end == p) {
            ++p;
            continue;
        }
        if (port && v == port)
            return true;
        p = end;
    }
    return false;
}

// Sends a broadcast datagram everywhere it should go: the limited broadcast
// as each interface's directed broadcast (and to the far end of each
// point-to-point link), anything else as it is; plus a unicast copy to each
// relay and RECOMP_NET_PEERS peer, and to the moved copies of a movable port
// on this machine. Bytes sent, or -1 with `*error` the first failure when
// nothing went out.
int64_t send_broadcast(Sock &k, const uint8_t *data, uint32_t len, int flags, uint32_t addr, uint16_t port,
                       OsNetError *error) {
    std::vector<uint32_t> to;
    uint32_t self = LOOPBACK;
    for (const Iface &f : ifaces_cached()) {
        if (f.flags & OS_IF_LOOPBACK)
            continue;
        if (self == LOOPBACK)
            self = f.addr;
        // A point-to-point link without a peer (a WireGuard or Tailscale tun)
        // reports its own address as the far end: nobody to reach there.
        if (addr == LIMITED_BROADCAST && f.far_end && f.far_end != f.addr &&
            std::find(to.begin(), to.end(), f.far_end) == to.end())
            to.push_back(f.far_end);
    }
    if (to.empty())
        to.push_back(addr); // a directed broadcast, or no interface to fan out to
    int64_t result = -1;
    *error = OS_NET_OK;
    for (uint32_t a : to) {
        const int64_t r = os_sock_sendto(k.os, data, len, flags, a, port);
        if (r >= 0)
            result = r;
        else if (*error == OS_NET_OK)
            *error = os_net_error();
    }
    // The unicast copies; they do not decide the result.
    std::set<uint32_t> copies;
    {
        std::lock_guard<std::mutex> l(g_relay_lock);
        for (auto &p : g_peers)
            copies.insert(p.second);
    }
    PeerList &peers = net_peers();
    if (peers.ready)
        copies.insert(peers.addrs.begin(), peers.addrs.end());
    for (uint32_t a : copies)
        os_sock_sendto(k.os, data, len, flags, a, port);
    if (movable_port(swap16(port)))
        for (uint32_t i = 1; i <= MOVED_PORT_SPAN && swap16(port) + i * MOVED_PORT_STRIDE <= 65535; ++i)
            os_sock_sendto(k.os, data, len, flags, self, swap16((uint16_t)(swap16(port) + i * MOVED_PORT_STRIDE)));
    if (result >= 0)
        *error = OS_NET_OK;
    return result;
}

// --------------------------------------------------------------- waiting --

// Polls the socket behind `s` for `events` until ready or `timeout_ms` (0 =
// forever) passes, letting other guest threads run between tries. True when
// ready; false on timeout, or when another thread closed the socket meanwhile
// (find(s) then fails: how WinSock cancels a blocked call).
bool wait_fd(uint32_t s, int events, uint32_t timeout_ms) {
    double end = timeout_ms ? now_ms() + timeout_ms : 0;
    for (;;) {
        Sock *k = find(s);
        if (!k)
            return false;
        OsSockPoll p{k->os, events, 0};
        if (os_sock_poll(&p, 1, 0) > 0)
            return true;
        if (end && now_ms() >= end)
            return false;
        sched_sleep_for_host_call(2);
    }
}
// The error for a wait that ended without the socket being ready.
uint32_t wait_error(uint32_t s) {
    return find(s) ? WSAETIMEDOUT : WSAEINTR;
}

// Settles a stream connect that was in progress, from a poll's `revents`:
// true once its outcome is known (connected, or failed with pending_error set).
bool settle_connect(Sock &k, int revents) {
    if (!k.connecting)
        return true;
    if (!(revents & (OS_POLL_OUT | OS_POLL_ERR | OS_POLL_HUP)))
        return false;
    int err = 0;
    os_sock_get_option(k.os, OS_SO_ERROR, &err);
    k.connecting = false;
    if (err != OS_NET_OK) {
        k.connect_failed = true;
        k.pending_error = wsa_error((OsNetError)err);
        if (trace())
            fprintf(stderr, "[sockets] connect failed: %s\n", wsa_error_name(k.pending_error));
    }
    return true;
}

} // namespace

// ------------------------------------------------------------- the calls --

bool winsock_real() {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_SOCKETS");
        on = v && *v ? *v == '1' : RECOMP_GAME_SOCKETS;
        if (on)
            fprintf(stderr, "[sockets] WinSock on the host's sockets\n");
    }
    return on;
}

// A call that failed, in the log: the first few of each (call, error) pair and
// then every thousandth, so a game polling a broken socket every frame cannot
// flood it. WSAEWOULDBLOCK is a non-blocking socket's "not yet", which a game
// polls for on purpose, so it is no failure here.
void winsock_log_failure(X86 *c, const char *call) {
    if (c->r[R_EAX] != SOCKET_ERROR)
        return;
    const uint32_t error = get_last_error();
    if (error == WSAEWOULDBLOCK)
        return;
    if (!strncmp(call, "winsock_", 8))
        call += 8;
    static std::map<std::pair<std::string, uint32_t>, uint64_t> seen;
    const uint64_t n = ++seen[{call, error}];
    if (n <= 3 || n % 1000 == 0)
        log_msg(1, "WSOCK32: %s(%#x, ...) failed with %u (%s)%s", call, arg(c, 0), error, wsa_error_name(error),
                n == 3 ? "; further ones logged every 1000th" : "");
}

void winsock_socket(X86 *c) {
    uint32_t af = arg(c, 0), type = arg(c, 1), proto = arg(c, 2);
    if (af != 2 || (type != OS_SOCK_STREAM && type != OS_SOCK_DGRAM)) {
        set_last_error(af != 2 ? WSAEAFNOSUPPORT : WSAEPROTOTYPE);
        set_eax(c, INVALID_SOCKET);
        return;
    }
    // IPPROTO_TCP 6 for a stream, IPPROTO_UDP 17 for a datagram, or 0.
    if (proto && proto != (type == OS_SOCK_STREAM ? 6u : 17u)) {
        set_last_error(WSAEPROTONOSUPPORT);
        set_eax(c, INVALID_SOCKET);
        return;
    }
    OsSocket os = os_sock_open((int)type);
    if (os == OS_SOCKET_INVALID) {
        set_last_error(last_wsa_error());
        set_eax(c, INVALID_SOCKET);
        return;
    }
    if (type == OS_SOCK_DGRAM)
        relay_start();
    uint32_t h = g_next_handle++;
    Sock k;
    k.os = os;
    k.type = (int)type;
    socks()[h] = k;
    if (trace())
        fprintf(stderr, "[sockets] socket(%s) = %#x\n", type == OS_SOCK_STREAM ? "tcp" : "udp", h);
    set_eax(c, h);
}

void winsock_closesocket(X86 *c) {
    uint32_t s = arg(c, 0);
    Sock *k = find(s);
    if (!k) {
        fail(c, WSAENOTSOCK);
        return;
    }
    if (trace())
        fprintf(stderr, "[sockets] closesocket(%#x)\n", s);
    os_sock_close(k->os);
    socks().erase(s);
    set_eax(c, 0);
}

void winsock_bind(X86 *c) {
    Sock *k = find(arg(c, 0));
    uint32_t addr;
    uint16_t port;
    if (!k)
        return (void)fail(c, WSAENOTSOCK);
    if (!to_host(arg(c, 1), arg(c, 2), &addr, &port))
        return (void)fail(c, WSAEFAULT);
    if (os_sock_bind(k->os, addr, port) == 0) {
        if (trace())
            fprintf(stderr, "[sockets] bind(%#x, %s)\n", arg(c, 0), addr_str(addr, port).c_str());
        set_eax(c, 0);
        return;
    }
    // A second copy of the game on this machine (see movable_port). Everything
    // else, TCP included, fails as on Windows.
    if (os_net_error() == OS_NET_ADDRINUSE && k->type == OS_SOCK_DGRAM && !k->reuseaddr &&
        movable_port(swap16(port))) {
        for (uint32_t i = 1; i <= MOVED_PORT_SPAN; ++i) {
            const uint32_t to = swap16(port) + i * MOVED_PORT_STRIDE;
            if (to > 65535)
                break;
            if (os_sock_bind(k->os, addr, swap16((uint16_t)to)) == 0) {
                log_msg(1, "WSOCK32: UDP port %u is in use (another copy of the game on this machine?); "
                           "socket %#x bound %u instead",
                        swap16(port), arg(c, 0), to);
                set_eax(c, 0);
                return;
            }
            if (os_net_error() != OS_NET_ADDRINUSE)
                break;
        }
        os_sock_bind(k->os, addr, port); // report the port the guest asked for
    }
    uint32_t e = last_wsa_error();
    if (trace())
        fprintf(stderr, "[sockets] bind(%#x, %s) failed: %s\n", arg(c, 0), addr_str(addr, port).c_str(),
                wsa_error_name(e));
    fail(c, e);
}

void winsock_connect(X86 *c) {
    uint32_t s = arg(c, 0);
    Sock *k = find(s);
    uint32_t addr;
    uint16_t port;
    if (!k)
        return (void)fail(c, WSAENOTSOCK);
    if (!to_host(arg(c, 1), arg(c, 2), &addr, &port))
        return (void)fail(c, WSAEFAULT);
    if (k->connecting)
        return (void)fail(c, WSAEALREADY);
    if (trace())
        fprintf(stderr, "[sockets] connect(%#x, %s)\n", s, addr_str(addr, port).c_str());
    k->connect_failed = false;
    k->pending_error = 0;
    if (os_sock_connect(k->os, addr, port) == 0) {
        set_eax(c, 0);
        return;
    }
    if (os_net_error() != OS_NET_INPROGRESS || k->type == OS_SOCK_DGRAM)
        return (void)fail(c, last_wsa_error());
    k->connecting = true;
    if (!k->blocking)
        return (void)fail(c, WSAEWOULDBLOCK);
    if (!wait_fd(s, OS_POLL_OUT, 30000)) {
        if ((k = find(s)))
            k->connecting = false;
        return (void)fail(c, wait_error(s));
    }
    k = find(s);
    settle_connect(*k, OS_POLL_OUT);
    if (k->connect_failed) {
        uint32_t e = k->pending_error;
        k->connect_failed = false;
        k->pending_error = 0;
        return (void)fail(c, e);
    }
    set_eax(c, 0);
}

void winsock_listen(X86 *c) {
    Sock *k = find(arg(c, 0));
    if (!k)
        return (void)fail(c, WSAENOTSOCK);
    if (k->type != OS_SOCK_STREAM)
        return (void)fail(c, WSAEOPNOTSUPP);
    if (os_sock_listen(k->os, (int)std::min<uint32_t>(arg(c, 1), 128)) != 0)
        return (void)fail(c, last_wsa_error());
    set_eax(c, 0);
}

void winsock_accept(X86 *c) {
    uint32_t s = arg(c, 0);
    for (;;) {
        Sock *k = find(s);
        if (!k) {
            fail(c, WSAENOTSOCK);
            set_eax(c, INVALID_SOCKET);
            return;
        }
        uint32_t addr = 0;
        uint16_t port = 0;
        OsSocket os = os_sock_accept(k->os, &addr, &port);
        if (os != OS_SOCKET_INVALID) {
            uint32_t h = g_next_handle++;
            Sock n;
            n.os = os;
            n.type = OS_SOCK_STREAM;
            socks()[h] = n;
            to_guest(addr, port, arg(c, 1), arg(c, 2));
            if (trace())
                fprintf(stderr, "[sockets] accept(%#x) = %#x from %s\n", s, h, addr_str(addr, port).c_str());
            set_eax(c, h);
            return;
        }
        if (os_net_error() != OS_NET_WOULDBLOCK || !k->blocking) {
            set_last_error(last_wsa_error());
            set_eax(c, INVALID_SOCKET);
            return;
        }
        if (!wait_fd(s, OS_POLL_IN, k->rcv_timeout)) {
            set_last_error(wait_error(s));
            set_eax(c, INVALID_SOCKET);
            return;
        }
    }
}

// send / sendto: (s, buf, len, flags[, to, tolen]). A blocking stream send
// returns once everything is queued, as on Windows; a non-blocking one takes
// what fits.
static void do_send(X86 *c, bool to) {
    uint32_t s = arg(c, 0), buf = arg(c, 1), len = arg(c, 2), flags = arg(c, 3);
    uint32_t addr = 0;
    uint16_t port = 0;
    if (len && (!buf || !gm_valid(buf, len)))
        return (void)fail(c, WSAEFAULT);
    Sock *k = find(s);
    if (!k)
        return (void)fail(c, WSAENOTSOCK);
    // A stream ignores sendto's address, as WinSock does.
    bool have_to = to && arg(c, 4) && k->type == OS_SOCK_DGRAM;
    if (have_to && !to_host(arg(c, 4), arg(c, 5), &addr, &port))
        return (void)fail(c, WSAEFAULT);
    bool bcast = have_to && is_broadcast(addr);
    // Windows refuses a broadcast without SO_BROADCAST; the copies below
    // would otherwise deliver it.
    if (bcast && !k->broadcast)
        return (void)fail(c, WSAEACCES);
    const int host_flags = (flags & 1) ? OS_MSG_OOB : 0;
    uint32_t sent = 0;
    for (;;) {
        k = find(s);
        if (!k)
            return (void)fail(c, WSAEINTR);
        OsNetError error = OS_NET_OK;
        int64_t n;
        if (bcast) {
            n = send_broadcast(*k, g_mem + buf, len, host_flags, addr, port, &error);
            // A broadcast the system refuses (iOS without the multicast
            // entitlement) still reached the relay's peers: report it sent.
            if (n < 0 && error != OS_NET_WOULDBLOCK)
                n = (int64_t)len;
        } else {
            n = have_to ? os_sock_sendto(k->os, g_mem + buf, len, host_flags, addr, port)
                        : os_sock_send(k->os, g_mem + buf + sent, len - sent, host_flags);
            if (n < 0)
                error = os_net_error();
        }
        if (n >= 0) {
            if (trace() && have_to)
                fprintf(stderr, "[sockets] sendto(%#x, %u bytes, %s): %s\n", s, len, addr_str(addr, port).c_str(),
                        hex_head(buf, len).c_str());
            sent += (uint32_t)n;
            if (have_to || sent >= len || !k->blocking || k->type != OS_SOCK_STREAM) {
                set_eax(c, have_to ? (uint32_t)n : sent);
                return;
            }
            continue;
        }
        uint32_t e = wsa_error(error);
        if (trace())
            fprintf(stderr, "[sockets] send%s(%#x, %u bytes%s%s) failed: %s\n", have_to ? "to" : "", s, len,
                    have_to ? ", " : "", have_to ? addr_str(addr, port).c_str() : "", wsa_error_name(e));
        if (error != OS_NET_WOULDBLOCK || !k->blocking) {
            if (sent)
                return set_eax(c, sent);
            return (void)fail(c, e);
        }
        if (!wait_fd(s, OS_POLL_OUT, k->snd_timeout))
            return (void)fail(c, wait_error(s));
    }
}
void winsock_send(X86 *c) {
    do_send(c, false);
}
void winsock_sendto(X86 *c) {
    do_send(c, true);
}

// recv / recvfrom: (s, buf, len, flags[, from, fromlen]). A datagram longer
// than the buffer fills it and fails with WSAEMSGSIZE, as on Windows (and is
// dropped, unless peeked at).
static void do_recv(X86 *c, bool from) {
    uint32_t s = arg(c, 0), buf = arg(c, 1), len = arg(c, 2), flags = arg(c, 3);
    if (len && (!buf || !gm_valid(buf, len)))
        return (void)fail(c, WSAEFAULT);
    const int host_flags = ((flags & 2) ? OS_MSG_PEEK : 0) | ((flags & 1) ? OS_MSG_OOB : 0);
    for (;;) {
        Sock *k = find(s);
        if (!k)
            return (void)fail(c, WSAENOTSOCK);
        uint32_t addr = 0;
        uint16_t port = 0;
        int truncated = 0;
        int64_t n;
        if (k->type == OS_SOCK_DGRAM) {
            n = os_sock_recvfrom(k->os, g_mem + buf, len, host_flags, &addr, &port, &truncated);
        } else {
            n = os_sock_recv(k->os, g_mem + buf, len, host_flags);
            if (n >= 0 && from)
                os_sock_peer(k->os, &addr, &port);
        }
        if (n >= 0) {
            if (from)
                to_guest(addr, port, arg(c, 4), arg(c, 5));
            if (trace() && k->type == OS_SOCK_DGRAM)
                fprintf(stderr, "[sockets] recvfrom(%#x) %lld bytes from %s%s: %s\n", s, (long long)n,
                        addr_str(addr, port).c_str(), truncated ? " (truncated)" : "",
                        hex_head(buf, (uint32_t)n).c_str());
            if (truncated)
                return (void)fail(c, WSAEMSGSIZE);
            set_eax(c, (uint32_t)n);
            return;
        }
        if (os_net_error() != OS_NET_WOULDBLOCK || !k->blocking)
            return (void)fail(c, last_wsa_error());
        if (!wait_fd(s, OS_POLL_IN, k->rcv_timeout))
            return (void)fail(c, wait_error(s));
    }
}
void winsock_recv(X86 *c) {
    do_recv(c, false);
}
void winsock_recvfrom(X86 *c) {
    do_recv(c, true);
}

// select(nfds, readfds, writefds, exceptfds, timeout). A guest fd_set is
// {u32 count; u32 sockets[64]}; a timeval {i32 sec; i32 usec}, none = forever.
// A stream still connecting is polled for its outcome whatever the caller
// asks: connected, it is writable; failed, it shows in exceptfds only.
void winsock_select(X86 *c) {
    uint32_t sets[3] = {arg(c, 1), arg(c, 2), arg(c, 3)}, tv = arg(c, 4);
    double timeout = tv ? (int32_t)rd32(tv) * 1000.0 + (int32_t)rd32(tv + 4) / 1000.0 : -1;
    double end = now_ms() + (timeout < 0 ? 0 : timeout);
    std::vector<uint32_t> want[3];
    for (int i = 0; i < 3; ++i)
        if (sets[i]) {
            uint32_t n = rd32(sets[i]);
            for (uint32_t j = 0; j < n && j < 64; ++j)
                want[i].push_back(rd32(sets[i] + 4 + 4 * j));
        }
    if (want[0].empty() && want[1].empty() && want[2].empty())
        return (void)fail(c, WSAEINVAL); // Windows' select is no sleep
    for (const auto &list : want)
        for (uint32_t s : list)
            if (!find(s))
                return (void)fail(c, WSAENOTSOCK);
    const int ev[3] = {OS_POLL_IN, OS_POLL_OUT, OS_POLL_PRI};
    for (;;) {
        std::vector<uint32_t> ready[3];
        uint32_t total = 0;
        for (int i = 0; i < 3; ++i)
            for (uint32_t s : want[i]) {
                Sock *k = find(s);
                if (!k)
                    return (void)fail(c, WSAEINTR); // closed by another thread meanwhile
                OsSockPoll p{k->os, ev[i] | (k->connecting ? OS_POLL_OUT : 0), 0};
                os_sock_poll(&p, 1, 0);
                settle_connect(*k, p.revents);
                bool is;
                if (i == 0) // data, a pending accept, or a closed or failed peer (the recv tells)
                    is = !k->connecting && !k->connect_failed &&
                         (p.revents & (OS_POLL_IN | OS_POLL_HUP | OS_POLL_ERR));
                else if (i == 1) // connected, or room to send
                    is = !k->connecting && !k->connect_failed && (p.revents & OS_POLL_OUT);
                else // out-of-band data, or a connect that failed
                    is = k->connect_failed || (p.revents & OS_POLL_PRI);
                if (is && std::find(ready[i].begin(), ready[i].end(), s) == ready[i].end()) {
                    ready[i].push_back(s);
                    ++total;
                }
            }
        if (total || (timeout >= 0 && now_ms() >= end)) {
            for (int i = 0; i < 3; ++i)
                if (sets[i]) {
                    wr32(sets[i], (uint32_t)ready[i].size());
                    for (size_t j = 0; j < ready[i].size(); ++j)
                        wr32(sets[i] + 4 + 4 * (uint32_t)j, ready[i][j]);
                }
            set_eax(c, total);
            return;
        }
        sched_sleep_for_host_call(timeout < 0 || end - now_ms() > 2 ? 2 : 1);
    }
}

void winsock_fd_isset(X86 *c) {
    uint32_t s = arg(c, 0), set = arg(c, 1);
    uint32_t n = set ? rd32(set) : 0;
    for (uint32_t j = 0; j < n && j < 64; ++j)
        if (rd32(set + 4 + 4 * j) == s) {
            set_eax(c, 1);
            return;
        }
    set_eax(c, 0);
}

void winsock_ioctlsocket(X86 *c) {
    Sock *k = find(arg(c, 0));
    uint32_t cmd = arg(c, 1), argp = arg(c, 2);
    if (!k)
        return (void)fail(c, WSAENOTSOCK);
    if (cmd == 0x8004667Eu) { // FIONBIO
        k->blocking = argp ? rd32(argp) == 0 : true;
    } else if (cmd == 0x4004667Fu) { // FIONREAD
        uint32_t n = 0;
        os_sock_pending(k->os, &n);
        if (argp)
            wr32(argp, n);
    } else {
        return (void)fail(c, WSAEINVAL);
    }
    set_eax(c, 0);
}

namespace {
// WinSock's SOL_SOCKET option numbers (BSD's) as the platform layer's, or false.
bool host_option(uint32_t name, OsSockOption *out) {
    switch (name) {
    case 0x0004: *out = OS_SO_REUSEADDR; return true;
    case 0x0008: *out = OS_SO_KEEPALIVE; return true;
    case 0x0010: *out = OS_SO_DONTROUTE; return true;
    case 0x0020: *out = OS_SO_BROADCAST; return true;
    case 0x0100: *out = OS_SO_OOBINLINE; return true;
    case 0x1001: *out = OS_SO_SNDBUF; return true;
    case 0x1002: *out = OS_SO_RCVBUF; return true;
    default: return false;
    }
}
} // namespace

// setsockopt(s, level, name, val, len) / getsockopt(s, level, name, val, lenp).
// SOL_SOCKET is 0xffff and the option numbers are the BSD ones (host_option).
void winsock_setsockopt(X86 *c) {
    Sock *k = find(arg(c, 0));
    uint32_t level = arg(c, 1), name = arg(c, 2), val = arg(c, 3), len = arg(c, 4);
    if (!k)
        return (void)fail(c, WSAENOTSOCK);
    uint32_t v = val && len >= 4 ? rd32(val) : val && len ? rd8(val) : 0;
    if (trace())
        fprintf(stderr, "[sockets] setsockopt(%#x, %#x, %#x, %u)\n", arg(c, 0), level, name, v);
    int iv = (int)v;
    OsSockOption opt;
    if (level == 0xffff) {
        switch (name) {
        case 0x1005: k->snd_timeout = v; set_eax(c, 0); return; // SO_SNDTIMEO (ms)
        case 0x1006: k->rcv_timeout = v; set_eax(c, 0); return; // SO_RCVTIMEO (ms)
        case 0x20: k->broadcast = v != 0; break;                // SO_BROADCAST
        case 0x4: k->reuseaddr = v != 0; break;                 // SO_REUSEADDR
        default: break;
        }
        if (name == 0x80) { // SO_LINGER {u16 l_onoff, l_linger}
            os_sock_set_option(k->os, OS_SO_LINGER, (v & 0xffff) ? (int)(v >> 16) : -1);
        } else if (name == 0xff7f) { // SO_DONTLINGER
            os_sock_set_option(k->os, OS_SO_LINGER, v ? -1 : 0);
        } else if (!host_option(name, &opt) || os_sock_set_option(k->os, opt, iv) != 0) {
            if (trace())
                fprintf(stderr, "[sockets]   ignored: %s\n",
                        host_option(name, &opt) ? wsa_error_name(last_wsa_error()) : "an unknown option");
        }
    } else if (level == 6 && name == 1) { // IPPROTO_TCP, TCP_NODELAY
        os_sock_set_option(k->os, OS_TCP_NODELAY, iv);
    }
    // Other options (IP multicast, keep-alive tuning...) are accepted unchanged.
    set_eax(c, 0);
}

void winsock_getsockopt(X86 *c) {
    Sock *k = find(arg(c, 0));
    uint32_t level = arg(c, 1), name = arg(c, 2), val = arg(c, 3), lenp = arg(c, 4);
    if (!k)
        return (void)fail(c, WSAENOTSOCK);
    int iv = 0;
    OsSockOption opt;
    if (level == 0xffff && name == 0x1007) { // SO_ERROR: a failed connect's, then the stack's
        if (k->pending_error) {
            iv = (int)k->pending_error;
            k->pending_error = 0;
            k->connect_failed = false;
        } else {
            os_sock_get_option(k->os, OS_SO_ERROR, &iv);
            iv = (int)wsa_error((OsNetError)iv);
        }
    } else if (level == 0xffff && name == 0x1006) {
        iv = (int)k->rcv_timeout;
    } else if (level == 0xffff && name == 0x1005) {
        iv = (int)k->snd_timeout;
    } else if (level == 0xffff && name == 0x1008) { // SO_TYPE
        iv = k->type;
    } else if (level == 0xffff && name == 0x80) { // SO_LINGER
        int secs = -1;
        os_sock_get_option(k->os, OS_SO_LINGER, &secs);
        iv = secs >= 0 ? (int)(1u | ((uint32_t)secs << 16)) : 0;
    } else if (level == 0xffff && host_option(name, &opt)) {
        os_sock_get_option(k->os, opt, &iv);
    } else if (level == 6 && name == 1) {
        os_sock_get_option(k->os, OS_TCP_NODELAY, &iv);
    }
    if (val)
        wr32(val, (uint32_t)iv);
    if (lenp)
        wr32(lenp, 4);
    set_eax(c, 0);
}

static void name_of(X86 *c, bool peer) {
    Sock *k = find(arg(c, 0));
    if (!k)
        return (void)fail(c, WSAENOTSOCK);
    uint32_t addr = 0;
    uint16_t port = 0;
    int r = peer ? os_sock_peer(k->os, &addr, &port) : os_sock_name(k->os, &addr, &port);
    if (r != 0)
        return (void)fail(c, last_wsa_error());
    if (!peer && !addr && !port)
        return (void)fail(c, WSAEINVAL); // unbound: POSIX answers 0.0.0.0:0, WinSock fails
    to_guest(addr, port, arg(c, 1), arg(c, 2));
    set_eax(c, 0);
}
void winsock_getsockname(X86 *c) {
    name_of(c, false);
}
void winsock_getpeername(X86 *c) {
    name_of(c, true);
}

void winsock_shutdown(X86 *c) {
    Sock *k = find(arg(c, 0));
    if (!k)
        return (void)fail(c, WSAENOTSOCK);
    if (arg(c, 1) > 2)
        return (void)fail(c, WSAEINVAL);
    os_sock_shutdown(k->os, (int)arg(c, 1));
    set_eax(c, 0);
}

// ------------------------------------------------------------- host names --

// The name gethostname gives: the game's id, which gethostbyname then knows as
// this machine's own addresses.
static const char *own_name() {
    return RECOMP_GAME_ID;
}

void winsock_gethostname(X86 *c) {
    uint32_t buf = arg(c, 0), len = arg(c, 1);
    if (buf && len)
        gm_put_str(buf, own_name(), len);
    set_eax(c, 0);
}

// A guest hostent {char *name; char **aliases; i16 type; i16 length; char **addrs}
// (one per thread in WinSock; one in all here), rebuilt on each call.
static uint32_t guest_hostent(const std::string &name, const std::vector<uint32_t> &addrs) {
    static uint32_t block = 0;
    const uint32_t size = 0x400;
    if (!block || !heap_owns(block))
        block = heap_alloc(size, true);
    if (!block)
        return 0;
    memset(g_mem + block, 0, size);
    uint32_t he = block, name_at = block + 0x20, aliases = block + 0x80, list = block + 0x88,
             data = block + 0x100;
    size_t n = addrs.size() < 16 ? addrs.size() : 16;
    gm_put_str(name_at, name.c_str(), 0x5f);
    wr32(he + 0, name_at);
    wr32(he + 4, aliases); // an empty list
    wr16(he + 8, 2);       // AF_INET
    wr16(he + 10, 4);
    wr32(he + 12, list);
    for (size_t i = 0; i < n; ++i) {
        memcpy(g_mem + data + 4 * i, &addrs[i], 4);
        wr32(list + 4 * (uint32_t)i, data + 4 * (uint32_t)i);
    }
    wr32(list + 4 * (uint32_t)n, 0);
    return he;
}

// Resolves `name` on a host thread while the guest keeps running: a DNS
// lookup can take seconds, and every guest thread would wait them out if the
// caller held the scheduler through it.
static std::vector<uint32_t> resolve(const std::string &name) {
    struct Job {
        std::string name;
        std::atomic<bool> done{false};
        int count = -1;
        uint32_t addrs[16];
        std::atomic<int> owners{2}; // the guest thread and the resolver
    };
    Job *job = new Job();
    job->name = name;
    OsThread *t = os_thread_create(
        [](void *arg) -> void * {
            Job *j = (Job *)arg;
            j->count = os_net_resolve(j->name.c_str(), j->addrs, 16);
            j->done = true;
            if (j->owners.fetch_sub(1) == 1)
                delete j;
            return nullptr;
        },
        job, 0);
    if (!t) {
        job->count = os_net_resolve(name.c_str(), job->addrs, 16);
        job->done = true;
        job->owners = 1;
    } else {
        os_thread_detach(t);
    }
    while (!job->done)
        sched_sleep_for_host_call(5);
    std::vector<uint32_t> out(job->addrs, job->addrs + std::max(job->count, 0));
    if (job->owners.fetch_sub(1) == 1)
        delete job;
    return out;
}

void winsock_gethostbyname(X86 *c) {
    std::string name = gm_str(arg(c, 0), 256);
    std::vector<uint32_t> addrs;
    unsigned v[4];
    char tail;
    if (name.empty() || name == own_name()) {
        for (const Iface &f : ifaces())
            addrs.push_back(f.addr);
    } else if (sscanf(name.c_str(), "%u.%u.%u.%u%c", &v[0], &v[1], &v[2], &v[3], &tail) == 4 && v[0] < 256 &&
               v[1] < 256 && v[2] < 256 && v[3] < 256) {
        addrs.push_back(v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24));
    } else {
        addrs = resolve(name);
    }
    if (trace())
        fprintf(stderr, "[sockets] gethostbyname(\"%s\"): %zu addresses%s%s\n", name.c_str(), addrs.size(),
                addrs.empty() ? "" : ", first ", addrs.empty() ? "" : dotted(addrs[0]).c_str());
    if (addrs.empty()) {
        log_once(("gethostbyname " + name).c_str(), "WSOCK32: gethostbyname(\"%s\") found no address", name.c_str());
        set_last_error(WSAHOST_NOT_FOUND);
        set_eax(c, 0);
        return;
    }
    set_eax(c, guest_hostent(name, addrs));
}

void winsock_gethostbyaddr(X86 *c) {
    uint32_t p = arg(c, 0), len = arg(c, 1);
    if (!p || len < 4) {
        set_last_error(WSAEFAULT);
        set_eax(c, 0);
        return;
    }
    uint32_t a = rd32(p);
    set_eax(c, guest_hostent(dotted(a), {a}));
}

// WSAIoctl(s, code, in, inlen, out, outlen, returned, overlapped, routine):
// SIO_GET_INTERFACE_LIST fills INTERFACE_INFO records (76 bytes: flags, then
// address, broadcast and netmask as 24-byte sockaddr_gen); a buffer too small
// for all of them is WSAEFAULT with the size needed in `returned`.
// SIO_UDP_CONNRESET asks for what these sockets already do (an ICMP error
// never fails a later recvfrom).
void winsock_WSAIoctl(X86 *c) {
    uint32_t code = arg(c, 1), out = arg(c, 4), outlen = arg(c, 5), returned = arg(c, 6);
    if (!find(arg(c, 0)))
        return (void)fail(c, WSAENOTSOCK);
    if (code == 0x9800000Cu) { // SIO_UDP_CONNRESET
        if (returned)
            wr32(returned, 0);
        set_eax(c, 0);
        return;
    }
    if (code != 0x4004747Fu)
        return (void)fail(c, WSAEINVAL);
    std::vector<Iface> list = ifaces();
    uint32_t need = 76 * (uint32_t)list.size();
    if (returned)
        wr32(returned, need);
    if (!out || outlen < need || !gm_valid(out, need))
        return (void)fail(c, WSAEFAULT);
    uint32_t n = 0;
    for (const Iface &f : list) {
        uint32_t r = out + 76 * n++;
        memset(g_mem + r, 0, 76);
        wr32(r, OS_IF_UP | (f.flags & (OS_IF_BROADCAST | OS_IF_LOOPBACK | OS_IF_POINTTOPOINT | OS_IF_MULTICAST)));
        const uint32_t three[3] = {f.addr, f.bcast, f.mask};
        for (int i = 0; i < 3; ++i) {
            uint32_t a = r + 4 + 24 * (uint32_t)i;
            wr16(a, 2); // AF_INET
            memcpy(g_mem + a + 4, &three[i], 4);
        }
    }
    if (trace())
        fprintf(stderr, "[sockets] WSAIoctl(SIO_GET_INTERFACE_LIST): %u interfaces\n", n);
    set_eax(c, 0);
}
