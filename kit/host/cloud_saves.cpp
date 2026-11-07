// cloud_saves.cpp - the saves kept in step with a synced folder (cloud_saves.h).
#include "cloud_saves.h"

#include <sys/stat.h>
#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace cloud_saves {

namespace {

constexpr int64_t kBusyNs = 3'000'000'000ll;     // a file this young may be being written
constexpr int64_t kSameTimeNs = 2'000'000'000ll; // two stamps this close are one save
constexpr size_t kBackupsKept = 10;

bool same_name(const std::string &a, const char *b) {
    size_t n = strlen(b);
    if (a.size() != n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

bool ends_with_cfg(const std::string &s) {
    return s.size() > 4 && same_name(s.substr(s.size() - 4), ".cfg");
}

std::vector<std::string> split(const std::string &rel) {
    std::vector<std::string> parts;
    size_t at = 0;
    while (at <= rel.size()) {
        size_t end = rel.find('/', at);
        if (end == std::string::npos)
            end = rel.size();
        parts.push_back(rel.substr(at, end - at));
        at = end + 1;
    }
    return parts;
}

} // namespace

namespace detail {

bool stat_path(const std::string &path, Stamp *out) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
#ifdef __APPLE__
    const struct timespec &m = st.st_mtimespec;
#else
    const struct timespec &m = st.st_mtim;
#endif
    out->size = static_cast<int64_t>(st.st_size);
    out->mtime_ns = static_cast<int64_t>(m.tv_sec) * 1'000'000'000ll + m.tv_nsec;
    return true;
}

bool set_mtime(const std::string &path, int64_t mtime_ns) {
    struct timespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = static_cast<time_t>(mtime_ns / 1'000'000'000ll);
    times[1].tv_nsec = static_cast<long>(mtime_ns % 1'000'000'000ll);
    return utimensat(AT_FDCWD, path.c_str(), times, 0) == 0;
}

// A temporary name beside `path`, hidden (listings skip dot files).
std::string temp_beside(const std::string &path) {
    const fs::path p(path);
    return (p.parent_path() / ("." + p.filename().string() + ".cloud-tmp")).string();
}

bool copy_keeping_time(const std::string &src, const std::string &dst) {
    std::error_code ec;
    Stamp st;
    if (!stat_path(src, &st))
        return false;
    fs::create_directories(fs::path(dst).parent_path(), ec);
    const std::string tmp = temp_beside(dst);
    fs::remove(tmp, ec);
    if (!fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec)) {
        fs::remove(tmp, ec);
        return false;
    }
    set_mtime(tmp, st.mtime_ns);
    fs::rename(tmp, dst, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool same_bytes(const std::string &a, const std::string &b) {
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb)
        return false;
    char ba[65536], bb[65536];
    for (;;) {
        fa.read(ba, sizeof ba);
        fb.read(bb, sizeof bb);
        const std::streamsize na = fa.gcount(), nb = fb.gcount();
        if (na != nb || memcmp(ba, bb, static_cast<size_t>(na)) != 0)
            return false;
        if (na == 0)
            return true;
    }
}

bool list_files(const std::string &root, Listing *out) {
    std::error_code ec;
    if (!fs::exists(root, ec))
        return !ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec)
        return false;
    for (; it != end; it.increment(ec)) {
        if (ec)
            return false;
        const std::string name = it->path().filename().string();
        if (!name.empty() && name[0] == '.') {
            if (it->is_directory(ec))
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec))
            continue;
        Entry e;
        if (!stat_path(it->path().string(), &e.stamp))
            continue;
        (*out)[fs::relative(it->path(), root, ec).generic_string()] = e;
    }
    return true;
}

} // namespace detail

namespace {

using detail::copy_keeping_time;
using detail::list_files;
using detail::same_bytes;
using detail::set_mtime;
using detail::stat_path;
using detail::temp_beside;

class PlainIo : public Io {
public:
    bool list(const std::string &root, Listing *out) override { return list_files(root, out); }
    bool read(const std::string &src, const std::string &dst) override { return copy_keeping_time(src, dst); }
    bool write(const std::string &src, const std::string &dst, Stamp *now) override {
        return copy_keeping_time(src, dst) && stat_path(dst, now);
    }
    bool remove(const std::string &path) override {
        std::error_code ec;
        return fs::remove(path, ec) || !fs::exists(path, ec);
    }
};

// The state file: one line per synced file, "<path>\t<local size>\t<local
// mtime ns>\t<cloud size>\t<cloud mtime ns>".
struct Seen {
    Stamp local, cloud;
};
using State = std::map<std::string, Seen>;

// The first line names the synced folder: another folder's state is none.
State load_state(const std::string &path, const std::string &cloud) {
    State s;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    if (!std::getline(f, line) || line != "#cloud\t" + cloud)
        return s;
    while (std::getline(f, line)) {
        std::vector<std::string> cols;
        size_t at = 0;
        while (at <= line.size()) {
            size_t end = line.find('\t', at);
            if (end == std::string::npos)
                end = line.size();
            cols.push_back(line.substr(at, end - at));
            at = end + 1;
        }
        if (cols.size() != 5 || cols[0].empty())
            continue;
        Seen seen;
        seen.local = {atoll(cols[1].c_str()), atoll(cols[2].c_str())};
        seen.cloud = {atoll(cols[3].c_str()), atoll(cols[4].c_str())};
        s[cols[0]] = seen;
    }
    return s;
}

bool save_state(const std::string &path, const std::string &cloud, const State &s) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = temp_beside(path);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f << "#cloud\t" << cloud << '\n';
        for (const auto &[rel, seen] : s)
            f << rel << '\t' << seen.local.size << '\t' << seen.local.mtime_ns << '\t' << seen.cloud.size << '\t'
              << seen.cloud.mtime_ns << '\n';
        if (!f)
            return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

std::string join(const std::string &root, const std::string &rel) {
    return (fs::path(root) / fs::path(rel)).string();
}

// This sync's backup folder, made when something is first put in it.
class Backup {
public:
    Backup(const std::string &root, int64_t now_ns) : root_(root) {
        const time_t t = static_cast<time_t>(now_ns / 1'000'000'000ll);
        struct tm tm{};
        localtime_r(&t, &tm);
        char name[32];
        strftime(name, sizeof name, "%Y-%m-%d %H%M%S", &tm);
        dir_ = join(root, name);
    }
    std::string path(const std::string &rel) {
        used_ = true;
        return join(dir_, rel);
    }
    // The newest kBackupsKept folders stay.
    void prune() {
        if (!used_)
            return;
        std::error_code ec;
        std::vector<fs::path> dirs;
        for (const auto &e : fs::directory_iterator(root_, ec))
            if (e.is_directory(ec))
                dirs.push_back(e.path());
        std::sort(dirs.begin(), dirs.end());
        for (size_t i = 0; i + kBackupsKept < dirs.size(); ++i)
            fs::remove_all(dirs[i], ec);
    }

private:
    std::string root_, dir_;
    bool used_ = false;
};

} // namespace

Kind classify(const std::string &rel) {
    const std::vector<std::string> parts = split(rel);
    for (const std::string &p : parts)
        if (p.empty() || p[0] == '.')
            return Kind::None;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!same_name(parts[i], "Profiles"))
            continue;
        const size_t below = parts.size() - i - 1; // components under Profiles/
        if (below >= 3 && same_name(parts[i + 2], "Saves"))
            return Kind::Sync; // <player>/Saves/...
        if ((below == 1 || below == 2) && ends_with_cfg(parts.back()))
            return Kind::Seed; // global.cfg, <player>/user.cfg, input.cfg
        return Kind::None;
    }
    return Kind::None;
}

Io &plain_io() {
    static PlainIo io;
    return io;
}

// Plain files unless the platform has its own (cloud_saves_apple.mm); the
// tests link this one.
__attribute__((weak)) Io &platform_io() {
    return plain_io();
}

Paths paths_for(const std::string &profile_dir, const std::string &cloud_root, const std::string &game_id) {
    Paths p;
    p.local = join(profile_dir, "Documents/My Games");
    p.cloud = join(cloud_root, game_id);
    p.state = join(profile_dir, "cloud-saves/state.tsv");
    p.backup = join(profile_dir, "cloud-saves/backup");
    return p;
}

Stats sync(const Paths &paths, Io &cloud, int64_t now_ns) {
    Stats stats;
    Listing local, remote;
    if (!list_files(paths.local, &local)) {
        stats.error = "the saves cannot be read";
        return stats;
    }
    if (!cloud.list(paths.cloud, &remote)) {
        stats.error = "the synced folder cannot be read";
        return stats;
    }
    State state = load_state(paths.state, paths.cloud);
    const auto synced = [](const Listing &l) {
        return std::count_if(l.begin(), l.end(), [](const auto &kv) { return classify(kv.first) == Kind::Sync; });
    };
    // A side whose synced files are all gone lost its folder: it takes the
    // other side's files back rather than deleting them there.
    const bool local_lost = !state.empty() && synced(local) == 0;
    const bool cloud_lost = !state.empty() && synced(remote) == 0;
    Backup backup(paths.backup, now_ns);

    std::set<std::string> all;
    for (const auto &kv : local)
        all.insert(kv.first);
    for (const auto &kv : remote)
        all.insert(kv.first);
    for (const auto &kv : state)
        all.insert(kv.first);

    State next;
    for (const std::string &rel : all) {
        const Kind kind = classify(rel);
        if (kind == Kind::None)
            continue;
        const auto li = local.find(rel);
        const auto ci = remote.find(rel);
        const auto si = state.find(rel);
        const Entry *l = li != local.end() ? &li->second : nullptr;
        const Entry *c = ci != remote.end() ? &ci->second : nullptr;
        const Seen *s = si != state.end() ? &si->second : nullptr;
        const std::string lpath = join(paths.local, rel), cpath = join(paths.cloud, rel);
        const bool busy = l && now_ns - l->stamp.mtime_ns < kBusyNs && l->stamp.mtime_ns <= now_ns;
        if ((c && c->pending) || busy) {
            ++stats.waiting;
            if (s)
                next[rel] = *s;
            continue;
        }
        const auto push = [&]() {
            Seen seen;
            seen.local = l->stamp;
            if (!cloud.write(lpath, cpath, &seen.cloud))
                return false;
            next[rel] = seen;
            ++stats.pushed;
            return true;
        };
        const auto pull = [&]() {
            Seen seen;
            seen.cloud = c->stamp;
            if (!cloud.read(cpath, lpath) || !stat_path(lpath, &seen.local))
                return false;
            next[rel] = seen;
            ++stats.pulled;
            return true;
        };
        const auto keep = [&]() {
            Seen seen;
            seen.local = l->stamp;
            seen.cloud = c->stamp;
            next[rel] = seen;
        };
        bool ok = true;
        if (kind == Kind::Seed) {
            // Only to a side that has none; never compared, never deleted.
            if (l && !c)
                ok = push();
            else if (c && !l)
                ok = pull();
            next.erase(rel);
            if (!ok)
                fprintf(stderr, "[cloud] %s could not be copied\n", rel.c_str());
            continue;
        }
        const bool local_changed = l ? (!s || l->stamp != s->local) : s != nullptr;
        const bool cloud_changed = c ? (!s || c->stamp != s->cloud) : s != nullptr;
        if (l && c) {
            if (s && !local_changed && !cloud_changed) {
                keep();
            } else if (s && local_changed && !cloud_changed) {
                ok = push();
            } else if (s && cloud_changed && !local_changed) {
                ok = pull();
            } else {
                // Both changed, or both had it before the first sync.
                bool same = false;
                if (l->stamp.size == c->stamp.size) {
                    same = llabs(l->stamp.mtime_ns - c->stamp.mtime_ns) < kSameTimeNs;
                    if (!same) {
                        const std::string probe = temp_beside(lpath) + ".cmp";
                        same = cloud.read(cpath, probe) && same_bytes(probe, lpath);
                        std::error_code ec;
                        fs::remove(probe, ec);
                    }
                }
                if (same) {
                    keep();
                } else {
                    ++stats.conflicts;
                    if (l->stamp.mtime_ns >= c->stamp.mtime_ns)
                        ok = cloud.read(cpath, backup.path(rel)) && push();
                    else
                        ok = copy_keeping_time(lpath, backup.path(rel)) && pull();
                }
            }
        } else if (l) {
            if (!s || local_changed || cloud_lost) {
                ok = push();
            } else {
                // Deleted in the synced folder, unchanged here.
                std::error_code ec;
                ok = copy_keeping_time(lpath, backup.path(rel)) && fs::remove(lpath, ec);
                if (ok)
                    ++stats.deleted_local;
            }
        } else if (c) {
            if (!s || cloud_changed || local_lost) {
                ok = pull();
            } else {
                // Deleted here (the game's Delete), unchanged there.
                ok = cloud.read(cpath, backup.path(rel)) && cloud.remove(cpath);
                if (ok)
                    ++stats.deleted_cloud;
            }
        }
        // else: gone on both sides, and from the state.
        if (!ok) {
            fprintf(stderr, "[cloud] %s could not be synced\n", rel.c_str());
            if (s)
                next[rel] = *s; // tried again next time
        }
    }
    if (!save_state(paths.state, paths.cloud, next))
        fprintf(stderr, "[cloud] the sync state could not be written\n");
    backup.prune();
    return stats;
}

// MARK: - The running game's sync

namespace {

int64_t wall_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A fingerprint of the game's own synced files: it changes when the game saves.
uint64_t local_fingerprint(const std::string &root) {
    Listing l;
    list_files(root, &l);
    uint64_t h = 1469598103934665603ull;
    for (const auto &[rel, e] : l) {
        if (classify(rel) != Kind::Sync)
            continue;
        for (char ch : rel)
            h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
        h = (h ^ static_cast<uint64_t>(e.stamp.size)) * 1099511628211ull;
        h = (h ^ static_cast<uint64_t>(e.stamp.mtime_ns)) * 1099511628211ull;
    }
    return h;
}

struct Session {
    Paths paths;
    std::thread worker;
    std::mutex mu;
    std::condition_variable wake, done;
    bool quit = false;
    bool wanted = false;
    uint64_t finished = 0; // syncs done
    uint64_t asked = 0;    // syncs asked for (a wait waits for this one)
};

Session *g_session = nullptr;
std::mutex g_session_mu;

constexpr auto kTick = std::chrono::seconds(15);
constexpr int64_t kFullEveryNs = 60'000'000'000ll;

void run_once(Session &s) {
    const Stats st = sync(s.paths, platform_io(), wall_ns());
    if (!st.error.empty())
        fprintf(stderr, "[cloud] not synced: %s (%s)\n", st.error.c_str(), s.paths.cloud.c_str());
    else if (st.changed() || st.conflicts)
        fprintf(stderr, "[cloud] synced: %d down, %d up, %d deleted here, %d deleted there, %d conflicts, %d waiting\n",
                st.pulled, st.pushed, st.deleted_local, st.deleted_cloud, st.conflicts, st.waiting);
}

void worker_main(Session *s) {
    uint64_t seen = 0;
    int64_t last_full = 0;
    for (;;) {
        uint64_t target = 0;
        bool quit = false, wanted = false;
        {
            std::unique_lock<std::mutex> lock(s->mu);
            s->wake.wait_for(lock, kTick, [&] { return s->quit || s->wanted; });
            quit = s->quit;
            wanted = s->wanted;
            s->wanted = false;
            target = s->asked;
        }
        const uint64_t now_print = local_fingerprint(s->paths.local);
        const int64_t now = wall_ns();
        if (quit || wanted || now_print != seen || now - last_full >= kFullEveryNs) {
            run_once(*s);
            seen = local_fingerprint(s->paths.local);
            last_full = now;
            std::lock_guard<std::mutex> lock(s->mu);
            s->finished = std::max(s->finished, target);
            s->done.notify_all();
        }
        if (quit)
            return;
    }
}

// Asks the worker for a sync and waits up to `wait_s` for it.
void ask(Session &s, double wait_s, bool quit) {
    std::unique_lock<std::mutex> lock(s.mu);
    const uint64_t mine = ++s.asked;
    s.wanted = true;
    if (quit)
        s.quit = true;
    s.wake.notify_all();
    if (wait_s > 0)
        s.done.wait_for(lock, std::chrono::duration<double>(wait_s), [&] { return s.finished >= mine; });
}

} // namespace

void start(const Paths &paths, double wait_s) {
    std::lock_guard<std::mutex> lock(g_session_mu);
    if (g_session)
        return;
    fprintf(stderr, "[cloud] saves synced with %s\n", paths.cloud.c_str());
    g_session = new Session();
    g_session->paths = paths;
    g_session->worker = std::thread(worker_main, g_session);
    ask(*g_session, wait_s, false);
}

void request() {
    std::lock_guard<std::mutex> lock(g_session_mu);
    if (g_session)
        ask(*g_session, 0, false);
}

void flush(double wait_s) {
    std::lock_guard<std::mutex> lock(g_session_mu);
    if (g_session)
        ask(*g_session, wait_s, false);
}

void stop(double wait_s) {
    std::lock_guard<std::mutex> lock(g_session_mu);
    if (!g_session)
        return;
    ask(*g_session, wait_s, true);
    bool finished = false;
    {
        std::lock_guard<std::mutex> l(g_session->mu);
        finished = g_session->finished >= g_session->asked;
    }
    if (finished) {
        g_session->worker.join();
        delete g_session;
    } else {
        // Still copying when the process goes: it is not waited for (the
        // next start syncs again).
        g_session->worker.detach();
    }
    g_session = nullptr;
}

bool running() {
    std::lock_guard<std::mutex> lock(g_session_mu);
    return g_session != nullptr;
}

} // namespace cloud_saves
