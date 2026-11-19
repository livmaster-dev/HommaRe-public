// android_files.cpp - see android_files.h.
#include "android_files.h"

#include "../../../platform/os.h"
#include "../launcher.h"
#include "../launcher_sdl.h"
#include "detect.h"
#include "l10n.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace launcher::native::android {

namespace {

std::string join(const std::string &a, const std::string &b) {
    return a.empty() || a.back() == '/' ? a + b : a + "/" + b;
}

void make_dirs(const std::string &dir) {
    for (size_t i = dir.find('/', 1); i != std::string::npos; i = dir.find('/', i + 1))
        os_mkdir(dir.substr(0, i).c_str());
    os_mkdir(dir.c_str());
}

void remove_tree(const std::string &path) {
    OsStat st{};
    if (os_lstat(path.c_str(), &st) != 0)
        return;
    if (st.is_dir) {
        std::vector<std::string> names;
        os_listdir(
            path.c_str(),
            [](const char *name, void *user) {
                static_cast<std::vector<std::string> *>(user)->push_back(name);
                return 0;
            },
            &names);
        for (const std::string &n : names)
            if (n != "." && n != "..")
                remove_tree(join(path, n));
        rmdir(path.c_str());
    } else {
        os_unlink(path.c_str());
    }
}

// A name the picker showed, safe as one path component.
std::string component(std::string name) {
    for (char &c : name)
        if (c == '/' || c == '\0')
            c = '_';
    return name.empty() || name == "." || name == ".." ? "picked" : name;
}

// A picked folder's name from its tree URI's document id ("primary:Download/x").
std::string tree_name(const ::launcher::Picked &p) {
    std::string name = p.name;
    for (const char sep : {'/', ':'}) {
        const size_t at = name.find_last_of(sep);
        if (at != std::string::npos)
            name = name.substr(at + 1);
    }
    std::string decoded;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '%' && i + 2 < name.size()) {
            decoded.push_back(char(strtol(name.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else {
            decoded.push_back(name[i]);
        }
    }
    return component(decoded);
}

// The documents linked by the last pick stay open while the app runs: the
// links name them through these descriptors.
std::mutex g_open_lock;
std::vector<int> g_open;

void keep_open(int fd) {
    std::lock_guard<std::mutex> lock(g_open_lock);
    g_open.push_back(fd);
}

void close_kept() {
    std::lock_guard<std::mutex> lock(g_open_lock);
    for (int fd : g_open)
        close(fd);
    g_open.clear();
}

// Where the links go: the app's internal storage, which holds symbolic
// links (its external files folder does not), emptied on each pick.
std::string staging_dir() {
    const char *internal = SDL_GetAndroidInternalStoragePath();
    return internal ? join(internal, "picked") : "/nonexistent";
}

struct Doc {
    std::string uri, relative;
    uint64_t size = 0;
};

// Copies `fd` (from its start) into `path`, adding to the progress as it goes;
// `last` is when the progress was last reported (shared by the files of one
// import, so a run of small files is reported too).
Outcome copy_fd(int fd, const std::string &path, Progress &p, const ProgressFn &progress, uint64_t &last) {
    const int out = os_fd_open(path.c_str(), OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC);
    if (out < 0)
        return Outcome::fail(t("importFailed", {{"reason", path + ": " + strerror(errno)}}));
    lseek(fd, 0, SEEK_SET);
    std::vector<uint8_t> buf(1 << 20);
    for (;;) {
        const ssize_t n = read(fd, buf.data(), buf.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            os_fd_close(out);
            if (n < 0) {
                os_unlink(path.c_str());
                return Outcome::fail(t("importFailed", {{"reason", strerror(errno)}}));
            }
            return {};
        }
        for (ssize_t off = 0; off < n;) {
            const int64_t w = os_fd_write(out, buf.data() + off, size_t(n - off));
            if (w <= 0) {
                const std::string why = strerror(errno);
                os_fd_close(out);
                os_unlink(path.c_str());
                return Outcome::fail(t("importFailed", {{"reason", why}}));
            }
            off += w;
        }
        p.bytes_done += n;
        if (SDL_GetTicks() - last > 200) {
            last = SDL_GetTicks();
            if (!progress(p)) {
                os_fd_close(out);
                os_unlink(path.c_str());
                return Outcome::cancel();
            }
        }
    }
}

// The installers' documents under `dir`, each by its own name: a link to its
// descriptor when this process can open the document again through it (the
// system then lets innoextract, a child that inherits the descriptor, read
// it in place), else a copy.
Outcome link_documents(const std::vector<Doc> &docs, const std::string &dir, Progress &p,
                       const ProgressFn &progress) {
    // The links of an earlier pick name descriptors closed here: all go.
    close_kept();
    remove_tree(staging_dir());
    make_dirs(dir);
    p.phase = Progress::Copying;
    p.files_total = int(docs.size());
    p.bytes_total = 0;
    for (const Doc &d : docs)
        p.bytes_total += int64_t(d.size);
    uint64_t last = 0;
    for (const Doc &d : docs) {
        p.current = d.relative;
        if (!progress(p))
            return Outcome::cancel();
        const std::string at = join(dir, d.relative);
        make_dirs(parent_dir(at));
        const int opened = ::launcher::android_open_document(d.uri);
        // A duplicate is not closed on exec: innoextract inherits it.
        const int fd = opened >= 0 ? dup(opened) : -1;
        if (opened >= 0)
            close(opened);
        if (fd < 0)
            return Outcome::fail(t("importFailed", {{"reason", d.relative + ": " + strerror(errno)}}));
        // The launcher only looks at the link (its name, its size through
        // stat); innoextract reads through the descriptor it names
        // (android-patches/ in third_party/innoextract), since opening a
        // document again by name is what Android refuses (a file in
        // Downloads that another app made).
        const std::string target = "/proc/self/fd/" + std::to_string(fd);
        bool linked = false;
        if (symlink(target.c_str(), at.c_str()) == 0) {
            struct stat st{};
            if (stat(at.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                linked = true;
            } else {
                fprintf(stderr, "[launcher] %s cannot be looked at through its link (%s): copying it\n",
                        d.relative.c_str(), strerror(errno));
                os_unlink(at.c_str());
            }
        }
        if (linked) {
            keep_open(fd);
            p.bytes_done += int64_t(d.size);
        } else {
            const Outcome o = copy_fd(fd, at, p, progress, last);
            close(fd);
            if (!o.ok)
                return o;
        }
        p.files_done += 1;
    }
    progress(p);
    return {};
}

bool installer_name(const std::string &name) {
    std::string base;
    int index = 0;
    return finder::is_setup_name(name) || finder::is_patch_name(name) || finder::slice(name, &base, &index);
}

// A picked folder: the installers in it, linked; or, when it holds none, the
// game in it copied into the Games folder.
Outcome materialize_tree(const ::launcher::Picked &picked, const std::string &games_dir,
                         const std::vector<std::string> &exclude, const ProgressFn &progress,
                         std::vector<std::string> *paths) {
    Progress p;
    p.name = tree_name(picked);
    p.phase = Progress::Copying;
    progress(p);
    std::vector<::launcher::Entry> entries;
    std::vector<std::string> documents;
    std::string error;
    if (!::launcher::android_list_tree(picked.uri, &entries, &documents, &error))
        return Outcome::fail(t("importFailed", {{"reason", error}}));
    std::vector<Doc> installers;
    for (const ::launcher::Entry &e : entries)
        if (!e.is_dir && installer_name(file_name(e.relative)))
            installers.push_back({documents[e.index], e.relative, e.size});
    if (!installers.empty()) {
        const std::string dir = join(staging_dir(), p.name);
        const Outcome o = link_documents(installers, dir, p, progress);
        if (o.ok)
            paths->push_back(dir);
        return o;
    }
    // An installed game, copied whole but for what the app never reads.
    const std::string dest = join(games_dir, p.name);
    std::vector<const ::launcher::Entry *> files;
    p.bytes_total = 0;
    for (const ::launcher::Entry &e : entries)
        if (!e.is_dir && !::launcher::excluded(e.relative, exclude)) {
            files.push_back(&e);
            p.bytes_total += int64_t(e.size);
        }
    p.files_total = int(files.size());
    uint64_t last = 0;
    for (const ::launcher::Entry *e : files) {
        const std::string at = join(dest, e->relative);
        p.current = e->relative;
        if (SDL_GetTicks() - last > 200) {
            last = SDL_GetTicks();
            if (!progress(p))
                return Outcome::cancel();
        }
        // A file already copied whole (an import that was cancelled) stays.
        OsStat st{};
        if (os_stat(at.c_str(), &st) == 0 && st.is_regular && uint64_t(st.size) == e->size) {
            p.bytes_done += int64_t(e->size);
            p.files_done += 1;
            continue;
        }
        make_dirs(parent_dir(at));
        const int fd = ::launcher::android_open_document(documents[e->index]);
        if (fd < 0)
            return Outcome::fail(t("importFailed", {{"reason", e->relative}}));
        const Outcome o = copy_fd(fd, at + ".part", p, progress, last);
        close(fd);
        if (!o.ok)
            return o;
        if (rename((at + ".part").c_str(), at.c_str()) != 0)
            return Outcome::fail(t("importFailed", {{"reason", at + ": " + strerror(errno)}}));
        p.files_done += 1;
    }
    progress(p);
    paths->push_back(dest);
    return {};
}

} // namespace

void forget_picked() {
    close_kept();
    remove_tree(staging_dir());
}

std::string data_dir() {
    const char *external = SDL_GetAndroidExternalStoragePath();
    return external ? external : "";
}

void pick(bool folder, const std::string &games_dir, const std::vector<std::string> &exclude,
          std::function<void(Materialize)> done) {
    if (folder) {
        ::launcher::android_pick_tree([=](std::vector<::launcher::Picked> picked, std::string) {
            if (picked.empty())
                return;
            const ::launcher::Picked tree = picked.front();
            done([=](const ProgressFn &progress, std::vector<std::string> *paths) {
                return materialize_tree(tree, games_dir, exclude, progress, paths);
            });
        });
        return;
    }
    ::launcher::android_pick_documents([=](std::vector<::launcher::Picked> picked, std::string) {
        if (picked.empty())
            return;
        std::vector<Doc> docs;
        for (const ::launcher::Picked &p : picked)
            docs.push_back({p.uri, component(p.name), 0});
        done([=](const ProgressFn &progress, std::vector<std::string> *paths) {
            Progress p;
            p.name = docs.front().relative;
            std::vector<Doc> sized = docs;
            // The sizes, for the progress of a copy (a link costs nothing).
            for (Doc &d : sized) {
                const int fd = ::launcher::android_open_document(d.uri);
                struct stat st{};
                if (fd >= 0 && fstat(fd, &st) == 0)
                    d.size = uint64_t(st.st_size);
                if (fd >= 0)
                    close(fd);
            }
            const std::string dir = join(staging_dir(), "installers");
            const Outcome o = link_documents(sized, dir, p, progress);
            if (o.ok)
                for (const Doc &d : sized)
                    paths->push_back(join(dir, d.relative));
            return o;
        });
    });
}

} // namespace launcher::native::android
