// gs_login.h - what a Ubisoft Game Service (GS) title sends as its password
// to a community server the player chose (curl_http.h): never the password.
//
// The GS router's LOGIN carries the account name and the password, and the
// protocol's encryption is no protection (keyless XOR, published Blowfish
// keys, 512-bit RSA with e=3): the password is as good as clear on the wire
// and in front of whoever runs the server. So when the server list came from
// a server the player chose, the game's native layer swaps the password for
// one of these right before the LOGIN is built
// (recomp_gs_login_password, native_seam.h):
//
//   T = base64url(scrypt(password,
//                        "ubi-gs login v1\0" + server_id + "\0" + lower(name),
//                        N = 2^13, r = 8, p = 1, 12 bytes))        16 chars
//
// The server's identity is its servers.ini's ServerID=<hex> ([Servers]), so
// it keeps its accounts when its address changes; without one, the address
// the player chose, "host:port" in lower case. The name is the one typed,
// ASCII letters lowered (names are case-insensitive on the server).
//
// What goes out depends on the server:
// - No LoginNonce in its servers.ini (the Python ubi-gs, upstream): T as a
//   plain password. The server hashes it like any password.
// - LoginNonce=<hex> (fresh per servers.ini, single use): a challenge.
//     registration   "~r:" + T, the first time with this server and name
//                    (or with the registered password when no fresh nonce
//                    is left);
//     login          "~1:" + base64url(HMAC-SHA256(V, nonce)[0..12]),
//                    V = HMAC-SHA256(T, "ubi-gs verifier v1"),
//   so after registration T itself stays home. The server keeps V and a
//   hash of T. Which names registered where, and with which password, is
//   kept in the profile's ubi-gs-logins.txt (server id, name, a 32-bit
//   fingerprint of T), one line per server and name: a login the router
//   accepts writes the name's line, a refused "~1:" from the registered T
//   removes it. A name with a line gets "~1:" whatever password is typed,
//   so a mistyped one is refused and never registers or adds a line.
//   The game's login dialog only lets a player type [A-Za-z0-9_.-], so no
//   password typed into an original game starts with '~'.
//
// Nothing here logs or stores the password or T.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gs_login {

// The token's scrypt cost, fixed: a different one is a different token.
// 8 MiB and 28 ms on one core of an i9-14900HX at -O2 (2026-10-29; 2^14 took
// 55 ms), so about 50 ms on a phone's.
constexpr uint32_t SCRYPT_N = 1u << 13, SCRYPT_R = 8, SCRYPT_P = 1;
constexpr size_t TOKEN_BYTES = 12; // 16 base64url characters

// Building blocks, exposed for the tests.
void sha256(const void *data, size_t n, uint8_t out[32]);
void hmac_sha256(const void *key, size_t key_len, const void *msg, size_t msg_len, uint8_t out[32]);
void pbkdf2_sha256(const void *pw, size_t pw_len, const void *salt, size_t salt_len,
                   uint32_t iterations, uint8_t *out, size_t out_len);
// False when the parameters are out of range or the memory is not there.
bool scrypt(const void *pw, size_t pw_len, const void *salt, size_t salt_len, uint32_t n,
            uint32_t r, uint32_t p, uint8_t *out, size_t out_len);
std::string base64url(const uint8_t *data, size_t n);

std::string token(const std::string &password, const std::string &server_id,
                  const std::string &name);
void verifier(const std::string &token, uint8_t v[32]);
std::string response(const uint8_t v[32], const std::string &nonce);
// 8 hex digits that tell one token from another, for the registration file.
std::string fingerprint(const std::string &token);

// [Servers] ServerID and LoginNonce of a servers.ini body: 16 to 64 hex
// digits each, lowered; "" when missing or malformed.
struct ServersIni {
    std::string server_id, nonce;
};
ServersIni parse_servers_ini(const std::string &body);
// The identity of a server without a ServerID: "host:port" ("[v6]:port").
std::string address_id(const std::string &host, uint16_t port);

// The registration file: "server_id<TAB>name<TAB>fingerprint" lines.
struct Record {
    std::string server_id, name, fp;
};
std::vector<Record> load_records(const std::string &path);
bool save_records(const std::string &path, const std::vector<Record> &records);

// The server the game's next LOGIN goes to (curl_http.cpp, after its
// servers.ini arrived): its identity and its nonce, if any. clear() when
// the game's server list came from nowhere a player chose.
void set_server(const std::string &server_id, const std::string &nonce);
void clear();
// What to send instead of `password` for `name`: false, *out untouched, when
// no chosen server is in play.
bool password_for(const std::string &name, const std::string &password, std::string *out);
// The router's answer to that LOGIN.
void login_result(bool accepted);
// Where the registration file is ("" the profile's ubi-gs-logins.txt).
void set_records_path(const std::string &path);

} // namespace gs_login
