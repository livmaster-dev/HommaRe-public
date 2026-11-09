// launcher_platform_android.cpp - the launcher's platform on Android: the
// system folder and document pickers through RecompActivity, content URIs read
// as descriptors, imports into the app's external files folder kept alive by
// a foreground service, and a folder copied there over USB moved into place.
#include "launcher_sdl.h"

#include "../../platform/os.h"
#include "../../runtime/layout.h"

#include <jni.h>

#include <map>
#include <mutex>

namespace launcher {
namespace {

constexpr int kPickTree = 0x5201, kPickZip = 0x5202, kPickSaves = 0x5203, kCreateExport = 0x5204,
              kPickFiles = 0x5205;

// The activity class, resolved on the main thread: FindClass from a worker
// thread sees only the system class loader.
jclass g_activity = nullptr;
std::mutex g_mutex;
std::map<int, PickDone> g_pending;

JNIEnv *env() {
    return static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
}

// Resolved once, from SDL's main thread (make_platform and the native
// launcher's first pick both run there).
void resolve_activity() {
    if (g_activity)
        return;
    JNIEnv *e = env();
    jclass local = e->FindClass("dev/recompkit/RecompActivity");
    g_activity = static_cast<jclass>(e->NewGlobalRef(local));
    e->DeleteLocalRef(local);
}

jstring jstr(JNIEnv *e, const std::string &s) {
    return e->NewStringUTF(s.c_str());
}

std::string cstr(JNIEnv *e, jstring s) {
    if (!s)
        return "";
    const char *c = e->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c)
        e->ReleaseStringUTFChars(s, c);
    return out;
}

void pick(int request, const std::string &suggested, PickDone done) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending[request] = std::move(done);
    }
    JNIEnv *e = env();
    jmethodID m = e->GetStaticMethodID(g_activity, "launcherPick", "(ILjava/lang/String;)V");
    jstring name = jstr(e, suggested);
    e->CallStaticVoidMethod(g_activity, m, jint(request), name);
    e->DeleteLocalRef(name);
}

int open_fd(const std::string &uri, const char *mode) {
    JNIEnv *e = env();
    jmethodID m = e->GetStaticMethodID(g_activity, "launcherOpenFd",
                                       "(Ljava/lang/String;Ljava/lang/String;)I");
    jstring u = jstr(e, uri), md = jstr(e, mode);
    const int fd = e->CallStaticIntMethod(g_activity, m, u, md);
    e->DeleteLocalRef(u);
    e->DeleteLocalRef(md);
    return fd;
}

bool copy_fd(int from, int to) {
    std::vector<uint8_t> buf(1 << 20);
    for (;;) {
        const int64_t n = os_fd_read(from, buf.data(), buf.size());
        if (n < 0)
            return false;
        if (n == 0)
            return true;
        for (int64_t off = 0; off < n;) {
            const int64_t w = os_fd_write(to, buf.data() + off, size_t(n - off));
            if (w <= 0)
                return false;
            off += w;
        }
    }
}

// A folder the player picked: its documents listed through the activity and
// each one read through a descriptor.
class TreeSource final : public Source {
  public:
    explicit TreeSource(std::string uri) : uri_(std::move(uri)) {}
    bool list(std::vector<Entry> *out, std::string *error) override {
        out->clear();
        docs_.clear();
        JNIEnv *e = env();
        jmethodID m = e->GetStaticMethodID(g_activity, "launcherListTree",
                                           "(Ljava/lang/String;)[Ljava/lang/String;");
        jstring u = jstr(e, uri_);
        auto rows = static_cast<jobjectArray>(e->CallStaticObjectMethod(g_activity, m, u));
        e->DeleteLocalRef(u);
        if (!rows) {
            if (error)
                *error = "the chosen folder cannot be read";
            return false;
        }
        const jsize n = e->GetArrayLength(rows);
        for (jsize i = 0; i < n; ++i) {
            auto row = static_cast<jstring>(e->GetObjectArrayElement(rows, i));
            const std::string text = cstr(e, row);
            e->DeleteLocalRef(row);
            std::vector<std::string> f;
            for (size_t start = 0;;) {
                const size_t tab = text.find('\t', start);
                f.push_back(
                    text.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                if (tab == std::string::npos)
                    break;
                start = tab + 1;
            }
            if (f.size() != 5)
                continue;
            Entry en;
            en.relative = f[0];
            en.size = strtoull(f[1].c_str(), nullptr, 10);
            en.mtime = strtoll(f[2].c_str(), nullptr, 10);
            en.is_dir = f[4] == "1";
            en.index = uint32_t(docs_.size());
            docs_.push_back(f[3]);
            out->push_back(en);
        }
        e->DeleteLocalRef(rows);
        return true;
    }
    bool read(const Entry &en, const std::function<bool(const uint8_t *, size_t)> &sink,
              std::string *error) override {
        const int fd = en.index < docs_.size() ? open_fd(docs_[en.index], "r") : -1;
        if (fd < 0) {
            if (error)
                *error = "cannot open " + en.relative;
            return false;
        }
        std::vector<uint8_t> buf(1 << 20);
        bool ok = true;
        for (;;) {
            const int64_t n = os_fd_read(fd, buf.data(), buf.size());
            if (n < 0) {
                if (error)
                    *error = "cannot read " + en.relative;
                ok = false;
                break;
            }
            if (n == 0 || !sink(buf.data(), size_t(n))) {
                ok = n == 0;
                break;
            }
        }
        os_fd_close(fd);
        return ok;
    }
    // The document URI of a listed entry.
    const std::string &document(uint32_t index) const {
        static const std::string none;
        return index < docs_.size() ? docs_[index] : none;
    }

  private:
    std::string uri_;
    std::vector<std::string> docs_;
};

class AndroidPlatform final : public Platform {
  public:
    explicit AndroidPlatform(std::string external) : external_(std::move(external)) {}

    PlatformInfo info() override {
        PlatformInfo i;
        i.plays_in_place = false;
        i.can_pick_folder = true;
        i.can_pick_zip = true;
        i.can_open_folder = false;
        i.touch = true;
        i.import_root = external_ + "/game";
        i.profile_dir = host_layout().profile_dir;
        i.drop_hint = "Or copy the game folder from a computer over USB into " + external_ +
                      " (any folder name), then open the app again.";
        return i;
    }
    void pick_folder(PickDone done) override {
        pick(kPickTree, "", wrap(std::move(done), true));
    }
    void pick_zip(PickDone done) override {
        pick(kPickZip, "", wrap(std::move(done), false));
    }
    void pick_saves(PickDone done) override {
        // import_profile reads a path: copy the document into the cache first.
        const std::string cache = external_ + "/cache-saves.zip";
        pick(kPickSaves, "", [done, cache](std::vector<Picked> picked, std::string error) {
            if (picked.empty()) {
                done(picked, error);
                return;
            }
            const int in = open_fd(picked[0].uri, "r");
            const int out = os_fd_open(cache.c_str(), OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC);
            const bool ok = in >= 0 && out >= 0 && copy_fd(in, out);
            if (in >= 0)
                os_fd_close(in);
            if (out >= 0)
                os_fd_close(out);
            if (!ok) {
                done({}, "cannot read " + picked[0].name);
                return;
            }
            picked[0].path = cache;
            done(picked, "");
        });
    }
    // Written to the cache first; export_ready asks where it goes.
    void pick_export(const std::string &suggested, PickDone done) override {
        Picked p;
        p.path = external_ + "/cache-export.zip";
        p.name = suggested;
        done({p}, "");
    }
    void export_ready(const Picked &p) override {
        const std::string path = p.path;
        pick(kCreateExport, p.name, [path](std::vector<Picked> picked, std::string) {
            if (picked.empty())
                return;
            const int out = open_fd(picked[0].uri, "w");
            const int in = os_fd_open(path.c_str(), OS_O_RDONLY);
            if (in >= 0 && out >= 0)
                copy_fd(in, out);
            if (in >= 0)
                os_fd_close(in);
            if (out >= 0)
                os_fd_close(out);
            os_unlink(path.c_str());
        });
    }
    std::unique_ptr<Source> open(const Picked &p, std::string *error) override {
        if (p.tree)
            return std::make_unique<TreeSource>(p.uri);
        if (!p.uri.empty())
            return zip_source_fd(open_fd(p.uri, "r"), error);
        return Platform::open(p, error);
    }
    std::vector<std::string> candidates(const Spec &spec) override {
        // A game folder copied into the app's files over USB, under any name.
        std::vector<std::string> out;
        std::pair<std::vector<std::string> *, const std::string *> ctx(&out, &external_);
        os_listdir(
            external_.c_str(),
            [](const char *name, void *user) {
                auto *c =
                    static_cast<std::pair<std::vector<std::string> *, const std::string *> *>(user);
                if (name[0] != '.')
                    c->first->push_back(*c->second + "/" + name);
                return 0;
            },
            &ctx);
        std::vector<std::string> found;
        for (const std::string &dir : out) {
            const std::string name = dir.substr(dir.find_last_of('/') + 1);
            if (name == "game" || name == "profile" || name.compare(0, 6, "cache-") == 0)
                continue;
            OsStat st{};
            if (os_stat(dir.c_str(), &st) == 0 && st.is_dir && !find_root(spec, dir).empty())
                found.push_back(dir);
        }
        return found;
    }
    bool movable(const Picked &p) override {
        const std::string prefix = external_ + "/";
        return !p.path.empty() && p.path.compare(0, prefix.size(), prefix) == 0 &&
               p.path.find('/', prefix.size()) == std::string::npos;
    }
    bool initial_pick(Picked *p) override {
        JNIEnv *e = env();
        jmethodID m =
            e->GetStaticMethodID(g_activity, "launcherTakeViewUri", "()Ljava/lang/String;");
        auto s = static_cast<jstring>(e->CallStaticObjectMethod(g_activity, m));
        const std::string uri = cstr(e, s);
        if (s)
            e->DeleteLocalRef(s);
        if (uri.empty())
            return false;
        p->uri = uri;
        p->name = "the opened ZIP";
        return true;
    }
    void open_url(const std::string &url) override {
        JNIEnv *e = env();
        jmethodID m = e->GetStaticMethodID(g_activity, "launcherOpenUrl", "(Ljava/lang/String;)V");
        jstring u = jstr(e, url);
        e->CallStaticVoidMethod(g_activity, m, u);
        e->DeleteLocalRef(u);
    }
    void import_activity(bool active, const Progress *progress) override {
        // The notification at most every half second.
        const uint64_t now = SDL_GetTicks();
        if (progress && active && now - last_update_ < 500)
            return;
        last_update_ = now;
        JNIEnv *e = env();
        jmethodID m =
            e->GetStaticMethodID(g_activity, "launcherImportActivity", "(ZJJLjava/lang/String;)V");
        jstring current = jstr(e, progress ? progress->current : "");
        e->CallStaticVoidMethod(g_activity, m, jboolean(active),
                                jlong(progress ? progress->bytes_done : 0),
                                jlong(progress ? progress->bytes_total : 0), current);
        e->DeleteLocalRef(current);
    }

  private:
    // Tree and document URIs arrive as Picked.uri.
    static PickDone wrap(PickDone done, bool tree) {
        return [done, tree](std::vector<Picked> picked, std::string error) {
            for (Picked &p : picked)
                p.tree = tree;
            done(picked, error);
        };
    }
    std::string external_;
    uint64_t last_update_ = 0;
};

} // namespace

std::unique_ptr<Platform> make_platform(SDL_Window *) {
    resolve_activity();
    const char *external = SDL_GetAndroidExternalStoragePath();
    return std::make_unique<AndroidPlatform>(external ? external : "");
}

void android_pick_documents(PickDone done) {
    resolve_activity();
    pick(kPickFiles, "", std::move(done));
}

void android_pick_tree(PickDone done) {
    resolve_activity();
    pick(kPickTree, "", [done](std::vector<Picked> picked, std::string error) {
        for (Picked &p : picked)
            p.tree = true;
        done(picked, error);
    });
}

int android_open_document(const std::string &uri) {
    resolve_activity();
    return open_fd(uri, "r");
}

bool android_list_tree(const std::string &tree, std::vector<Entry> *entries,
                       std::vector<std::string> *documents, std::string *error) {
    resolve_activity();
    TreeSource source(tree);
    if (!source.list(entries, error))
        return false;
    documents->clear();
    for (const Entry &e : *entries)
        documents->push_back(source.document(e.index));
    return true;
}

} // namespace launcher

extern "C" JNIEXPORT void JNICALL Java_dev_recompkit_RecompActivity_nativePicked(
    JNIEnv *e, jclass, jint request, jobjectArray uris, jobjectArray names) {
    launcher::PickDone done;
    {
        std::lock_guard<std::mutex> lock(launcher::g_mutex);
        auto it = launcher::g_pending.find(int(request));
        if (it == launcher::g_pending.end())
            return;
        done = std::move(it->second);
        launcher::g_pending.erase(it);
    }
    std::vector<launcher::Picked> picked;
    const jsize n = uris ? e->GetArrayLength(uris) : 0;
    for (jsize i = 0; i < n; ++i) {
        auto u = static_cast<jstring>(e->GetObjectArrayElement(uris, i));
        auto nm = static_cast<jstring>(e->GetObjectArrayElement(names, i));
        launcher::Picked p;
        p.uri = launcher::cstr(e, u);
        p.name = launcher::cstr(e, nm);
        picked.push_back(p);
        e->DeleteLocalRef(u);
        e->DeleteLocalRef(nm);
    }
    if (done)
        done(picked, "");
}
