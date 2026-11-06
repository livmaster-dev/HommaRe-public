// gs_login_tests.cpp - the password a GS game sends to a chosen community
// server (gs_login.h): the primitives against published vectors, the token,
// verifier and response against vectors shared with the Go server
// (computed independently with Python's hashlib), servers.ini's ServerID and
// LoginNonce, the address fallback, case-insensitive names, and the session:
// plain token, registration, response, a used nonce, a mistyped password, a
// refused response and the registration file.
//
// Run from the repository root:
//   .venv/bin/python tools/test.py --compile-only && build/recomp/gs_login_tests
#include "../gs_login.h"
#include "../native_seam.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

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
static std::string hex(const uint8_t *d, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; ++i) {
        snprintf(b, sizeof b, "%02x", d[i]);
        s += b;
    }
    return s;
}

static void test_primitives() {
    section("SHA-256, HMAC, PBKDF2 and scrypt against their published vectors");
    uint8_t d[64];
    gs_login::sha256("abc", 3, d);
    check(hex(d, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256(\"abc\")");
    // RFC 4231 test case 2
    gs_login::hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
    check(hex(d, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
          "HMAC-SHA256, RFC 4231 case 2");
    // RFC 4231 test case 6: a key longer than the block
    uint8_t key[131];
    memset(key, 0xaa, sizeof key);
    const char *msg6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    gs_login::hmac_sha256(key, sizeof key, msg6, strlen(msg6), d);
    check(hex(d, 32) == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
          "HMAC-SHA256, RFC 4231 case 6 (long key)");
    // RFC 7914 section 11
    gs_login::pbkdf2_sha256("passwd", 6, "salt", 4, 1, d, 64);
    check(hex(d, 64) == "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                        "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
          "PBKDF2-HMAC-SHA256, RFC 7914");
    check(gs_login::scrypt("", 0, "", 0, 16, 1, 1, d, 64) &&
              hex(d, 64) == "77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede2144"
                            "2fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906",
          "scrypt, RFC 7914 vector 1");
    check(gs_login::scrypt("password", 8, "NaCl", 4, 1024, 8, 16, d, 64) &&
              hex(d, 64) == "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b373162"
                            "2eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640",
          "scrypt, RFC 7914 vector 2");
    check(!gs_login::scrypt("", 0, "", 0, 1000, 8, 1, d, 16),
          "scrypt refuses an N not a power of 2");
    const uint8_t b[] = {0xfb, 0xff, 0x00, 0x10};
    check(gs_login::base64url(b, 4) == "-_8AEA", "base64url: URL alphabet, no padding (\"%s\")",
          gs_login::base64url(b, 4).c_str());
}

struct Vector {
    const char *password, *server_id, *name, *nonce, *token, *v, *response;
};
// Shared with the Go server; computed independently with Python's hashlib.
static const Vector kVectors[] = {
    {"secret", "00112233445566778899aabbccddeeff", "Alice", "0123456789abcdef0123456789abcdef",
     "Wgs4hXgot3GJCinm", "121e3493e77372e7a18d88f1f93c10ca5b0536820417c47fb06bee82360b5bd4",
     "~1:Mha-DE-4deymFMIO"},
    {"hunter2", "127.0.0.1:8099", "bob", "ffeeddccbbaa99887766554433221100", "aYUq3a-GMqcDMkz5",
     "853736af49e9ca3336ea426c85d2551370ca585acd2f96a04d29fdf465e915c4", "~1:-Dy9MVAnJbvZxjty"},
    {"Pass.word-_9", "c0ffee00c0ffee00c0ffee00c0ffee00", "Mixed.Case-Name_1", "aaaaaaaaaaaaaaaa",
     "kRAM7GjtYIrRc_V4", "7190e19fed5aa6a75d33273fb1c392828d8f87116e24e7735406a34f8b3d522e",
     "~1:KbL3cuvrkJisIY-5"},
};

static bool ubi_chars(const std::string &s) {
    for (char ch : s)
        if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_')
            return false;
    return true;
}

static void test_derivation() {
    section("the token, the verifier and the response");
    for (const Vector &t : kVectors) {
        const std::string tok = gs_login::token(t.password, t.server_id, t.name);
        check(tok == t.token, "token(%s, %s, %s) = %s", t.password, t.server_id, t.name,
              tok.c_str());
        uint8_t v[32];
        gs_login::verifier(tok, v);
        check(hex(v, 32) == t.v, "its verifier");
        check(gs_login::response(v, t.nonce) == t.response, "its response to %s", t.nonce);
    }
    const std::string tok = gs_login::token("secret", "00112233445566778899aabbccddeeff", "Alice");
    check(tok.size() == 16 && ubi_chars(tok),
          "16 characters the game's own password field accepts ([A-Za-z0-9_-])");
    check(gs_login::token("secret", "00112233445566778899aabbccddeeff", "ALICE") == tok &&
              gs_login::token("secret", "00112233445566778899aabbccddeeff", "alice") == tok,
          "the name's case does not matter");
    check(gs_login::token("Secret", "00112233445566778899aabbccddeeff", "Alice") != tok,
          "the password's does");
    check(gs_login::token("secret", "00112233445566778899aabbccddeef0", "Alice") != tok,
          "another server, another token");
    check(gs_login::token("secret", "00112233445566778899aabbccddeeff", "Alice2") != tok,
          "another name, another token");
    check(gs_login::token("", "x", "a").size() == 16, "an empty password still makes one");
    uint8_t v[32];
    gs_login::verifier(tok, v);
    const std::string r = gs_login::response(v, "0123456789abcdef0123456789abcdef");
    check(r.size() == 19 && r.compare(0, 3, "~1:") == 0 && ubi_chars(r.substr(3)),
          "a response is ~1: and 16 characters");
    check(gs_login::response(v, "0123456789abcdef0123456789abcdee") != r,
          "another nonce, another response");
    check(gs_login::fingerprint(tok).size() == 8 && gs_login::fingerprint(tok) != tok.substr(0, 8),
          "the fingerprint is 8 hex digits, not the token's");
}

static void test_servers_ini() {
    section("servers.ini's ServerID and LoginNonce");
    const std::string base = "[Servers]\r\nRouterIP0=127.0.0.1\r\nRouterPort0=7777\r\n";
    auto ini = gs_login::parse_servers_ini(base);
    check(ini.server_id.empty() && ini.nonce.empty(), "none in ubi-gs's own file");
    ini = gs_login::parse_servers_ini(base + "ServerID=00112233445566778899AABBCCDDEEFF\r\n");
    check(ini.server_id == "00112233445566778899aabbccddeeff" && ini.nonce.empty(),
          "ServerID, lowered");
    ini = gs_login::parse_servers_ini(
        "[Servers]\nserverid = 0011223344556677\nLOGINNONCE=  ffffffffffffffff0000000000000000 \n");
    check(ini.server_id == "0011223344556677" && ini.nonce == "ffffffffffffffff0000000000000000",
          "keys in any case, spaces around values, LF line ends");
    ini = gs_login::parse_servers_ini("[Other]\nServerID=0011223344556677\n[Servers]\n");
    check(ini.server_id.empty(), "another section's ServerID does not count");
    ini = gs_login::parse_servers_ini(
        "[Servers]\nServerID=0011223344556677\nServerID=8899aabbccddeeff\n");
    check(ini.server_id == "0011223344556677", "the first one counts");
    for (const char *bad :
         {"ServerID=xyz0112233445566", "ServerID=0011223344",
          "ServerID=", "ServerID=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff0",
          "ServerID=0011 2233 4455 6677"})
        check(
            gs_login::parse_servers_ini("[Servers]\n" + std::string(bad) + "\n").server_id.empty(),
            "malformed: %s", bad);
    check(gs_login::address_id("Example.ORG", 80) == "example.org:80" &&
              gs_login::address_id("127.0.0.1", 8099) == "127.0.0.1:8099" &&
              gs_login::address_id("FE80::1", 7000) == "[fe80::1]:7000",
          "the address fallback: lower case, the port always, IPv6 in brackets");
}

static std::string g_dir;

static std::string send(const char *name, const char *password) {
    char out[64];
    memset(out, '#', sizeof out);
    if (!recomp_gs_login_password(name, password, out, sizeof out))
        return "(original)";
    return out;
}

// The response a password makes to a nonce.
static std::string answer(const char *password, const std::string &server_id, const char *name,
                          const char *nonce) {
    uint8_t v[32];
    gs_login::verifier(gs_login::token(password, server_id, name), v);
    return gs_login::response(v, nonce);
}

// A name registered here, then a password other than its own: a response,
// which the server refuses, never a registration or another line. Starts
// with Alice's "secret" on kVectors[0]'s server as the file's only line.
static void test_mistyped(const std::string &path) {
    section("a mistyped password");
    const Vector &v0 = kVectors[0];
    auto only_alice = [&](const char *password) {
        const auto records = gs_login::load_records(path);
        return records.size() == 1 && records[0].server_id == v0.server_id &&
               records[0].name == "alice" &&
               records[0].fp ==
                   gs_login::fingerprint(gs_login::token(password, v0.server_id, "Alice"));
    };
    gs_login::set_server(v0.server_id, "1111111111111111");
    check(send("Alice", "secreT") == answer("secreT", v0.server_id, "Alice", "1111111111111111"),
          "the name has a line here, another password: the response from the typed one, "
          "not a registration");
    recomp_gs_login_result(0);
    check(only_alice("secret"), "refused: no line added, the registered one kept");
    check(send("ALICE", "secrte") == answer("secrte", v0.server_id, "ALICE", "1111111111111111"),
          "again before a fresh nonce: the response to the spent one, still no registration");
    recomp_gs_login_result(0);
    check(only_alice("secret"), "refused: the file as it was");
    check(send("alice", "secret") == std::string("~r:") + v0.token,
          "the right password with the nonce spent: the registered token as a registration");
    recomp_gs_login_result(1);
    check(only_alice("secret"), "accepted: still the one line");

    bool all = true;
    for (int i = 0; i < 20; ++i) {
        char nonce[24], password[16];
        snprintf(nonce, sizeof nonce, "%016x", 0xa000 + i);
        snprintf(password, sizeof password, "typo%d", i);
        gs_login::set_server(v0.server_id, nonce);
        all = all && send("Alice", password) == answer(password, v0.server_id, "Alice", nonce);
        recomp_gs_login_result(0);
    }
    check(all, "20 mistyped passwords, a fresh nonce each: 20 responses");
    check(only_alice("secret"), "and the file never grew");
    gs_login::set_server(v0.server_id, "b000000000000000");
    check(send("Alice", "secret") == answer("secret", v0.server_id, "Alice", "b000000000000000"),
          "then the right password: the response");
    recomp_gs_login_result(1);
    check(only_alice("secret"), "accepted: the line kept");

    gs_login::set_server(v0.server_id, "b100000000000000");
    check(send("Alice", "changed") == answer("changed", v0.server_id, "Alice", "b100000000000000"),
          "another password the server takes (changed there): a response");
    recomp_gs_login_result(1);
    check(only_alice("changed"), "accepted: the name's one line now has that password");
    gs_login::set_server(v0.server_id, "b200000000000000");
    send("Alice", "secret");
    recomp_gs_login_result(1);
    check(only_alice("secret"), "and back");

    const std::string elsewhere = "ffeeddccbbaa99887766554433221100";
    gs_login::set_server(elsewhere, "b300000000000000");
    check(send("Alice", "secret") == "~r:" + gs_login::token("secret", elsewhere, "Alice"),
          "the same name on another server: registration");
    recomp_gs_login_result(1);
    gs_login::set_server(v0.server_id, "b400000000000000");
    check(send("Bob", "secret") == "~r:" + gs_login::token("secret", v0.server_id, "Bob"),
          "a fresh name: registration");
    recomp_gs_login_result(1);
    const auto records = gs_login::load_records(path);
    check(records.size() == 3 && records[0].name == "bob" && records[1].server_id == elsewhere &&
              records[2].server_id == v0.server_id && records[2].name == "alice",
          "both recorded, Alice's line here still the one");
}

static void test_session() {
    section("the session");
    const std::string path = g_dir + "/ubi-gs-logins.txt";
    gs_login::set_records_path(path);
    gs_login::clear();
    check(send("Alice", "secret") == "(original)", "no chosen server: the password goes as typed");

    const Vector &v0 = kVectors[0];
    gs_login::set_server(v0.server_id, "");
    check(send("Alice", "secret") == v0.token, "a server without a nonce: the token");
    recomp_gs_login_result(1);
    check(gs_login::load_records(path).empty(), "and nothing is recorded");

    // a server with nonces
    gs_login::set_server(v0.server_id, v0.nonce);
    check(send("Alice", "secret") == std::string("~r:") + v0.token,
          "first time with a nonce server: registration");
    recomp_gs_login_result(0);
    check(gs_login::load_records(path).empty(), "a refused registration records nothing");
    check(send("Alice", "secret") == std::string("~r:") + v0.token, "so the next one registers");
    recomp_gs_login_result(1);
    auto records = gs_login::load_records(path);
    check(records.size() == 1 && records[0].server_id == v0.server_id &&
              records[0].name == "alice" && records[0].fp == gs_login::fingerprint(v0.token),
          "an accepted one is recorded: server, lowered name, fingerprint");
    FILE *f = fopen(path.c_str(), "rb");
    char body[1024] = {};
    if (f) {
        fread(body, 1, sizeof body - 1, f);
        fclose(f);
    }
    check(!strstr(body, "secret") && !strstr(body, v0.token),
          "the file holds neither the password nor the token");

    check(send("ALICE", "secret") == v0.response, "registered: the response to the nonce");
    recomp_gs_login_result(1);
    check(send("alice", "secret") == std::string("~r:") + v0.token,
          "the nonce is used up: registration again, never a replay");
    recomp_gs_login_result(1);
    check(gs_login::load_records(path).size() == 1, "one line still");

    test_mistyped(path);
    section("a refused response, a file from before");

    gs_login::set_server(v0.server_id, "2222222222222222");
    const std::string r = send("Alice", "secret");
    check(r == answer("secret", v0.server_id, "Alice", "2222222222222222"),
          "a fresh nonce: a response");
    recomp_gs_login_result(0);
    records = gs_login::load_records(path);
    check(records.size() == 2 && records[0].name == "bob" && records[1].server_id != v0.server_id,
          "a refused response from the registered password forgets that name on that server only");
    gs_login::set_server(v0.server_id, "3333333333333333");
    check(send("Alice", "secret") == std::string("~r:") + v0.token,
          "so the next login registers again (lost server state)");
    recomp_gs_login_result(1);
    check(gs_login::load_records(path).size() == 3, "and is recorded again");

    // A file from before, with a line per mistyped password: the name is
    // known through any of them, and its lines go together.
    const std::string other_fp =
        gs_login::fingerprint(gs_login::token("typo", v0.server_id, "Alice"));
    {
        std::vector<gs_login::Record> old = gs_login::load_records(path);
        old.push_back({v0.server_id, "ALICE", other_fp});
        old.push_back({v0.server_id, "alice", "0badf00d"});
        gs_login::save_records(path, old);
    }
    gs_login::set_server(v0.server_id, "6666666666666666");
    check(send("alice", "secret") == answer("secret", v0.server_id, "alice", "6666666666666666"),
          "several lines for the name, one of them in capitals: a response");
    recomp_gs_login_result(1);
    records = gs_login::load_records(path);
    int alices = 0;
    for (const auto &rec : records)
        alices += rec.server_id == v0.server_id && rec.name == "alice";
    check(records.size() == 3 && alices == 1 && records[0].fp == gs_login::fingerprint(v0.token),
          "accepted: one line for the name, the accepted password's");
    {
        std::vector<gs_login::Record> old = gs_login::load_records(path);
        old.push_back({v0.server_id, "alice", other_fp});
        gs_login::save_records(path, old);
    }
    gs_login::set_server(v0.server_id, "7777777777777777");
    send("Alice", "secret");
    recomp_gs_login_result(0);
    records = gs_login::load_records(path);
    check(records.size() == 2, "the registered password refused: all of the name's lines go");
    gs_login::set_server(v0.server_id, "8888888888888888");
    check(send("Alice", "secret") == std::string("~r:") + v0.token,
          "so the next login registers, not stuck on a stale line");
    recomp_gs_login_result(1);

    unlink(path.c_str());
    gs_login::set_server(v0.server_id, "4444444444444444");
    check(send("Alice", "secret") == std::string("~r:") + v0.token,
          "lost local state: registration");
    recomp_gs_login_result(1);

    gs_login::set_server(v0.server_id, "5555555555555555");
    char tiny[8];
    check(recomp_gs_login_password("Alice", "secret", tiny, sizeof tiny) == 1 && tiny[0] == 0,
          "a buffer too small gets an empty password, never the real one");
    gs_login::clear();
    check(send("Alice", "secret") == "(original)", "cleared: the original again");
    unlink(path.c_str());
}

int main() {
    char tmpl[] = "/tmp/gs_login_tests.XXXXXX";
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        return 1;
    }
    g_dir = tmpl;
    test_primitives();
    test_derivation();
    test_servers_ini();
    test_session();
    rmdir(g_dir.c_str());
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
