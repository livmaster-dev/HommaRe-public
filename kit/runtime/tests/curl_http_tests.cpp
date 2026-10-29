// curl_http_tests.cpp - libcurl's stand-in (curl_http.h) and its HTTP GET
// (http_get.h): server addresses as a player types them, the gsconnect URL's
// rewrite, the reply parser, real requests to a server on 127.0.0.1, and the
// imports end to end - a chooser that answers, a guest write callback that
// receives the body - with no game image.
//
// Run from the repository root:
//   .venv/bin/python tools/test.py --compile-only && build/recomp/curl_http_tests
#include "../curl_http.h"
#include "../http_get.h"
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../win32.h"
#include "../../platform/os.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
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
static void section(const char *s) {
    printf("\n== %s\n", s);
}

// ---------------------------------------------------------------------------
// A one-shot HTTP server on 127.0.0.1, on the platform layer's sockets like
// the client, so the suite runs wherever the kit does: it accepts
// `connections` connections and answers each with `reply` (or, with reply
// empty, never answers).
// ---------------------------------------------------------------------------
static const uint32_t kLoopback = __builtin_bswap32(0x7f000001); // network order

struct TestServer {
    OsSocket fd = OS_SOCKET_INVALID;
    uint16_t port = 0;
    std::thread thread;
    std::string last_request;
    std::atomic<int> served{0};
    std::atomic<bool> stopping{false};

    // Waits until `s` can do `events`; false once the server is stopping.
    bool wait(OsSocket s, int events) {
        while (!stopping) {
            OsSockPoll p{s, events, 0};
            if (os_sock_poll(&p, 1, 20) != 0)
                return true;
        }
        return false;
    }
    // recv, waiting while there is nothing yet: >0 bytes, 0 closed, -1 failed or stopping.
    int64_t receive(OsSocket c, char *buf, size_t cap) {
        for (;;) {
            const int64_t n = os_sock_recv(c, buf, cap, 0);
            if (n >= 0 || os_net_error() != OS_NET_WOULDBLOCK)
                return n;
            if (!wait(c, OS_POLL_IN))
                return -1;
        }
    }
    void serve(const std::string &reply, int connections) {
        for (int i = 0; i < connections; ++i) {
            OsSocket c = OS_SOCKET_INVALID;
            while (c == OS_SOCKET_INVALID) {
                if (!wait(fd, OS_POLL_IN))
                    return;
                c = os_sock_accept(fd, nullptr, nullptr);
            }
            std::string req;
            char buf[1024];
            while (req.find("\r\n\r\n") == std::string::npos) {
                const int64_t n = receive(c, buf, sizeof buf);
                if (n <= 0)
                    break;
                req.append(buf, size_t(n));
            }
            last_request = req;
            if (reply.empty()) {
                // Never answer: hold the connection until the client gives up.
                while (receive(c, buf, sizeof buf) > 0) {
                }
            } else {
                for (size_t sent = 0; sent < reply.size();) {
                    const int64_t n = os_sock_send(c, reply.data() + sent, reply.size() - sent, 0);
                    if (n > 0)
                        sent += size_t(n);
                    else if (n == 0 || os_net_error() != OS_NET_WOULDBLOCK || !wait(c, OS_POLL_OUT))
                        break;
                }
            }
            os_sock_close(c);
            ++served;
        }
    }

    bool start(const std::string &reply, int connections = 1) {
        fd = os_sock_open(OS_SOCK_STREAM);
        if (fd == OS_SOCKET_INVALID)
            return false;
        uint16_t p = 0;
        if (os_sock_bind(fd, kLoopback, 0) != 0 || os_sock_listen(fd, 4) != 0 ||
            os_sock_name(fd, nullptr, &p) != 0)
            return false;
        port = __builtin_bswap16(p);
        thread = std::thread([this, reply, connections] { serve(reply, connections); });
        return true;
    }
    void stop() {
        stopping = true;
        if (thread.joinable())
            thread.join();
        if (fd != OS_SOCKET_INVALID) {
            os_sock_close(fd);
            fd = OS_SOCKET_INVALID;
        }
    }
    ~TestServer() {
        stop();
    }
};

// A port nothing listens on: bound, then let go.
static uint16_t closed_port() {
    const OsSocket fd = os_sock_open(OS_SOCK_STREAM);
    uint16_t p = 0;
    os_sock_bind(fd, kLoopback, 0);
    os_sock_name(fd, nullptr, &p);
    os_sock_close(fd);
    return __builtin_bswap16(p);
}

static const char *kIni = "[Servers]\r\nRouterIP0=127.0.0.1\r\nRouterPort0=7777\r\n";
static std::string ok_reply(const std::string &body) {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

static void test_parse_server() {
    section("server addresses as typed");
    struct Case {
        const char *text, *host;
        unsigned port;
        const char *canonical;
    } good[] = {
        {"127.0.0.1", "127.0.0.1", 80, "127.0.0.1"},
        {"  127.0.0.1:8080 ", "127.0.0.1", 8080, "127.0.0.1:8080"},
        {"Play.Example.ORG:80", "play.example.org", 80, "play.example.org"},
        {"http://play.example.org:8099/", "play.example.org", 8099, "play.example.org:8099"},
        {"HTTP://localhost", "localhost", 80, "localhost"},
        {"my_server.lan:65535", "my_server.lan", 65535, "my_server.lan:65535"},
        {"[::1]:9000", "::1", 9000, "[::1]:9000"},
        {"[2001:db8::7]", "2001:db8::7", 80, "[2001:db8::7]"},
        {"fe80::1", "fe80::1", 80, "[fe80::1]"},
    };
    for (const Case &k : good) {
        CurlServer s;
        std::string canonical;
        const bool ok = curl_http_parse_server(k.text, &s, &canonical);
        check(ok && s.host == k.host && s.port == k.port && canonical == k.canonical,
              "\"%s\" -> host %s port %u, shown as %s (got %d: %s %u %s)", k.text, k.host, k.port,
              k.canonical, int(ok), s.host.c_str(), unsigned(s.port), canonical.c_str());
    }
    const char *bad[] = {"",           "   ",         "host:",         "host:0", "host:65536",
                         "host:8o",    "host:123456", "https://x.org", "a b",    "host/path",
                         "[::1",       "[::1]x",      "[host]:80",     "-host",  "user@host",
                         "host:80:90", ".host",       "ftp://x"};
    for (const char *t : bad)
        check(!curl_http_parse_server(t, nullptr, nullptr), "\"%s\" is not a server", t);
}

static void test_rewrite() {
    section("the gsconnect URL, rewritten");
    const std::string gs = "gsconnect.ubisoft.com";
    const std::string url = "http://gsconnect.ubisoft.com/gsinit.php?dp=HEROES_0123456789abcdef";
    CurlServer local{"127.0.0.1", 8099}, plain{"play.example.org", 80}, v6{"::1", 8080};
    check(curl_http_rewrite(url, gs, local) ==
              "http://127.0.0.1:8099/gsinit.php?dp=HEROES_0123456789abcdef",
          "host and port replaced, path and query kept");
    check(curl_http_rewrite(url, gs, plain) ==
              "http://play.example.org/gsinit.php?dp=HEROES_0123456789abcdef",
          "port 80 is left out");
    check(curl_http_rewrite(url, gs, v6) ==
              "http://[::1]:8080/gsinit.php?dp=HEROES_0123456789abcdef",
          "an IPv6 server goes in brackets");
    check(curl_http_rewrite("HTTP://GSConnect.Ubisoft.COM/gsinit.php?dp=1", gs, local) ==
              "http://127.0.0.1:8099/gsinit.php?dp=1",
          "scheme and host compared without case");
    check(curl_http_rewrite("http://gsconnect.ubisoft.com:80/gsinit.php", gs, local) ==
              "http://127.0.0.1:8099/gsinit.php",
          "the original's own port is dropped");
    check(curl_http_rewrite("http://gsconnect.ubisoft.com?dp=1", gs, local) ==
              "http://127.0.0.1:8099/?dp=1",
          "a query with no path gets one");
    check(curl_http_rewrite("http://gsconnect.ubisoft.com/x#frag", gs, local) ==
              "http://127.0.0.1:8099/x",
          "a fragment is not sent");
    check(curl_http_rewrite("http://example.com/gsinit.php", gs, local).empty(),
          "another host is not rewritten");
    check(curl_http_rewrite("http://gsconnect.ubisoft.com.example.org/", gs, local).empty(),
          "a longer host that starts the same is not gsconnect");
    check(curl_http_rewrite("https://gsconnect.ubisoft.com/gsinit.php", gs, local).empty(),
          "https is not rewritten");
    check(curl_http_rewrite(url, "", local).empty(), "no gsconnect host: nothing is rewritten");
    check(curl_http_rewrite(url, gs, CurlServer{}).empty(), "no server: nothing is rewritten");
}

static void test_parse_response() {
    section("HTTP replies");
    HttpGetResult r;
    check(http_parse_response(ok_reply(kIni), 1 << 20, &r) && r.ok && r.status == 200 &&
              r.body == kIni,
          "200 with Content-Length");
    r = {};
    check(http_parse_response("HTTP/1.0 200 OK\r\n\r\nbody to the end", 1 << 20, &r) && r.ok &&
              r.body == "body to the end",
          "200 without a length: the body runs to the close");
    r = {};
    check(http_parse_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n"
                              "7;ext=1\r\n, world\r\n0\r\n\r\n",
                              1 << 20, &r) &&
              r.ok && r.body == "hello, world",
          "chunked, with an extension");
    r = {};
    check(http_parse_response("HTTP/1.1 404 NOT FOUND\r\nContent-Length: 9\r\n\r\nnot found",
                              1 << 20, &r) &&
              !r.ok && r.status == 404 && r.error == "HTTP 404 NOT FOUND",
          "404 is an error naming the status (%s)", r.error.c_str());
    r = {};
    check(!http_parse_response("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort", 1 << 20, &r),
          "a body shorter than its length is cut short");
    r = {};
    check(!http_parse_response("SSH-2.0-OpenSSH\r\n\r\n", 1 << 20, &r), "not HTTP");
    r = {};
    check(!http_parse_response("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n", 1 << 20, &r),
          "headers never ended");
    r = {};
    check(http_parse_response(ok_reply(std::string(100, 'x')), 10, &r) && !r.ok,
          "a body over the limit fails");
}

static void test_http_get() {
    section("HTTP GET over sockets");
    {
        TestServer server;
        check(server.start(ok_reply(kIni)), "test server on 127.0.0.1");
        const HttpGetResult r =
            http_get("127.0.0.1", server.port, "/gsinit.php?dp=HEROES_0123456789abcdef", 5000);
        server.stop();
        check(r.ok && r.body == kIni, "the body arrives (%s)", r.error.c_str());
        check(server.last_request.rfind("GET /gsinit.php?dp=HEROES_0123456789abcdef HTTP/1.0\r\n",
                                        0) == 0,
              "request line as the game's URL had it");
        check(server.last_request.find("\r\nHost: 127.0.0.1:" + std::to_string(server.port) +
                                       "\r\n") != std::string::npos,
              "Host names the server and its port");
    }
    {
        // A name from the hosts file, resolved to its IPv4 address.
        TestServer server;
        server.start(ok_reply("x"));
        const HttpGetResult r = http_get("localhost", server.port, "/", 5000);
        server.stop();
        check(r.ok && r.body == "x", "a name resolves (%s)", r.error.c_str());
    }
    {
        const HttpGetResult r = http_get("::1", 8080, "/", 3000);
        check(!r.ok && r.unresolved && r.error.find("IPv6") != std::string::npos,
              "an IPv6 address is refused with a reason (%s)", r.error.c_str());
    }
    {
        const HttpGetResult r = http_get("no-such-host.invalid", 80, "/", 3000);
        check(!r.ok && r.unresolved && r.error.find("no-such-host.invalid") != std::string::npos,
              "an unknown name does not resolve (%s)", r.error.c_str());
    }
    {
        // Windows retries a refused connect for about two seconds.
        const HttpGetResult r = http_get("127.0.0.1", closed_port(), "/", 5000);
        check(!r.ok && !r.timed_out && r.error.find("refused") != std::string::npos,
              "nothing listening: refused (%s)", r.error.c_str());
    }
    {
        TestServer server;
        server.start(""); // accepts and never answers
        const uint64_t t0 = os_monotonic_ns();
        const HttpGetResult r = http_get("127.0.0.1", server.port, "/", 400);
        const double took = double(os_monotonic_ns() - t0) / 1e6;
        server.stop();
        check(!r.ok && r.timed_out && took < 3000, "a silent server times out (%.0f ms: %s)", took,
              r.error.c_str());
    }
    {
        TestServer server;
        server.start("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n");
        const HttpGetResult r = http_get("127.0.0.1", server.port, "/", 3000);
        server.stop();
        check(!r.ok && r.status == 500 && r.error.find("500") != std::string::npos,
              "a 500 is an error (%s)", r.error.c_str());
    }
}

// ---------------------------------------------------------------------------
// The imports end to end: a guest write callback (a registered shim standing
// in for the game's own function) and a scripted chooser.
// ---------------------------------------------------------------------------
static uint32_t g_fake_ret = 0;
static uint32_t call_import(X86 *c, const char *dll, const char *name,
                            const std::vector<uint32_t> &args) {
    const uint32_t tramp = imports_resolve(dll, name);
    if (!tramp) {
        check(false, "a trampoline for %s!%s", dll, name);
        return 0;
    }
    const uint32_t before = c->r[R_ESP];
    uint32_t esp = before;
    for (size_t i = args.size(); i-- > 0;) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, g_fake_ret);
    c->r[R_ESP] = esp;
    imports_dispatch(c, tramp);
    c->r[R_ESP] = before;
    return c->r[R_EAX];
}

static std::string g_written;
static uint32_t g_write_cookie = 0, g_write_calls = 0;
static void test_write(X86 *c) {
    const uint32_t ptr = arg(c, 0), size = arg(c, 1), n = arg(c, 2);
    g_write_cookie = arg(c, 3);
    ++g_write_calls;
    g_written.append((const char *)g_mem + ptr, size_t(size) * n);
    set_eax(c, size * n);
}

struct Script {
    std::vector<std::string> answers; // one per begin; "" cancels
    std::vector<std::string> errors;  // what each begin was told
    std::vector<std::string> used;
    size_t asked = 0;
    bool pending = false;
    int ends = 0;
} g_script;
static bool script_begin(const char *error) {
    g_script.errors.push_back(error ? error : "");
    g_script.pending = true;
    return true;
}
static int script_poll(char *buf, size_t cap) {
    if (!g_script.pending)
        return 0; // being tried
    g_script.pending = false;
    if (g_script.asked >= g_script.answers.size() || g_script.answers[g_script.asked].empty()) {
        ++g_script.asked;
        return -1;
    }
    snprintf(buf, cap, "%s", g_script.answers[g_script.asked++].c_str());
    return 1;
}
static void script_end() {
    ++g_script.ends;
}
static void script_used(const char *s) {
    g_script.used.push_back(s);
}
static const CurlServerChooser kScript = {script_begin, script_poll, script_end, script_used};

static void test_imports() {
    section("libcurl's imports");
    static const ImportShim write_shim[] = {{"curltest.dll", "write", ARGC_CDECL, test_write}};
    imports_register(write_shim, 1);
    const uint32_t write_fn = imports_resolve("curltest.dll", "write");
    X86 cpu;
    X86 *c = &cpu;
    loader_init_context(c);
    curl_http_set_gsconnect_host("gsconnect.ubisoft.com");
    curl_http_set_timeout_ms(3000);

    const uint32_t url = heap_alloc(256, true);
    gm_put_str(url, "http://gsconnect.ubisoft.com/gsinit.php?dp=HEROES_0123456789abcdef", 256);
    auto easy = [&](uint32_t u) {
        const uint32_t h = call_import(c, "libcurl.dll", "curl_easy_init", {});
        check(h != 0, "curl_easy_init gives a handle");
        check(call_import(c, "libcurl.dll", "curl_easy_setopt", {h, 20011, write_fn}) == 0 &&
                  call_import(c, "libcurl.dll", "curl_easy_setopt", {h, 10001, 0xc00c1e}) == 0 &&
                  call_import(c, "libcurl.dll", "curl_easy_setopt", {h, 10002, u}) == 0,
              "the game's three options are taken");
        return h;
    };
    check(call_import(c, "libcurl.dll", "curl_global_init", {3}) == 0, "curl_global_init");

    // No chooser: the request fails at once, as it did offline.
    curl_http_set_chooser(nullptr);
    uint32_t h = easy(url);
    check(call_import(c, "libcurl.dll", "curl_easy_perform", {h}) == 6,
          "nobody to ask: couldn't resolve");
    call_import(c, "libcurl.dll", "curl_easy_cleanup", {h});

    // The player names a server that refuses, then one that answers.
    TestServer server;
    server.start(ok_reply(kIni));
    const std::string good = "127.0.0.1:" + std::to_string(server.port);
    const std::string refused = "127.0.0.1:" + std::to_string(closed_port());
    g_script = {};
    g_script.answers = {refused, good};
    curl_http_set_chooser(&kScript);
    g_written.clear();
    h = easy(url);
    const uint32_t code = call_import(c, "libcurl.dll", "curl_easy_perform", {h});
    server.stop();
    check(code == 0, "perform succeeds (%u)", code);
    check(g_script.errors.size() == 2 && g_script.errors[0].empty() &&
              g_script.errors[1].find(refused) == 0 &&
              g_script.errors[1].find("refused") != std::string::npos,
          "the refusal is shown when it asks again (\"%s\")",
          g_script.errors.size() > 1 ? g_script.errors[1].c_str() : "");
    check(server.last_request.rfind("GET /gsinit.php?dp=HEROES_0123456789abcdef ", 0) == 0,
          "the server got the game's path and query");
    check(g_written == kIni && g_write_cookie == 0xc00c1e && g_write_calls == 1,
          "the body reaches the write callback with its cookie");
    check(g_script.used.size() == 1 && g_script.used[0] == good,
          "the server that answered is reported used");
    check(g_script.ends == 1, "the question ends once");
    call_import(c, "libcurl.dll", "curl_easy_cleanup", {h});

    // Cancelled: an error, nothing written, nothing remembered.
    g_script = {};
    g_script.answers = {""};
    g_written.clear();
    h = easy(url);
    check(call_import(c, "libcurl.dll", "curl_easy_perform", {h}) == 7 && g_written.empty() &&
              g_script.used.empty(),
          "cancelled: couldn't connect, nothing written");
    call_import(c, "libcurl.dll", "curl_easy_cleanup", {h});

    // Anything else stays offline and never asks.
    g_script = {};
    const uint32_t other = heap_alloc(64, true);
    gm_put_str(other, "http://stats.example.com/report", 64);
    h = easy(other);
    check(call_import(c, "libcurl.dll", "curl_easy_perform", {h}) == 7 && g_script.errors.empty(),
          "another URL: couldn't connect, nobody asked");
    const uint32_t text = call_import(c, "libcurl.dll", "curl_easy_strerror", {7});
    check(text && gm_str(text) == "Couldn't connect to server", "curl_easy_strerror(7)");
    check(call_import(c, "libcurl.dll", "curl_easy_perform", {0x1234}) == 43, "an unknown handle");
    check(call_import(c, "libcurl.dll", "curl_easy_setopt", {0, 10002, other}) == 2,
          "setopt on NULL");
    curl_http_set_chooser(nullptr);
}

int main() {
    mem_init();
    imports_init();
    test_parse_server();
    test_rewrite();
    test_parse_response();
    test_http_get();
    test_imports();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    mem_shutdown();
    return g_failures ? 1 : 0;
}
