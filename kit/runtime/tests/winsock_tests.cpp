// WSOCK32 on the host's loopback: the Winsock answers a game sees, on real
// sockets, with no game image. Every check talks to 127.0.0.1 only, except the
// broadcast fan-out, which needs an interface that broadcasts and says so when
// the machine has none.
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../net_order.h"
#include "../win32.h"
#include "../../platform/os.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_checks = 0, g_failures = 0;
static bool check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool check(bool ok, const char *fmt, ...) {
    ++g_checks;
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!ok)
        ++g_failures;
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", msg);
    return ok;
}

// Pushes the arguments and a return address, dispatches, and checks the shim
// popped exactly what its declared argument count says. A WinSock call unless
// `dll` names another.
static uint32_t call(X86 *c, const char *name, const std::vector<uint32_t> &args,
                     const char *dll = nullptr) {
    uint32_t tramp = imports_resolve(dll ? dll : "WSOCK32.dll", name);
    if (!tramp && !dll)
        tramp = imports_resolve("WS2_32.dll", name);
    if (!tramp) {
        check(false, "no trampoline for %s", name);
        return 0;
    }
    const uint32_t before = c->r[R_ESP];
    uint32_t esp = before;
    for (size_t i = args.size(); i-- > 0;) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, 0);
    c->r[R_ESP] = esp;
    imports_dispatch(c, tramp);
    if (c->r[R_ESP] != before)
        check(false, "%s left ESP at %08x, expected %08x", name, c->r[R_ESP], before);
    c->r[R_ESP] = before;
    return c->r[R_EAX];
}

const uint32_t SOCKET_ERROR = 0xffffffffu;
// A fresh guest block; the whole heap goes with mem_shutdown.
static uint32_t block(uint32_t size) {
    return heap_alloc(size, true);
}
static uint32_t last_error(X86 *c) {
    return call(c, "WSAGetLastError", {});
}
// A guest sockaddr_in for addr:port (both in host order here, for reading ease).
static uint32_t sockaddr_at(uint32_t ip_host, uint16_t port_host) {
    uint32_t sa = block(16);
    wr16(sa, 2);
    wr8(sa + 2, (uint8_t)(port_host >> 8));
    wr8(sa + 3, (uint8_t)port_host);
    wr8(sa + 4, (uint8_t)(ip_host >> 24));
    wr8(sa + 5, (uint8_t)(ip_host >> 16));
    wr8(sa + 6, (uint8_t)(ip_host >> 8));
    wr8(sa + 7, (uint8_t)ip_host);
    return sa;
}
static uint16_t sockaddr_port(uint32_t sa) {
    return (uint16_t)(rd8(sa + 2) << 8 | rd8(sa + 3));
}
static const uint32_t LOOPBACK = 0x7f000001u;

static uint32_t bound_udp(X86 *c, uint32_t ip, uint16_t *port) {
    uint32_t s = call(c, "socket", {2, 2, 0});
    uint32_t sa = sockaddr_at(ip, *port);
    check(call(c, "bind", {s, sa, 16}) == 0, "bind udp %u", *port);
    uint32_t len = block(4);
    wr32(len, 16);
    check(call(c, "getsockname", {s, sa, len}) == 0 && rd32(len) == 16, "getsockname after bind");
    *port = sockaddr_port(sa);
    return s;
}

// Whether a UDP socket of the host's binds 0.0.0.0:port now.
static bool udp_port_free(uint32_t port) {
    OsSocket s = os_sock_open(OS_SOCK_DGRAM);
    const bool ok =
        s != OS_SOCKET_INVALID && port <= 65535 && os_sock_bind(s, 0, net_port((uint16_t)port)) == 0;
    if (s != OS_SOCKET_INVALID)
        os_sock_close(s);
    return ok;
}

// A port the system hands out whose port + 100 and port + 200 are free as
// well, for the move off a movable port. On Windows other programs hold
// ports all over that range (a bind there is WSAEACCES when the program is
// another account's, a service's), and the system may exclude whole ranges.
static uint16_t port_with_room() {
    for (int tries = 0; tries < 100; ++tries) {
        OsSocket s = os_sock_open(OS_SOCK_DGRAM);
        uint16_t port = 0;
        if (s != OS_SOCKET_INVALID && os_sock_bind(s, 0, 0) == 0)
            os_sock_name(s, nullptr, &port);
        if (s != OS_SOCKET_INVALID)
            os_sock_close(s);
        port = net_port_number(port);
        if (port && udp_port_free(port) && udp_port_free(port + 100u) && udp_port_free(port + 200u))
            return port;
    }
    return 0;
}

static void test_startup(X86 *c) {
    uint32_t data = block(400);
    check(call(c, "WSAStartup", {0x0101, data}) == 0 && rd16(data) == 0x0101,
          "WSAStartup grants 1.1");
    check(call(c, "WSACleanup", {}) == 0, "WSACleanup");
    // Not a WinSock answer: an unknown protocol for the type.
    check(call(c, "socket", {2, 2, 6}) == SOCKET_ERROR && last_error(c) == 10043,
          "a UDP socket with IPPROTO_TCP is WSAEPROTONOSUPPORT");
}

static void test_udp(X86 *c) {
    uint16_t pa = 0, pb = 0;
    uint32_t a = bound_udp(c, LOOPBACK, &pa), b = bound_udp(c, LOOPBACK, &pb);
    check(pa != 0 && pb != 0 && pa != pb, "two ephemeral ports %u, %u", pa, pb);
    {
        // A taken UDP port fails as on Windows, unless the game lists it as
        // movable: then a second copy's bind lands 100 ports along.
        uint32_t again = call(c, "socket", {2, 2, 0});
        check(call(c, "bind", {again, sockaddr_at(LOOPBACK, pa), 16}) == SOCKET_ERROR &&
                  last_error(c) == 10048,
              "a taken UDP port is WSAEADDRINUSE");
        call(c, "closesocket", {again});
        char list[32];
        snprintf(list, sizeof list, "1,%u", pa);
        os_setenv("RECOMP_NET_MOVABLE_UDP_PORTS", list);
        uint16_t moved = pa;
        again = bound_udp(c, LOOPBACK, &moved);
        check(moved != pa && (moved - pa) % 100 == 0 && moved <= pa + 400,
              "a taken movable UDP port %u moves to %u, and getsockname says so", pa, moved);
        call(c, "closesocket", {again});
        os_unsetenv("RECOMP_NET_MOVABLE_UDP_PORTS");
    }
    uint32_t msg = block(256), to = sockaddr_at(LOOPBACK, pa);
    memcpy(g_mem + msg, "hello over loopback", 20);
    check(call(c, "sendto", {b, msg, 20, 0, to, 16}) == 20, "sendto 20 bytes");

    // select sees it, then recvfrom takes it with the sender's address.
    uint32_t set = block(4 + 64 * 4), tv = block(8);
    wr32(set, 1);
    wr32(set + 4, a);
    wr32(tv, 1);
    wr32(tv + 4, 0);
    check(call(c, "select", {0, set, 0, 0, tv}) == 1 && rd32(set) == 1 && rd32(set + 4) == a,
          "select reports the datagram");
    check(call(c, "__WSAFDIsSet", {a, set}) == 1, "__WSAFDIsSet finds it");
    uint32_t buf = block(256), from = block(16), fromlen = block(4);
    wr32(fromlen, 16);
    check(call(c, "recvfrom", {a, buf, 256, 0, from, fromlen}) == 20 &&
              memcmp(g_mem + buf, "hello over loopback", 20) == 0,
          "recvfrom returns the datagram");
    check(sockaddr_port(from) == pb && rd32(from + 4) == 0x0100007fu,
          "recvfrom reports 127.0.0.1:%u", sockaddr_port(from));

    // A datagram larger than the buffer: WSAEMSGSIZE, peeked or not; a peek
    // leaves it queued, a read drops the tail.
    uint32_t onb = block(4);
    wr32(onb, 1);
    check(call(c, "ioctlsocket", {a, 0x8004667eu, onb}) == 0, "FIONBIO on");
    check(call(c, "sendto", {b, msg, 100, 0, to, 16}) == 100, "sendto 100 bytes");
    // Loopback delivery is immediate, but poll for it rather than trust that.
    wr32(set, 1);
    wr32(set + 4, a);
    call(c, "select", {0, set, 0, 0, tv});
    uint32_t pending = block(4);
    check(call(c, "ioctlsocket", {a, 0x4004667fu, pending}) == 0 && rd32(pending) >= 100,
          "FIONREAD sees %u bytes", rd32(pending));
    check(call(c, "recvfrom", {a, buf, 10, 2 /* MSG_PEEK */, from, fromlen}) == SOCKET_ERROR &&
              last_error(c) == 10040,
          "a peek into a small buffer is WSAEMSGSIZE");
    check(call(c, "recvfrom", {a, buf, 256, 2, 0, 0}) == 100, "the peeked datagram is still there");
    check(call(c, "recvfrom", {a, buf, 10, 0, 0, 0}) == SOCKET_ERROR && last_error(c) == 10040,
          "a short read is WSAEMSGSIZE");
    check(call(c, "recvfrom", {a, buf, 256, 0, 0, 0}) == SOCKET_ERROR && last_error(c) == 10035,
          "and drops the datagram: next read is WSAEWOULDBLOCK");

    // A blocking read with SO_RCVTIMEO times out, after the time and not before.
    wr32(onb, 0);
    call(c, "ioctlsocket", {a, 0x8004667eu, onb});
    uint32_t ms = block(4);
    wr32(ms, 60);
    check(call(c, "setsockopt", {a, 0xffff, 0x1006, ms, 4}) == 0, "SO_RCVTIMEO 60 ms");
    const uint64_t t0 = os_monotonic_ns();
    check(call(c, "recvfrom", {a, buf, 256, 0, 0, 0}) == SOCKET_ERROR && last_error(c) == 10060,
          "a blocking read with nothing to read times out");
    const double waited = (double)(os_monotonic_ns() - t0) / 1e6;
    check(waited >= 55 && waited < 1000, "after %.0f ms", waited);

    // A blocking read waits for a datagram another (host) thread sends later.
    wr32(ms, 0);
    call(c, "setsockopt", {a, 0xffff, 0x1006, ms, 4});
    struct Late {
        uint16_t port;
    } late{pa};
    OsThread *t = os_thread_create(
        [](void *p) -> void * {
            os_sleep_us(40000);
            OsSocket s = os_sock_open(OS_SOCK_DGRAM);
            const uint16_t port = ((Late *)p)->port;
            os_sock_sendto(s, "late", 4, 0, 0x0100007fu, (uint16_t)(port >> 8 | port << 8));
            os_sock_close(s);
            return nullptr;
        },
        &late, 0);
    const uint64_t t1 = os_monotonic_ns();
    check(call(c, "recvfrom", {a, buf, 256, 0, 0, 0}) == 4 && memcmp(g_mem + buf, "late", 4) == 0,
          "a blocking recvfrom returns what arrives later");
    check((os_monotonic_ns() - t1) / 1000000u >= 30, "and waited for it");
    os_thread_join(t);

    // select with a timeout and nothing to report: 0, empty set.
    wr32(set, 1);
    wr32(set + 4, a);
    wr32(tv, 0);
    wr32(tv + 4, 30000);
    check(call(c, "select", {0, set, 0, 0, tv}) == 0 && rd32(set) == 0,
          "select times out with an empty set");
    check(call(c, "select", {0, 0, 0, 0, tv}) == SOCKET_ERROR && last_error(c) == 10022,
          "select with no sockets is WSAEINVAL, not a sleep");

    // Without SO_BROADCAST a broadcast is refused, as on Windows.
    uint32_t bc = sockaddr_at(0xffffffffu, pa);
    check(call(c, "sendto", {b, msg, 4, 0, bc, 16}) == SOCKET_ERROR && last_error(c) == 10013,
          "a broadcast without SO_BROADCAST is WSAEACCES");

    check(call(c, "closesocket", {a}) == 0 && call(c, "closesocket", {b}) == 0, "closesocket");
    check(call(c, "closesocket", {a}) == SOCKET_ERROR && last_error(c) == 10038,
          "a closed socket is WSAENOTSOCK");
}

// A UDP port in 49152-65535 whose bind is WSAEACCES (OS_NET_ACCES) and whose
// port - 100 and port + 100 are free, or 0. On Windows that is a port another
// account's program holds (a service's), or one of the system's excluded
// ranges; elsewhere there is usually none up there.
static uint32_t refused_port() {
    for (uint32_t q = 49152 + 100; q + 100 <= 65535; ++q) {
        OsSocket s = os_sock_open(OS_SOCK_DGRAM);
        if (s == OS_SOCKET_INVALID)
            return 0;
        const bool refused =
            os_sock_bind(s, 0, net_port((uint16_t)q)) != 0 && os_net_error() == OS_NET_ACCES;
        os_sock_close(s);
        if (refused && udp_port_free(q - 100) && udp_port_free(q + 100))
            return q;
    }
    return 0;
}

// A move off a movable port skips a port it may not have as it skips a taken
// one. A service may let go of its port meanwhile, and then the move lands
// there: that try says nothing, and another is made.
static void test_refused_port(X86 *c) {
    for (int tries = 0; tries < 5; ++tries) {
        const uint32_t refused = refused_port();
        if (!refused) {
            printf("  [skip] no refused UDP port beside two free ones here\n");
            return;
        }
        const uint16_t port = (uint16_t)(refused - 100);
        OsSocket first = os_sock_open(OS_SOCK_DGRAM); // the first copy of the game
        const bool taken = first != OS_SOCKET_INVALID && os_sock_bind(first, 0, net_port(port)) == 0;
        char list[16];
        snprintf(list, sizeof list, "%u", port);
        os_setenv("RECOMP_NET_MOVABLE_UDP_PORTS", list);
        uint32_t second = call(c, "socket", {2, 2, 0});
        const bool bound = call(c, "bind", {second, sockaddr_at(0, port), 16}) == 0;
        uint32_t sa = sockaddr_at(0, 0), len = block(4);
        wr32(len, 16);
        call(c, "getsockname", {second, sa, len});
        const uint16_t moved = sockaddr_port(sa);
        call(c, "closesocket", {second});
        os_unsetenv("RECOMP_NET_MOVABLE_UDP_PORTS");
        if (first != OS_SOCKET_INVALID)
            os_sock_close(first);
        if (taken && bound && moved == refused)
            continue; // let go of meanwhile
        check(taken, "the first copy has %u", port);
        check(bound && moved == refused + 100,
              "the second copy moves from %u past the refused %u to %u", port, refused, moved);
        return;
    }
    printf("  [skip] the refused ports here were let go of before each move\n");
}

static void test_tcp(X86 *c) {
    uint32_t listener = call(c, "socket", {2, 1, 0});
    uint32_t sa = sockaddr_at(LOOPBACK, 0), len = block(4);
    check(call(c, "bind", {listener, sa, 16}) == 0 && call(c, "listen", {listener, 4}) == 0,
          "bind and listen");
    wr32(len, 16);
    call(c, "getsockname", {listener, sa, len});
    const uint16_t port = sockaddr_port(sa);
    uint32_t clash = call(c, "socket", {2, 1, 0});
    check(call(c, "bind", {clash, sockaddr_at(LOOPBACK, port), 16}) == SOCKET_ERROR &&
              last_error(c) == 10048,
          "a taken TCP port stays WSAEADDRINUSE");
    call(c, "closesocket", {clash});

    uint32_t client = call(c, "socket", {2, 1, 0}), on = block(4);
    wr32(on, 1);
    call(c, "ioctlsocket", {client, 0x8004667eu, on});
    uint32_t unbound = block(16);
    wr32(len, 16);
    check(call(c, "getsockname", {client, unbound, len}) == SOCKET_ERROR && last_error(c) == 10022,
          "getsockname on an unbound socket is WSAEINVAL");
    uint32_t to = sockaddr_at(LOOPBACK, port);
    const uint32_t r = call(c, "connect", {client, to, 16});
    check(r == 0 || last_error(c) == 10035, "a non-blocking connect is WSAEWOULDBLOCK");
    uint32_t wset = block(4 + 64 * 4), tv = block(8);
    wr32(wset, 1);
    wr32(wset + 4, client);
    wr32(tv, 2);
    wr32(tv + 4, 0);
    check(call(c, "select", {0, 0, wset, 0, tv}) == 1, "select: the connect completed");
    uint32_t err = block(4), errlen = block(4);
    wr32(errlen, 4);
    check(call(c, "getsockopt", {client, 0xffff, 0x1007, err, errlen}) == 0 && rd32(err) == 0,
          "SO_ERROR is 0");

    uint32_t peer = block(16), peerlen = block(4);
    wr32(peerlen, 16);
    uint32_t server = call(c, "accept", {listener, peer, peerlen});
    check(server != SOCKET_ERROR && rd32(peer + 4) == 0x0100007fu, "accept from 127.0.0.1");
    uint32_t nodelay = block(4);
    wr32(nodelay, 1);
    check(call(c, "setsockopt", {server, 6, 1, nodelay, 4}) == 0, "TCP_NODELAY");

    uint32_t msg = block(64), buf = block(64);
    memcpy(g_mem + msg, "stream", 6);
    check(call(c, "send", {client, msg, 6, 0}) == 6, "send");
    check(call(c, "recv", {server, buf, 64, 0}) == 6 && memcmp(g_mem + buf, "stream", 6) == 0,
          "a blocking recv gets it");
    check(call(c, "closesocket", {client}) == 0, "the client closes");
    check(call(c, "recv", {server, buf, 64, 0}) == 0, "recv reports the orderly close as 0");
    // Sending on into the closed stream: the first send may still be queued,
    // a later one fails, and no SIGPIPE ends the process.
    uint32_t r2 = 0;
    for (int i = 0; i < 20 && r2 != SOCKET_ERROR; ++i) {
        r2 = call(c, "send", {server, msg, 6, 0});
        os_sleep_us(2000);
    }
    // Windows itself may call it WSAECONNABORTED once the peer's reset came in.
    const uint32_t send_error = r2 == SOCKET_ERROR ? last_error(c) : 0;
    check(send_error == 10054 || send_error == 10053,
          "a send to a closed stream is WSAECONNRESET or WSAECONNABORTED (%u), without SIGPIPE",
          send_error);
    call(c, "closesocket", {server});

    // A connect nobody answers: exceptfds and SO_ERROR, not writefds.
    wr32(len, 16);
    uint32_t closed_sa = sockaddr_at(LOOPBACK, 0);
    call(c, "getsockname", {listener, closed_sa, len});
    call(c, "closesocket", {listener});
    uint32_t refused = call(c, "socket", {2, 1, 0});
    call(c, "ioctlsocket", {refused, 0x8004667eu, on});
    call(c, "connect", {refused, closed_sa, 16});
    uint32_t eset = block(4 + 64 * 4);
    wr32(wset, 1);
    wr32(wset + 4, refused);
    wr32(eset, 1);
    wr32(eset + 4, refused);
    // Windows answers a refused connect after its SYN retries, about two
    // seconds even on the loopback; POSIX at once.
    wr32(tv, 10);
    check(call(c, "select", {0, 0, wset, eset, tv}) == 1 && rd32(wset) == 0 && rd32(eset) == 1,
          "a refused connect shows in exceptfds only");
    wr32(errlen, 4);
    check(call(c, "getsockopt", {refused, 0xffff, 0x1007, err, errlen}) == 0 && rd32(err) == 10061,
          "SO_ERROR is WSAECONNREFUSED (%u)", rd32(err));
    call(c, "closesocket", {refused});
}

// The last error is per thread, as on Windows. Heroes V's Ubi.com thread
// polls its lobby connection with recv and asks WSAGetLastError whether the
// failure was WSAEWOULDBLOCK; any call into the runtime may let another guest
// thread run, and when the main thread failed to open a file in between, the
// Ubi.com thread read ERROR_FILE_NOT_FOUND, closed the connection as broken,
// and creating a game failed. Here the other guest thread is a host thread,
// as the scheduler gives each guest thread one.
struct OtherThread {
    X86 ctx;
    uint32_t read_back = 0;
};
static void *other_thread_fails_a_file(void *arg) {
    OtherThread *t = (OtherThread *)arg;
    call(&t->ctx, "SetLastError", {2}, "KERNEL32.dll"); // ERROR_FILE_NOT_FOUND
    t->read_back = call(&t->ctx, "GetLastError", {}, "KERNEL32.dll");
    return nullptr;
}
static void test_last_error_per_thread(X86 *c) {
    uint32_t listener = call(c, "socket", {2, 1, 0});
    uint32_t sa = sockaddr_at(LOOPBACK, 0), len = block(4), on = block(4);
    wr32(len, 16);
    wr32(on, 1);
    call(c, "bind", {listener, sa, 16});
    call(c, "listen", {listener, 4});
    call(c, "getsockname", {listener, sa, len});
    uint32_t client = call(c, "socket", {2, 1, 0});
    check(call(c, "connect", {client, sockaddr_at(LOOPBACK, sockaddr_port(sa)), 16}) == 0,
          "connect to the listener");
    uint32_t server = call(c, "accept", {listener, 0, 0});
    call(c, "ioctlsocket", {client, 0x8004667eu, on});

    uint32_t buf = block(64);
    check(call(c, "recv", {client, buf, 64, 0}) == SOCKET_ERROR,
          "a non-blocking recv with nothing to read fails");
    OtherThread other;
    other.ctx = *c;
    other.ctx.r[R_ESP] = block(0x1000) + 0x1000; // its own guest stack
    OsThread *t = os_thread_create(other_thread_fails_a_file, &other, 0);
    check(t != nullptr, "another thread runs");
    if (t)
        os_thread_join(t);
    check(other.read_back == 2, "that thread reads its own ERROR_FILE_NOT_FOUND (%u)", other.read_back);
    const uint32_t e = last_error(c);
    check(e == 10035, "WSAGetLastError still says WSAEWOULDBLOCK on the recv's thread (%u)", e);
    for (uint32_t s : {client, server, listener})
        call(c, "closesocket", {s});
}

static void test_names(X86 *c) {
    uint32_t name = block(256);
    check(call(c, "gethostname", {name, 256}) == 0 && rd8(name) != 0, "gethostname: %s",
          gm_str(name, 256).c_str());
    uint32_t he = call(c, "gethostbyname", {name});
    check(he != 0 && rd16(he + 8) == 2 && rd16(he + 10) == 4 && rd32(rd32(he + 12)) != 0,
          "gethostbyname(own name) lists an address");
    uint32_t text = block(32);
    gm_put_str(text, "127.0.0.1", 32);
    he = call(c, "gethostbyname", {text});
    check(he && rd32(rd32(rd32(he + 12))) == 0x0100007fu, "gethostbyname of a dotted quad");
    gm_put_str(text, "localhost", 32);
    he = call(c, "gethostbyname", {text});
    check(he && rd32(rd32(rd32(he + 12))) == 0x0100007fu, "gethostbyname(localhost)");
    uint32_t addr = block(4);
    wr32(addr, 0x0100007fu);
    he = call(c, "gethostbyaddr", {addr, 4, 2});
    check(he && gm_str(rd32(he), 256) == "127.0.0.1" && rd32(rd32(rd32(he + 12))) == 0x0100007fu,
          "gethostbyaddr(127.0.0.1) names it by its address");
    check(call(c, "inet_addr", {text}) == 0xffffffffu, "inet_addr(localhost) is INADDR_NONE");
    check(gm_str(call(c, "inet_ntoa", {0x0100a8c0u}), 32) == "192.168.0.1", "inet_ntoa");

    // SIO_GET_INTERFACE_LIST: 76-byte INTERFACE_INFO records, loopback among them.
    uint32_t s = call(c, "socket", {2, 2, 0}), out = block(76 * 32), got = block(4);
    check(call(c, "WSAIoctl", {s, 0x4004747fu, 0, 0, out, 76 * 32, got, 0, 0}) == 0 &&
              rd32(got) % 76 == 0 && rd32(got) >= 76,
          "SIO_GET_INTERFACE_LIST: %u interfaces", rd32(got) / 76);
    bool loopback = false;
    for (uint32_t i = 0; i < rd32(got) / 76; ++i) {
        const uint32_t at = out + i * 76;
        if ((rd32(at) & 4) && rd32(at + 8) == 0x0100007fu && rd32(at + 0x38) == 0x000000ffu)
            loopback = true;
    }
    check(loopback, "the list has 127.0.0.1/8 flagged IFF_LOOPBACK");
    check(call(c, "WSAIoctl", {s, 0x4004747fu, 0, 0, out, 10, got, 0, 0}) == SOCKET_ERROR &&
              last_error(c) == 10014,
          "a short buffer is WSAEFAULT");
    call(c, "closesocket", {s});
}

// Byte order: network-order values are their wire bytes on any host, and what
// the guest gets back is laid out byte for byte as a Windows sockaddr_in,
// hostent and in_addr hold it, whatever the host's own order.
static void test_byte_order(X86 *c) {
    const uint32_t ip = net_ip(192, 168, 1, 20);
    const uint8_t *b = (const uint8_t *)&ip;
    check(b[0] == 192 && b[1] == 168 && b[2] == 1 && b[3] == 20,
          "net_ip lays out the octets in order");
    check(net_ip_octet(ip, 0) == 192 && net_ip_octet(ip, 3) == 20, "net_ip_octet");
    const uint16_t port = net_port(8888);
    const uint8_t *p = (const uint8_t *)&port;
    check(p[0] == 0x22 && p[1] == 0xb8 && net_port_number(port) == 8888,
          "net_port is big-endian, both ways");
    check(net_port_number(net_port(8888 + 100)) == 8988, "a moved port's number");
    uint32_t g = block(4);
    wr32(g, 0x11223344u);
    check(g_mem[g] == 0x44 && rd8(g + 3) == 0x11, "guest memory is little-endian");

    // A bound socket's name, byte for byte: family 02 00, the port
    // big-endian, then 127 0 0 1.
    uint16_t bound = 0;
    uint32_t s = bound_udp(c, LOOPBACK, &bound);
    uint32_t sa = block(16), len = block(4);
    wr32(len, 16);
    check(call(c, "getsockname", {s, sa, len}) == 0 && rd8(sa) == 2 && rd8(sa + 1) == 0 &&
              rd8(sa + 2) == (bound >> 8) && rd8(sa + 3) == (bound & 0xff) && rd8(sa + 4) == 127 &&
              rd8(sa + 5) == 0 && rd8(sa + 6) == 0 && rd8(sa + 7) == 1,
          "getsockname: 02 00 | port big-endian | 7f 00 00 01");
    // A short name buffer gets what fits, and nothing past it.
    uint32_t small = block(16);
    memset(g_mem + small, 0xcc, 16);
    wr32(len, 4);
    call(c, "getsockname", {s, small, len});
    check(rd8(small) == 2 && rd8(small + 4) == 0xcc, "a 4-byte name buffer is not overrun");
    call(c, "closesocket", {s});

    uint32_t text = block(32);
    gm_put_str(text, "10.20.30.40", 32);
    uint32_t he = call(c, "gethostbyname", {text});
    const uint32_t a0 = he ? rd32(rd32(he + 12)) : 0;
    check(he && rd8(a0) == 10 && rd8(a0 + 1) == 20 && rd8(a0 + 2) == 30 && rd8(a0 + 3) == 40,
          "gethostbyname(10.20.30.40): the address's bytes in order");
    uint32_t addr = block(4);
    wr8(addr, 10);
    wr8(addr + 1, 20);
    wr8(addr + 2, 30);
    wr8(addr + 3, 40);
    he = call(c, "gethostbyaddr", {addr, 4, 2});
    check(he && gm_str(rd32(he), 256) == "10.20.30.40",
          "gethostbyaddr names an address by its bytes");
    check(call(c, "gethostbyaddr", {0xfffffff0u, 4, 2}) == 0 && last_error(c) == 10014,
          "gethostbyaddr of a pointer outside guest memory is WSAEFAULT");
    check(call(c, "inet_addr", {text}) == 0x281e140au, "inet_addr: the guest's in_addr value");
    check(call(c, "htons", {8888}) == 0xb822u && call(c, "ntohs", {0xb822u}) == 8888,
          "the guest's htons and ntohs swap");
    check(call(c, "htonl", {0x0a141e28u}) == 0x281e140au, "the guest's htonl swaps");
}

// SO_REUSEADDR shares a UDP port, and a limited broadcast reaches it through
// every broadcasting interface; RECOMP_NET_PEERS adds a unicast copy.
static void test_broadcast(X86 *c) {
    uint32_t one = block(4);
    wr32(one, 1);
    uint32_t a = call(c, "socket", {2, 2, 0}), b = call(c, "socket", {2, 2, 0});
    for (uint32_t s : {a, b}) {
        call(c, "setsockopt", {s, 0xffff, 4, one, 4});
        call(c, "setsockopt", {s, 0xffff, 0x20, one, 4});
        call(c, "ioctlsocket", {s, 0x8004667eu, one});
    }
    const uint16_t port = port_with_room();
    uint32_t any = sockaddr_at(0, 0), len = block(4);
    check(port != 0 && call(c, "bind", {a, sockaddr_at(0, port), 16}) == 0, "bind 0.0.0.0:%u",
          port);
    check(call(c, "bind", {b, sockaddr_at(0, port), 16}) == 0,
          "a second SO_REUSEADDR socket binds the same port %u", port);

    uint32_t sender = call(c, "socket", {2, 2, 0});
    call(c, "setsockopt", {sender, 0xffff, 0x20, one, 4});
    uint32_t msg = block(16), buf = block(64), bc = sockaddr_at(0xffffffffu, port);
    memcpy(g_mem + msg, "anyone?", 8);

    OsNetInterface list[32];
    const int n = os_net_interfaces(list, 32);
    bool broadcasting = false;
    for (int i = 0; i < n; ++i)
        broadcasting |= (list[i].flags & OS_IF_UP) && !(list[i].flags & OS_IF_LOOPBACK) &&
                        (list[i].flags & OS_IF_BROADCAST);
    // The peer list is resolved off-thread; send until a copy arrives. A
    // unicast copy reaches only one of the sockets sharing the port, so with
    // no broadcasting interface one is all there can be.
    bool got_a = false, got_b = false;
    auto done = [&] { return broadcasting ? got_a && got_b : got_a || got_b; };
    for (int tries = 0; tries < 100 && !done(); ++tries) {
        call(c, "sendto", {sender, msg, 8, 0, bc, 16});
        os_sleep_us(5000);
        while (call(c, "recvfrom", {a, buf, 64, 0, 0, 0}) == 8)
            got_a = true;
        while (call(c, "recvfrom", {b, buf, 64, 0, 0, 0}) == 8)
            got_b = true;
    }
    check(done(), "%s",
          broadcasting ? "a broadcast reaches both sockets sharing the port"
                       : "a broadcast reaches the port through RECOMP_NET_PEERS "
                         "(no broadcasting interface here)");

    // A second copy that moved off a movable port still hears broadcasts to it.
    char ports[32];
    snprintf(ports, sizeof ports, "%u", port);
    os_setenv("RECOMP_NET_MOVABLE_UDP_PORTS", ports);
    uint32_t moved = call(c, "socket", {2, 2, 0});
    call(c, "ioctlsocket", {moved, 0x8004667eu, one});
    uint32_t taken = call(c, "socket", {2, 2, 0});
    check(call(c, "bind", {taken, sockaddr_at(0, port + 100), 16}) == 0, "another program has %u",
          port + 100);
    check(call(c, "bind", {moved, sockaddr_at(0, port), 16}) == 0, "the movable port moves");
    wr32(len, 16);
    call(c, "getsockname", {moved, any, len});
    const uint16_t moved_port = sockaddr_port(any);
    bool got_moved = false;
    for (int tries = 0; tries < 50 && !got_moved; ++tries) {
        call(c, "sendto", {sender, msg, 8, 0, bc, 16});
        os_sleep_us(5000);
        while (call(c, "recvfrom", {moved, buf, 64, 0, 0, 0}) == 8)
            got_moved = true;
    }
    check(got_moved && moved_port == port + 200, "a broadcast to %u reaches the copy moved to %u",
          port, moved_port);
    os_unsetenv("RECOMP_NET_MOVABLE_UDP_PORTS");
    for (uint32_t s : {a, b, sender, moved, taken})
        call(c, "closesocket", {s});
}

int main() {
    // The host's sockets whatever the game's [game] sockets says.
    os_setenv("RECOMP_SOCKETS", "1");
    // Every broadcast also goes to these peers: loopback here, so the
    // fan-out is testable on a machine without a broadcasting interface.
    os_setenv("RECOMP_NET_PEERS", "127.0.0.1");
    mem_init();
    imports_init();
    X86 c;
    loader_init_context(&c);
    printf("winsock startup\n");
    test_startup(&c);
    printf("winsock udp\n");
    test_udp(&c);
    printf("winsock refused port\n");
    test_refused_port(&c);
    printf("winsock tcp\n");
    test_tcp(&c);
    printf("winsock byte order\n");
    test_byte_order(&c);
    printf("winsock last error per thread\n");
    test_last_error_per_thread(&c);
    printf("winsock names\n");
    test_names(&c);
    printf("winsock broadcast\n");
    test_broadcast(&c);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    mem_shutdown();
    return g_failures ? 1 : 0;
}
