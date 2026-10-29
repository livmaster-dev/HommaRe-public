// game_modules.cpp - where a multi-game app keeps its games' modules
// (game_modules.h).
#include "game_modules.h"

namespace {

std::string parent(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool ends_with(const std::string &s, const std::string &tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

const char *suffix(ModuleLayout layout) {
    switch (layout) {
    case ModuleLayout::Apple:
        return ".dylib";
    case ModuleLayout::Windows:
        return ".dll";
    default:
        return ".so";
    }
}

const char kPrefix[] = "libgame.";

} // namespace

std::string game_module_file(ModuleLayout layout, const std::string &id) {
    return kPrefix + id + suffix(layout);
}

std::string game_modules_dir(ModuleLayout layout, const std::string &exe_path) {
    const std::string dir = parent(exe_path);
    if (layout != ModuleLayout::Apple)
        return dir + "/modules";
    static const std::string mac = "/Contents/MacOS";
    if (dir.size() > mac.size() && ends_with(dir, mac))
        return dir.substr(0, dir.size() - mac.size()) + "/Contents/Frameworks";
    return dir + "/Frameworks"; // iOS: a flat bundle
}

std::vector<std::string> game_module_ids(ModuleLayout layout,
                                         const std::vector<std::string> &names) {
    const std::string prefix = kPrefix, tail = suffix(layout);
    std::vector<std::string> ids;
    for (const std::string &name : names)
        if (name.size() > prefix.size() + tail.size() &&
            name.compare(0, prefix.size(), prefix) == 0 && ends_with(name, tail))
            ids.push_back(name.substr(prefix.size(), name.size() - prefix.size() - tail.size()));
    return ids;
}

std::vector<std::string> game_module_resource_dirs(ModuleLayout layout,
                                                   const std::string &module_path,
                                                   const std::string &app_name) {
    // Up from Frameworks (modules/): Contents on macOS, the app on iOS, the
    // app's folder on Linux and Windows.
    const std::string dir = parent(parent(module_path));
    if (layout != ModuleLayout::Apple)
        return {dir + "/resources/games/" + app_name};
    return {dir + "/Resources/games/" + app_name, dir + "/games/" + app_name};
}
