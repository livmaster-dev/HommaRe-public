// game_modules.h - where a multi-game app keeps its games' modules
// (RECOMP_GAME_MODULES, host/sdl/main.cpp, tools/assemble_games.py):
//
//   macOS    <App>.app/Contents/Frameworks/libgame.<id>.dylib,
//            resources in Contents/Resources/games/<app>
//   iOS      <App>.app/Frameworks/libgame.<id>.dylib, resources in games/<app>
//   Linux    <App>/modules/libgame.<id>.so, resources in resources/games/<app>
//   Windows  <App>\modules\libgame.<id>.dll, resources in resources\games\<app>
//
// String work only, on paths in '/' as platform/os.h hands them out, so
// host/tests/game_modules_tests.cpp checks every layout on any machine.
#pragma once
#include <string>
#include <vector>

enum class ModuleLayout { Apple, Linux, Windows };

#if defined(__APPLE__)
constexpr ModuleLayout kModuleLayout = ModuleLayout::Apple;
#elif defined(_WIN32)
constexpr ModuleLayout kModuleLayout = ModuleLayout::Windows;
#else
constexpr ModuleLayout kModuleLayout = ModuleLayout::Linux;
#endif

// "libgame.<id>.dylib" (.so, .dll).
std::string game_module_file(ModuleLayout layout, const std::string &id);

// The folder the modules sit in, for the app at `exe_path`.
std::string game_modules_dir(ModuleLayout layout, const std::string &exe_path);

// The game ids of the modules among a folder's `names`, in their order.
std::vector<std::string> game_module_ids(ModuleLayout layout,
                                         const std::vector<std::string> &names);

// Where the module at `module_path` may find its own resources, most likely
// first; the caller takes the first that exists.
std::vector<std::string> game_module_resource_dirs(ModuleLayout layout,
                                                   const std::string &module_path,
                                                   const std::string &app_name);
