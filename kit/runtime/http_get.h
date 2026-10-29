// http_get.h - one plain HTTP GET, for libcurl's stand-in (curl_http.cpp).
//
// The one request a Ubisoft Game Service title makes over HTTP is a small
// text file (gsconnect's servers list), so this is HTTP/1.0 with
// "Connection: close": the reply ends where the connection does, and nothing
// here keeps a connection, follows a redirect or speaks TLS. A server that
// answers chunked anyway is read correctly.
//
// http_get.cpp does it over the platform layer's sockets on every platform
// (os_net_resolve, so a name in the hosts file or a literal IPv4 address works
// as well as DNS), with every step - the lookup excepted - bounded by one
// deadline.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

struct HttpGetResult {
    bool ok = false; // a 200 and its whole body
    int status = 0;  // the status code, 0 when no reply arrived
    std::string body;
    // What went wrong, in words for a person: "connection refused", "no
    // answer within 10 s", "HTTP 404 Not Found". Empty when ok.
    std::string error;
    // The failure was the deadline (curl's CURLE_OPERATION_TIMEDOUT), or the
    // name did not resolve (CURLE_COULDNT_RESOLVE_HOST).
    bool timed_out = false, unresolved = false;
};

// GET `target` (the path and query, "/gsinit.php?dp=...") from host:port.
// `host` is a name or a literal address, without brackets. Blocks for at most
// about timeout_ms (a stalled name lookup can take longer: the caller runs
// this on a thread of its own). A body over max_body bytes fails.
HttpGetResult http_get(const std::string &host, uint16_t port, const std::string &target,
                       int timeout_ms, size_t max_body = 1u << 20);

// The reply `raw` (status line, headers, body: everything the connection
// carried) into `out`: status, body (de-chunked) and, for anything but a 200,
// the error. False when it is not an HTTP reply at all, or is cut short.
// Portable, so it lives with curl_http.cpp and the tests reach it on every
// platform.
bool http_parse_response(const std::string &raw, size_t max_body, HttpGetResult *out);
