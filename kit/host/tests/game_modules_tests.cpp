// game_modules_tests.cpp - where a multi-game app finds its games' modules
// (game_modules.h), for every platform's layout on any machine.
#include "../game_modules.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using Ids = std::vector<std::string>;

static void test_file_names() {
    CHECK(game_module_file(ModuleLayout::Apple, "second") == "libgame.second.dylib");
    CHECK(game_module_file(ModuleLayout::Linux, "second") == "libgame.second.so");
    CHECK(game_module_file(ModuleLayout::Windows, "second") == "libgame.second.dll");
}

static void test_modules_sit_beside_the_app() {
    CHECK(game_modules_dir(ModuleLayout::Apple, "/Applications/Game.app/Contents/MacOS/Game") ==
          "/Applications/Game.app/Contents/Frameworks");
    CHECK(game_modules_dir(ModuleLayout::Apple, "/private/var/containers/Bundle/X/Game.app/Game") ==
          "/private/var/containers/Bundle/X/Game.app/Frameworks");
    CHECK(game_modules_dir(ModuleLayout::Linux, "/home/me/Game/Game") == "/home/me/Game/modules");
    // os.h hands Windows paths out in '/', drive letters and spaces kept.
    CHECK(game_modules_dir(ModuleLayout::Windows, "C:/Program Files/Game/Game.exe") ==
          "C:/Program Files/Game/modules");
    CHECK(game_modules_dir(ModuleLayout::Windows, "D:/Игры/Game/Game.exe") ==
          "D:/Игры/Game/modules");
}

static void test_ids_from_a_listing() {
    const Ids names = {
        "libgame.second.dll", "libgame.third.dll",      "libgame.second.so", "libgame..dll",
        "game.first.dll",     "libgame.fourth.dll.bak", "SDL3.dll"};
    CHECK(game_module_ids(ModuleLayout::Windows, names) == (Ids{"second", "third"}));
    CHECK(game_module_ids(ModuleLayout::Linux, names) == (Ids{"second"}));
    CHECK(game_module_ids(ModuleLayout::Apple, names).empty());
    CHECK(game_module_ids(ModuleLayout::Apple, {"libgame.a.dylib", "libSDL3.0.dylib"}) ==
          (Ids{"a"}));
}

static void test_a_module_finds_its_resources() {
    CHECK(game_module_resource_dirs(ModuleLayout::Windows, "C:/Games/My Game/modules/libgame.x.dll",
                                    "Game-X") == (Ids{"C:/Games/My Game/resources/games/Game-X"}));
    CHECK(game_module_resource_dirs(ModuleLayout::Linux, "/opt/Game/modules/libgame.x.so",
                                    "Game-X") == (Ids{"/opt/Game/resources/games/Game-X"}));
    CHECK(
        game_module_resource_dirs(ModuleLayout::Apple,
                                  "/A/Game.app/Contents/Frameworks/libgame.x.dylib", "Game-X") ==
        (Ids{"/A/Game.app/Contents/Resources/games/Game-X", "/A/Game.app/Contents/games/Game-X"}));
    // The module file and the folder it is in round-trip through the app's own path.
    const std::string dir = game_modules_dir(ModuleLayout::Windows, "C:/G/Game.exe");
    CHECK(game_module_resource_dirs(ModuleLayout::Windows,
                                    dir + "/" + game_module_file(ModuleLayout::Windows, "x"),
                                    "Game-X")[0] == "C:/G/resources/games/Game-X");
}

int main() {
    test_file_names();
    test_modules_sit_beside_the_app();
    test_ids_from_a_listing();
    test_a_module_finds_its_resources();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("game_modules_tests: ok\n");
    return 0;
}
