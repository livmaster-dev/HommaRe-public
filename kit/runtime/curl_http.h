// curl_http.h - libcurl.dll's stand-in: the one request that still has
// somewhere to go, and nothing else.
//
// A Ubisoft Game Service (GS) title finds its online services by asking
// gsconnect - for Heroes V http://gsconnect.ubisoft.com/gsinit.php?dp=... -
// for an ini file naming the router, IRC, CD-key and NAT servers by address;
// everything after that goes to those addresses directly, over Winsock.
// Ubisoft shut those services down (2015 for Heroes V); community servers
// such as ubi-gs (github.com/michal-kapala/ubi-gs) answer the same requests.
//
// So a request whose host is the game's gsconnect host (game.toml [online]
// gsconnect_host) goes to the server the player chooses instead - the host
// asks (host/server_prompt.cpp) while the game's own thread waits here - over
// plain HTTP (http_get.h), with the path and query kept as the game wrote
// them. Ubisoft's own domain is never contacted. Every other URL fails as it
// always has, offline, and so does gsconnect when the game names no
// gsconnect host, the host cannot ask, or the player cancels.
//
// The servers.ini that a chosen server answered with also names that server
// for the game's login (its ServerID and LoginNonce, else its address): the
// password the game then sends is a token derived from it (gs_login.h).
//
// The game sets a write callback (CURLOPT_WRITEFUNCTION) and its cookie
// (CURLOPT_WRITEDATA) and nothing else that matters; the reply's body goes to
// that callback in a guest buffer, as libcurl would hand it over.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

struct X86;

// The libcurl imports, all cdecl, for legacy_dlls.cpp's table.
void curl_http_global_init(X86 *c);
void curl_http_global_cleanup(X86 *c);
void curl_http_easy_init(X86 *c);
void curl_http_easy_setopt(X86 *c);
void curl_http_easy_perform(X86 *c);
void curl_http_easy_cleanup(X86 *c);
void curl_http_easy_strerror(X86 *c);
void curl_http_slist_append(X86 *c);
void curl_http_slist_free_all(X86 *c);

// A server as the player names it: "host", "host:port", "[v6 address]" or
// "[v6 address]:port", optionally after "http://" and before a trailing "/".
// The port is 80 unless given. `canonical` (may be null) is how it is shown
// and stored: lower case, no scheme, no ":80".
struct CurlServer {
    std::string host; // without brackets
    uint16_t port = 80;
};
bool curl_http_parse_server(const std::string &text, CurlServer *out, std::string *canonical);

// Where a request for `url` goes instead: `url` with its scheme, host and port
// replaced by the server's, path and query kept. "" when url is not an http
// URL whose host is `gsconnect_host` (compared without case), or either is empty.
std::string curl_http_rewrite(const std::string &url, const std::string &gsconnect_host,
                              const CurlServer &server);

// The host that is asked about (game.toml [online] gsconnect_host by default;
// "" turns the redirection off). For tests and for a host that knows better.
void curl_http_set_gsconnect_host(const char *host);
const char *curl_http_gsconnect_host();
// How long one attempt may take, in milliseconds (default 10000).
void curl_http_set_timeout_ms(int ms);

// The host's half: whoever asks the player which server. Called on the guest
// thread that made the request (Heroes V's main thread), which waits until
// poll answers, sleeping with the baton handed on so the game's other threads
// and the window's events keep going.
struct CurlServerChooser {
    // Shows the question, or shows it again with why the last answer failed
    // (`error`, "" the first time). False when this host cannot ask.
    bool (*begin)(const char *error);
    // 0 while the player decides, 1 when they chose (the canonical server in
    // buf), -1 when they cancelled. Still asked while that server is being
    // tried, so the player can cancel a slow one.
    int (*poll)(char *buf, size_t cap);
    // The question is over: the server answered, or the request is abandoned.
    void (*end)(void);
    // That server answered: the host remembers it for next time.
    void (*used)(const char *server);
};
// Null uninstalls: gsconnect then fails at once, as it did offline.
void curl_http_set_chooser(const CurlServerChooser *chooser);
