// The clean-up of Metal's gpuarchiver work folders (metal/gpuarchiver_cleanup.h). No device, no
// game: plain files in a temporary folder.
#include "../metal/gpuarchiver_cleanup.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0, checks = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                       \
        }                                                                                          \
    } while (0)

static bool exists(const std::string &p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0;
}
static void touch(const std::string &p) {
    if (FILE *f = fopen(p.c_str(), "wb"))
        fclose(f);
}

int main() {
    const char *tmpdir = getenv("TMPDIR");
    std::string root = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/gac-XXXXXX";
    if (!mkdtemp(root.data())) {
        std::fprintf(stderr, "no temporary folder\n");
        return 1;
    }
    {   // only gpuarchiver-* entries inside com.apple.gpuarchiver, links not followed
        const std::string g = root + "/com.apple.gpuarchiver";
        mkdir(g.c_str(), 0700);
        mkdir((g + "/gpuarchiver-old").c_str(), 0700);
        mkdir((g + "/gpuarchiver-old/sub").c_str(), 0700);
        touch(g + "/gpuarchiver-old/sub/f");
        const std::string outside = root + "/outside.txt";
        touch(outside);
        CHECK(symlink(outside.c_str(), (g + "/gpuarchiver-old/link").c_str()) == 0);
        mkdir((g + "/other").c_str(), 0700);
        const std::set<std::string> before = gpuarchiver_cleanup::snapshot(g);
        CHECK(before.size() == 1 && before.count("gpuarchiver-old"));
        mkdir((g + "/gpuarchiver-new").c_str(), 0700);
        touch(g + "/gpuarchiver-new/f");
        touch(g + "/gpuarchiver-file");
        // after an archive write: only what appeared since the snapshot
        uint32_t n = gpuarchiver_cleanup::remove_where(
            g, [&](const gpuarchiver_cleanup::Entry &e) { return !before.count(e.name); });
        CHECK(n == 2 && !exists(g + "/gpuarchiver-new") && !exists(g + "/gpuarchiver-file"));
        CHECK(exists(g + "/gpuarchiver-old"));
        // at start: entries older than a limit
        n = gpuarchiver_cleanup::remove_where(g, [](const gpuarchiver_cleanup::Entry &e) { return e.mtime < 0; });
        CHECK(n == 0 && exists(g + "/gpuarchiver-old"));
        n = gpuarchiver_cleanup::remove_where(g, [](const gpuarchiver_cleanup::Entry &) { return true; });
        CHECK(n == 1 && !exists(g + "/gpuarchiver-old"));
        // the link went, its target did not; other names stay
        CHECK(exists(outside) && exists(g + "/other"));
        // a folder of another name is never cleaned
        mkdir((root + "/gpuarchiver-x").c_str(), 0700);
        CHECK(gpuarchiver_cleanup::remove_where(root, [](const gpuarchiver_cleanup::Entry &) { return true; }) == 0);
        CHECK(exists(root + "/gpuarchiver-x"));
        // a missing folder is no error
        CHECK(gpuarchiver_cleanup::remove_where(root + "/none/com.apple.gpuarchiver",
                                                [](const gpuarchiver_cleanup::Entry &) { return true; }) == 0);
    }
    // leave nothing behind
    nftw(root.c_str(), gpuarchiver_cleanup::remove_one, 16, FTW_DEPTH | FTW_PHYS);
    std::printf("gpuarchiver_cleanup_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
