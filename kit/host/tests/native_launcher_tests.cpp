// native_launcher_tests.cpp - the native launcher on Linux (launcher/linux/):
// its words, launcher-games.json, finding games, the layout's decisions, the
// TrueType text, and the model and screens over a fake install in a scratch
// folder. No window, no game; the installer and the skin have their own tests.
#include "../launcher/linux/config.h"
#include "../launcher/linux/detect.h"
#include "../launcher/linux/installer.h"
#include "../launcher/linux/l10n.h"
#include "../launcher/linux/layout.h"
#include "../launcher/linux/model.h"
#include "../launcher/linux/text.h"
#include "../launcher/linux/view.h"

#include "../../mods/settings_file.h"
#include "../../platform/os.h"
#include "../../third_party/miniz/miniz.h"
#include "../launcher/launcher.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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

// The settings file the model reads and writes (mods/settings.cpp in the app).
static std::map<std::string, int64_t> g_settings;
extern "C" int mods_settings_file_get(const char *key, int64_t *out) {
    auto it = g_settings.find(key);
    if (it == g_settings.end())
        return 0;
    *out = it->second;
    return 1;
}
extern "C" int mods_settings_file_set(const char *key, int64_t value) {
    g_settings[key] = value;
    return 1;
}
static std::string g_overlay;
extern "C" const char *mods_settings_file_dir(void) {
    return g_overlay.c_str();
}

using launcher::Canvas;
using launcher::Rect;
using namespace launcher::native;

namespace {

std::string g_scratch;

void write_file(const std::string &path, const std::string &text) {
    for (size_t i = 1; i < path.size(); ++i)
        if (path[i] == '/')
            os_mkdir(path.substr(0, i).c_str());
    FILE *f = fopen(path.c_str(), "wb");
    if (f) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

void remove_tree(const std::string &path) {
    OsStat st{};
    if (os_lstat(path.c_str(), &st) != 0)
        return;
    if (st.is_dir && !st.is_symlink) {
        std::vector<std::string> names;
        os_listdir(
            path.c_str(),
            [](const char *name, void *user) {
                static_cast<std::vector<std::string> *>(user)->push_back(name);
                return 0;
            },
            &names);
        for (const std::string &n : names)
            remove_tree(path + "/" + n);
        os_rmdir(path.c_str());
    } else {
        os_unlink(path.c_str());
    }
}

std::string fresh(const std::string &name) {
    const std::string dir = g_scratch + "/" + name;
    remove_tree(dir);
    os_mkdir(dir.c_str());
    return dir;
}

const std::string kExeBytes = "MZ fake game executable\n";

// Two games: Alpha this app plays, Beta another module's; Gamma's
// executable is Alpha's file name in another GOG product's folder.
std::string config_json() {
    write_file(g_scratch + "/exe-bytes", kExeBytes);
    const std::string sum = launcher::sha256_file(g_scratch + "/exe-bytes");
    return R"({
  "title": "Test Port",
  "tagline": {"en": "A test", "ru": "Тест"},
  "installNames": ["Alpha Game"],
  "installers": [{"id": "gog-alpha", "gogId": "111", "folder": "Alpha Game", "manifests": ["m.json"],
                  "games": ["alpha"]}],
  "games": [
    {"id": "alpha", "module": "alpha", "title": {"en": "Alpha", "ru": "Альфа"}, "subtitle": "The first",
     "year": "2006", "version": "1.0", "gogId": "111", "exe": ["bin/GAME.EXE", "GAME.EXE"],
     "sha256": [")" +
           sum + R"("], "requiredDirs": ["data"], "accent": "#E3BA6E"},
    {"id": "beta", "module": "beta", "title": "Beta", "exe": ["beta.exe"], "sha256": ["00"]},
    {"id": "gamma", "module": "alpha", "title": "Gamma", "gogId": "222", "exe": ["bin/GAME.EXE"],
     "sha256": [")" +
           sum + R"("]}
  ]
})";
}

void test_l10n() {
    CHECK(languages().size() == 10);
    CHECK(!languages().empty() && languages()[0].code == "en");
    CHECK(pick_language({{"ru", "RU"}}) == "ru");
    CHECK(pick_language({{"zh", "CN"}}) == "zh-Hans");
    CHECK(pick_language({{"zh", "TW"}, {"de", "DE"}}) == "de");
    CHECK(pick_language({{"pt", "BR"}}) == "en");
    CHECK(pick_language({}) == "en");
    // With no choice saved: the system's language, from the environment as
    // gettext reads it; English where it speaks none of the launcher's.
    const char *vars[] = {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"};
    std::map<std::string, std::string> saved;
    for (const char *v : vars) {
        if (const char *value = getenv(v))
            saved[v] = value;
        os_unsetenv(v);
    }
    auto system = [] { return pick_language(preferred_languages()); };
    CHECK(system() == "en");
    os_setenv("LANG", "en_US.UTF-8");
    CHECK(system() == "en");
    os_setenv("LANG", "ru_RU.UTF-8");
    CHECK(system() == "ru");
    os_setenv("LC_MESSAGES", "de_DE.UTF-8");
    CHECK(system() == "de");
    os_setenv("LC_ALL", "zh_CN.UTF-8");
    CHECK(system() == "zh-Hans");
    os_setenv("LC_ALL", "zh_TW.UTF-8");
    CHECK(system() == "en");
    os_setenv("LANGUAGE", "pt_BR:fr:en");
    CHECK(system() == "fr");
    os_setenv("LC_ALL", "C"); // LANGUAGE does not count under the C locale
    CHECK(system() == "en");
    os_setenv("LANG", "pl_PL.UTF-8");
    os_unsetenv("LC_ALL");
    os_unsetenv("LC_MESSAGES");
    os_unsetenv("LANGUAGE");
    CHECK(system() == "pl");
    for (const char *v : vars) {
        os_unsetenv(v);
        if (saved.count(v))
            os_setenv(v, saved[v].c_str());
    }
    os_setenv("RECOMP_LAUNCHER_LANG", "en");
    CHECK(t("play") == "Play");
    CHECK(t("no such key") == "no such key");
    CHECK(file_of(3, 7) == "File 3 of 7");
    // The platform's variant wins where there is one: Windows', else the
    // desktop's (Linux).
#ifdef _WIN32
    CHECK(t("noHelper").find("innoextract.exe is missing") != std::string::npos);
#else
    CHECK(t("noHelper").find("dnf install innoextract") != std::string::npos);
#endif
    CHECK(t("showInFinder") == "Open folder");
    CHECK(bytes(4076506937) == "4.08 GB");
    CHECK(bytes(1500000) == "1.5 MB");
    CHECK(verify_failed({"a", "b", "c", "d"}) ==
          "4 files do not match the installer's manifest (a, b, c…). The installer may be damaged: "
          "download it again.");
    os_setenv("RECOMP_LAUNCHER_LANG", "ru");
    CHECK(t("play") == "Играть");
    CHECK(bytes(4076506937) == "4,08 ГБ");
    os_setenv("RECOMP_LAUNCHER_LANG", "en");
#ifdef _WIN32
    // Windows shows the path as it is, whatever HOME says (Git Bash sets one).
    CHECK(location("C:/Users/x/Games/x") == "C:/Users/x/Games/x");
#else
    const char *home = getenv("HOME");
    if (home && *home && strcmp(home, "/") != 0)
        CHECK(location(std::string(home) + "/Games/x") == "~/Games/x");
#endif
    CHECK(location("/opt/x") == "/opt/x");
}

void test_config() {
    Config c;
    std::string error;
    CHECK(parse_config(config_json(), &c, &error));
    CHECK(c.games.size() == 3 && c.installers.size() == 1);
    CHECK(c.title.text() == "Test Port");
    CHECK(c.games[0].subtitle.text() == "The first");
    CHECK(c.games[0].exe.size() == 2 &&
          c.games[0].required_dirs == std::vector<std::string>{"data"});
    os_setenv("RECOMP_LAUNCHER_LANG", "ru");
    CHECK(c.games[0].title.text() == "Альфа");
    CHECK(c.games[1].title.text() == "Beta"); // English stands in
    os_setenv("RECOMP_LAUNCHER_LANG", "en");
    CHECK(c.tagline.text() == "A test"); // no taglines: the common one
    // This platform's own tagline wins over the common one.
    CHECK(parse_config(R"({"tagline": "Everywhere",
                           "taglines": {"macos": "Mac", "linux": "Linux", "windows": "Windows"},
                           "games": [{"id": "a", "exe": ["a.exe"]}]})",
                       &c, &error));
#ifdef _WIN32
    CHECK(c.tagline.text() == "Windows");
#else
    CHECK(c.tagline.text() == "Linux");
#endif
    CHECK(!parse_config(R"({"games": []})", &c, &error));
    CHECK(!parse_config("{", &c, &error));
    Manifest m;
    CHECK(parse_manifest(
        R"({"gogId": "111", "files": [{"path": "bin/a.exe", "size": 3, "sha256": ")" +
            std::string(64, 'a') + R"("}]})",
        &m));
    CHECK(m.files.size() == 1 && m.files[0].size == 3);
    CHECK(!parse_manifest(R"({"files": [{"path": "x", "size": 3, "sha256": "short"}]})", &m));
    // The game repository's own files, where the kit sits in one.
    std::string text;
    if (read_text("../launcher-games.json", &text)) {
        CHECK(parse_config(text, &c, &error));
        for (const InstallerSpec &i : c.installers)
            for (const std::string &name : i.manifests)
                if (read_text("../launcher-manifests/" + name, &text)) {
                    CHECK(parse_manifest(text, &m));
                    CHECK(m.gog_id == i.gog_id && m.files.size() > 300);
                }
    }
}

void test_detection() {
    Config c;
    std::string error;
    CHECK(parse_config(config_json(), &c, &error));
    const std::string dir = fresh("detect");
    // Ready, spelled as Windows would.
    write_file(dir + "/ready/BIN/game.exe", kExeBytes);
    os_mkdir((dir + "/ready/Data").c_str());
    write_file(dir + "/ready/goggame-111.info", "{}");
    // Another build, and one without its data.
    write_file(dir + "/other/GAME.EXE", "MZ other\n");
    write_file(dir + "/partial/bin/GAME.EXE", kExeBytes);
    // Another GOG product's folder: its executable is not Gamma's.
    write_file(dir + "/elsewhere/bin/GAME.EXE", kExeBytes);
    write_file(dir + "/elsewhere/goggame-999.info", "{}");
    CHECK(entry(dir + "/ready", "bin/GAME.EXE") == dir + "/ready/BIN/game.exe");
    CHECK(entry(dir + "/ready", "nothing/here").empty());
    CHECK(has_gog_info(dir + "/ready") && !has_gog_info(dir + "/other"));
    Found f;
    CHECK(probe(c.games[0], dir + "/ready", &f) && f.state.kind == GameState::Ready);
    CHECK(probe(c.games[0], dir + "/other", &f) && f.state.kind == GameState::WrongVersion &&
          f.state.digest.size() == 64);
    CHECK(probe(c.games[0], dir + "/partial", &f) && f.state.kind == GameState::Incomplete &&
          f.state.missing == std::vector<std::string>{"data"});
    CHECK(!probe(c.games[2], dir + "/elsewhere", &f));
    CHECK(!probe(c.games[2], dir + "/ready", &f)); // goggame-111 is Alpha's product
    // The best copy wins; an unfinished install's games wait for it.
    auto found = scan(c.games, {dir + "/other", dir + "/partial", dir + "/ready"});
    CHECK(found.count("alpha") && found["alpha"].state.kind == GameState::Ready &&
          found["alpha"].root == dir + "/ready");
    CHECK(!found.count("beta"));
    InstallMarker marker;
    marker.installer = "gog-alpha";
    marker.setup = "/somewhere/setup_alpha.exe";
    marker.games = {"alpha"};
    CHECK(marker.write(dir + "/partial"));
    found = scan(c.games, {dir + "/partial"});
    CHECK(found.count("alpha") && found["alpha"].state.kind == GameState::Interrupted &&
          found["alpha"].state.setup == "/somewhere/setup_alpha.exe");
    // Roots up to two levels down.
    write_file(dir + "/deep/a/b/GAME.EXE", kExeBytes);
    write_file(dir + "/deeper/a/b/c/GAME.EXE", kExeBytes);
    CHECK(roots_below(dir + "/deep", c.games) == std::vector<std::string>{dir + "/deep/a/b"});
    CHECK(roots_below(dir + "/deeper", c.games).empty());
    CHECK(canonical(dir + "/deep/a/../a/./b") == canonical(dir + "/deep/a/b"));
    CHECK(parent_dir("/a/b/") == "/a" && file_name("/a/b/") == "b" && parent_dir("/a") == "/");
}

void test_layout() {
    // The game's look at 1200 x 760 and over; scaled down to fit under it.
    Look l = choose_look(1280, 800, true);
    CHECK(l.skinned && l.wide && l.scale == 1 && l.width == 1280);
    l = choose_look(960, 640, true);
    CHECK(l.skinned && l.scale < 1 && l.width >= 1200 - 0.01f && l.height >= 760 - 0.01f);
    l = choose_look(700, 400, true); // sideways, small: still the game's look
    CHECK(l.skinned && !l.wide);
    l = choose_look(600, 800, true); // upright: the flat column
    CHECK(!l.skinned && !l.wide && l.scale == 1);
    l = choose_look(1280, 800, false);
    CHECK(!l.skinned && l.wide);
    CHECK(modal_scale(1280, 800, 900, 460) == 1);
    CHECK(modal_scale(600, 400, 900, 460) < 0.65f);
    const auto places = flow({100, 100, 100}, 250, 20);
    CHECK(places.size() == 3 && places[0].row == 0 && places[1].row == 0 && places[1].x == 120 &&
          places[2].row == 1 && places[2].x == 0);
    CHECK(flow({300}, 100, 20)[0].row == 0); // one too wide still takes the first row
}

void test_text() {
    const std::string mixed = "Héroes — Тест 魔法门 ✓";
    CHECK(encode_utf8(decode_utf8(mixed)) == mixed);
    CHECK(decode_utf8("\xff").size() == 1 && decode_utf8("\xff")[0] == 0xfffd);
    CHECK(upper("Выберите игру ąłóźż ő") == "ВЫБЕРИТЕ ИГРУ ĄŁÓŹŻ Ő");
    CHECK(upper("choose a game") == "CHOOSE A GAME");
    const Font body{Face::Body, 15};
    CHECK(text_width("Ashan at war", body) > 40);
    CHECK(line_height(body) >= 15 && ascent(body) > descent(body));
    const std::string blurb =
        "Ashan at war: six campaigns for the Haven, Inferno, Necropolis, Sylvan, Academy and "
        "Dungeon factions.";
    const auto lines = wrap_text(blurb, 200, body);
    CHECK(lines.size() >= 3);
    for (const std::string &line : lines)
        CHECK(text_width(line, body) <= 200.5f && line.front() != ' ' && line.back() != ' ');
    // Chinese breaks between characters.
    const auto cjk = wrap_text(
        "战火中的亚山：圣堂、地狱、墓园、森林、学院和地下城六大派系的六场战役。", 120, body);
    CHECK(cjk.size() >= 2);
    for (const std::string &line : cjk)
        CHECK(text_width(line, body) <= 120.5f);
    CHECK(wrap_text("one\n\ntwo", 500, body).size() == 3);
    const std::string path = "~/.local/share/HommaRe/Games/Heroes of Might and Magic V";
    const std::string tail = fit_text(path, 150, body),
                      middle = fit_text(path, 150, body, Cut::Middle);
    CHECK(text_width(tail, body) <= 150.5f && tail.size() > 3 &&
          tail.compare(tail.size() - 3, 3, "…") == 0);
    CHECK(text_width(middle, body) <= 150.5f && middle.find("…") != std::string::npos &&
          middle.compare(middle.size() - 1, 1, "V") == 0);
    CHECK(fit_text("short", 500, body) == "short");
    // Glyphs land on the canvas.
    Canvas c;
    c.resize(200, 40, false);
    c.clear({0, 0, 0, 255});
    draw_text(c, 5, 30, "Play 开始", {Face::Label, 21}, {255, 255, 255, 255});
    int lit = 0;
    for (int i = 0; i < 200 * 40; ++i)
        lit += c.pixels()[i * 4] > 128;
    CHECK(lit > 100);
}

// Runs what the model's threads hand back until `done` or five seconds.
template <class Pred> bool settle(Model &m, Pred done) {
    for (int i = 0; i < 500 && !done(); ++i) {
        m.drain();
        os_sleep_us(10000);
    }
    m.drain();
    return done();
}

void test_model_and_screens() {
    Config c;
    std::string error;
    CHECK(parse_config(config_json(), &c, &error));
    const std::string games = fresh("games");
    write_file(games + "/Alpha Game/bin/game.exe", kExeBytes);
    os_mkdir((games + "/Alpha Game/data").c_str());
    write_file(games + "/Alpha Game/goggame-111.info", "{}");
    g_overlay = fresh("overlay");
    os_setenv("RECOMP_LAUNCHER_GAMES_DIR", games.c_str());
    os_setenv("RECOMP_LAUNCHER_SKIN", "0");
    const std::string state = g_scratch + "/profile/launcher.json";
    remove_tree(g_scratch + "/profile");
    Model m(c, "alpha", "", g_scratch + "/saves", g_scratch + "/manifests", state);
    CHECK(m.selected_id() == "alpha");
    int scans = 0;
    m.on_scanned = [&] { ++scans; };
    std::string played;
    m.on_play = [&](const Entry &e) { played = e.exe; };
    m.refresh();
    CHECK(settle(m, [&] { return m.has_scanned(); }));
    CHECK(scans == 1 && !m.scanning());
    CHECK(m.entries().size() == 3);
    CHECK(m.entries()[0].state.kind == GameState::Ready && m.entries()[0].can_play());
    CHECK(m.entries()[1].state.kind == GameState::NotFound && !m.entries()[1].playable_here);
    CHECK(installs(m.entries()[1]) && !installs(m.entries()[0]));
    CHECK(Model::import_base() == games);

    // The settings, and the startup videos the profile shadows.
    m.set_frame_limit(3);
    CHECK(m.frame_limit() == 3 && g_settings["host.display/frame_limit"] == 3);
    m.set_gpu_skin(false);
    CHECK(!m.gpu_skin() && g_settings["homm5.perf/gpu_skin"] == 2);
    m.set_skip_intro(true);
    CHECK(m.skip_intro() && exists(g_overlay + "/video/intro.xml") &&
          exists(g_overlay + "/video/intro_a1.xml"));
    m.set_skip_intro(false);
    CHECK(!m.skip_intro() && !exists(g_overlay + "/video/intro.xml"));

    // The screens at a desktop's sizes, a narrow window's and a phone-sized one.
    View v(m);
    Canvas canvas;
    for (auto size : {std::pair<int, int>{1280, 800}, {960, 640}, {600, 800}, {2560, 1600}}) {
        canvas.resize(size.first, size.second, true);
        v.draw(canvas, size.first > 2000 ? 2 : 1);
        CHECK(canvas.pixels()[3] == 255); // drawn opaque
    }
    canvas.resize(1280, 800, true);
    v.draw(canvas, 1);
    // A click on the list picks the game under it: found by moving down the rail.
    bool picked = false;
    for (float y = 100; y < 500 && !picked; y += 4) {
        if (!v.pointer(150, y))
            continue;
        v.draw(canvas, 1);
        v.press(150, y);
        v.release(150, y);
        picked = m.selected_id() == "beta";
        v.draw(canvas, 1);
    }
    CHECK(picked);
    // The arrows go round the list; Return presses Play.
    v.key(SDLK_UP, 0);
    CHECK(m.selected_id() == "alpha");
    v.key(SDLK_UP, 0);
    CHECK(m.selected_id() == "gamma");
    v.key(SDLK_DOWN, 0);
    v.draw(canvas, 1);
    v.key(SDLK_RETURN, 0);
    CHECK(played == games + "/Alpha Game/bin/game.exe");
    // The game is remembered, its folder too.
    std::string text;
    CHECK(read_text(state, &text) && text.find("\"lastGame\": \"alpha\"") != std::string::npos);
    CHECK(m.remembered_roots().size() == 1);
    // An alert, closed by Escape; no Settings without the game's look.
    m.set_notice("Something to say");
    v.draw(canvas, 1);
    v.key(SDLK_ESCAPE, 0);
    CHECK(m.notice().empty());
    CHECK(!v.key(SDLK_COMMA, SDL_KMOD_CTRL) && !v.settings_shown());
    // A drop of something that is no installer says so.
    write_file(g_scratch + "/notes.txt", "x");
    m.accept({g_scratch + "/notes.txt"});
    CHECK(m.notice().find("is not a GOG installer") != std::string::npos);
    m.clear_notice();
    // A language chosen is kept.
    m.set_language("de");
    CHECK(language() == "de" || getenv("RECOMP_LAUNCHER_LANG"));
    CHECK(read_text(state, &text) && text.find("\"language\": \"de\"") != std::string::npos);
    m.set_language("");
    os_unsetenv("RECOMP_LAUNCHER_GAMES_DIR");
    os_unsetenv("RECOMP_LAUNCHER_SKIN");
}

// A 4 x 4 uncompressed DDS in one colour.
std::string tiny_dds(uint32_t argb) {
    std::string d(128, '\0');
    auto put = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            d[at + size_t(i)] = char(v >> (8 * i));
    };
    put(0, 0x20534444); // "DDS "
    put(4, 124);
    put(12, 4);
    put(16, 4);
    put(76, 32);
    put(80, 0x41); // RGB with alpha
    put(88, 32);
    put(92, 0x00ff0000);
    put(96, 0x0000ff00);
    put(100, 0x000000ff);
    put(104, 0xff000000);
    for (int i = 0; i < 16; ++i)
        d += std::string(reinterpret_cast<const char *>(&argb), 4);
    return d;
}

// The game's look over a fake install whose data.pak holds every texture,
// tiny: Settings, its language menu, the menu's hover and choice.
void test_skinned_screens() {
    Config c;
    std::string error;
    CHECK(parse_config(config_json(), &c, &error));
    const std::string games = fresh("skinned");
    write_file(games + "/Alpha Game/bin/game.exe", kExeBytes);
    os_mkdir((games + "/Alpha Game/data").c_str());
    mz_zip_archive zip{};
    CHECK(mz_zip_writer_init_file(&zip, (games + "/Alpha Game/data/data.pak").c_str(), 0));
    for (int i = 0; i < int(Tex::Count); ++i) {
        const std::string dds = tiny_dds(0xff806040u + uint32_t(i));
        CHECK(mz_zip_writer_add_mem(&zip, tex_info(Tex(i)).path, dds.data(), dds.size(),
                                    MZ_BEST_SPEED));
    }
    CHECK(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
    os_setenv("RECOMP_LAUNCHER_GAMES_DIR", games.c_str());
    os_setenv("XDG_CACHE_HOME", (g_scratch + "/cache").c_str()); // the skin's bake
    Model m(c, "alpha", "", g_scratch + "/saves", g_scratch + "/manifests",
            g_scratch + "/profile2/launcher.json");
    m.refresh();
    CHECK(settle(m, [&] { return m.has_scanned(); }));
    CHECK(m.skin() != nullptr);
    View v(m);
    Canvas canvas;
    canvas.resize(1280, 800, false);
    v.draw(canvas, 1);
    CHECK(v.key(SDLK_COMMA, SDL_KMOD_CTRL) && v.settings_shown());
    v.draw(canvas, 1);
    // The language's name opens the menu: found by moving over the page.
    for (float y = 60; y < 400 && !v.menu_; y += 3)
        for (float x = 300; x < 900 && !v.menu_; x += 12)
            if (v.pointer(x, y)) {
                v.press(x, y);
                v.release(x, y);
                v.draw(canvas, 1);
            }
    CHECK(v.menu_);
    // An item lights up under the pointer, and choosing it sets the language.
    const Rect a = v.menu_anchor_;
    bool lit = false, chose = false;
    for (int y = a.y + 40; y < a.y + 300 && !chose; y += 6) {
        const float x = float(a.x) + 60;
        v.leave();
        v.draw(canvas, 1);
        const uint8_t *p = canvas.pixels() + (size_t(y) * 1280 + size_t(a.x) + 6) * 4;
        const int before = p[0] + p[1] + p[2];
        if (!v.pointer(x, float(y)))
            continue;
        v.draw(canvas, 1);
        if (p[0] + p[1] + p[2] <= before + 30)
            continue; // a divider: no item under the pointer
        lit = true;
        v.press(x, float(y));
        v.release(x, float(y));
        chose = !chosen_language().empty();
    }
    CHECK(lit && chose && !v.menu_);
    // Escape closes Settings; a progress dialog takes its place while installing.
    v.draw(canvas, 1);
    v.key(SDLK_ESCAPE, 0);
    CHECK(!v.settings_shown());
    // A drag over the window shows where to let go.
    v.draw(canvas, 1);
    const uint8_t centre = canvas.pixels()[(400 * 1280 + 640) * 4];
    v.dragging(true);
    v.draw(canvas, 1);
    CHECK(canvas.pixels()[(400 * 1280 + 640) * 4] != centre);
    v.dragging(false);
    m.set_language("");
    os_unsetenv("RECOMP_LAUNCHER_GAMES_DIR");
    os_unsetenv("XDG_CACHE_HOME");
}

} // namespace

int main() {
    const char *base = getenv("RECOMP_LAUNCHER_TEST_DIR");
    g_scratch = std::string(base && *base ? base : "build/recomp") + "/native-launcher-test";
    for (size_t i = g_scratch.find('/'); i != std::string::npos; i = g_scratch.find('/', i + 1))
        os_mkdir(g_scratch.substr(0, i).c_str());
    remove_tree(g_scratch);
    os_mkdir(g_scratch.c_str());
    if (g_scratch[0] != '/') {
        char cwd[4096];
        if (os_getcwd(cwd, sizeof cwd) == 0)
            g_scratch = std::string(cwd) + "/" + g_scratch;
    }

    struct Test {
        const char *name;
        void (*run)();
    } tests[] = {
        {"words", test_l10n},
        {"config and manifests", test_config},
        {"detection", test_detection},
        {"layout", test_layout},
        {"text", test_text},
        {"model and screens", test_model_and_screens},
        {"the game's look", test_skinned_screens},
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
