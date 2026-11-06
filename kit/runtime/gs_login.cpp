// gs_login.cpp - the password a GS title sends to a community server the
// player chose: a token derived from it, or a response to the server's
// nonce (gs_login.h).
#include "gs_login.h"
#include "guest.h"
#include "layout.h"
#include "native_seam.h"
#include "sha256.h"
#include "../platform/os.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

namespace gs_login {

// ------------------------------------------------------------ primitives --

void sha256(const void *data, size_t n, uint8_t out[32]) {
    Sha256 s;
    s.update(static_cast<const uint8_t *>(data), n);
    s.digest(out);
}

void hmac_sha256(const void *key, size_t key_len, const void *msg, size_t msg_len,
                 uint8_t out[32]) {
    uint8_t k[64] = {};
    if (key_len > 64)
        sha256(key, key_len, k);
    else if (key_len)
        memcpy(k, key, key_len);
    uint8_t pad[64];
    for (int i = 0; i < 64; ++i)
        pad[i] = uint8_t(k[i] ^ 0x36);
    Sha256 inner;
    inner.update(pad, 64);
    inner.update(static_cast<const uint8_t *>(msg), msg_len);
    uint8_t ih[32];
    inner.digest(ih);
    for (int i = 0; i < 64; ++i)
        pad[i] = uint8_t(k[i] ^ 0x5c);
    Sha256 outer;
    outer.update(pad, 64);
    outer.update(ih, 32);
    outer.digest(out);
    memset(k, 0, sizeof k);
}

void pbkdf2_sha256(const void *pw, size_t pw_len, const void *salt, size_t salt_len,
                   uint32_t iterations, uint8_t *out, size_t out_len) {
    std::vector<uint8_t> msg(static_cast<const uint8_t *>(salt),
                             static_cast<const uint8_t *>(salt) + salt_len);
    msg.resize(salt_len + 4);
    for (uint32_t block = 1; out_len; ++block) {
        msg[salt_len] = uint8_t(block >> 24);
        msg[salt_len + 1] = uint8_t(block >> 16);
        msg[salt_len + 2] = uint8_t(block >> 8);
        msg[salt_len + 3] = uint8_t(block);
        uint8_t u[32], t[32];
        hmac_sha256(pw, pw_len, msg.data(), msg.size(), u);
        memcpy(t, u, 32);
        for (uint32_t i = 1; i < iterations; ++i) {
            hmac_sha256(pw, pw_len, u, 32, u);
            for (int j = 0; j < 32; ++j)
                t[j] ^= u[j];
        }
        const size_t take = out_len < 32 ? out_len : 32;
        memcpy(out, t, take);
        out += take;
        out_len -= take;
    }
}

namespace {

inline uint32_t rotl(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

// Salsa20/8's core on 16 words, in place.
void salsa20_8(uint32_t b[16]) {
    uint32_t x[16];
    memcpy(x, b, sizeof x);
    for (int i = 0; i < 8; i += 2) {
        x[4] ^= rotl(x[0] + x[12], 7);
        x[8] ^= rotl(x[4] + x[0], 9);
        x[12] ^= rotl(x[8] + x[4], 13);
        x[0] ^= rotl(x[12] + x[8], 18);
        x[9] ^= rotl(x[5] + x[1], 7);
        x[13] ^= rotl(x[9] + x[5], 9);
        x[1] ^= rotl(x[13] + x[9], 13);
        x[5] ^= rotl(x[1] + x[13], 18);
        x[14] ^= rotl(x[10] + x[6], 7);
        x[2] ^= rotl(x[14] + x[10], 9);
        x[6] ^= rotl(x[2] + x[14], 13);
        x[10] ^= rotl(x[6] + x[2], 18);
        x[3] ^= rotl(x[15] + x[11], 7);
        x[7] ^= rotl(x[3] + x[15], 9);
        x[11] ^= rotl(x[7] + x[3], 13);
        x[15] ^= rotl(x[11] + x[7], 18);
        x[1] ^= rotl(x[0] + x[3], 7);
        x[2] ^= rotl(x[1] + x[0], 9);
        x[3] ^= rotl(x[2] + x[1], 13);
        x[0] ^= rotl(x[3] + x[2], 18);
        x[6] ^= rotl(x[5] + x[4], 7);
        x[7] ^= rotl(x[6] + x[5], 9);
        x[4] ^= rotl(x[7] + x[6], 13);
        x[5] ^= rotl(x[4] + x[7], 18);
        x[11] ^= rotl(x[10] + x[9], 7);
        x[8] ^= rotl(x[11] + x[10], 9);
        x[9] ^= rotl(x[8] + x[11], 13);
        x[10] ^= rotl(x[9] + x[8], 18);
        x[12] ^= rotl(x[15] + x[14], 7);
        x[13] ^= rotl(x[12] + x[15], 9);
        x[14] ^= rotl(x[13] + x[12], 13);
        x[15] ^= rotl(x[14] + x[13], 18);
    }
    for (int i = 0; i < 16; ++i)
        b[i] += x[i];
}

// scryptBlockMix on 2r 64-byte blocks: b in, y out (both 32r words).
void block_mix(const uint32_t *b, uint32_t *y, uint32_t r) {
    uint32_t x[16];
    memcpy(x, b + (2 * r - 1) * 16, 64);
    for (uint32_t i = 0; i < 2 * r; ++i) {
        for (int j = 0; j < 16; ++j)
            x[j] ^= b[i * 16 + j];
        salsa20_8(x);
        // even blocks to the first half, odd ones to the second
        memcpy(y + ((i & 1) * r + i / 2) * 16, x, 64);
    }
}

} // namespace

bool scrypt(const void *pw, size_t pw_len, const void *salt, size_t salt_len, uint32_t n,
            uint32_t r, uint32_t p, uint8_t *out, size_t out_len) {
    if (n < 2 || (n & (n - 1)) || r == 0 || p == 0 || r > 64 || p > 16 || n > (1u << 22))
        return false;
    const size_t words = size_t(32) * r; // one 128r-byte block
    std::vector<uint8_t> b;
    std::vector<uint32_t> v, x, y;
    // A 32-bit size_t (wasm32) cannot hold 128·r·N bytes at the top of the
    // range; the product would wrap and leave v too small.
    if (n > v.max_size() / words)
        return false;
    try {
        b.resize(size_t(128) * r * p);
        v.resize(words * n);
        x.resize(words);
        y.resize(words);
    } catch (const std::bad_alloc &) {
        return false;
    }
    pbkdf2_sha256(pw, pw_len, salt, salt_len, 1, b.data(), b.size());
    for (uint32_t k = 0; k < p; ++k) {
        uint8_t *bk = b.data() + size_t(128) * r * k;
        for (size_t i = 0; i < words; ++i)
            x[i] = uint32_t(bk[4 * i]) | uint32_t(bk[4 * i + 1]) << 8 |
                   uint32_t(bk[4 * i + 2]) << 16 | uint32_t(bk[4 * i + 3]) << 24;
        for (uint32_t i = 0; i < n; ++i) {
            memcpy(&v[words * i], x.data(), words * 4);
            block_mix(x.data(), y.data(), r);
            x.swap(y);
        }
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t j = x[(2 * r - 1) * 16] & (n - 1);
            for (size_t w = 0; w < words; ++w)
                x[w] ^= v[words * j + w];
            block_mix(x.data(), y.data(), r);
            x.swap(y);
        }
        for (size_t i = 0; i < words; ++i)
            for (int s = 0; s < 4; ++s)
                bk[4 * i + s] = uint8_t(x[i] >> (8 * s));
    }
    pbkdf2_sha256(pw, pw_len, b.data(), b.size(), 1, out, out_len);
    memset(b.data(), 0, b.size());
    memset(v.data(), 0, v.size() * 4);
    return true;
}

std::string base64url(const uint8_t *data, size_t n) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string s;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        acc = (acc << 8) | data[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            s += digits[(acc >> bits) & 63];
        }
    }
    if (bits)
        s += digits[(acc << (6 - bits)) & 63];
    return s;
}

// ----------------------------------------------------------- the formats --

namespace {

std::string lower_ascii(std::string s) {
    for (char &ch : s)
        if (ch >= 'A' && ch <= 'Z')
            ch = char(ch - 'A' + 'a');
    return s;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}

std::string hex_value(const std::string &text) {
    const std::string v = lower_ascii(trim(text));
    if (v.size() < 16 || v.size() > 64)
        return "";
    for (char ch : v)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return "";
    return v;
}

const char VERIFIER_LABEL[] = "ubi-gs verifier v1";
const char RECORD_LABEL[] = "ubi-gs record v1";

} // namespace

std::string token(const std::string &password, const std::string &server_id,
                  const std::string &name) {
    std::string salt("ubi-gs login v1", 15);
    salt += '\0';
    salt += server_id;
    salt += '\0';
    salt += lower_ascii(name);
    uint8_t key[TOKEN_BYTES];
    if (!scrypt(password.data(), password.size(), salt.data(), salt.size(), SCRYPT_N, SCRYPT_R,
                SCRYPT_P, key, sizeof key))
        return "";
    const std::string t = base64url(key, sizeof key);
    memset(key, 0, sizeof key);
    return t;
}

void verifier(const std::string &token, uint8_t v[32]) {
    hmac_sha256(token.data(), token.size(), VERIFIER_LABEL, sizeof VERIFIER_LABEL - 1, v);
}

std::string response(const uint8_t v[32], const std::string &nonce) {
    uint8_t mac[32];
    hmac_sha256(v, 32, nonce.data(), nonce.size(), mac);
    return "~1:" + base64url(mac, TOKEN_BYTES);
}

std::string fingerprint(const std::string &token) {
    uint8_t mac[32];
    hmac_sha256(token.data(), token.size(), RECORD_LABEL, sizeof RECORD_LABEL - 1, mac);
    char hex[9];
    snprintf(hex, sizeof hex, "%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3]);
    return hex;
}

ServersIni parse_servers_ini(const std::string &body) {
    ServersIni out;
    bool in_servers = false;
    size_t at = 0;
    while (at < body.size()) {
        size_t end = body.find('\n', at);
        if (end == std::string::npos)
            end = body.size();
        const std::string line = trim(body.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line[0] == '[') {
            const size_t close = line.find(']');
            in_servers = close != std::string::npos &&
                         lower_ascii(trim(line.substr(1, close - 1))) == "servers";
            continue;
        }
        const size_t eq = line.find('=');
        if (!in_servers || eq == std::string::npos)
            continue;
        const std::string key = lower_ascii(trim(line.substr(0, eq)));
        // the first of each counts, as GetPrivateProfileString reads it
        if (key == "serverid" && out.server_id.empty())
            out.server_id = hex_value(line.substr(eq + 1));
        else if (key == "loginnonce" && out.nonce.empty())
            out.nonce = hex_value(line.substr(eq + 1));
    }
    return out;
}

std::string address_id(const std::string &host, uint16_t port) {
    const std::string h = lower_ascii(host);
    return (h.find(':') != std::string::npos ? "[" + h + "]" : h) + ":" + std::to_string(port);
}

std::vector<Record> load_records(const std::string &path) {
    std::vector<Record> out;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return out;
    char line[512];
    while (fgets(line, sizeof line, f) && out.size() < 1000) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#')
            continue;
        const size_t a = s.find('\t'), b = a == std::string::npos ? a : s.find('\t', a + 1);
        if (b == std::string::npos)
            continue;
        Record r{s.substr(0, a), s.substr(a + 1, b - a - 1), s.substr(b + 1)};
        if (r.server_id.empty() || r.name.empty() || r.fp.size() != 8)
            continue;
        out.push_back(r);
    }
    fclose(f);
    return out;
}

bool save_records(const std::string &path, const std::vector<Record> &records) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0)
        os_mkdir(path.substr(0, slash).c_str());
    const std::string tmp = path + ".new";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    fputs("# ubi-gs servers this HommaRe registered a name with: server id, name, "
          "token fingerprint\n",
          f);
    for (const Record &r : records)
        fprintf(f, "%s\t%s\t%s\n", r.server_id.c_str(), r.name.c_str(), r.fp.c_str());
    const bool ok = fflush(f) == 0;
    fclose(f);
    if (!ok || os_rename(tmp.c_str(), path.c_str()) != 0) {
        os_unlink(tmp.c_str());
        return false;
    }
    return true;
}

// ------------------------------------------------------------ the session --

namespace {

const size_t MAX_RECORDS = 200;

struct Session {
    std::mutex mu;
    bool active = false;    // the server list came from a server the player chose
    bool challenge = false; // that server sent a LoginNonce
    std::string server_id, nonce;
    bool nonce_used = false; // answered once already: the server has spent it
    // the LOGIN on its way
    char mode = 0; // 0 none, 'p' plain token, 'r' registration, '1' response
    Record pending;
    std::string records_path;
};
Session &session() {
    static Session s;
    return s;
}

std::string records_path(Session &s) {
    if (!s.records_path.empty())
        return s.records_path;
    const std::string &dir = host_layout().profile_dir;
    return dir.empty() ? "" : dir + "/ubi-gs-logins.txt";
}

// The same server and name; the file's names are lowered when written, and
// a hand-edited one is lowered here, as the token's salt lowers it.
bool same_name(const Record &a, const Record &b) {
    return a.server_id == b.server_id && lower_ascii(a.name) == lower_ascii(b.name);
}

} // namespace

void set_server(const std::string &server_id, const std::string &nonce) {
    Session &s = session();
    std::lock_guard lock(s.mu);
    s.active = !server_id.empty();
    s.server_id = server_id;
    s.nonce = nonce;
    s.nonce_used = false;
    s.challenge = !nonce.empty();
    s.mode = 0;
}

void clear() {
    set_server("", "");
}

void set_records_path(const std::string &path) {
    Session &s = session();
    std::lock_guard lock(s.mu);
    s.records_path = path;
}

bool password_for(const std::string &name, const std::string &password, std::string *out) {
    Session &s = session();
    std::lock_guard lock(s.mu);
    if (!s.active)
        return false;
    const std::string t = token(password, s.server_id, name);
    if (t.empty()) {
        // not the password either: an empty one, which every server refuses
        LOGW("ubi-gs login: the token could not be made (out of memory)");
        out->clear();
        s.mode = 0;
        return true;
    }
    if (!s.challenge) {
        *out = t;
        s.mode = 'p';
        LOGW("ubi-gs login: %s gets a token, not the password", s.server_id.c_str());
        return true;
    }
    const Record mine{s.server_id, lower_ascii(name), fingerprint(t)};
    // known: this file has a line for the name on this server, so the name
    // registered here; registered: one of them holds this very token.
    bool known = false, registered = false;
    const std::string path = records_path(s);
    if (!path.empty())
        for (const Record &r : load_records(path))
            if (same_name(r, mine)) {
                known = true;
                registered = registered || r.fp == mine.fp;
            }
    // A known name with another token is almost certainly a mistyped (or
    // changed) password: it gets a response too, never a registration, so
    // the server refuses it and the file gets no line for it. Once the nonce
    // is spent, only the registered token goes as a registration (the server
    // logs in a taken name registered with the same token); any other
    // answers the spent nonce, which the server refuses.
    if (known && (!s.nonce_used || !registered)) {
        uint8_t v[32];
        verifier(t, v);
        *out = response(v, s.nonce);
        memset(v, 0, sizeof v);
        s.mode = '1';
        LOGW("ubi-gs login: %s gets a response to its %snonce%s", s.server_id.c_str(),
             s.nonce_used ? "spent " : "",
             registered ? "" : " (not the password registered here)");
        s.nonce_used = true; // single use
    } else {
        *out = "~r:" + t;
        s.mode = 'r';
        LOGW("ubi-gs login: %s gets a registration (%s)", s.server_id.c_str(),
             known ? "no fresh nonce" : "first time here with this name");
    }
    s.pending = mine;
    return true;
}

void login_result(bool accepted) {
    Session &s = session();
    std::lock_guard lock(s.mu);
    const char mode = s.mode;
    s.mode = 0;
    if (mode != 'r' && mode != '1')
        return;
    const std::string path = records_path(s);
    if (path.empty())
        return;
    // One line per server and name: the token the server last accepted.
    std::vector<Record> records = load_records(path), kept;
    bool had = false;
    for (const Record &r : records) {
        if (same_name(r, s.pending))
            had = had || r.fp == s.pending.fp;
        else
            kept.push_back(r);
    }
    if (accepted) {
        kept.insert(kept.begin(), s.pending); // most recent first
        if (kept.size() > MAX_RECORDS)
            kept.resize(MAX_RECORDS);
    } else if (mode != '1' || !had) {
        // A refused registration, or a refused response from a token that
        // never registered here (a mistyped password), leaves the file as
        // it was. So a mistyped password never adds a line, and the name
        // keeps getting responses until the right password comes.
        return;
    }
    // Refused, the registered token itself: the server lost the account, or
    // the nonce expired, or the server asked to slow down after misses (the
    // router's answer doesn't tell which). As before, the name's lines go
    // and its next login registers again, whatever is typed then. That can
    // neither loop nor grow the file: each login is one LOGIN the player
    // started; a server that still holds the name logs in a registration
    // with the same token and refuses one with any other (ubi-gs-go since
    // its #12), a refused one writes nothing, and an accepted login leaves
    // one line for the name. A server that did lose the account
    // takes the next password typed as the new one, as with any first login.
    if (!save_records(path, kept))
        LOGW("ubi-gs login: %s could not be written", path.c_str());
    if (!accepted)
        LOGW("ubi-gs login: %s refused the registered password; the next login registers again",
             s.pending.server_id.c_str());
}

} // namespace gs_login

// ---------------------------------------------------------------- the seam --

extern "C" int recomp_gs_login_password(const char *name, const char *password, char *out,
                                        size_t out_len) {
    if (!name || !password || !out || out_len == 0)
        return 0;
    std::string replacement;
    if (!gs_login::password_for(name, password, &replacement))
        return 0;
    if (replacement.size() >= out_len)
        replacement.clear();
    memcpy(out, replacement.c_str(), replacement.size() + 1);
    return 1;
}

extern "C" void recomp_gs_login_result(int accepted) {
    gs_login::login_result(accepted != 0);
}
