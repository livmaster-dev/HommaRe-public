// installer.cpp - see installer.h. innoextract runs as a child process, as the
// Mac's bundled helper does; the system's 1.9 prints the same listing and GOG
// id lines (the Mac helper's patches are build fixes only).
#include "installer.h"

#include "../../../platform/os.h"
#include "../../../runtime/sha256.h"
#include "../../../third_party/miniz/miniz.h"
#include "../../controls/json.h"
#include "detect.h"
#include "l10n.h"

// miniz's zlib names are macros; gog::inflate is this file's own.
#undef inflate

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <regex>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace launcher::native {

namespace {

// ASCII and Cyrillic to lower case: enough for the names compared here
// (Finder's and browsers' copy counters include "копия").
std::string lower(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') {
            out.push_back(char(c - 'A' + 'a'));
        } else if (c == 0xD0 && i + 1 < s.size()) {
            const unsigned char d = (unsigned char)s[i + 1];
            if (d >= 0x90 && d <= 0x9F) { // А-П
                out += char(0xD0);
                out += char(d + 0x20);
            } else if (d >= 0xA0 && d <= 0xAF) { // Р-Я
                out += char(0xD1);
                out += char(d - 0x20);
            } else if (d == 0x81) { // Ё
                out += char(0xD1);
                out += char(0x91);
            } else {
                out += char(c);
                out += char(d);
            }
            ++i;
        } else {
            out.push_back(char(c));
        }
    }
    return out;
}

bool ends_with(const std::string &s, const char *suffix) {
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string join(const std::string &dir, const std::string &name) {
    return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name;
}

// "setup_x.exe" -> "exe", "" without one.
std::string extension(const std::string &path) {
    const std::string name = file_name(path);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? "" : lower(name.substr(dot + 1));
}

std::string stem(const std::string &path) {
    const std::string name = file_name(path);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

int64_t size_of(const std::string &path) {
    OsStat st{};
    return os_stat(path.c_str(), &st) == 0 && st.is_regular ? int64_t(st.size) : -1;
}

std::string os_error(const std::string &what, const std::string &path) {
    return what + " " + path + ": " + strerror(errno);
}

bool make_dirs(const std::string &path) {
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/')
            os_mkdir(path.substr(0, i).c_str());
    return is_dir(path);
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
            remove_tree(join(path, n));
        os_rmdir(path.c_str());
    } else {
        os_unlink(path.c_str());
    }
}

// Every regular file under `dir`, recursively.
void walk_files(const std::string &dir, std::vector<std::string> *out) {
    std::vector<std::string> names;
    os_listdir(
        dir.c_str(),
        [](const char *name, void *user) {
            static_cast<std::vector<std::string> *>(user)->push_back(name);
            return 0;
        },
        &names);
    std::sort(names.begin(), names.end());
    for (const std::string &n : names) {
        const std::string p = join(dir, n);
        OsStat st{};
        if (os_lstat(p.c_str(), &st) != 0)
            continue;
        if (st.is_dir && !st.is_symlink)
            walk_files(p, out);
        else if (st.is_regular)
            out->push_back(p);
    }
}

bool read_all(const std::string &path, std::string *out) {
    return read_text(path, out);
}

bool write_all(const std::string &path, const std::string &data) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

bool copy_file(const std::string &from, const std::string &to) {
    const int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return false;
    const int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        close(in);
        return false;
    }
    std::vector<char> buf(1 << 20);
    bool ok = true;
    for (ssize_t n; ok && (n = read(in, buf.data(), buf.size())) != 0;) {
        if (n < 0) {
            ok = errno == EINTR;
            continue;
        }
        for (ssize_t done = 0; ok && done < n;) {
            const ssize_t w = write(out, buf.data() + done, size_t(n - done));
            ok = w > 0;
            done += w;
        }
    }
    close(in);
    return close(out) == 0 && ok;
}

// SHA-256 of `path`, reporting each megabyte read; false when unreadable or
// when `chunk` says stop (then *cancelled).
bool digest(const std::string &path, std::string *out, const std::function<bool(int64_t)> &chunk,
            bool *cancelled = nullptr) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    Sha256 h;
    std::vector<uint8_t> buf(1 << 20);
    bool ok = true;
    for (;;) {
        const ssize_t n = read(fd, buf.data(), buf.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        h.update(buf.data(), size_t(n));
        if (chunk && !chunk(n)) {
            if (cancelled)
                *cancelled = true;
            ok = false;
            break;
        }
    }
    close(fd);
    if (ok)
        *out = h.hex();
    return ok;
}

// The natural order Finder sorts names in: case aside, digit runs by value.
bool natural_less(const std::string &a, const std::string &b) {
    const std::string x = lower(a), y = lower(b);
    size_t i = 0, j = 0;
    while (i < x.size() && j < y.size()) {
        if (isdigit((unsigned char)x[i]) && isdigit((unsigned char)y[j])) {
            size_t ei = i, ej = j;
            while (ei < x.size() && isdigit((unsigned char)x[ei]))
                ++ei;
            while (ej < y.size() && isdigit((unsigned char)y[ej]))
                ++ej;
            std::string nx = x.substr(i, ei - i), ny = y.substr(j, ej - j);
            nx.erase(0, std::min(nx.find_first_not_of('0'), nx.size()));
            ny.erase(0, std::min(ny.find_first_not_of('0'), ny.size()));
            if (nx.size() != ny.size())
                return nx.size() < ny.size();
            if (nx != ny)
                return nx < ny;
            i = ei;
            j = ej;
            continue;
        }
        if (x[i] != y[j])
            return (unsigned char)x[i] < (unsigned char)y[j];
        ++i;
        ++j;
    }
    if (x.size() - i != y.size() - j)
        return x.size() - i < y.size() - j;
    return a < b;
}

// The files (not folders, not hidden) in `dir`, in natural order.
std::vector<std::string> files_in(const std::string &dir) {
    std::vector<std::string> names;
    os_listdir(
        dir.c_str(),
        [](const char *name, void *user) {
            if (name[0] != '.')
                static_cast<std::vector<std::string> *>(user)->push_back(name);
            return 0;
        },
        &names);
    std::sort(names.begin(), names.end(), natural_less);
    std::vector<std::string> out;
    for (const std::string &n : names) {
        OsStat st{};
        if (os_stat(join(dir, n).c_str(), &st) == 0 && !st.is_dir)
            out.push_back(join(dir, n));
    }
    return out;
}

// "setup_x-1 (1).bin" -> "setup_x-1.bin", so the slice number still parses.
std::string normalized_bin_name(const std::string &name) {
    if (!ends_with(lower(name), ".bin"))
        return name;
    return finder::normalized(name.substr(0, name.size() - 4)) + ".bin";
}

bool any_slice(const std::string &name, std::string *base, int *index) {
    return finder::slice(name, base, index) ||
           finder::slice(normalized_bin_name(name), base, index);
}

// The installer a slice belongs to: 0 when `bin` is no slice, 1 with the
// installer in *exe, 2 with what is wrong in *problem.
int setup_for(const std::string &bin, std::string *exe, finder::Problem *problem) {
    std::string base;
    int index = 0;
    if (!any_slice(file_name(bin), &base, &index))
        return 0;
    const std::string dir = parent_dir(bin);
    const std::string expected = base + ".exe";
    if (const std::string found = entry(dir, expected); !found.empty() && !is_dir(found)) {
        *exe = found;
        return 1;
    }
    const std::string want = finder::normalized(base);
    problem->kind = finder::Problem::MissingSetup;
    problem->file = bin;
    problem->expected = expected;
    problem->renamed.clear();
    for (const std::string &f : files_in(dir))
        if (extension(f) == "exe" && finder::normalized(stem(f)) == want) {
            problem->renamed = f;
            break;
        }
    return 2;
}

std::string last_lines(const std::string &text) {
    std::vector<std::string> lines;
    for (size_t start = 0; start < text.size();) {
        const size_t nl = std::min(text.find('\n', start), text.size());
        std::string line = text.substr(start, nl - start);
        start = nl + 1;
        const size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a != std::string::npos)
            lines.push_back(line.substr(a, b - a + 1));
    }
    if (lines.empty())
        return "";
    return lines.size() == 1 ? lines.back() : lines[lines.size() - 2] + " " + lines.back();
}

// A child process with its output read through pipes.
struct Child {
    pid_t pid = -1;
    int out = -1, err = -1;

    bool start(const std::string &program, const std::vector<std::string> &args) {
        int o[2], e[2];
        if (pipe2(o, O_CLOEXEC) != 0)
            return false;
        if (pipe2(e, O_CLOEXEC) != 0) {
            close(o[0]);
            close(o[1]);
            return false;
        }
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, o[1], 1);
        posix_spawn_file_actions_adddup2(&fa, e[1], 2);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        std::vector<char *> argv{const_cast<char *>(program.c_str())};
        for (const std::string &a : args)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        const int rc = posix_spawn(&pid, program.c_str(), &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        close(o[1]);
        close(e[1]);
        if (rc != 0) {
            close(o[0]);
            close(e[0]);
            errno = rc;
            pid = -1;
            return false;
        }
        out = o[0];
        err = e[0];
        fcntl(out, F_SETFL, fcntl(out, F_GETFL) | O_NONBLOCK);
        fcntl(err, F_SETFL, fcntl(err, F_GETFL) | O_NONBLOCK);
        return true;
    }

    // Reads what is there, waiting up to `ms`; false once both pipes are done.
    bool pump(int ms, std::string *out_text, std::string *err_text) {
        pollfd fds[2];
        int n = 0;
        if (out >= 0)
            fds[n++] = {out, POLLIN, 0};
        if (err >= 0)
            fds[n++] = {err, POLLIN, 0};
        if (n == 0)
            return false;
        if (poll(fds, nfds_t(n), ms) <= 0)
            return true;
        char buf[65536];
        for (int i = 0; i < n; ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            int &fd = fds[i].fd == out ? out : err;
            std::string *text = fds[i].fd == out ? out_text : err_text;
            for (;;) {
                const ssize_t r = read(fd, buf, sizeof buf);
                if (r > 0) {
                    text->append(buf, size_t(r));
                    continue;
                }
                if (r < 0 && errno == EINTR)
                    continue;
                if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    close(fd);
                    fd = -1;
                }
                break;
            }
        }
        return out >= 0 || err >= 0;
    }

    // The exit status (-1 when it did not exit normally).
    int wait() {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if (out >= 0)
            close(out);
        if (err >= 0)
            close(err);
        out = err = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
};

std::mutex g_running_lock;
pid_t g_running = -1;

void set_running(pid_t pid) {
    std::lock_guard<std::mutex> lock(g_running_lock);
    g_running = pid;
}

// Runs the helper to the end and returns what it printed.
Outcome capture(const std::string &helper, const std::vector<std::string> &args, std::string *out) {
    std::vector<std::string> all{"--color=0", "--progress=0"};
    all.insert(all.end(), args.begin(), args.end());
    Child c;
    if (!c.start(helper, all))
        return Outcome::fail(os_error("Could not run", helper));
    std::string text, err;
    while (c.pump(1000, &text, &err)) {
    }
    const int status = c.wait();
    if (status != 0) {
        const std::string why = last_lines(err);
        return Outcome::fail(why.empty() ? t("helperFailed", {{"status", std::to_string(status)}})
                                         : why);
    }
    *out = std::move(text);
    return {};
}

// `path` under `dest` through what is already there, each part matched
// without case; parts not there yet keep the manifest's spelling.
std::string place(const std::string &path, const std::string &dest) {
    std::string at = dest;
    for (size_t start = 0; start < path.size();) {
        const size_t slash = std::min(path.find('/', start), path.size());
        const std::string part = path.substr(start, slash - start);
        start = slash + 1;
        if (part.empty())
            continue;
        const std::string found = entry(at, part);
        at = found.empty() ? join(at, part) : found;
    }
    return at;
}

uint64_t now_ns() {
    return os_monotonic_ns();
}

// Checks `files` under `dest` against the manifest: removes the ones that
// differ, leaves the ones that need unpacking in *pending.
Outcome check(const std::vector<ManifestFile> &files, const std::string &dest,
              Progress::Phase phase, Progress &p, const ProgressFn &progress,
              std::vector<ManifestFile> *pending) {
    std::vector<const ManifestFile *> present;
    int64_t total = 0;
    for (const ManifestFile &f : files) {
        const std::string at = entry(dest, f.path);
        if (!at.empty() && size_of(at) == f.size) {
            present.push_back(&f);
            total += f.size;
        }
    }
    p.phase = phase;
    p.files_done = 0;
    p.files_total = int(present.size());
    p.bytes_done = 0;
    p.bytes_total = total;
    std::set<std::string> good;
    for (const ManifestFile *f : present) {
        p.current = f->path;
        if (!progress(p))
            return Outcome::cancel();
        const std::string at = entry(dest, f->path);
        if (at.empty())
            continue;
        uint64_t last = now_ns();
        bool cancelled = false;
        std::string sum;
        const bool read = digest(
            at, &sum,
            [&](int64_t n) {
                p.bytes_done += n;
                if (now_ns() - last > 200000000ull) {
                    last = now_ns();
                    return progress(p);
                }
                return true;
            },
            &cancelled);
        if (cancelled)
            return Outcome::cancel();
        if (read && sum == f->sha256)
            good.insert(f->path);
        else if (read)
            os_unlink(at.c_str());
        p.files_done += 1;
    }
    progress(p);
    pending->clear();
    for (const ManifestFile &f : files)
        if (!good.count(f.path))
            pending->push_back(f);
    return {};
}

// Ends `pid` when it is still this launcher's innoextract.
void stop_stale(int64_t pid, const std::string &helper) {
    if (pid <= 0)
        return;
    char buf[4096];
    const ssize_t n =
        readlink(("/proc/" + std::to_string(pid) + "/exe").c_str(), buf, sizeof buf - 1);
    if (n <= 0)
        return;
    buf[n] = 0;
    if (canonical(buf) != canonical(helper))
        return;
    kill(pid_t(pid), SIGTERM);
    for (int i = 0; i < 40 && kill(pid_t(pid), 0) == 0; ++i)
        os_sleep_us(50000);
}

Outcome extract(const std::vector<ManifestFile> &files, const gog::Plan &plan,
                const std::string &helper, InstallMarker &marker, Progress &p,
                const ProgressFn &progress) {
    // --include /path keeps exactly the listed files (innoextract matches without case).
    std::vector<std::string> args{"--extract", "--color=0", "--progress=0", "--output-dir",
                                  plan.dest};
    for (const ManifestFile &f : files) {
        args.push_back("--include");
        args.push_back("/" + f.path);
    }
    args.push_back(plan.setup);
    Child c;
    if (!c.start(helper, args))
        return Outcome::fail(os_error("Could not run", helper));
    marker.helper_pid = c.pid;
    marker.write(plan.dest);
    set_running(c.pid);

    p.phase = Progress::Extracting;
    p.files_total = int(files.size());
    p.files_done = 0;
    p.bytes_done = 0;
    p.bytes_total = 0;
    for (const ManifestFile &f : files)
        p.bytes_total += f.size;
    std::string out, err;
    size_t scanned = 0;
    auto poll_files = [&] {
        int done = 0;
        int64_t bytes = 0;
        for (const ManifestFile &f : files) {
            const std::string at = entry(plan.dest, f.path);
            const int64_t have =
                at.empty() ? 0 : std::min(std::max<int64_t>(size_of(at), 0), f.size);
            bytes += have;
            if (have == f.size)
                ++done;
        }
        p.files_done = done;
        p.bytes_done = bytes;
        // innoextract names each file as it starts on it: ` - "data/data.pak"`.
        for (size_t nl; (nl = out.find('\n', scanned)) != std::string::npos; scanned = nl + 1) {
            const std::string line = out.substr(scanned, nl - scanned);
            if (line.compare(0, 4, " - \"") == 0)
                p.current = line.substr(4, line.find('"', 4) - 4);
        }
    };
    uint64_t last = now_ns();
    bool running = true;
    int status = 0;
    while (running) {
        c.pump(50, &out, &err);
        int st = 0;
        const pid_t r = waitpid(c.pid, &st, WNOHANG);
        if (r == c.pid) {
            running = false;
            status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            while (c.pump(0, &out, &err)) {
            }
            break;
        }
        if (now_ns() - last < 250000000ull)
            continue;
        last = now_ns();
        poll_files();
        if (!progress(p)) {
            kill(c.pid, SIGTERM);
            c.wait();
            set_running(-1);
            return Outcome::cancel();
        }
    }
    if (c.out >= 0)
        close(c.out);
    if (c.err >= 0)
        close(c.err);
    c.out = c.err = -1;
    set_running(-1);
    poll_files();
    progress(p);
    if (status != 0) {
        const std::string why = last_lines(err);
        return Outcome::fail(why.empty() ? t("helperFailed", {{"status", std::to_string(status)}})
                                         : why);
    }
    return {};
}

// A GOG update (patch_*.exe), unpacked: an Inno Setup program without a GOG
// id or slices, which carries each file of the newer build it brings whole,
// zlib-compressed, under tmp/ (its own script, which the launcher does not
// run, puts them in place). A file it carries is a manifest's when its
// SHA-256 is the manifest's, so the manifest says where it goes.
struct Patch {
    std::string dir;
    std::map<std::string, std::string> files; // SHA-256 -> the file, inflated, in `dir`
};

constexpr const char *kPatchStaging = ".recomp-patch";
constexpr const char *kProbeStaging = ".recomp-probe";

// SHA-256 of one file of `setup` (`path` in the install folder), unpacked
// alone into `dir` (removed after); empty when the installer has none.
Outcome file_digest(const std::string &helper, const std::string &setup, const std::string &path,
                    const std::string &dir, std::string *out) {
    out->clear();
    remove_tree(dir);
    if (!make_dirs(dir))
        return Outcome::fail(os_error("Could not create", dir));
    std::string ignored;
    Outcome o = capture(helper, {"--extract", "--include", "/" + path, "--output-dir", dir, setup}, &ignored);
    std::vector<std::string> found;
    walk_files(dir, &found);
    // The installer's spelling, which need not be the manifest's.
    for (const std::string &f : found) {
        std::string data;
        if (o.ok && lower(file_name(f)) == lower(file_name(path)) && read_all(f, &data)) {
            *out = sha256_hex(data.data(), data.size());
            break;
        }
    }
    remove_tree(dir);
    return o;
}

// Unpacks what `patch` carries into `dir` (emptied first), each file
// inflated and named by its SHA-256.
Outcome unpack_patch(const std::string &helper, const std::string &patch, const std::string &dir,
                     Patch *out) {
    remove_tree(dir);
    const std::string raw = join(dir, "tmp-unpacked");
    if (!make_dirs(raw))
        return Outcome::fail(os_error("Could not create", raw));
    std::string ignored;
    Outcome o =
        capture(helper, {"--extract", "--include", "/tmp", "--output-dir", raw, patch}, &ignored);
    if (!o.ok) {
        remove_tree(raw);
        return o;
    }
    out->dir = dir;
    out->files.clear();
    std::vector<std::string> found;
    walk_files(raw, &found);
    for (const std::string &f : found) {
        std::string data, bytes;
        if (!read_all(f, &data))
            continue;
        if (!gog::inflate(data, &bytes))
            bytes = std::move(data);
        const std::string sum = sha256_hex(bytes.data(), bytes.size());
        if (out->files.count(sum))
            continue;
        const std::string at = join(dir, sum);
        if (!write_all(at, bytes)) {
            remove_tree(raw);
            return Outcome::fail(os_error("Could not write", at));
        }
        out->files[sum] = at;
    }
    remove_tree(raw);
    return {};
}

// Puts each of `files` that `patch` carries in place under `root` where the
// file there is not that one already; how many it replaced.
Outcome apply(const Patch &patch, const std::vector<ManifestFile> &files, const std::string &root,
              int *replaced) {
    *replaced = 0;
    for (const ManifestFile &f : files) {
        auto source = patch.files.find(f.sha256);
        if (source == patch.files.end())
            continue;
        const std::string target = place(f.path, root);
        std::string sum;
        if (size_of(target) == f.size && digest(target, &sum, nullptr) && sum == f.sha256)
            continue;
        const std::string dir = parent_dir(target);
        if (!make_dirs(dir))
            return Outcome::fail(os_error("Could not create", dir));
        const std::string partial = join(dir, "." + file_name(target) + ".patch");
        os_unlink(partial.c_str());
        if (!copy_file(source->second, partial)) {
            os_unlink(partial.c_str());
            return Outcome::fail(os_error("Could not write", partial));
        }
        if (os_rename(partial.c_str(), target.c_str()) != 0)
            return Outcome::fail(os_error("Could not replace", target));
        ++*replaced;
    }
    return {};
}

bool load_manifest(const std::string &dir, const std::string &name, Manifest *out) {
    std::string text;
    return !dir.empty() && read_text(join(dir, name), &text) && parse_manifest(text, out);
}

} // namespace

// ---------------------------------------------------------------------------
// The marker
// ---------------------------------------------------------------------------

bool InstallMarker::read(const std::string &root, InstallMarker *out) {
    std::string text, error;
    controls::Json j;
    if (!read_text(join(root, kName), &text) || !controls::json_parse(text, &j, &error) ||
        j.type != controls::Json::Object)
        return false;
    InstallMarker m;
    m.installer = j.str("installer", "");
    m.setup = j.str("setup", "");
    m.manifest = j.str("manifest", "");
    if (const controls::Json *games = j.get("games"))
        for (const controls::Json &g : games->a)
            if (g.type == controls::Json::String)
                m.games.push_back(g.s);
    m.helper_pid = int64_t(j.num("helperPid", 0));
    if (m.installer.empty() || m.setup.empty())
        return false;
    *out = std::move(m);
    return true;
}

bool InstallMarker::write(const std::string &root) const {
    controls::Json j = controls::Json::object();
    j.set("installer", controls::Json::string(installer));
    j.set("setup", controls::Json::string(setup));
    j.set("manifest", controls::Json::string(manifest));
    controls::Json list = controls::Json::array();
    for (const std::string &g : games)
        list.a.push_back(controls::Json::string(g));
    j.set("games", list);
    if (helper_pid > 0)
        j.set("helperPid", controls::Json::number(double(helper_pid)));
    return write_all(join(root, kName), controls::json_write(j));
}

// ---------------------------------------------------------------------------
// Finding installers
// ---------------------------------------------------------------------------

namespace finder {

bool slice(const std::string &name, std::string *base, int *index) {
    if (!ends_with(lower(name), ".bin"))
        return false;
    const std::string s = name.substr(0, name.size() - 4);
    const size_t dash = s.find_last_of('-');
    if (dash == std::string::npos || dash + 1 >= s.size() || s.size() - dash > 9)
        return false;
    int n = 0;
    for (size_t i = dash + 1; i < s.size(); ++i) {
        if (!isdigit((unsigned char)s[i]))
            return false;
        n = n * 10 + (s[i] - '0');
    }
    if (n <= 0)
        return false;
    *base = s.substr(0, dash);
    *index = n;
    return true;
}

std::string normalized(const std::string &stem) {
    std::string s = lower(stem);
    static const std::regex patterns[] = {
        std::regex(R"(\s+\(\d+\))"), std::regex(R"(\s+copy(\s+\d+)?)"),
        std::regex("\\s+\xD0\xBA\xD0\xBE\xD0\xBF\xD0\xB8\xD1\x8F(\\s+\\d+)?")};
    for (const std::regex &re : patterns)
        s = std::regex_replace(s, re, "");
    const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool is_setup_name(const std::string &name) {
    const std::string l = lower(name);
    return l.compare(0, 6, "setup_") == 0 && ends_with(l, ".exe");
}

bool is_patch_name(const std::string &name) {
    const std::string l = lower(name);
    return l.compare(0, 6, "patch_") == 0 && ends_with(l, ".exe");
}

bool check_slice(const std::string &exe, Problem *problem) {
    const std::string dir = parent_dir(exe);
    const std::string base = stem(exe);
    const std::string expected = base + "-1.bin";
    if (!entry(dir, expected).empty())
        return false;
    const std::string want = normalized(base);
    problem->kind = Problem::MissingSlice;
    problem->file = exe;
    problem->expected = expected;
    problem->renamed.clear();
    for (const std::string &f : files_in(dir)) {
        std::string b;
        int index = 0;
        if (any_slice(file_name(f), &b, &index) && index == 1 && normalized(b) == want) {
            problem->renamed = f;
            break;
        }
    }
    return true;
}

Found find(const std::vector<std::string> &paths) {
    Found found;
    std::vector<std::string> exes;
    auto add_exe = [&](const std::string &exe) {
        const std::string s = canonical(exe);
        std::vector<std::string> &list = is_patch_name(file_name(s)) ? found.patches : exes;
        if (std::find(list.begin(), list.end(), s) == list.end())
            list.push_back(s);
    };
    auto add_problem = [&](const Problem &p) {
        if (std::find(found.problems.begin(), found.problems.end(), p) == found.problems.end())
            found.problems.push_back(p);
    };
    for (const std::string &url : paths) {
        std::string exe;
        Problem problem;
        if (is_dir(url)) {
            std::vector<std::string> level{url};
            bool any = false;
            for (int depth = 0; depth < 3; ++depth) {
                std::vector<std::string> next;
                for (const std::string &dir : level) {
                    const std::vector<std::string> here = files_in(dir);
                    for (const std::string &f : here)
                        if (is_setup_name(file_name(f)) || is_patch_name(file_name(f))) {
                            add_exe(f);
                            any = true;
                        }
                    // A slice whose installer is missing or renamed.
                    for (const std::string &f : here) {
                        const int r = setup_for(f, &exe, &problem);
                        if (r == 0)
                            continue;
                        if (r == 1)
                            add_exe(exe);
                        else
                            add_problem(problem);
                        any = true;
                    }
                    for (const std::string &c : children(dir))
                        next.push_back(c);
                }
                level = std::move(next);
            }
            if (!any)
                found.empty_folders.push_back(url);
        } else if (extension(url) == "exe") {
            add_exe(url);
        } else if (const int r = setup_for(url, &exe, &problem); r == 1) {
            add_exe(exe);
        } else if (r == 2) {
            add_problem(problem);
        } else {
            found.empty_folders.push_back(url);
        }
    }
    for (const std::string &exe : exes) {
        Problem p;
        if (check_slice(exe, &p)) {
            // Some other program (not setup_*.exe, no slice in sight) is not
            // an installer at all.
            if (p.renamed.empty() && !is_setup_name(file_name(exe)))
                found.empty_folders.push_back(exe);
            else
                add_problem(p);
        } else {
            found.setups.push_back(exe);
        }
    }
    // A renamed installer shows up from both of its files; tell it once.
    std::set<std::string> reported;
    for (const Problem &p : found.problems)
        if (p.kind == Problem::MissingSlice)
            reported.insert(canonical(p.file));
    found.problems.erase(std::remove_if(found.problems.begin(), found.problems.end(),
                                        [&](const Problem &p) {
                                            return p.kind == Problem::MissingSetup &&
                                                   !p.renamed.empty() &&
                                                   reported.count(canonical(p.renamed));
                                        }),
                         found.problems.end());
    auto by_name = [](const std::string &a, const std::string &b) {
        return natural_less(file_name(a), file_name(b));
    };
    std::sort(found.setups.begin(), found.setups.end(), by_name);
    std::sort(found.patches.begin(), found.patches.end(), by_name);
    return found;
}

std::string describe(const Problem &problem) {
    const std::string name = file_name(problem.file);
    if (problem.kind == Problem::MissingSlice) {
        if (!problem.renamed.empty()) {
            const std::string renamed = file_name(problem.renamed);
            // A slice with its own clean name: the installer is the renamed one.
            std::string base;
            int index = 0;
            if (slice(renamed, &base, &index) && normalized(base) == lower(base))
                return t("renamedSetup",
                         {{"bin", renamed}, {"exe", base + ".exe"}, {"found", name}});
            return t("renamedSlice",
                     {{"exe", name}, {"slice", problem.expected}, {"found", renamed}});
        }
        return t("missingSlice", {{"exe", name}, {"slice", problem.expected}});
    }
    if (!problem.renamed.empty())
        return t("renamedSetup",
                 {{"bin", name}, {"exe", problem.expected}, {"found", file_name(problem.renamed)}});
    return t("missingSetup", {{"bin", name}, {"exe", problem.expected}});
}

} // namespace finder

// ---------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------

namespace gog {

std::string helper() {
    if (const char *p = getenv("RECOMP_INNOEXTRACT"); p && *p)
        return access(p, X_OK) == 0 && !is_dir(p) ? std::string(p) : std::string();
    // One the app carries beside its executable, then the system's.
    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) {
        self[n] = 0;
        const std::string beside = join(parent_dir(self), "innoextract");
        if (access(beside.c_str(), X_OK) == 0 && !is_dir(beside))
            return beside;
    }
    const char *path = getenv("PATH");
    const std::string dirs = path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
    for (size_t start = 0; start <= dirs.size();) {
        const size_t colon = std::min(dirs.find(':', start), dirs.size());
        const std::string dir = dirs.substr(start, colon - start);
        start = colon + 1;
        if (dir.empty())
            continue;
        const std::string candidate = join(dir, "innoextract");
        if (access(candidate.c_str(), X_OK) == 0 && !is_dir(candidate))
            return candidate;
    }
    return "";
}

void stop_running() {
    std::lock_guard<std::mutex> lock(g_running_lock);
    if (g_running > 0)
        kill(g_running, SIGTERM);
}

std::set<std::string> listed_paths(const std::string &listing) {
    std::set<std::string> out;
    for (size_t start = 0; start < listing.size();) {
        const size_t nl = std::min(listing.find('\n', start), listing.size());
        const std::string line = listing.substr(start, nl - start);
        start = nl + 1;
        if (line.compare(0, 4, " - \"") != 0)
            continue;
        // Every other part between quotes is a path; folders end in a slash.
        size_t i = 0;
        for (int part = 0;; ++part) {
            const size_t q = line.find('"', i);
            const std::string piece =
                line.substr(i, q == std::string::npos ? std::string::npos : q - i);
            if (part % 2 == 1 && (piece.empty() || piece.back() != '/'))
                out.insert(lower(piece));
            if (q == std::string::npos)
                break;
            i = q + 1;
        }
    }
    return out;
}

bool inflate(const std::string &data, std::string *out) {
    if (data.size() <= 6)
        return false;
    const unsigned cmf = (unsigned char)data[0], flg = (unsigned char)data[1];
    if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7 || (flg & 0x20) || ((cmf << 8) | flg) % 31 != 0)
        return false;
    size_t size = 0;
    void *bytes = tinfl_decompress_mem_to_heap(data.data() + 2, data.size() - 2, &size, 0);
    if (!bytes)
        return false;
    out->assign(static_cast<const char *>(bytes), size);
    mz_free(bytes);
    return true;
}

Outcome plan(const std::string &setup, const Config &config, const std::string &manifest_dir,
             const std::string &base, Plan *out) {
    const std::string h = helper();
    if (h.empty())
        return Outcome::fail(t("noHelper"));
    if (finder::Problem problem; finder::check_slice(setup, &problem))
        return Outcome::fail(finder::describe(problem));
    std::string text;
    if (Outcome o = capture(h, {"--gog-game-id", setup}, &text); !o.ok)
        return o;
    std::string id;
    if (const size_t at = text.find("GOG.com game ID is "); at != std::string::npos)
        for (size_t i = at + 19; i < text.size() && isdigit((unsigned char)text[i]); ++i)
            id.push_back(text[i]);
    const InstallerSpec *installer = nullptr;
    for (const InstallerSpec &i : config.installers)
        if (i.gog_id == id) {
            installer = &i;
            break;
        }
    if (!installer)
        return Outcome::fail(t("unknownInstaller", {{"name", file_name(setup)}}));
    if (Outcome o = capture(h, {"--list", setup}, &text); !o.ok)
        return o;
    const std::set<std::string> listed = listed_paths(text);
    std::vector<std::pair<std::string, Manifest>> fitting;
    for (const std::string &name : installer->manifests) {
        Manifest m;
        if (load_manifest(manifest_dir, name, &m) &&
            std::all_of(m.files.begin(), m.files.end(),
                        [&](const ManifestFile &f) { return listed.count(lower(f.path)) != 0; }))
            fitting.emplace_back(name, std::move(m));
    }
    // Two languages of a build can hold the same paths (English and French
    // do): GOG's description of the installer, which names its language,
    // tells them apart.
    auto chosen = fitting.begin();
    if (fitting.size() > 1) {
        const std::string info = "goggame-" + id + ".info";
        std::string sum;
        if (Outcome o = file_digest(h, setup, info, join(base, kProbeStaging), &sum); !o.ok)
            return o;
        chosen = std::find_if(fitting.begin(), fitting.end(), [&](const auto &c) {
            return std::any_of(c.second.files.begin(), c.second.files.end(), [&](const ManifestFile &f) {
                return lower(f.path) == info && f.sha256 == sum;
            });
        });
    }
    if (chosen != fitting.end()) {
        Plan p;
        p.installer = *installer;
        p.manifest_name = chosen->first;
        p.manifest = std::move(chosen->second);
        p.setup = setup;
        p.dest = join(base, installer->folder);
        *out = std::move(p);
        return {};
    }
    return Outcome::fail(t("otherInstallerBuild", {{"name", file_name(setup)}}));
}

Outcome run(const Plan &plan, const ProgressFn &progress) {
    const std::string h = helper();
    if (h.empty())
        return Outcome::fail(t("noHelper"));
    const std::string &dest = plan.dest;
    Progress p;
    p.name =
        plan.installer.title.text().empty() ? plan.installer.folder : plan.installer.title.text();
    if (!make_dirs(dest))
        return Outcome::fail(os_error("Could not create", dest));
    // A helper left from a launcher that died mid-install stops at its next
    // file (its output pipe is gone); make sure it is gone first.
    if (InstallMarker old; InstallMarker::read(dest, &old))
        stop_stale(old.helper_pid, h);
    InstallMarker marker;
    marker.installer = plan.installer.id;
    marker.setup = plan.setup;
    marker.manifest = plan.manifest_name;
    marker.games = plan.installer.games;
    if (!marker.write(dest))
        return Outcome::fail(os_error("Could not write", join(dest, InstallMarker::kName)));

    std::vector<ManifestFile> pending;
    if (Outcome o = check(plan.manifest.files, dest, Progress::Checking, p, progress, &pending);
        !o.ok)
        return o;
    if (!pending.empty()) {
        int64_t need = 0;
        for (const ManifestFile &f : pending)
            need += f.size;
        uint64_t free = 0;
        if (os_free_space(dest.c_str(), &free) == 0 && free > 0 &&
            int64_t(free) < need + (int64_t(64) << 20))
            return Outcome::fail(no_space(need, int64_t(free)));
        for (const ManifestFile &f : pending)
            if (const std::string stale = entry(dest, f.path); !stale.empty())
                os_unlink(stale.c_str());
        if (Outcome o = extract(pending, plan, h, marker, p, progress); !o.ok)
            return o;
        std::vector<ManifestFile> left;
        if (Outcome o = check(pending, dest, Progress::Verifying, p, progress, &left); !o.ok)
            return o;
        pending = std::move(left);
        for (const std::string &patch : plan.patches) {
            if (pending.empty())
                break;
            p.phase = Progress::Patching;
            p.current = file_name(patch);
            if (!progress(p))
                return Outcome::cancel();
            Patch unpacked;
            const std::string staging = join(dest, kPatchStaging);
            Outcome o = unpack_patch(h, patch, staging, &unpacked);
            int replaced = 0;
            if (o.ok)
                o = apply(unpacked, pending, dest, &replaced);
            remove_tree(staging);
            if (!o.ok)
                return o;
            if (o = check(pending, dest, Progress::Verifying, p, progress, &left); !o.ok)
                return o;
            pending = std::move(left);
        }
        if (!pending.empty()) {
            std::vector<std::string> paths;
            for (const ManifestFile &f : pending)
                paths.push_back(f.path);
            return Outcome::fail(verify_failed(paths));
        }
    }
    if (os_unlink(join(dest, InstallMarker::kName).c_str()) != 0)
        return Outcome::fail(os_error("Could not remove", join(dest, InstallMarker::kName)));
    return {};
}

Outcome apply_patch(const std::string &patch, const Config &config, const std::string &manifest_dir,
                    const std::string &base, const ProgressFn &progress,
                    std::vector<PatchResult> *results) {
    const std::string h = helper();
    if (h.empty())
        return Outcome::fail(t("noHelper"));
    Progress p;
    p.name = file_name(patch);
    p.phase = Progress::Patching;
    p.current = file_name(patch);
    if (!progress(p))
        return Outcome::cancel();
    if (!make_dirs(base))
        return Outcome::fail(os_error("Could not create", base));
    const std::string staging = join(base, kPatchStaging);
    Patch unpacked;
    if (Outcome o = unpack_patch(h, patch, staging, &unpacked); !o.ok) {
        remove_tree(staging);
        return o;
    }
    results->clear();
    std::vector<std::string> missing;
    Outcome outcome;
    for (const InstallerSpec &installer : config.installers) {
        std::vector<Manifest> manifests;
        for (const std::string &name : installer.manifests) {
            Manifest m;
            if (load_manifest(manifest_dir, name, &m) &&
                std::any_of(m.files.begin(), m.files.end(), [&](const ManifestFile &f) {
                    return unpacked.files.count(f.sha256) != 0;
                }))
                manifests.push_back(std::move(m));
        }
        if (manifests.empty())
            continue;
        const std::string title =
            installer.title.text().empty() ? installer.folder : installer.title.text();
        const std::string root = join(base, installer.folder);
        if (!exists(root)) {
            missing.push_back(title);
            continue;
        }
        // The manifest the install's files fit best, by size.
        auto fits = [&](const Manifest &m) {
            return std::count_if(m.files.begin(), m.files.end(), [&](const ManifestFile &f) {
                return size_of(place(f.path, root)) == f.size;
            });
        };
        const Manifest *fit = &manifests.front();
        for (const Manifest &m : manifests)
            if (fits(m) > fits(*fit))
                fit = &m;
        p.name = title;
        if (!progress(p)) {
            outcome = Outcome::cancel();
            break;
        }
        PatchResult r;
        r.title = title;
        if (outcome = apply(unpacked, fit->files, root, &r.replaced); !outcome.ok)
            break;
        results->push_back(r);
    }
    remove_tree(staging);
    if (!outcome.ok)
        return outcome;
    if (results->empty()) {
        if (missing.empty())
            return Outcome::fail(t("unknownPatch", {{"name", file_name(patch)}}));
        std::string list;
        for (const std::string &m : missing)
            list += (list.empty() ? "" : ", ") + m;
        return Outcome::fail(t("patchNeedsGame", {{"patch", file_name(patch)}, {"name", list}}));
    }
    return {};
}

} // namespace gog

} // namespace launcher::native
