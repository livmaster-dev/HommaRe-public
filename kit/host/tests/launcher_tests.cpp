// launcher_tests.cpp - finding, checking and importing a game, over fake
// installs built in a scratch folder. No window, no game.
#include "../launcher/launcher.h"
#include "../launcher/launcher_ui.h"

#include "../../platform/os.h"
#include "../../third_party/miniz/miniz.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0, g_checks = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using namespace launcher;

namespace {

std::string g_scratch;

void write_file(const std::string &path, const std::string &text) {
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' && i)
            os_mkdir(path.substr(0, i).c_str());
    }
    FILE *f = fopen(path.c_str(), "wb");
    if (f) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

std::string read_file(const std::string &path) {
    std::string out;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    fclose(f);
    return out;
}

bool exists(const std::string &path) {
    OsStat st{};
    return os_stat(path.c_str(), &st) == 0;
}

// A fresh folder under the scratch directory.
std::string fresh(const char *name) {
    const std::string dir = g_scratch + "/" + name;
    remove_tree(dir);
    os_mkdir(dir.c_str());
    return dir;
}

const std::string kExe = "GAME.EXE";
const std::string kExeBytes = "MZ fake game executable\n";

Spec spec() {
    Spec s;
    s.title = "Test Game";
    s.executable = kExe;
    s.sha256 = sha256_file(g_scratch + "/exe-bytes");
    s.required_dirs = {"data", "levels"};
    s.exclude = {"__redist", "*.dll", "Setup?.txt"};
    s.install_names = {"Test Game"};
    return s;
}

// A plausible install: the executable, required folders, clutter to skip.
void make_install(const std::string &root, const std::string &exe_name = "game.exe") {
    write_file(root + "/" + exe_name, kExeBytes);
    write_file(root + "/DATA/sprites.bin", std::string(10000, 's'));
    write_file(root + "/levels/one.lvl", "level one");
    write_file(root + "/levels/deep/two.lvl", "level two");
    write_file(root + "/__redist/vcredist.exe", "skip me");
    write_file(root + "/binkw32.dll", "skip me too");
    write_file(root + "/Setup1.txt", "skip");
    write_file(root + "/readme.txt", "keep");
}

void make_zip(const std::string &zip_path, const std::string &prefix,
              const std::vector<std::pair<std::string, std::string>> &files) {
    mz_zip_archive zip{};
    CHECK(mz_zip_writer_init_file(&zip, zip_path.c_str(), 0));
    for (const auto &f : files) {
        const std::string name = prefix + f.first;
        CHECK(mz_zip_writer_add_mem(&zip, name.c_str(), f.second.data(), f.second.size(),
                                    MZ_BEST_SPEED));
    }
    CHECK(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
}

void test_sha256() {
    write_file(g_scratch + "/abc", "abc");
    CHECK(sha256_file(g_scratch + "/abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    write_file(g_scratch + "/empty", "");
    CHECK(sha256_file(g_scratch + "/empty") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // Longer than one block and than the 1 MiB read buffer.
    write_file(g_scratch + "/long", std::string(1500000, 'a'));
    CHECK(sha256_file(g_scratch + "/long").size() == 64);
    CHECK(sha256_file(g_scratch + "/no-such-file").empty());
}

void test_wildcards() {
    CHECK(wildcard_match("*.dll", "BINKW32.DLL"));
    CHECK(wildcard_match("Setup?.txt", "setup1.TXT"));
    CHECK(!wildcard_match("Setup?.txt", "Setup10.txt"));
    CHECK(wildcard_match("[a-c]x", "Bx"));
    CHECK(!wildcard_match("[!a-c]x", "bx"));
    CHECK(wildcard_match("*", ""));
    CHECK(!wildcard_match("app", "apps"));
    const std::vector<std::string> ex = {"__redist", "*.dll"};
    CHECK(excluded("__redist/vc/x.exe", ex));
    CHECK(excluded("data/sub/x.dll", ex));
    CHECK(excluded("data/__redist", ex));        // a file name matches too
    CHECK(!excluded("data/__redist/x.exe", ex)); // a folder below the top does not
    CHECK(!excluded("data/x.dat", ex));
}

void test_find_and_check() {
    const Spec s = spec();
    const std::string top = fresh("find");
    CHECK(find_root(s, top).empty());
    make_install(top + "/GOG Games/Test Game/app");
    const std::string root = top + "/GOG Games/Test Game/app";
    CHECK(find_root(s, top + "/GOG Games") == root); // two levels down
    CHECK(find_root(s, top).empty());                // three is too deep
    CHECK(find_root(s, root + "/game.exe") == root); // the executable itself
    CHECK(find_root(s, root + "/readme.txt").empty());
    CHECK(find_name(root, "Levels") == "levels");

    Status st = check(s, root);
    CHECK(st.state == State::Ready);
    CHECK(st.exe == root + "/game.exe");
    CHECK(st.digest == s.sha256);
    CHECK(check(s, top).state == State::NotFound);

    // A missing required folder, then a wrong executable.
    remove_tree(root + "/levels");
    st = check(s, root);
    CHECK(st.state == State::Incomplete);
    CHECK(st.missing.size() == 1 && st.missing[0] == "levels");
    write_file(root + "/game.exe", "another build");
    CHECK(check(s, root).state == State::WrongVersion);

    // The cache spares a second hash; a changed file invalidates it.
    const std::string cache = top + "/profile/verified.txt";
    make_install(top + "/cached");
    CHECK(check(s, top + "/cached", cache).state == State::Ready);
    const std::string text = read_file(cache);
    CHECK(text.find(s.sha256) != std::string::npos);
    std::string forged = text;
    forged.replace(forged.find(s.sha256), s.sha256.size(), std::string(64, '0'));
    write_file(cache, forged);
    CHECK(check(s, top + "/cached", cache).state == State::WrongVersion); // trusts the cache
    write_file(top + "/cached/game.exe", kExeBytes + "x");                // size changes
    CHECK(check(s, top + "/cached", cache).state == State::WrongVersion);
    write_file(top + "/cached/game.exe", kExeBytes);
    os_set_mtime((top + "/cached/game.exe").c_str(), 12345);
    CHECK(check(s, top + "/cached", cache).state == State::Ready); // rehashed
    // One cache line per executable.
    const std::string after = read_file(cache);
    size_t lines = 0;
    for (char c : after)
        lines += c == '\n';
    CHECK(lines == 1);
}

void test_folder_import() {
    const Spec s = spec();
    const std::string src = fresh("src");
    make_install(src + "/nested/Test Game");
    const std::string dest = fresh("dest") + "/game";
    auto source = folder_source(src);
    std::vector<Progress> seen;
    ImportOutcome out = import_game(s, *source, dest, [&](const Progress &p) {
        seen.push_back(p);
        return true;
    });
    CHECK(out.result == ImportResult::Done);
    if (out.result != ImportResult::Done)
        fprintf(stderr, "  import: %s: %s\n", import_result_name(out.result), out.error.c_str());
    CHECK(out.files_copied == 5); // game.exe, sprites, two levels, readme
    CHECK(exists(dest + "/game.exe"));
    CHECK(exists(dest + "/DATA/sprites.bin"));
    CHECK(exists(dest + "/levels/deep/two.lvl"));
    CHECK(!exists(dest + "/__redist"));
    CHECK(!exists(dest + "/binkw32.dll"));
    CHECK(!exists(dest + "/Setup1.txt"));
    CHECK(read_file(dest + "/.stamp") == s.sha256 + "\n");
    CHECK(!seen.empty() && seen.back().files_done == 5 &&
          seen.back().bytes_done == seen.back().bytes_total);
    CHECK(check(s, dest).state == State::Ready);

    // Again: everything is kept, nothing is copied.
    out = import_game(s, *source, dest);
    CHECK(out.result == ImportResult::Done);
    CHECK(out.files_copied == 0 && out.files_skipped == 5);

    // A changed source file is copied again.
    write_file(src + "/nested/Test Game/levels/one.lvl", "level one, patched");
    out = import_game(s, *source, dest);
    CHECK(out.result == ImportResult::Done && out.files_copied == 1);
    CHECK(read_file(dest + "/levels/one.lvl") == "level one, patched");
}

void test_cancel_and_resume() {
    const Spec s = spec();
    const std::string src = fresh("resume-src");
    make_install(src);
    for (int i = 0; i < 20; ++i)
        write_file(src + "/data/file" + std::to_string(i) + ".bin",
                   std::string(1000, char('a' + i)));
    const std::string dest = fresh("resume-dest") + "/game";
    auto source = folder_source(src);
    int calls = 0;
    ImportOutcome out = import_game(s, *source, dest, [&](const Progress &p) {
        return ++calls < 8 || p.files_done == p.files_total;
    });
    CHECK(out.result == ImportResult::Cancelled);
    CHECK(!exists(dest + "/.stamp"));
    CHECK(check(s, dest).state != State::Ready || !exists(dest + "/.stamp"));
    // No half-written files are left under their real names.
    const uint64_t first = out.files_copied;
    CHECK(first > 0 && first < 25);
    out = import_game(s, *source, dest);
    CHECK(out.result == ImportResult::Done);
    CHECK(out.files_skipped >= first);
    CHECK(out.files_copied + out.files_skipped == 25);
    CHECK(exists(dest + "/.stamp"));
}

void test_move_import() {
    const Spec s = spec();
    const std::string src = fresh("move-src");
    make_install(src + "/copied");
    const std::string dest = fresh("move-dest") + "/game";
    auto source = folder_source(src);
    ImportOutcome out = import_game(s, *source, dest, nullptr, true);
    CHECK(out.result == ImportResult::Done);
    CHECK(out.files_copied == 5);
    CHECK(exists(dest + "/levels/deep/two.lvl"));
    CHECK(!exists(src + "/copied/levels/deep/two.lvl")); // moved, not copied
    CHECK(exists(src + "/copied/binkw32.dll"));          // excluded files stay
    CHECK(check(s, dest).state == State::Ready);
}

void test_failures() {
    Spec s = spec();
    // No executable at all.
    const std::string empty = fresh("empty-src");
    write_file(empty + "/readme.txt", "hi");
    auto source = folder_source(empty);
    CHECK(import_game(s, *source, fresh("empty-dest")).result == ImportResult::NoExecutable);
    // A required folder is missing: nothing is copied.
    const std::string partial = fresh("partial-src");
    make_install(partial);
    remove_tree(partial + "/levels");
    source = folder_source(partial);
    const std::string pdest = fresh("partial-dest") + "/game";
    ImportOutcome out = import_game(s, *source, pdest);
    CHECK(out.result == ImportResult::Incomplete);
    CHECK(out.error.find("levels") != std::string::npos);
    CHECK(!exists(pdest));
    // The wrong build: copied, but never stamped.
    const std::string wrong = fresh("wrong-src");
    make_install(wrong);
    write_file(wrong + "/game.exe", "not the supported build");
    source = folder_source(wrong);
    const std::string wdest = fresh("wrong-dest") + "/game";
    out = import_game(s, *source, wdest);
    CHECK(out.result == ImportResult::WrongVersion);
    CHECK(!exists(wdest + "/.stamp"));
    // More headroom than any disk has.
    s.min_free_bytes = uint64_t(1) << 62;
    const std::string ndest = fresh("nospace-dest") + "/game";
    make_install(fresh("nospace-src"));
    source = folder_source(g_scratch + "/nospace-src");
    out = import_game(s, *source, ndest);
    CHECK(out.result == ImportResult::NoSpace);
    CHECK(out.bytes_needed > out.bytes_free);
    CHECK(!exists(ndest + "/game.exe"));
    std::vector<Entry> none;
    CHECK(!folder_source(g_scratch + "/nowhere")->list(&none, &out.error));
}

void test_zip_import() {
    const Spec s = spec();
    const std::string dir = fresh("zip");
    const std::vector<std::pair<std::string, std::string>> files = {
        {"GAME.exe", kExeBytes},        {"data/a.bin", "aaaa"}, {"Levels/one.lvl", "1"},
        {"__redist/setup.exe", "skip"}, {"x.dll", "skip"},      {"notes.txt", "keep"}};
    // Inside a top folder, as most archives are, with macOS clutter.
    std::vector<std::pair<std::string, std::string>> with_junk = files;
    make_zip(dir + "/game.zip", "Test Game/", with_junk);
    {
        mz_zip_archive zip{};
        // Add clutter and a hostile name to a second archive.
        CHECK(mz_zip_writer_init_file(&zip, (dir + "/evil.zip").c_str(), 0));
        for (const auto &f : files)
            mz_zip_writer_add_mem(&zip, f.first.c_str(), f.second.data(), f.second.size(), 0);
        mz_zip_writer_add_mem(&zip, "__MACOSX/._GAME.exe", "x", 1, 0);
        mz_zip_writer_add_mem(&zip, "../escape.txt", "x", 1, 0);
        mz_zip_writer_add_mem(&zip, "data/.DS_Store", "x", 1, 0);
        CHECK(mz_zip_writer_finalize_archive(&zip));
        mz_zip_writer_end(&zip);
    }
    std::string error;
    auto zip = zip_source(dir + "/game.zip", &error);
    CHECK(zip != nullptr);
    if (!zip)
        return;
    const std::string dest = dir + "/out/game";
    ImportOutcome out = import_game(s, *zip, dest);
    CHECK(out.result == ImportResult::Done);
    CHECK(out.files_copied == 4);
    CHECK(read_file(dest + "/GAME.exe") == kExeBytes);
    CHECK(exists(dest + "/Levels/one.lvl"));
    CHECK(!exists(dest + "/x.dll"));
    CHECK(check(s, dest).state == State::Ready);

    auto evil = zip_source_fd(os_fd_open((dir + "/evil.zip").c_str(), OS_O_RDONLY), &error);
    CHECK(evil != nullptr);
    if (evil) {
        const std::string edest = dir + "/evil-out/game";
        out = import_game(s, *evil, edest);
        CHECK(out.result == ImportResult::Done);
        CHECK(!exists(dir + "/evil-out/escape.txt"));
        CHECK(!exists(edest + "/__MACOSX"));
        CHECK(!exists(edest + "/data/.DS_Store"));
    }
    write_file(dir + "/not.zip", "plain text");
    CHECK(zip_source(dir + "/not.zip", &error) == nullptr);
    CHECK(!error.empty());
    CHECK(zip_source_fd(-1, &error) == nullptr);
}

void test_folders_and_profile() {
    const std::string dir = fresh("folders");
    const std::string file = dir + "/profile/launcher-folders.txt";
    CHECK(load_folders(file).empty());
    CHECK(remember_folder(file, "/a"));
    CHECK(remember_folder(file, "/b/"));
    CHECK(remember_folder(file, "/a"));
    std::vector<std::string> f = load_folders(file);
    CHECK(f.size() == 2 && f[0] == "/a" && f[1] == "/b");
    for (int i = 0; i < 12; ++i)
        remember_folder(file, "/x" + std::to_string(i));
    CHECK(load_folders(file).size() == 8);
    CHECK(forget_folder(file, "/x11"));
    CHECK(load_folders(file)[0] == "/x10");

    const std::string profile = fresh("profile");
    write_file(profile + "/mod-settings.json", "{}\n");
    write_file(profile + "/saves/slot1.sav", "save one");
    write_file(profile + "/game/huge.bin", "not a save");
    write_file(profile + "/logs/run.log", "log");
    const std::string zip = dir + "/saves.zip";
    std::string error;
    CHECK(export_profile(profile, zip, {"game", "logs"}, &error));
    const std::string restored = fresh("restored");
    write_file(restored + "/saves/slot2.sav", "kept");
    CHECK(import_profile(zip, restored, &error));
    CHECK(read_file(restored + "/saves/slot1.sav") == "save one");
    CHECK(read_file(restored + "/saves/slot2.sav") == "kept");
    CHECK(exists(restored + "/mod-settings.json"));
    CHECK(!exists(restored + "/game"));
    CHECK(!exists(restored + "/logs"));
    CHECK(!import_profile(dir + "/missing.zip", restored, &error));
}

void test_detection() {
    const Spec s = spec();
    const std::string base = fresh("detect");
    make_install(base + "/Games/Test Game/sub");              // named: two levels
    make_install(base + "/Games/Other Name");                 // direct
    make_install(base + "/Games/Unrelated/deep/deeper");      // too deep, unnamed
    make_install(base + "/Steam/steamapps/common/Test Game"); // a base that is the game
    write_file(base + "/Games/A Patch/game.exe", kExeBytes);  // no data: not offered
    write_file(base + "/Games/A Patch/levels/one.lvl", "patched");
    std::vector<std::string> found = detect_under(
        s, {base + "/Games", base + "/Steam/steamapps/common/Test Game", base + "/missing"});
    CHECK(found.size() == 3);
    auto has = [&](const std::string &p) {
        for (const std::string &f : found)
            if (f == p)
                return true;
        return false;
    };
    CHECK(has(base + "/Games/Test Game/sub"));
    CHECK(has(base + "/Games/Other Name"));
    CHECK(has(base + "/Steam/steamapps/common/Test Game"));
    CHECK(!has(base + "/Games/A Patch"));
    // The real scan runs and returns folders that hold the executable.
    for (const std::string &f : detect_installs(s))
        CHECK(!find_name(f, s.executable).empty());
}

// A platform whose pickers answer at once with whatever the test set.
struct FakePlatform : Platform {
    PlatformInfo pi;
    std::vector<Picked> next;
    std::vector<std::string> cands;
    bool move_all = false;
    std::string opened_url, opened_folder;
    int activity_on = 0, activity_off = 0, protected_count = 0, released = 0, exported = 0;
    PlatformInfo info() override {
        return pi;
    }
    void answer(PickDone done) {
        auto n = next;
        next.clear();
        done(n, "");
    }
    void pick_folder(PickDone done) override {
        answer(done);
    }
    void pick_zip(PickDone done) override {
        answer(done);
    }
    void pick_export(const std::string &, PickDone done) override {
        answer(done);
    }
    void pick_saves(PickDone done) override {
        answer(done);
    }
    std::vector<std::string> candidates(const Spec &) override {
        return cands;
    }
    void open_folder(const std::string &f) override {
        opened_folder = f;
    }
    void open_url(const std::string &u) override {
        opened_url = u;
    }
    void import_activity(bool active, const Progress *p) override {
        if (!p)
            (active ? activity_on : activity_off)++;
    }
    void protect_import(const std::string &) override {
        ++protected_count;
    }
    void release(const Picked &) override {
        ++released;
    }
    bool movable(const Picked &) override {
        return move_all;
    }
    void export_ready(const Picked &) override {
        ++exported;
    }
    void run_in_background(std::function<void()> work) override {
        work();
    }
};

bool has_button(Launcher &l, int id) {
    for (const Button &b : l.buttons())
        if (b.id == id)
            return true;
    return false;
}

void test_ui_desktop() {
    Spec s = spec();
    s.store_url = "https://example.test/buy";
    const std::string dir = fresh("ui-desktop");
    FakePlatform fp;
    fp.pi.plays_in_place = true;
    fp.pi.can_open_folder = true;
    fp.pi.import_root = dir + "/profile/game";
    fp.pi.profile_dir = dir + "/profile";
    fp.pi.folders_file = dir + "/profile/launcher-folders.txt";
    fp.pi.cache_file = dir + "/profile/launcher-verified.txt";
    Launcher l(s, fp);
    l.start("");
    CHECK(l.status().state == State::NotFound);
    CHECK(!has_button(l, kPlay));
    CHECK(has_button(l, kLocate) && has_button(l, kStore) && has_button(l, kQuit));
    l.activate(kStore);
    CHECK(fp.opened_url == s.store_url);

    // Locate a folder two levels above the install.
    make_install(dir + "/Games/Test Game/app");
    fp.next = {Picked{dir + "/Games", "", false, ""}};
    l.activate(kLocate);
    l.tick();
    CHECK(l.status().state == State::Ready);
    CHECK(has_button(l, kPlay) && has_button(l, kCopyIntoApp));
    CHECK(load_folders(fp.pi.folders_file).size() == 1);

    // A new launcher remembers it; Play finishes with the executable.
    Launcher again(s, fp);
    again.start("");
    CHECK(again.status().state == State::Ready);
    again.key(Key::Activate); // Play has the focus
    CHECK(again.finished());
    CHECK(again.exe() == dir + "/Games/Test Game/app/game.exe");

    // A wrong pick explains itself.
    fp.next = {Picked{dir + "/profile", "", false, ""}};
    l.activate(kLocate);
    l.tick();
    CHECK(l.screen() == Screen::Message);
    CHECK(l.message().find("GAME.EXE") != std::string::npos);
    l.key(Key::Back);
    CHECK(l.screen() == Screen::Main);

    // Copy into app storage, then export and import saves.
    l.activate(kCopyIntoApp);
    l.tick();
    CHECK(l.screen() == Screen::Main);
    CHECK(l.status().state == State::Ready && l.status().root == fp.pi.import_root);
    CHECK(fp.activity_on == 1 && fp.activity_off == 1 && fp.protected_count == 1);
    write_file(fp.pi.profile_dir + "/saves/a.sav", "A");
    l.activate(kManage);
    CHECK(l.screen() == Screen::Manage && has_button(l, kDeleteData));
    fp.next = {Picked{dir + "/export.zip", "", false, ""}};
    l.activate(kExportSaves);
    l.tick();
    CHECK(l.message().find("exported") != std::string::npos);
    CHECK(fp.exported == 1);
    l.activate(kOk);
    l.activate(kManage);
    write_file(fp.pi.profile_dir + "/saves/a.sav", "changed");
    fp.next = {Picked{dir + "/export.zip", "", false, ""}};
    l.activate(kImportSaves);
    l.tick();
    CHECK(read_file(fp.pi.profile_dir + "/saves/a.sav") == "A");
    // The export leaves the copied game out.
    std::string err;
    auto zip = zip_source(dir + "/export.zip", &err);
    std::vector<Entry> entries;
    CHECK(zip && zip->list(&entries, &err));
    for (const Entry &e : entries)
        CHECK(e.relative.compare(0, 5, "game/") != 0);
    l.activate(kOk);
    l.activate(kManage);
    l.activate(kOpenFolder);
    CHECK(fp.opened_folder == fp.pi.import_root);
    // Delete needs a second press.
    l.activate(kDeleteData);
    CHECK(exists(fp.pi.import_root));
    l.activate(kDeleteData);
    CHECK(!exists(fp.pi.import_root));
    CHECK(exists(fp.pi.profile_dir + "/saves/a.sav"));
    CHECK(l.screen() == Screen::Message);

    // Dropping a folder locates it; dropping a ZIP imports it.
    make_install(dir + "/Dropped");
    l.activate(kOk);
    l.drop(dir + "/Dropped");
    CHECK(l.status().state == State::Ready && l.status().root == dir + "/Dropped");
    l.drop(dir + "/Dropped/readme.txt");
    CHECK(l.screen() == Screen::Message); // not a ZIP
    l.drop(dir + "/nothing-here");
    CHECK(l.screen() == Screen::Message);

    // Drawing lays every button out inside the canvas; clicks hit them.
    Canvas c;
    for (auto size : {std::pair<int, int>{1280, 800}, {640, 400}, {2732, 2048}}) {
        c.resize(size.first, size.second, true);
        const int scale = std::max(1, std::min(size.first / 480, size.second / 300));
        l.activate(kOk);
        l.draw(c, scale);
        for (const Button &b : l.buttons())
            CHECK(b.rect.w > 0 && b.rect.x >= 0 && b.rect.y >= 0 &&
                  b.rect.x + b.rect.w <= c.width() && b.rect.y + b.rect.h <= c.height());
    }
    // A short, wide screen puts the buttons in two columns; Down keeps the column.
    c.resize(2856, 800, true);
    l.draw(c, 4);
    {
        const std::vector<Button> bs = l.buttons();
        CHECK(bs.size() >= 4 && bs[1].rect.y == bs[0].rect.y);
        const int from = l.focus();
        l.key(Key::Down);
        CHECK(l.focus() != from && bs[size_t(l.focus())].rect.x == bs[size_t(from)].rect.x);
        l.key(Key::Up);
        CHECK(l.focus() == from);
        l.key(Key::Right);
        CHECK(bs[size_t(l.focus())].rect.y == bs[size_t(from)].rect.y || l.focus() == from + 1);
        l.key(Key::Left);
        CHECK(l.focus() == from);
    }
    const Button quit = l.buttons().back();
    CHECK(quit.id == kQuit);
    l.pointer(quit.rect.x + 2, quit.rect.y + 2, true);
    l.pointer(quit.rect.x + 2, quit.rect.y + 2, false);
    CHECK(l.quit());
}

void test_ui_mobile() {
    const Spec s = spec();
    const std::string dir = fresh("ui-mobile");
    FakePlatform fp;
    fp.pi.plays_in_place = false;
    fp.pi.touch = true;
    fp.pi.import_root = dir + "/Documents/game";
    fp.pi.profile_dir = dir + "/profile";
    fp.pi.drop_hint = "Or copy the folder into the app's Documents.";
    Launcher l(s, fp);
    l.start("");
    CHECK(l.status().state == State::NotFound);
    CHECK(has_button(l, kImportFolder) && has_button(l, kImportZip) && !has_button(l, kLocate));

    // Importing a ZIP.
    make_zip(dir + "/game.zip", "Test Game/",
             {{"GAME.EXE", kExeBytes}, {"DATA/x.bin", "x"}, {"levels/l.lvl", "l"}});
    fp.next = {Picked{dir + "/game.zip", "", false, "game.zip"}};
    l.activate(kImportZip);
    l.tick();
    CHECK(l.status().state == State::Ready);
    CHECK(l.screen() == Screen::Main);
    CHECK(has_button(l, kPlay) && has_button(l, kImportFolder));
    CHECK(fp.protected_count == 1 && fp.released == 1);

    // A phone in landscape: every screen's buttons stay on it, whatever the text.
    {
        FakePlatform tall = fp;
        tall.pi.drop_hint = std::string(400, 'x');
        tall.pi.import_root = dir + "/elsewhere/game";
        tall.pi.profile_dir = dir + "/" + std::string(300, 'p');
        Launcher small(s, tall);
        small.start("");
        CHECK(small.status().state == State::NotFound);
        Canvas c;
        c.resize(2856, 1280, true);
        for (int id : {0, int(kManage)}) {
            if (id)
                small.activate(id);
            small.draw(c, 4);
            CHECK(!small.buttons().empty());
            for (const Button &b : small.buttons())
                CHECK(b.rect.y >= 0 && b.rect.y + b.rect.h <= c.height());
        }
    }

    // A device that cannot run the game says so at once, still imports, and
    // Play explains why not.
    Launcher cannot(s, fp);
    cannot.start("");
    cannot.set_unplayable("No usable graphics.");
    CHECK(cannot.screen() == Screen::Message && cannot.message() == "No usable graphics.");
    cannot.set_auto_play(1.5);
    CHECK(!cannot.counting_down());
    cannot.activate(kPlay);
    CHECK(!cannot.finished() && cannot.screen() == Screen::Message);
    CHECK(cannot.message() == "No usable graphics.");

    // A ready game starts by itself unless the screen is touched.
    Launcher auto_play(s, fp);
    auto_play.start("");
    auto_play.set_auto_play(1.5);
    CHECK(auto_play.counting_down());
    Canvas c;
    c.resize(960, 600, false);
    auto_play.draw(c, 2);
    auto_play.advance(1.0);
    CHECK(!auto_play.finished());
    auto_play.advance(1.0);
    CHECK(auto_play.finished());
    Launcher stopped(s, fp);
    stopped.start("");
    stopped.set_auto_play(1.5);
    stopped.pointer(5, 5, true);
    stopped.pointer(5, 5, false);
    stopped.advance(5);
    CHECK(!stopped.finished() && !stopped.quit() && stopped.screen() == Screen::Main);

    // A developer build's bundled copy is imported without asking.
    const std::string bundle = dir + "/App.app/game";
    make_install(bundle);
    remove_tree(fp.pi.import_root);
    fp.pi.auto_import = bundle;
    Launcher seeded(s, fp);
    seeded.start("");
    seeded.tick();
    CHECK(seeded.status().state == State::Ready && seeded.status().root == fp.pi.import_root);
    fp.pi.auto_import.clear();

    // A folder copied into Documents shows as a candidate and imports.
    make_install(dir + "/Documents/My Copy");
    fp.cands = {dir + "/Documents/My Copy"};
    remove_tree(fp.pi.import_root);
    l.start("");
    CHECK(l.status().state == State::NotFound);
    CHECK(has_button(l, kUseCandidate));
    fp.move_all = true;
    l.activate(kUseCandidate);
    l.tick();
    CHECK(l.status().state == State::Ready);
    CHECK(!exists(dir + "/Documents/My Copy")); // moved in, and the rest removed
    fp.move_all = false;
    make_install(dir + "/Documents/My Copy");

    // A failing import says why; cancelling says it can continue.
    write_file(dir + "/empty/readme.txt", "nothing here");
    fp.next = {Picked{dir + "/empty", "", false, ""}};
    l.activate(kImportFolder);
    l.tick();
    CHECK(l.screen() == Screen::Message);
    CHECK(l.message().find("do not contain") != std::string::npos);
    CHECK(l.status().state == State::Ready); // the earlier import is untouched
    // The app's own copy, or a folder holding it, is refused.
    fp.next = {Picked{dir + "/Documents", "", false, ""}};
    l.activate(kOk);
    remove_tree(dir + "/Documents/My Copy");
    l.activate(kImportFolder);
    l.tick();
    CHECK(l.message().find("already imported") != std::string::npos);

    fp.next = {Picked{dir + "/missing.zip", "", false, "missing.zip"}};
    l.activate(kOk);
    l.activate(kImportZip);
    l.tick();
    CHECK(l.message().find("Cannot read missing.zip") != std::string::npos);
}

} // namespace

void test_ui_text_fits() {
    // Paths wrap after a slash, not inside a folder name.
    const std::vector<std::string> lines =
        Canvas::wrap("/Users/player/Documents/profile", 12 * 6, 1);
    CHECK((lines == std::vector<std::string>{"/Users/", "player/", "Documents/", "profile"}));

    // A long title keeps the right margin clear, at any width.
    Spec s = spec();
    s.title = "Siege of Avalon: Anthology of Long Titles";
    FakePlatform fp;
    fp.pi.profile_dir = fresh("ui-title") + "/profile";
    Launcher l(s, fp);
    l.start("");
    Canvas c;
    for (int w : {640, 1280, 2420}) {
        const int scale = std::max(1, std::min(w / 480, 1668 / 300));
        c.resize(w, 1668, false);
        l.draw(c, scale);
        const int margin = 16 * scale;
        int lit = 0;
        for (int y = margin; y < margin + 24 * scale; ++y)
            for (int x = w - margin; x < w; ++x)
                lit += c.pixels()[(y * w + x) * 4] > 0x80;
        CHECK(lit == 0);
    }
}

int main() {
    const char *base = getenv("RECOMP_LAUNCHER_TEST_DIR");
    g_scratch = std::string(base && *base ? base : "build/recomp") + "/launcher-test";
    for (size_t i = g_scratch.find('/'); i != std::string::npos; i = g_scratch.find('/', i + 1))
        os_mkdir(g_scratch.substr(0, i).c_str());
    remove_tree(g_scratch);
    os_mkdir(g_scratch.c_str());
    write_file(g_scratch + "/exe-bytes", kExeBytes);

    struct Test {
        const char *name;
        void (*run)();
    } tests[] = {
        {"sha256", test_sha256},
        {"wildcards and exclusion", test_wildcards},
        {"find and check", test_find_and_check},
        {"folder import", test_folder_import},
        {"cancel and resume", test_cancel_and_resume},
        {"move import", test_move_import},
        {"failures", test_failures},
        {"zip import", test_zip_import},
        {"folders and profile", test_folders_and_profile},
        {"detection", test_detection},
        {"launcher screen, desktop", test_ui_desktop},
        {"launcher screen, mobile", test_ui_mobile},
        {"launcher text fits", test_ui_text_fits},
    };
    for (const Test &t : tests) {
        const int before = g_failures;
        t.run();
        printf("%-28s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    remove_tree(g_scratch);
    return g_failures ? 1 : 0;
}
