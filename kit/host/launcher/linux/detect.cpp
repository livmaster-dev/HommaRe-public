// detect.cpp - see detect.h.
#include "detect.h"

#include "../../../platform/os.h"
#include "../launcher.h"
#include "installer.h"
#include "skin.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <mutex>
#include <unistd.h>

namespace launcher::native {

namespace {

struct Digest {
    int64_t size, mtime;
    std::string sha256;
};
std::mutex g_digest_lock;
std::map<std::string, Digest> g_digests;

std::vector<std::string> names(const std::string &dir) {
    std::vector<std::string> out;
    os_listdir(
        dir.c_str(),
        [](const char *name, void *user) {
            if (name[0] != '.')
                static_cast<std::vector<std::string> *>(user)->push_back(name);
            return 0;
        },
        &out);
    std::sort(out.begin(), out.end());
    return out;
}

std::string join(const std::string &dir, const std::string &name) {
    return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name;
}

std::shared_ptr<const Image> art_of(const GameSpec &spec, const std::string &root) {
    for (const std::string &rel : spec.art)
        if (const std::string at = entry(root, rel); !at.empty())
            if (auto art = load_art(at, spec.art_crop))
                return art;
    return nullptr;
}

int rank(const GameState &s) {
    switch (s.kind) {
    case GameState::Ready:
        return 3;
    case GameState::Incomplete:
    case GameState::Interrupted:
        return 2;
    case GameState::WrongVersion:
        return 1;
    default:
        return 0;
    }
}

} // namespace

std::vector<std::string> children(const std::string &dir) {
    std::vector<std::string> out;
    for (const std::string &n : names(dir))
        if (is_dir(join(dir, n)))
            out.push_back(join(dir, n));
    return out;
}

bool is_dir(const std::string &path) {
    OsStat st{};
    return os_stat(path.c_str(), &st) == 0 && st.is_dir;
}

bool exists(const std::string &path) {
    OsStat st{};
    return os_stat(path.c_str(), &st) == 0;
}

bool has_gog_info(const std::string &dir) {
    for (const std::string &n : names(dir))
        if (n.compare(0, 8, "goggame-") == 0 && n.size() > 13 &&
            n.compare(n.size() - 5, 5, ".info") == 0)
            return true;
    return false;
}

std::string entry(const std::string &dir, const std::string &relative) {
    std::string at = dir;
    for (size_t start = 0; start < relative.size();) {
        const size_t slash = std::min(relative.find('/', start), relative.size());
        const std::string part = relative.substr(start, slash - start);
        start = slash + 1;
        if (part.empty())
            continue;
        if (exists(join(at, part))) {
            at = join(at, part);
            continue;
        }
        const std::string found = find_name(at, part);
        if (found.empty())
            return "";
        at = join(at, found);
    }
    return at;
}

std::string sha256_cached(const std::string &path) {
    OsStat st{};
    if (os_stat(path.c_str(), &st) != 0)
        return "";
    {
        std::lock_guard<std::mutex> lock(g_digest_lock);
        auto it = g_digests.find(path);
        if (it != g_digests.end() && it->second.size == int64_t(st.size) &&
            it->second.mtime == int64_t(st.mtime))
            return it->second.sha256;
    }
    const std::string digest = sha256_file(path);
    std::lock_guard<std::mutex> lock(g_digest_lock);
    g_digests[path] = {int64_t(st.size), int64_t(st.mtime), digest};
    return digest;
}

bool probe(const GameSpec &spec, const std::string &root, Found *out) {
    // Another GOG product's folder (Tribes of the East keeps its executable
    // at bin/H5_Game.exe too): only that product's games count there.
    if (!spec.gog_id.empty() && has_gog_info(root) &&
        entry(root, "goggame-" + spec.gog_id + ".info").empty())
        return false;
    for (const std::string &rel : spec.exe) {
        const std::string exe = entry(root, rel);
        if (exe.empty() || is_dir(exe))
            continue;
        Found f;
        f.root = root;
        f.exe = exe;
        f.art = art_of(spec, root);
        const std::string digest = sha256_cached(exe);
        if (std::find(spec.sha256.begin(), spec.sha256.end(), digest) == spec.sha256.end()) {
            f.state.kind = GameState::WrongVersion;
            f.state.digest = digest;
        } else {
            for (const std::string &dir : spec.required_dirs)
                if (entry(root, dir).empty())
                    f.state.missing.push_back(dir);
            f.state.kind = f.state.missing.empty() ? GameState::Ready : GameState::Incomplete;
        }
        *out = std::move(f);
        return true;
    }
    return false;
}

std::map<std::string, Found> scan(const std::vector<GameSpec> &specs,
                                  const std::vector<std::string> &roots) {
    std::map<std::string, Found> best;
    for (const std::string &root : roots) {
        // An unfinished install: its games wait for it to resume.
        InstallMarker marker;
        if (InstallMarker::read(root, &marker)) {
            for (const GameSpec &spec : specs) {
                if (std::find(marker.games.begin(), marker.games.end(), spec.id) ==
                    marker.games.end())
                    continue;
                if (auto old = best.find(spec.id);
                    old != best.end() && rank(old->second.state) >= 2)
                    continue;
                Found f;
                f.state.kind = GameState::Interrupted;
                f.state.setup = marker.setup;
                f.root = root;
                f.art = art_of(spec, root);
                best[spec.id] = std::move(f);
            }
            continue;
        }
        for (const GameSpec &spec : specs) {
            Found f;
            if (!probe(spec, root, &f))
                continue;
            if (auto old = best.find(spec.id);
                old != best.end() && rank(old->second.state) >= rank(f.state))
                continue;
            best[spec.id] = std::move(f);
        }
    }
    return best;
}

std::vector<std::string> roots_below(const std::string &dir, const std::vector<GameSpec> &specs) {
    std::vector<std::string> out, level{dir};
    for (int depth = 0; depth < 3; ++depth) {
        std::vector<std::string> next;
        for (const std::string &d : level) {
            Found f;
            if (std::any_of(specs.begin(), specs.end(),
                            [&](const GameSpec &s) { return probe(s, d, &f); }))
                out.push_back(d);
            else
                for (const std::string &c : children(d))
                    next.push_back(c);
        }
        level = std::move(next);
    }
    return out;
}

std::string canonical(const std::string &path) {
    char buf[PATH_MAX];
    // A link to an open descriptor (/proc/self/fd/<n>: a document the player
    // picked on Android, android_files.cpp) stands for that document under
    // its own name, beside the other files linked with it (an installer's
    // slices are found by name): its target names a file only the
    // descriptor can reach.
    const ssize_t n = readlink(path.c_str(), buf, sizeof buf - 1);
    if (n > 0 && std::string(buf, size_t(n)).rfind("/proc/self/fd/", 0) == 0)
        return canonical(parent_dir(path)) + "/" + file_name(path);
    if (realpath(path.c_str(), buf))
        return buf;
    std::vector<std::string> parts;
    for (size_t start = 0; start <= path.size();) {
        const size_t slash = std::min(path.find('/', start), path.size());
        const std::string part = path.substr(start, slash - start);
        start = slash + 1;
        if (part.empty() || part == ".")
            continue;
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    std::string out;
    for (const std::string &p : parts)
        out += "/" + p;
    return out.empty() ? "/" : out;
}

std::string parent_dir(const std::string &path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    const size_t slash = p.find_last_of('/');
    if (slash == std::string::npos)
        return ".";
    return slash == 0 ? "/" : p.substr(0, slash);
}

std::string file_name(const std::string &path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

} // namespace launcher::native
