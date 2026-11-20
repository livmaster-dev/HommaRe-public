// The work folders Metal leaves while it writes a binary archive
// (<Caches>[/<bundle id>]/com.apple.gpuarchiver/gpuarchiver-*): it never removes them, and each
// write of the pipeline archive (d3d9_metal.mm, class ShaderWarm) left one more behind.
// Kept free of Metal so its rules can be tested without a device
// (tests/gpuarchiver_cleanup_tests.cpp).
//
// Rules:
//  - only entries named gpuarchiver-* inside a folder named com.apple.gpuarchiver are removed;
//  - links are removed, never followed; nothing outside such a folder is touched;
//  - the caller picks which entries go (the ones that appeared during its own archive write, or at
//    start the ones untouched for a while).
#pragma once

#include <dirent.h>
#include <ftw.h>
#include <sys/stat.h>
#include <time.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace gpuarchiver_cleanup {

struct Entry {
    std::string name;
    time_t mtime = 0;
};

inline std::vector<Entry> list(const std::string &folder) {
    std::vector<Entry> out;
    if (DIR *d = opendir(folder.c_str())) {
        while (struct dirent *e = readdir(d)) {
            if (strncmp(e->d_name, "gpuarchiver-", 12) != 0)
                continue;
            struct stat st;
            if (lstat((folder + "/" + e->d_name).c_str(), &st) != 0)
                continue;
            out.push_back({e->d_name, st.st_mtime});
        }
        closedir(d);
    }
    return out;
}

inline int remove_one(const char *path, const struct stat *, int, struct FTW *) {
    remove(path);   // files and links first, each folder once empty (FTW_DEPTH)
    return 0;
}

inline bool folder_ok(const std::string &folder) {
    static const char kName[] = "/com.apple.gpuarchiver";
    const size_t n = sizeof kName - 1;
    return folder.size() > n && folder.compare(folder.size() - n, n, kName) == 0;
}

// Removes the entries `drop` picks; returns how many.
inline uint32_t remove_where(const std::string &folder, const std::function<bool(const Entry &)> &drop) {
    if (!folder_ok(folder))
        return 0;
    uint32_t n = 0;
    for (const Entry &e : list(folder)) {
        if (!drop(e))
            continue;
        const std::string p = folder + "/" + e.name;
        struct stat st;
        if (lstat(p.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            nftw(p.c_str(), remove_one, 16, FTW_DEPTH | FTW_PHYS);
        else
            remove(p.c_str());
        if (lstat(p.c_str(), &st) != 0)
            ++n;
    }
    return n;
}

// The names there now (taken before an archive write).
inline std::set<std::string> snapshot(const std::string &folder) {
    std::set<std::string> s;
    for (const Entry &e : list(folder))
        s.insert(e.name);
    return s;
}

}   // namespace gpuarchiver_cleanup
