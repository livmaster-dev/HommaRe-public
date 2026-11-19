// ui_mods_tests.cpp - the interface mods' bookkeeping (host/ui_mods.cpp) over
// recipe and profile folders in a scratch directory: an app with a desktop
// set and a touch set of recipes keeps only the mods of the set it applies.
// No game: the mods are not built here, only their outputs looked at.
#include "../ui_mods.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int g_failures = 0, g_checks = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(c)) {                                                                                \
            ++g_failures;                                                                          \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                  \
        }                                                                                          \
    } while (0)

static void write_file(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

static std::string recipe(const std::string &name, const std::string &out) {
    return "recipe\t2\nname\t" + name + "\nout\t" + out + "\nlayer\tdata/data.pak\n";
}

int main() {
    const char *base = getenv("RECOMP_UI_MODS_TEST_DIR");
    const fs::path dir = fs::absolute(std::string(base && *base ? base : "build/recomp") + "/ui-mods-test");
    fs::remove_all(dir);
    const fs::path desktop = dir / "ui-mods", touch = dir / "ui-mods-touch", profile = dir / "profile";
    write_file(desktop / "Widescreen.recipe", recipe("Widescreen", "UserMODs/Widescreen.h5u"));
    write_file(touch / "Widescreen.recipe", recipe("Widescreen", "UserMODs/Widescreen.h5u"));
    write_file(touch / "TouchUI.recipe", recipe("TouchUI", "UserMODs/TouchUI.h5u"));
    write_file(touch / "PhoneUI.recipe", recipe("PhoneUI", "phone/UserMODs/PhoneUI.h5u"));
    write_file(touch / "notes.txt", "not a recipe");
    // What a start in touch form left behind: every touch mod and its stamp.
    for (const char *name : {"Widescreen", "TouchUI", "PhoneUI"}) {
        write_file(profile / "UserMODs" / (std::string(name) + ".h5u"), "PK");
        write_file(profile / ".ui-mods" / (std::string(name) + ".stamp"), "key");
    }

    // Now the desktop's set: the touch screen's own mods go, the shared one stays.
    host_ui_mods_retire(profile.string(), touch.string(), desktop.string());
    CHECK(fs::exists(profile / "UserMODs/Widescreen.h5u"));
    CHECK(fs::exists(profile / ".ui-mods/Widescreen.stamp"));
    CHECK(!fs::exists(profile / "UserMODs/TouchUI.h5u"));
    CHECK(!fs::exists(profile / ".ui-mods/TouchUI.stamp"));
    // A phone/ mod is kept in the profile without the prefix.
    CHECK(!fs::exists(profile / "UserMODs/PhoneUI.h5u"));
    CHECK(!fs::exists(profile / ".ui-mods/PhoneUI.stamp"));

    // The touch set applied: the desktop's has nothing the touch set lacks.
    host_ui_mods_retire(profile.string(), desktop.string(), touch.string());
    CHECK(fs::exists(profile / "UserMODs/Widescreen.h5u"));
    // No other set (an app with one): nothing happens.
    host_ui_mods_retire(profile.string(), "", desktop.string());
    host_ui_mods_retire(profile.string(), (dir / "missing").string(), desktop.string());
    CHECK(fs::exists(profile / "UserMODs/Widescreen.h5u"));

    fs::remove_all(dir);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
