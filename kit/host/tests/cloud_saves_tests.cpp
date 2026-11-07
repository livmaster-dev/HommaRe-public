// cloud_saves_tests.cpp - the saves' sync with a synced folder (cloud_saves.h),
// two plain folders standing in for a device's profile and iCloud Drive.
#include "../cloud_saves.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace cloud_saves;

static int g_failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

static const int64_t kSec = 1'000'000'000ll;
static const char kSave[] = "Heroes of Might and Magic V/Profiles/Player/Saves/save_1.sav";
static const char kSave2[] = "Heroes of Might and Magic V/Profiles/Player/Saves/save_2.sav";
static const char kUser[] = "Heroes of Might and Magic V/Profiles/Player/user.cfg";

static int64_t wall() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Two devices' profiles and one synced folder, fresh for each test.
struct World {
    fs::path root;
    std::string cloud_root;
    explicit World(const char *name) {
        root = fs::temp_directory_path() / ("cloud_saves_tests-" + std::to_string(getpid())) / name;
        fs::remove_all(root);
        fs::create_directories(root);
        cloud_root = (root / "iCloud").string();
    }
    Paths device(const char *name) const { return paths_for((root / name).string(), cloud_root, "homm5"); }
};

// A file with `text`, `age_s` seconds old.
static void put(const std::string &dir, const std::string &rel, const std::string &text, int age_s) {
    const fs::path p = fs::path(dir) / rel;
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
    detail::set_mtime(p.string(), wall() - int64_t(age_s) * kSec);
}

static std::string get(const std::string &dir, const std::string &rel) {
    std::ifstream f(fs::path(dir) / rel, std::ios::binary);
    if (!f)
        return "<none>";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool has(const std::string &dir, const std::string &rel) {
    return fs::exists(fs::path(dir) / rel);
}

static int backups(const Paths &p) {
    int n = 0;
    std::error_code ec;
    for (const auto &e : fs::recursive_directory_iterator(p.backup, ec))
        if (e.is_regular_file())
            ++n;
    return n;
}

// CLOUD_SAVES_TESTS_PLATFORM=1: through the platform's side (Apple's file
// coordination, when the test is linked with cloud_saves_apple.mm).
static Io &io() {
    const char *e = getenv("CLOUD_SAVES_TESTS_PLATFORM");
    return e && *e == '1' ? platform_io() : plain_io();
}

static Stats run(const Paths &p) {
    return sync(p, io(), wall());
}

static void test_classify() {
    CHECK(classify(kSave) == Kind::Sync);
    CHECK(classify("Heroes of Might and Magic V/Hammers of Fate/Profiles/Игрок/Saves/save_0.sav") == Kind::Sync);
    CHECK(classify("Heroes of Might and Magic V/Profiles/Player/Saves/Campaign.sav") == Kind::Sync);
    CHECK(classify(kUser) == Kind::Seed);
    CHECK(classify("Heroes of Might and Magic V/Profiles/global.cfg") == Kind::Seed);
    CHECK(classify("Heroes of Might and Magic V/Profiles/Player/notes.txt") == Kind::None);
    CHECK(classify("Heroes of Might and Magic V/Screenshots/a.jpg") == Kind::None);
    CHECK(classify("Heroes of Might and Magic V/Profiles/Player/Saves/.save_1.sav.icloud") == Kind::None);
}

// A save made on one device reaches the other; a newer one there comes back.
static void test_both_ways() {
    World w("both_ways");
    const Paths mac = w.device("mac"), phone = w.device("phone");
    put(mac.local, kSave, "mac 1", 60);
    Stats s = run(mac);
    CHECK(s.error.empty() && s.pushed == 1);
    s = run(phone);
    CHECK(s.pulled == 1);
    CHECK(get(phone.local, kSave) == "mac 1");
    // Nothing changed: nothing copied.
    s = run(phone);
    CHECK(!s.changed());
    put(phone.local, kSave, "phone 2", 30);
    CHECK(run(phone).pushed == 1);
    CHECK(run(mac).pulled == 1);
    CHECK(get(mac.local, kSave) == "phone 2");
    CHECK(backups(mac) == 0); // an ordinary update keeps no backup
}

// The game's settings go only to a side that has none.
static void test_settings_seed_only() {
    World w("seed");
    const Paths mac = w.device("mac"), phone = w.device("phone");
    put(mac.local, kUser, "gfx_resolution = 2560x1600", 60);
    run(mac);
    run(phone);
    CHECK(get(phone.local, kUser) == "gfx_resolution = 2560x1600");
    put(phone.local, kUser, "gfx_resolution = 2868x1320", 30);
    run(phone);
    run(mac);
    CHECK(get(mac.local, kUser) == "gfx_resolution = 2560x1600");
}

// Changed on both devices since the last sync: the newer stays, the other is
// in the backup.
static void test_conflict_keeps_the_loser() {
    World w("conflict");
    const Paths mac = w.device("mac"), phone = w.device("phone");
    put(mac.local, kSave, "base", 120);
    run(mac);
    run(phone);
    put(mac.local, kSave, "mac newer", 20);
    put(phone.local, kSave, "phone older", 40);
    run(phone); // the phone first: its change goes up
    const Stats s = run(mac);
    CHECK(s.conflicts == 1);
    CHECK(get(mac.local, kSave) == "mac newer");
    CHECK(get(mac.cloud, kSave) == "mac newer");
    CHECK(backups(mac) == 1);
    run(phone);
    CHECK(get(phone.local, kSave) == "mac newer");
}

// Two devices that both had saves before the first sync: the same file is
// one save, different files are merged.
static void test_first_sync_merges() {
    World w("merge");
    const Paths mac = w.device("mac"), phone = w.device("phone");
    put(mac.local, kSave, "same", 100);
    put(phone.local, kSave, "same", 50); // copied by hand: other times
    put(phone.local, kSave2, "phone only", 50);
    run(mac);
    Stats s = run(phone);
    CHECK(s.conflicts == 0 && s.pushed == 1 && s.pulled == 0);
    run(mac);
    CHECK(get(mac.local, kSave2) == "phone only");
}

// A save deleted in the game goes on the other device too, into its backup.
static void test_delete_reaches_the_other_side() {
    World w("delete");
    const Paths mac = w.device("mac"), phone = w.device("phone");
    put(mac.local, kSave, "one", 100);
    put(mac.local, kSave2, "two", 100);
    run(mac);
    run(phone);
    fs::remove(fs::path(phone.local) / kSave2);
    Stats s = run(phone);
    CHECK(s.deleted_cloud == 1 && !has(phone.cloud, kSave2));
    s = run(mac);
    CHECK(s.deleted_local == 1 && !has(mac.local, kSave2));
    CHECK(has(mac.local, kSave));
    CHECK(backups(mac) == 1 && backups(phone) == 1);
}

// The synced folder emptied (another account, a wiped iCloud Drive): the
// device's saves go back up rather than being deleted.
static void test_lost_folder_is_not_a_delete() {
    World w("lost");
    const Paths mac = w.device("mac");
    put(mac.local, kSave, "one", 100);
    put(mac.local, kSave2, "two", 100);
    run(mac);
    fs::remove_all(mac.cloud);
    const Stats s = run(mac);
    CHECK(s.deleted_local == 0 && s.pushed == 2);
    CHECK(get(mac.local, kSave) == "one" && get(mac.cloud, kSave2) == "two");
    // And a profile wiped here takes the saves back.
    fs::remove_all(mac.local);
    CHECK(run(mac).pulled == 2);
    CHECK(get(mac.local, kSave) == "one");
}

// A save being written right now waits for the next sync.
static void test_young_file_waits() {
    World w("young");
    const Paths mac = w.device("mac");
    put(mac.local, kSave, "being written", 0);
    Stats s = run(mac);
    CHECK(s.waiting == 1 && s.pushed == 0);
    s = sync(mac, io(), wall() + 10 * kSec);
    CHECK(s.pushed == 1);
}

// Another synced folder starts from nothing: the old one's state does not
// turn into deletions there.
static void test_new_folder_new_state() {
    World w("refolder");
    Paths mac = w.device("mac");
    put(mac.local, kSave, "one", 100);
    run(mac);
    Paths other = paths_for((w.root / "mac").string(), (w.root / "Dropbox").string(), "homm5");
    put(other.cloud, kSave2, "from there", 100);
    const Stats s = run(other);
    CHECK(s.deleted_local == 0 && s.deleted_cloud == 0);
    CHECK(has(other.cloud, kSave) && has(other.local, kSave2));
}

// Per game: the expansions sit in folders of their own.
static void test_games_apart() {
    World w("games");
    const Paths base = w.device("mac");
    const Paths hof = paths_for((w.root / "mac-hof").string(), w.cloud_root, "homm5-hof");
    CHECK(base.cloud != hof.cloud);
    put(base.local, kSave, "base", 100);
    run(base);
    CHECK(run(hof).pulled == 0);
}

int main() {
    test_classify();
    test_both_ways();
    test_settings_seed_only();
    test_conflict_keeps_the_loser();
    test_first_sync_merges();
    test_delete_reaches_the_other_side();
    test_lost_folder_is_not_a_delete();
    test_young_file_waits();
    test_new_folder_new_state();
    test_games_apart();
    fs::remove_all(fs::temp_directory_path() / ("cloud_saves_tests-" + std::to_string(getpid())));
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("cloud_saves_tests: ok\n");
    return 0;
}
