// game_path.h - which D3DPopTB.exe to load, without any UI: the command line,
// the environment, the developer checkout, or the path the player picked once.
#pragma once
#include <string>

enum class GamePathSource { Flag, Environment, Checkout, Saved, DataRoot, None };
struct GamePath {
    std::string exe; // "" when nothing resolved
    GamePathSource source = GamePathSource::None;
};
// Order: `flag`, RECOMP_EXE, the checkout's original/gog above the
// executable (developer mode), <profile_dir>/game-path.txt when its file
// exists and hashes to the supported digest.
// When data_root is supplied, search only <data_root>/game/<executable>.
// The platform host supplies the root; the loader still verifies the hash.
GamePath game_path_resolve(const char *flag, const char *data_root = nullptr);
// True when `path` exists and its SHA-256 is LOADER_EXPECTED_SHA256;
// `digest_out` receives the digest ("" when unreadable).
bool game_path_is_supported(const std::string &path, std::string *digest_out);
// Writes <profile_dir>/game-path.txt (creating profile_dir); false on failure.
bool game_path_save(const std::string &path);
// The path as the host takes paths apart, at '/': on Windows a game path in
// the system's own spelling (RECOMP_EXE typed in PowerShell, a file picker's
// answer, C:\Games\bin\game.exe) gets '/' for '\'. Unchanged elsewhere,
// where '\' is a file name's own character.
std::string game_path_normal(std::string path);
