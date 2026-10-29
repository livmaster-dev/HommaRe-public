// http_get.cpp - http_get (http_get.h) over the platform layer's sockets
// (platform/os.h): one implementation for Linux, macOS, iOS, Android and
// Windows. Those sockets are non-blocking already and never raise SIGPIPE, so
// a server that hangs up mid-request is an error here and never ends the game;
// os_sock_poll bounds every wait by the one deadline.
//
// IPv4 only, as the platform layer is: an IPv6 literal is refused with a
// reason. The servers the list names are IPv4 anyway, because the game's own
// connections (runtime/winsock.cpp) are.
//
// iOS: an address on the player's own network (192.168.x.x, a .local name)
// may make the system ask for Local Network permission the first time, and is
// refused until the player allows it; the app's Info.plist has to say why it
// asks (NSLocalNetworkUsageDescription). A server on the Internet needs
// nothing.
#include "http_get.h"
#include "../platform/os.h"

namespace {

double now_ms() {
    return double(os_monotonic_ns()) / 1e6;
}

// What a socket error means to the person who typed the address.
std::string describe(OsNetError err) {
    switch (err) {
    case OS_NET_CONNREFUSED:
        return "connection refused (nothing is listening on that port)";
    case OS_NET_TIMEDOUT:
        return "the connection timed out";
    case OS_NET_HOSTUNREACH:
        return "the host is unreachable";
    case OS_NET_NETUNREACH:
    case OS_NET_NETDOWN:
        return "the network is unreachable";
    case OS_NET_CONNRESET:
    case OS_NET_CONNABORTED:
    case OS_NET_SHUTDOWN:
    case OS_NET_NOTCONN:
        return "the server closed the connection";
    case OS_NET_ACCES:
        return "the system did not allow the connection";
    case OS_NET_ADDRNOTAVAIL:
        return "the address is not usable";
    default:
        return "network error " + std::to_string(int(err));
    }
}

// Waits until `s` can do `events` or the deadline passes. 1 ready (or failed,
// which the next call reports), 0 timed out, -1 failed (os_net_error()).
int wait_for(OsSocket s, int events, double deadline) {
    const double left = deadline - now_ms();
    if (left <= 0)
        return 0;
    OsSockPoll p{s, events, 0};
    const int n = os_sock_poll(&p, 1, int(left) + 1);
    return n < 0 ? -1 : n == 0 ? 0 : 1;
}

// A connected socket to one of `addrs`, or OS_SOCKET_INVALID with *err set
// to the last address's failure.
OsSocket connect_any(const uint32_t *addrs, int count, uint16_t port, double deadline,
                     OsNetError *err, bool *timed_out) {
    *err = OS_NET_OK;
    for (int i = 0; i < count; ++i) {
        const OsSocket s = os_sock_open(OS_SOCK_STREAM);
        if (s == OS_SOCKET_INVALID) {
            *err = os_net_error();
            continue;
        }
        if (os_sock_connect(s, addrs[i], port) == 0)
            return s;
        if (os_net_error() != OS_NET_INPROGRESS && os_net_error() != OS_NET_WOULDBLOCK) {
            *err = os_net_error();
            os_sock_close(s);
            continue;
        }
        const int ready = wait_for(s, OS_POLL_OUT, deadline);
        if (ready == 1) {
            int so = 0;
            if (os_sock_get_option(s, OS_SO_ERROR, &so) == 0 && so == OS_NET_OK)
                return s;
            *err = so ? OsNetError(so) : os_net_error();
        } else if (ready == 0) {
            *timed_out = true;
            *err = OS_NET_TIMEDOUT;
        } else {
            *err = os_net_error();
        }
        os_sock_close(s);
        if (*timed_out)
            return OS_SOCKET_INVALID; // the deadline is shared: no time left for another address
    }
    return OS_SOCKET_INVALID;
}

} // namespace

HttpGetResult http_get(const std::string &host, uint16_t port, const std::string &target,
                       int timeout_ms, size_t max_body) {
    HttpGetResult r;
    const double deadline = now_ms() + timeout_ms;
    const int seconds = (timeout_ms + 999) / 1000;
    if (host.empty() || !port || target.empty() || target[0] != '/') {
        r.error = "not a usable address";
        return r;
    }
    if (host.find(':') != std::string::npos) {
        r.unresolved = true;
        r.error = "IPv6 addresses are not supported: use the server's IPv4 address or its name";
        return r;
    }

    uint32_t addrs[8];
    const int count = os_net_resolve(host.c_str(), addrs, 8);
    if (count <= 0) {
        r.unresolved = true;
        r.error = "cannot find the host \"" + host + "\"";
        return r;
    }
    OsNetError err = OS_NET_OK;
    bool timed_out = false;
    const OsSocket s =
        connect_any(addrs, count, __builtin_bswap16(port), deadline, &err, &timed_out);
    if (s == OS_SOCKET_INVALID) {
        r.timed_out = timed_out;
        r.error = timed_out ? "no answer within " + std::to_string(seconds) + " s" : describe(err);
        return r;
    }

    std::string authority = host;
    if (port != 80)
        authority += ":" + std::to_string(port);
    const std::string request =
        "GET " + target + " HTTP/1.0\r\nHost: " + authority +
        "\r\nUser-Agent: recomp-kit\r\nAccept: */*\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < request.size()) {
        const int64_t n = os_sock_send(s, request.data() + sent, request.size() - sent, 0);
        if (n > 0) {
            sent += size_t(n);
            continue;
        }
        if (n < 0 && os_net_error() == OS_NET_WOULDBLOCK) {
            const int ready = wait_for(s, OS_POLL_OUT, deadline);
            if (ready == 1)
                continue;
            r.timed_out = ready == 0;
            r.error = ready == 0 ? "no answer within " + std::to_string(seconds) + " s"
                                 : describe(os_net_error());
        } else {
            r.error = describe(n < 0 ? os_net_error() : OS_NET_CONNRESET);
        }
        os_sock_close(s);
        return r;
    }

    // Everything until the server closes, a little over max_body for the headers.
    std::string raw;
    char buf[4096];
    for (;;) {
        const int64_t n = os_sock_recv(s, buf, sizeof buf, 0);
        if (n > 0) {
            raw.append(buf, size_t(n));
            if (raw.size() > max_body + 16384) {
                os_sock_close(s);
                r.error = "the reply is too large";
                return r;
            }
            continue;
        }
        if (n == 0)
            break;
        const OsNetError e = os_net_error();
        if (e == OS_NET_WOULDBLOCK) {
            const int ready = wait_for(s, OS_POLL_IN, deadline);
            if (ready == 1)
                continue;
            r.timed_out = ready == 0;
            r.error = ready == 0 ? "no complete answer within " + std::to_string(seconds) + " s"
                                 : describe(os_net_error());
            os_sock_close(s);
            return r;
        }
        if ((e == OS_NET_CONNRESET || e == OS_NET_CONNABORTED) && !raw.empty())
            break; // some servers reset rather than close once they are done
        r.error = describe(e);
        os_sock_close(s);
        return r;
    }
    os_sock_close(s);
    if (!http_parse_response(raw, max_body, &r) && r.error.empty())
        r.error = raw.empty() ? "the server closed the connection without answering"
                              : "the reply is not HTTP";
    return r;
}
