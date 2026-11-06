// curl_http.cpp - libcurl.dll's stand-in (see curl_http.h): easy handles that
// remember a URL and a write callback, and a perform that answers exactly one
// kind of request - the GS server list - from the server the player chose.
//
// THE WAIT. curl_easy_perform is synchronous. Heroes V calls it when Multi
// Player > Ubi.com is clicked, never at start-up: the click's handler runs
// NUbi::CStateUninitialized::ProcessInit on the game's main thread, which
// initialises sockets and then fetches the list (GetServersConfig) before
// anything else. While the player answers the host's question and while the
// chosen server is tried, that thread sleeps in guest_sleep_ms, which hands
// the baton on: the game's other threads run, and the run thread's idle
// slices keep the window's events coming (sched_wait_locked). The main thread
// presents nothing meanwhile - the original froze on that request too - so
// the host draws its prompt on the frames the presenter repeats
// (page_overlay.h). A game that asks from a worker thread keeps presenting,
// and the prompt rides on its frames as the settings menu does.
//
// The HTTP request runs on a host thread of its own, because a name lookup
// cannot be bounded and the guest thread must keep yielding meanwhile; a
// request the player abandons is left to finish (or time out) on its own.
#include "curl_http.h"
#include "gs_login.h"
#include "http_get.h"

#include "guest.h"
#include "imports.h"
#include "memory.h"
#include "win32.h"
#include "game_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string.h>
#include <thread>

namespace {

enum : uint32_t {
    CURLE_OK = 0,
    CURLE_UNSUPPORTED_PROTOCOL = 1,
    CURLE_FAILED_INIT = 2,
    CURLE_URL_MALFORMAT = 3,
    CURLE_COULDNT_RESOLVE_HOST = 6,
    CURLE_COULDNT_CONNECT = 7,
    CURLE_HTTP_RETURNED_ERROR = 22,
    CURLE_WRITE_ERROR = 23,
    CURLE_OPERATION_TIMEDOUT = 28,
    CURLE_ABORTED_BY_CALLBACK = 42,
    CURLE_BAD_FUNCTION_ARGUMENT = 43,
};

// The options the stand-in keeps; every other one is accepted and ignored.
enum : uint32_t {
    CURLOPT_TIMEOUT = 13,
    CURLOPT_CONNECTTIMEOUT = 78,
    CURLOPT_TIMEOUT_MS = 155,
    CURLOPT_CONNECTTIMEOUT_MS = 156,
    CURLOPT_WRITEDATA = 10001,
    CURLOPT_URL = 10002,
    CURLOPT_WRITEFUNCTION = 20011,
};

// libcurl hands a write callback at most this much at a time.
const uint32_t CURL_MAX_WRITE_SIZE = 16384;

struct Easy {
    std::string url;
    uint32_t write_fn = 0, write_data = 0;
    int timeout_ms = 0; // 0: the stand-in's own
};

// Guest threads only, but one of them may sleep in a perform while another
// makes or frees a handle (the game's telemetry), so a perform holds its
// handle by shared_ptr across the wait.
std::map<uint32_t, std::shared_ptr<Easy>> &handles() {
    static std::map<uint32_t, std::shared_ptr<Easy>> m;
    return m;
}
std::shared_ptr<Easy> find_handle(uint32_t h) {
    auto it = handles().find(h);
    return it == handles().end() ? nullptr : it->second;
}

std::string g_gsconnect = RECOMP_ONLINE_GSCONNECT_HOST;
const CurlServerChooser *g_chooser = nullptr;
int g_timeout_ms = 10000;

std::string lower(std::string s) {
    for (char &ch : s)
        ch = char((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
    return s;
}
bool starts_with_nocase(const std::string &s, const char *prefix) {
    const size_t n = strlen(prefix);
    return s.size() >= n && lower(s.substr(0, n)) == prefix;
}
std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}

// "host" or "[v6]", then ":port" unless it is 80.
std::string authority(const CurlServer &server) {
    const bool v6 = server.host.find(':') != std::string::npos;
    std::string a = v6 ? "[" + server.host + "]" : server.host;
    if (server.port != 80)
        a += ":" + std::to_string(server.port);
    return a;
}

// The path and query of an http URL whose host is `gs`; false for anything else.
bool gsconnect_target(const std::string &url, const std::string &gs, std::string *target) {
    if (gs.empty() || !starts_with_nocase(url, "http://"))
        return false;
    const size_t at = 7;
    size_t end = url.find_first_of("/?#", at);
    const std::string auth =
        url.substr(at, end == std::string::npos ? std::string::npos : end - at);
    const std::string host = auth.substr(0, auth.find(':'));
    if (host.empty() || lower(host) != lower(gs))
        return false;
    std::string rest = end == std::string::npos ? "/" : url.substr(end);
    rest = rest.substr(0, rest.find('#')); // a fragment never goes on the wire
    if (rest.empty() || rest[0] != '/')
        rest = "/" + rest;
    *target = rest;
    return true;
}

// A guest copy of each error text, made once.
uint32_t guest_text(const char *s) {
    static std::map<std::string, uint32_t> copies;
    auto it = copies.find(s);
    if (it != copies.end() && heap_owns(it->second))
        return it->second;
    const uint32_t n = uint32_t(strlen(s)) + 1, a = heap_alloc(n, true);
    if (a)
        gm_put_str(a, s, n);
    copies[s] = a;
    return a;
}

// The body to the guest's write callback, in libcurl's pieces. A callback
// that takes less than it was given stops the transfer, as libcurl's does.
uint32_t deliver(X86 *c, const Easy &e, const std::string &body) {
    if (body.empty())
        return CURLE_OK;
    if (!e.write_fn) {
        LOGW("libcurl: the reply has nowhere to go (no CURLOPT_WRITEFUNCTION)");
        return CURLE_WRITE_ERROR;
    }
    const uint32_t cap = std::min<uint32_t>(uint32_t(body.size()), CURL_MAX_WRITE_SIZE);
    const uint32_t buf = heap_alloc(cap + 1, true);
    if (!buf)
        return CURLE_WRITE_ERROR;
    uint32_t code = CURLE_OK;
    for (size_t at = 0; at < body.size();) {
        const uint32_t n = std::min<uint32_t>(cap, uint32_t(body.size() - at));
        memcpy(g_mem + buf, body.data() + at, n);
        const uint32_t took = guest_call(c, e.write_fn, buf, 1, n, e.write_data);
        if (took != n) {
            code = CURLE_WRITE_ERROR;
            break;
        }
        at += n;
    }
    heap_free(buf);
    return code;
}

// One attempt at the server, on a host thread; the guest thread polls it.
struct Attempt {
    std::mutex mu;
    bool done = false;
    HttpGetResult result;
};

// The gsconnect request: ask, try, ask again with the reason until a server
// answers or the player gives up.
uint32_t gsconnect_request(X86 *c, const Easy &e, const std::string &target) {
    // A login token only for the server this request reaches (gs_login.h).
    gs_login::clear();
    const CurlServerChooser *chooser = g_chooser;
    if (!chooser) {
        LOGW("libcurl: %s is a GS server list, and nothing here can ask which server to use",
             e.url.c_str());
        return CURLE_COULDNT_RESOLVE_HOST;
    }
    std::string error;
    uint32_t last = CURLE_COULDNT_CONNECT;
    for (;;) {
        if (!chooser->begin(error.c_str()))
            return last;
        char buf[512] = {};
        int answer;
        while ((answer = chooser->poll(buf, sizeof buf)) == 0)
            guest_sleep_ms(30);
        if (answer < 0) {
            chooser->end();
            LOGW("libcurl: no server chosen for %s", e.url.c_str());
            return last;
        }
        buf[sizeof buf - 1] = 0;
        CurlServer server;
        std::string name;
        if (!curl_http_parse_server(buf, &server, &name)) {
            error = std::string("\"") + buf + "\" is not a server address";
            continue;
        }
        LOGW("libcurl: %s goes to http://%s%s", e.url.c_str(), authority(server).c_str(),
             target.c_str());

        auto attempt = std::make_shared<Attempt>();
        const int timeout = e.timeout_ms > 0 ? e.timeout_ms : g_timeout_ms;
        std::thread([attempt, server, target, timeout] {
            HttpGetResult r = http_get(server.host, server.port, target, timeout, 1u << 20);
            std::lock_guard lock(attempt->mu);
            attempt->result = std::move(r);
            attempt->done = true;
        }).detach();
        bool cancelled = false;
        for (;;) {
            {
                std::lock_guard lock(attempt->mu);
                if (attempt->done)
                    break;
            }
            if (chooser->poll(buf, sizeof buf) < 0) {
                cancelled = true;
                break;
            }
            guest_sleep_ms(20);
        }
        if (cancelled) {
            chooser->end();
            LOGW("libcurl: %s abandoned", name.c_str());
            return CURLE_ABORTED_BY_CALLBACK;
        }
        HttpGetResult r;
        {
            std::lock_guard lock(attempt->mu);
            r = std::move(attempt->result);
        }
        if (!r.ok) {
            LOGW("libcurl: %s: %s", name.c_str(), r.error.c_str());
            last = r.timed_out    ? CURLE_OPERATION_TIMEDOUT
                   : r.unresolved ? CURLE_COULDNT_RESOLVE_HOST
                   : r.status     ? CURLE_HTTP_RETURNED_ERROR
                                  : CURLE_COULDNT_CONNECT;
            error = name + ": " + r.error;
            continue;
        }
        LOGW("libcurl: %s answered with %zu bytes", name.c_str(), r.body.size());
        // The server's identity and its login nonce, from the same servers.ini
        // the game reads (it looks its own keys up and passes over these).
        const gs_login::ServersIni ini = gs_login::parse_servers_ini(r.body);
        gs_login::set_server(ini.server_id.empty() ? gs_login::address_id(server.host, server.port)
                                                   : ini.server_id,
                             ini.nonce);
        const uint32_t code = deliver(c, e, r.body);
        if (code != CURLE_OK)
            gs_login::clear();
        chooser->end();
        if (code == CURLE_OK && chooser->used)
            chooser->used(name.c_str());
        return code;
    }
}

} // namespace

bool curl_http_parse_server(const std::string &text, CurlServer *out, std::string *canonical) {
    std::string s = trim(text);
    if (starts_with_nocase(s, "http://"))
        s = s.substr(7);
    if (!s.empty() && s.back() == '/')
        s.pop_back();
    if (s.empty() || s.size() > 260)
        return false;
    std::string host, port_text;
    bool has_port = false;
    if (s[0] == '[') {
        const size_t close = s.find(']');
        if (close == std::string::npos)
            return false;
        host = s.substr(1, close - 1);
        const std::string rest = s.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':')
                return false;
            has_port = true;
            port_text = rest.substr(1);
        }
        if (host.find(':') == std::string::npos)
            return false;
        for (char ch : host)
            if (!isxdigit((unsigned char)ch) && ch != ':' && ch != '.')
                return false;
    } else {
        const size_t colon = s.find(':');
        if (colon != std::string::npos && s.find(':', colon + 1) != std::string::npos) {
            host = s; // a bare IPv6 address, which can carry no port
            for (char ch : host)
                if (!isxdigit((unsigned char)ch) && ch != ':' && ch != '.')
                    return false;
        } else {
            host = s.substr(0, colon);
            if (colon != std::string::npos) {
                has_port = true;
                port_text = s.substr(colon + 1);
            }
            for (char ch : host)
                if (!isalnum((unsigned char)ch) && ch != '.' && ch != '-' && ch != '_')
                    return false;
            if (host.front() == '.' || host.front() == '-')
                return false;
        }
    }
    if (host.empty() || host.size() > 253)
        return false;
    unsigned port = 80;
    if (has_port) {
        if (port_text.empty() || port_text.size() > 5)
            return false;
        port = 0;
        for (char ch : port_text) {
            if (ch < '0' || ch > '9')
                return false;
            port = port * 10 + unsigned(ch - '0');
        }
        if (port == 0 || port > 65535)
            return false;
    }
    CurlServer server;
    server.host = lower(host);
    server.port = uint16_t(port);
    if (out)
        *out = server;
    if (canonical)
        *canonical = authority(server);
    return true;
}

std::string curl_http_rewrite(const std::string &url, const std::string &gsconnect_host,
                              const CurlServer &server) {
    std::string target;
    if (server.host.empty() || !gsconnect_target(url, gsconnect_host, &target))
        return "";
    return "http://" + authority(server) + target;
}

void curl_http_set_gsconnect_host(const char *host) {
    g_gsconnect = host ? host : "";
}
const char *curl_http_gsconnect_host() {
    return g_gsconnect.c_str();
}
void curl_http_set_timeout_ms(int ms) {
    g_timeout_ms = ms > 0 ? ms : 10000;
}
void curl_http_set_chooser(const CurlServerChooser *chooser) {
    g_chooser = chooser;
}

// ---------------------------------------------------------------- the imports --

void curl_http_global_init(X86 *c) {
    set_eax(c, CURLE_OK);
}
void curl_http_global_cleanup(X86 *c) {
    set_eax(c, 0);
}

// A handle is a few zeroed guest bytes, so it is a real, distinct guest
// address; what it stands for lives here.
void curl_http_easy_init(X86 *c) {
    const uint32_t h = heap_alloc(16, true);
    if (h)
        handles()[h] = std::make_shared<Easy>();
    set_eax(c, h);
}

// curl_easy_setopt(handle, option, parameter): the parameter is one 32-bit
// argument for every option the game passes (a long, a pointer or a function).
void curl_http_easy_setopt(X86 *c) {
    const std::shared_ptr<Easy> e = find_handle(arg(c, 0));
    if (!e) {
        set_eax(c, CURLE_FAILED_INIT);
        return;
    }
    const uint32_t option = arg(c, 1), value = arg(c, 2);
    switch (option) {
    case CURLOPT_URL:
        e->url = value ? gm_str(value, 4096) : "";
        break;
    case CURLOPT_WRITEFUNCTION:
        e->write_fn = value;
        break;
    case CURLOPT_WRITEDATA:
        e->write_data = value;
        break;
    case CURLOPT_TIMEOUT:
    case CURLOPT_CONNECTTIMEOUT:
        if (value)
            e->timeout_ms = int(std::min<uint32_t>(value, 600) * 1000);
        break;
    case CURLOPT_TIMEOUT_MS:
    case CURLOPT_CONNECTTIMEOUT_MS:
        if (value)
            e->timeout_ms = int(std::min<uint32_t>(value, 600000));
        break;
    default:
        break; // headers, a POST body, verbosity: nothing a refused request needs
    }
    set_eax(c, CURLE_OK);
}

void curl_http_easy_perform(X86 *c) {
    const std::shared_ptr<Easy> e = find_handle(arg(c, 0));
    if (!e) {
        set_eax(c, CURLE_BAD_FUNCTION_ARGUMENT);
        return;
    }
    std::string target;
    if (!gsconnect_target(e->url, g_gsconnect, &target)) {
        log_once("libcurl offline", "libcurl: no network for %s (only a GS server list is fetched)",
                 e->url.c_str());
        set_eax(c, CURLE_COULDNT_CONNECT);
        return;
    }
    set_eax(c, gsconnect_request(c, *e, target));
}

void curl_http_easy_cleanup(X86 *c) {
    const uint32_t h = arg(c, 0);
    if (handles().erase(h))
        heap_free(h);
    set_eax(c, 0);
}

void curl_http_easy_strerror(X86 *c) {
    const char *text;
    switch (arg(c, 0)) {
    case CURLE_OK:
        text = "No error";
        break;
    case CURLE_UNSUPPORTED_PROTOCOL:
        text = "Unsupported protocol";
        break;
    case CURLE_FAILED_INIT:
        text = "Failed initialization";
        break;
    case CURLE_URL_MALFORMAT:
        text = "URL using bad/illegal format or missing URL";
        break;
    case CURLE_COULDNT_RESOLVE_HOST:
        text = "Couldn't resolve host name";
        break;
    case CURLE_COULDNT_CONNECT:
        text = "Couldn't connect to server";
        break;
    case CURLE_HTTP_RETURNED_ERROR:
        text = "HTTP response code said error";
        break;
    case CURLE_WRITE_ERROR:
        text = "Failed writing received data to disk/application";
        break;
    case CURLE_OPERATION_TIMEDOUT:
        text = "Timeout was reached";
        break;
    case CURLE_ABORTED_BY_CALLBACK:
        text = "Operation was aborted by an application callback";
        break;
    case CURLE_BAD_FUNCTION_ARGUMENT:
        text = "A libcurl function was given a bad argument";
        break;
    default:
        text = "Unknown error";
        break;
    }
    set_eax(c, guest_text(text));
}

// No header list is kept: nothing that is sent needs one.
void curl_http_slist_append(X86 *c) {
    set_eax(c, 0);
}
void curl_http_slist_free_all(X86 *c) {
    set_eax(c, 0);
}

// ------------------------------------------------------------ the reply ----

bool http_parse_response(const std::string &raw, size_t max_body, HttpGetResult *out) {
    const size_t head_end = raw.find("\r\n\r\n");
    const size_t lf_end = raw.find("\n\n");
    size_t body_at;
    size_t head_len;
    if (head_end != std::string::npos && (lf_end == std::string::npos || head_end < lf_end)) {
        head_len = head_end;
        body_at = head_end + 4;
    } else if (lf_end != std::string::npos) {
        head_len = lf_end;
        body_at = lf_end + 2;
    } else {
        return false;
    }
    const std::string head = raw.substr(0, head_len);
    // "HTTP/1.x 200 OK"
    if (!starts_with_nocase(head, "http/"))
        return false;
    const size_t sp = head.find(' ');
    if (sp == std::string::npos)
        return false;
    const int status = atoi(head.c_str() + sp + 1);
    if (status < 100 || status > 999)
        return false;
    out->status = status;
    const size_t line_end = head.find('\n');
    const std::string status_line = trim(head.substr(0, line_end));
    const size_t reason_at = status_line.find(' ', sp + 1);
    const std::string reason =
        reason_at == std::string::npos ? "" : trim(status_line.substr(reason_at));

    bool chunked = false;
    long long length = -1;
    for (size_t at = line_end; at != std::string::npos && at < head.size();) {
        const size_t next = head.find('\n', at + 1);
        const std::string line = trim(
            head.substr(at + 1, next == std::string::npos ? std::string::npos : next - at - 1));
        at = next;
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string value = lower(trim(line.substr(colon + 1)));
        if (name == "transfer-encoding" && value.find("chunked") != std::string::npos)
            chunked = true;
        else if (name == "content-length")
            length = atoll(value.c_str());
    }

    std::string body;
    if (chunked) {
        size_t at = body_at;
        for (;;) {
            const size_t eol = raw.find('\n', at);
            if (eol == std::string::npos)
                return false; // cut short
            const unsigned long size = strtoul(raw.c_str() + at, nullptr, 16);
            at = eol + 1;
            if (size == 0)
                break;
            if (raw.size() - at < size)
                return false;
            body.append(raw, at, size);
            if (body.size() > max_body)
                break;
            at += size;
            if (at < raw.size() && raw[at] == '\r')
                ++at;
            if (at < raw.size() && raw[at] == '\n')
                ++at;
        }
    } else {
        body = raw.substr(body_at);
        if (length >= 0) {
            if ((long long)body.size() < length)
                return false; // the connection ended early
            body.resize(size_t(length));
        }
    }
    if (body.size() > max_body) {
        out->error = "the reply is too large";
        return true;
    }
    if (status != 200) {
        out->error = "HTTP " + std::to_string(status) + (reason.empty() ? "" : " " + reason);
        return true;
    }
    out->body = std::move(body);
    out->ok = true;
    out->error.clear();
    return true;
}
