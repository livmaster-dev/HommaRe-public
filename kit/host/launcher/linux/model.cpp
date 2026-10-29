// model.cpp - see model.h.
#include "model.h"

#include "../../../mods/settings_file.h"
#include "../../../platform/os.h"
#include "../../controls/json.h"
#include "game_config.h"
#include "l10n.h"
#ifdef __ANDROID__
#include "android_files.h"
#endif

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace launcher::native {

struct Model::Shared {
    std::mutex lock;
    std::vector<Task> tasks;
    std::function<void()> wake;
};

namespace {

const char *const kFrameLimit = "host.display/frame_limit";
const char *const kEnergySaver = "host.display/energy_saver";
const char *const kIdleFrames = "host.display/idle_frames";
const char *const kUiBounds = "homm5.wide/ui_bounds";
const char *const kGpuSkin = "homm5.perf/gpu_skin";

// The startup videos (video/intro.xml, and intro_a1.xml for the expansions)
// are lists the game plays in turn. An empty list in the profile, which
// shadows the game's files, skips them. The marker tells these files from a
// mod's own.
const char *const kIntroFiles[] = {"video/intro.xml", "video/intro_a1.xml"};
const char *const kIntroMarker = "<!-- launcher: skip the startup videos -->";

int setting(const char *key, int fallback) {
    int64_t value = 0;
    return mods_settings_file_get(key, &value) ? int(value) : fallback;
}

std::string env(const char *name) {
    const char *v = getenv(name);
    return v ? v : "";
}

std::string join(const std::string &dir, const std::string &name) {
    return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name;
}

std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

bool ends_with(const std::string &s, const char *suffix) {
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void mkdirs(const std::string &dir) {
    for (size_t i = dir.find('/', 1); i != std::string::npos; i = dir.find('/', i + 1))
        os_mkdir(dir.substr(0, i).c_str());
    os_mkdir(dir.c_str());
}

bool write_text(const std::string &path, const std::string &text) {
    const std::string part = path + ".part";
    FILE *f = fopen(part.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (fclose(f) != 0 || !ok) {
        os_unlink(part.c_str());
        return false;
    }
    return os_rename(part.c_str(), path.c_str()) == 0;
}

std::string overlay_root() {
    const char *dir = mods_settings_file_dir();
    return dir ? dir : "";
}

bool intro_skipped() {
    const std::string root = overlay_root();
    std::string text;
    return !root.empty() && read_text(join(root, kIntroFiles[0]), &text) &&
           text.find(kIntroMarker) != std::string::npos;
}

// The textures of the install the launcher would play first: this app's own
// game when it is ready, else any other found copy. RECOMP_LAUNCHER_SKIN=0
// keeps the flat look.
std::shared_ptr<const Skin> skin_for(const std::map<std::string, Found> &found,
                                     const std::vector<GameSpec> &specs, const std::string &module,
                                     const std::shared_ptr<const Skin> &current) {
    if (env("RECOMP_LAUNCHER_SKIN") == "0")
        return nullptr;
    std::vector<std::pair<int, std::string>> ranked;
    for (const GameSpec &spec : specs) {
        auto f = found.find(spec.id);
        if (f == found.end() || f->second.state.kind == GameState::Interrupted)
            continue;
        ranked.push_back(
            {f->second.state.kind == GameState::Ready ? (spec.module == module ? 0 : 1) : 2,
             f->second.root});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    std::string cache;
#ifdef __ANDROID__
    // The app's own cache folder: Android has no XDG one.
    if (const char *app = SDL_GetAndroidCachePath())
        cache = app;
#else
    if (const std::string xdg = env("XDG_CACHE_HOME"); !xdg.empty())
        cache = xdg;
    else if (const std::string home = env("HOME"); !home.empty())
        cache = home + "/.cache";
#endif
    if (!cache.empty())
        cache += std::string("/") + RECOMP_APP_NAME + "/LauncherSkin";
    for (const auto &r : ranked) {
        if (current && current->root == r.second)
            return current;
        if (auto skin = Skin::load(r.second, cache))
            return skin;
    }
    return nullptr;
}

} // namespace

bool installs(const Entry &e) {
    if (e.interrupted())
        return false;
    switch (e.state.kind) {
    case GameState::NotFound:
    case GameState::Incomplete:
    case GameState::WrongVersion:
        return true;
    default:
        return false;
    }
}

Model::Model(Config config, const std::string &modules, const std::string &known_exe,
             const std::string &profile_dir, const std::string &manifest_dir,
             const std::string &state_file)
    : config_(std::move(config)), known_exe_(known_exe), profile_dir_(profile_dir),
      manifest_dir_(manifest_dir), state_file_(state_file), shared_(std::make_shared<Shared>()),
      cancel_(std::make_shared<std::atomic<bool>>()) {
    for (size_t start = 0; start <= modules.size();) {
        const size_t comma = std::min(modules.find(',', start), modules.size());
        std::string m = modules.substr(start, comma - start);
        m.erase(0, m.find_first_not_of(' '));
        m.erase(m.find_last_not_of(' ') + 1);
        if (!m.empty())
            modules_.push_back(m);
        start = comma + 1;
    }
    this_module_ = modules_.empty() ? "" : modules_.front();
    for (const GameSpec &spec : config_.games) {
        Entry e;
        e.spec = spec;
        e.playable_here =
            std::find(modules_.begin(), modules_.end(), spec.module) != modules_.end();
        entries_.push_back(std::move(e));
    }
    // launcher.json: what the Mac keeps in its user defaults.
    std::string text;
    controls::Json state;
    std::string error;
    if (!state_file_.empty() && read_text(state_file_, &text) &&
        controls::json_parse(text, &state, &error)) {
        if (const controls::Json *roots = state.get("roots"))
            for (const controls::Json &r : roots->a)
                if (r.type == controls::Json::String)
                    roots_.push_back(r.s);
        last_game_ = state.str("lastGame", "");
        choose_language(state.str("language", ""));
    }
    frame_limit_ = setting(kFrameLimit, 2);
    energy_saver_ = setting(kEnergySaver, 0);
    idle_frames_ = setting(kIdleFrames, 0);
    ui_bounds_ = setting(kUiBounds, 0);
    gpu_skin_ = setting(kGpuSkin, 0) != 2;
    skip_intro_ = intro_skipped();
    // RECOMP_LAUNCHER_PICK=<game id>: an unattended run's choice.
    const std::string picked = env("RECOMP_LAUNCHER_PICK");
    const std::string last = picked.empty() ? last_game_ : picked;
    auto with = [&](auto pred) {
        auto it = std::find_if(config_.games.begin(), config_.games.end(), pred);
        return it == config_.games.end() ? std::string() : it->id;
    };
    selected_id_ = with([&](const GameSpec &g) { return g.id == last; });
    if (selected_id_.empty())
        selected_id_ = with([&](const GameSpec &g) { return g.module == this_module_; });
    if (selected_id_.empty() && !config_.games.empty())
        selected_id_ = config_.games.front().id;
}

Model::~Model() {
    cancel_->store(true);
    gog::stop_running();
    for (Worker &w : workers_)
        if (w.thread.joinable())
            w.thread.join();
}

void Model::set_wake(std::function<void()> wake) {
    std::lock_guard<std::mutex> lock(shared_->lock);
    shared_->wake = std::move(wake);
}

void Model::post(const std::shared_ptr<Shared> &shared, Task task) {
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lock(shared->lock);
        shared->tasks.push_back(std::move(task));
        wake = shared->wake;
    }
    if (wake)
        wake();
}

bool Model::drain() {
    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(shared_->lock);
        tasks.swap(shared_->tasks);
    }
    for (Task &t : tasks)
        t(*this);
    // Threads that finished are joined as they go.
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (it->done->load()) {
            it->thread.join();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
    return !tasks.empty();
}

void Model::run_worker(std::function<void(const std::shared_ptr<Shared> &)> work) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<Shared> shared = shared_;
    workers_.push_back({std::thread([work = std::move(work), shared, done] {
                            work(shared);
                            done->store(true);
                        }),
                        done});
}

const Entry *Model::selected() const {
    for (const Entry &e : entries_)
        if (e.spec.id == selected_id_)
            return &e;
    return nullptr;
}

void Model::select(const std::string &id) {
    selected_id_ = id;
}

void Model::step(int by) {
    const int n = int(entries_.size());
    for (int i = 0; i < n; ++i)
        if (entries_[size_t(i)].spec.id == selected_id_) {
            selected_id_ = entries_[size_t(((i + by) % n + n) % n)].spec.id;
            return;
        }
}

std::string Model::title() const {
    return config_.title.empty() ? RECOMP_APP_NAME : config_.title.text();
}

void Model::set_notice(const std::string &text) {
    notice_ = text;
    // Unattended runs see the alerts in the log.
    if (!text.empty())
        fprintf(stderr, "[launcher] %s\n", text.c_str());
}

// MARK: Detection

void Model::refresh() {
    if (scanning_)
        return;
    scanning_ = true;
    const std::vector<GameSpec> specs = config_.games;
    const std::vector<std::string> roots = candidate_roots();
    const std::string module = this_module_;
    const std::shared_ptr<const Skin> current = skin_;
    run_worker([specs, roots, module, current](const std::shared_ptr<Shared> &shared) {
        auto found = std::make_shared<std::map<std::string, Found>>(scan(specs, roots));
        std::shared_ptr<const Skin> skin = skin_for(*found, specs, module, current);
        post(shared, [found, skin](Model &m) {
            m.apply(*found);
            m.skin_ = skin;
            m.scanning_ = false;
            if (m.on_scanned)
                m.on_scanned();
        });
    });
}

void Model::apply(const std::map<std::string, Found> &found) {
    has_scanned_ = true;
    for (Entry &e : entries_) {
        auto f = found.find(e.spec.id);
        if (f != found.end()) {
            e.state = f->second.state;
            e.root = f->second.root;
            e.exe = f->second.exe;
            e.art = f->second.art;
        } else {
            e.state = GameState();
            e.root.clear();
            e.exe.clear();
            e.art.reset();
        }
    }
    // A first launch lands on a game that is actually there.
    const Entry *sel = selected();
    if (sel && !sel->can_play() && last_game_.empty())
        for (const Entry &e : entries_)
            if (e.can_play()) {
                selected_id_ = e.spec.id;
                break;
            }
}

std::vector<std::string> Model::live_roots(const std::vector<std::string> &paths) {
    std::vector<std::string> out, seen;
    for (const std::string &p : paths) {
        const std::string c = canonical(p);
        if (!is_dir(c) || std::find(seen.begin(), seen.end(), c) != seen.end())
            continue;
        seen.push_back(c);
        out.push_back(p);
    }
    return out;
}

std::vector<std::string> Model::remembered_roots() const {
    return live_roots(roots_);
}

std::string Model::import_base() {
    // Test runs keep their installs away from the player's own folder.
    if (const std::string dir = env("RECOMP_LAUNCHER_GAMES_DIR"); !dir.empty())
        return dir;
#ifdef __ANDROID__
    // The app's external files folder, where `adb push` and a computer over
    // USB reach too (Android/data/<package>/files/Games).
    const std::string data = android::data_dir();
    return data.empty() ? "" : data + "/Games";
#endif
    char buf[4096];
    if (os_user_data_dir(RECOMP_APP_NAME, buf, sizeof buf) != 0)
        return "";
    return std::string(buf) + "/Games";
}

// Where a copy may be: folders the player picked, the Games folder, the
// executable the host already knows, and the usual places on Linux: the
// home folder, ~/Games (Lutris, Heroic) and Wine prefixes' GOG folders.
std::vector<std::string> Model::candidate_roots() const {
    std::vector<std::string> roots;
    auto add = [&](const std::string &p) {
        if (p.empty())
            return;
        const std::string s = canonical(p);
        if (std::find(roots.begin(), roots.end(), s) == roots.end())
            roots.push_back(s);
    };
    // A test run's own Games folder is the only place it looks.
    if (!env("RECOMP_LAUNCHER_GAMES_DIR").empty()) {
        for (const std::string &c : children(import_base()))
            add(c);
        return roots;
    }
    for (const std::string &p : live_roots(roots_))
        add(p);
    if (!known_exe_.empty()) {
        // The flat developer view keeps the executable at the root; a GOG
        // install keeps it one folder down (bin/, bina1/).
        add(parent_dir(known_exe_));
        add(parent_dir(parent_dir(known_exe_)));
    }
    for (const std::string &c : children(import_base()))
        add(c);
#ifdef __ANDROID__
    // The app reaches no other folder by path: the folders in its files
    // folder are where a copy pushed with adb or over USB lands (adb cannot
    // write into the Games folder the app made), game/ the canvas launcher's
    // import and tools/build.py --push-game's.
    for (const std::string &c : children(android::data_dir()))
        add(c);
    return roots;
#endif
    const std::string home = env("HOME");
    if (home.empty())
        return roots;
    std::vector<std::string> bases = {home, home + "/Games", home + "/GOG Games",
                                      home + "/Games/Heroic"};
    auto prefix = [&](const std::string &drive_c) {
        bases.push_back(drive_c + "/GOG Games");
        bases.push_back(drive_c + "/Program Files (x86)/GOG Galaxy/Games");
        bases.push_back(drive_c + "/Program Files/GOG Galaxy/Games");
    };
    prefix(home + "/.wine/drive_c");
    for (const std::string &g : children(home + "/Games"))
        prefix(g + "/drive_c");
    for (const char *bottles :
         {"/.local/share/bottles/bottles", "/.var/app/com.usebottles.bottles/data/bottles/bottles"})
        for (const std::string &b : children(home + bottles))
            prefix(b + "/drive_c");
    std::vector<std::string> names;
    for (const std::string &n : config_.install_names)
        names.push_back(lower(n));
    for (const std::string &base : bases)
        for (const std::string &child : children(base)) {
            const std::string n = lower(file_name(child));
            if (std::any_of(names.begin(), names.end(),
                            [&](const std::string &x) { return n.find(x) != std::string::npos; }) ||
                has_gog_info(child)) {
                add(child);
                for (const std::string &grandchild : children(child))
                    add(grandchild);
            }
        }
    return roots;
}

void Model::remember(const std::string &root) {
    const std::string c = canonical(root);
    std::vector<std::string> list = live_roots(roots_);
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const std::string &p) { return canonical(p) == c; }),
               list.end());
    list.insert(list.begin(), root);
    if (list.size() > 8)
        list.resize(8);
    roots_ = list;
    save_state();
}

void Model::save_state() const {
    if (state_file_.empty())
        return;
    controls::Json state = controls::Json::object();
    controls::Json roots = controls::Json::array();
    for (const std::string &r : roots_)
        roots.a.push_back(controls::Json::string(r));
    state.set("roots", roots);
    if (!last_game_.empty())
        state.set("lastGame", controls::Json::string(last_game_));
    if (!chosen_language().empty())
        state.set("language", controls::Json::string(chosen_language()));
    mkdirs(parent_dir(state_file_));
    if (!write_text(state_file_, controls::json_write(state) + "\n"))
        fprintf(stderr, "[launcher] could not write %s\n", state_file_.c_str());
}

// MARK: Settings

bool Model::store(const char *key, int value) {
    if (mods_settings_file_set(key, value))
        return true;
    set_notice(t("settingNotSaved"));
    return false;
}

void Model::set_frame_limit(int value) {
    if (store(kFrameLimit, value))
        frame_limit_ = value;
}

void Model::set_energy_saver(int value) {
    if (store(kEnergySaver, value))
        energy_saver_ = value;
}

void Model::set_idle_frames(int value) {
    if (store(kIdleFrames, value))
        idle_frames_ = value;
}

void Model::set_ui_bounds(int value) {
    if (store(kUiBounds, value))
        ui_bounds_ = value;
}

void Model::set_gpu_skin(bool on) {
    if (store(kGpuSkin, on ? 1 : 2))
        gpu_skin_ = on;
}

void Model::set_skip_intro(bool on) {
    const std::string root = overlay_root();
    if (root.empty()) {
        set_notice(t("settingNotSaved"));
        return;
    }
    const std::string empty = std::string("<?xml version=\"1.0\"?>\n") + kIntroMarker +
                              "\n<Base>\n\t<Movies/>\n</Base>\n";
    for (const char *name : kIntroFiles) {
        const std::string path = join(root, name);
        std::string text;
        if (on) {
            mkdirs(parent_dir(path));
            if (!write_text(path, empty)) {
                set_notice(t("settingNotSaved"));
                return;
            }
        } else if (read_text(path, &text) && text.find(kIntroMarker) != std::string::npos &&
                   os_unlink(path.c_str()) != 0) {
            set_notice(t("settingNotSaved"));
            return;
        }
    }
    skip_intro_ = on;
}

void Model::set_language(const std::string &code) {
    choose_language(code);
    save_state();
}

// MARK: Actions

void Model::play() {
    const Entry *e = selected();
    if (!e || !e->can_play())
        return;
    last_game_ = e->spec.id;
    if (!e->root.empty())
        remember(e->root);
    else
        save_state();
    if (on_play)
        on_play(*e);
}

void Model::quit() {
    cancel_->store(true);
    gog::stop_running();
    if (on_quit)
        on_quit();
}

void Model::accept(const std::vector<std::string> &paths) {
    if (importing_ || paths.empty())
        return;
    const std::vector<GameSpec> &specs = config_.games;
    auto holds_game = [&](const std::string &dir) {
        Found f;
        return std::any_of(specs.begin(), specs.end(),
                           [&](const GameSpec &s) { return probe(s, dir, &f); });
    };
    std::vector<std::string> roots, rest;
    for (const std::string &p : paths) {
        const std::string name = file_name(p);
        if (is_dir(p)) {
            const std::vector<std::string> found = roots_below(p, specs);
            if (found.empty())
                rest.push_back(p);
            else
                roots.insert(roots.end(), found.begin(), found.end());
        } else if (ends_with(lower(name), ".exe") && !finder::is_setup_name(name) &&
                   !finder::is_patch_name(name) &&
                   (holds_game(parent_dir(p)) || holds_game(parent_dir(parent_dir(p))))) {
            // The game's own H5_Game.exe: its install, not an installer.
            roots.push_back(holds_game(parent_dir(p)) ? parent_dir(p) : parent_dir(parent_dir(p)));
        } else {
            rest.push_back(p);
        }
    }
    for (const std::string &r : roots)
        remember(r);
    if (!roots.empty())
        refresh();
    const finder::Found found = finder::find(rest);
    std::vector<std::string> messages;
    for (const finder::Problem &p : found.problems)
        messages.push_back(finder::describe(p));
    if (found.setups.empty() && found.patches.empty() && roots.empty() && messages.empty())
        for (const std::string &p : found.empty_folders)
            messages.push_back(is_dir(p) ? t("noInstallerIn", {{"name", file_name(p)}})
                                         : t("notInstaller", {{"name", file_name(p)}}));
    if (!messages.empty()) {
        std::string text;
        for (const std::string &m : messages)
            text += (text.empty() ? "" : "\n\n") + m;
        set_notice(text);
    }
    install_queue_ = found.setups;
    install_queue_.insert(install_queue_.end(), found.patches.begin(), found.patches.end());
    quiet_patches_ = found.setups.empty() ? std::vector<std::string>() : found.patches;
    install_next();
}

void Model::install_next() {
    if (importing_)
        return;
    if (install_queue_.empty()) {
#ifdef __ANDROID__
        // Nothing more to install from what the player picked: its links
        // (and any copy of an installer, gigabytes) go.
        android::forget_picked();
#endif
        return;
    }
    const std::string next = install_queue_.front();
    install_queue_.erase(install_queue_.begin());
    if (finder::is_patch_name(file_name(next)))
        apply_patch(next, std::find(quiet_patches_.begin(), quiet_patches_.end(), next) !=
                              quiet_patches_.end());
    else
        install(next);
}

void Model::apply_patch(const std::string &patch, bool quiet) {
    const std::string base = import_base();
    if (importing_ || base.empty())
        return;
    Progress p;
    p.name = p.current = file_name(patch);
    p.phase = Progress::Patching;
    importing_ = p;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    const Config config = config_;
    const std::string manifest_dir = manifest_dir_;
    std::shared_ptr<std::atomic<bool>> cancel = cancel_;
    run_worker([=](const std::shared_ptr<Shared> &shared) {
        std::vector<gog::PatchResult> results;
        const Outcome o = gog::apply_patch(
            patch, config, manifest_dir, base,
            [&](const Progress &now) {
                post(shared, [now](Model &m) {
                    if (m.importing_)
                        m.importing_ = now;
                });
                return !cancel->load();
            },
            &results);
        post(shared, [o, results, quiet](Model &m) {
            m.importing_.reset();
            if (o.ok) {
                std::string text;
                for (const gog::PatchResult &r : results) {
                    const std::string line = r.replaced > 0 ? t("patchApplied", {{"name", r.title}})
                                             : quiet ? ""
                                                     : t("patchUpToDate", {{"name", r.title}});
                    if (!line.empty())
                        text += (text.empty() ? "" : "\n\n") + line;
                }
                if (!text.empty())
                    m.set_notice(text);
            } else {
                m.install_queue_.clear();
                if (!o.cancelled)
                    m.set_notice(t("installFailed", {{"reason", o.error}}));
            }
            m.refresh();
            m.install_next();
        });
    });
}

void Model::resume_install() {
    const Entry *e = selected();
    if (!e || !e->interrupted())
        return;
    if (exists(e->state.setup))
        install(e->state.setup);
    else
        pick_installer(t("setupGone", {{"path", e->state.setup}}));
}

void Model::install(const std::string &setup) {
    const std::string base = import_base();
    if (importing_ || base.empty())
        return;
    std::vector<std::string> patches;
    for (const std::string &q : install_queue_)
        if (finder::is_patch_name(file_name(q)))
            patches.push_back(q);
    Progress p;
    p.name = p.current = file_name(setup);
    p.phase = Progress::Checking;
    importing_ = p;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    const Config config = config_;
    const std::string manifest_dir = manifest_dir_;
    std::shared_ptr<std::atomic<bool>> cancel = cancel_;
    run_worker([=](const std::shared_ptr<Shared> &shared) {
        gog::Plan plan;
        Outcome o = gog::plan(setup, config, manifest_dir, base, &plan);
        if (o.ok) {
            plan.patches = patches;
            o = gog::run(plan, [&](const Progress &now) {
                post(shared, [now](Model &m) {
                    if (m.importing_)
                        m.importing_ = now;
                });
                return !cancel->load();
            });
        }
        const std::vector<std::string> games = plan.installer.games;
        post(shared, [o, games](Model &m) {
            m.importing_.reset();
            if (o.ok) {
                // The installed game this app plays; another one only when
                // nothing playable is selected (a queue installs several).
                std::string pick;
                for (const std::string &id : games)
                    for (const Entry &e : m.entries_)
                        if (pick.empty() && e.spec.id == id && e.playable_here)
                            pick = id;
                const Entry *sel = m.selected();
                if (pick.empty() && !(sel && sel->can_play()) && !games.empty())
                    pick = games.front();
                if (!pick.empty())
                    m.selected_id_ = pick;
            } else {
                m.install_queue_.clear();
                if (!o.cancelled)
                    m.set_notice(t("installFailed", {{"reason", o.error}}));
            }
            m.refresh();
            m.install_next();
        });
    });
}

void Model::cancel_import() {
    cancel_->store(true);
}

#ifndef __ANDROID__
namespace {

// What a file dialog hands back, posted to the main thread.
struct Picker {
    std::function<void(std::vector<std::string>)> done;
};

void SDLCALL picked(void *userdata, const char *const *files, int) {
    std::unique_ptr<Picker> picker(static_cast<Picker *>(userdata));
    std::vector<std::string> paths;
    if (!files)
        fprintf(stderr, "[launcher] file dialog: %s\n", SDL_GetError());
    for (const char *const *f = files; f && *f; ++f)
        paths.push_back(*f);
    picker->done(std::move(paths));
}

} // namespace
#endif

#ifdef __ANDROID__
// The system's pickers hand back documents, not paths: android_files.cpp
// links the installers by name or copies a game folder in, on a worker.
void Model::pick_folder() {
    std::shared_ptr<Shared> shared = shared_;
    android::pick(true, import_base(), config_.exclude, [shared](android::Materialize work) {
        post(shared, [work](Model &m) { m.import_picked(work); });
    });
}

void Model::pick_installer(const std::string &message) {
    if (!message.empty())
        set_notice(message);
    std::shared_ptr<Shared> shared = shared_;
    android::pick(false, import_base(), config_.exclude, [shared](android::Materialize work) {
        post(shared, [work](Model &m) { m.import_picked(work); });
    });
}
#else
void Model::pick_folder() {
    std::shared_ptr<Shared> shared = shared_;
    auto *picker = new Picker{[shared](std::vector<std::string> paths) {
        if (!paths.empty())
            post(shared, [paths](Model &m) { m.accept(paths); });
    }};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
    const std::string title = t("pickMessage"), accept = t("choose");
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, accept.c_str());
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, picked, picker, props);
    SDL_DestroyProperties(props);
}

// The player's GOG offline installers: setup_*.exe and any of its .bin
// slices, several at once. SDL's dialogs choose files or a folder, not both:
// a folder of installers comes in through the folder dialog (pick_folder).
void Model::pick_installer(const std::string &message) {
    static const SDL_DialogFileFilter filters[] = {
        {"GOG installer (setup_*.exe, *.bin)", "exe;bin"}};
    std::shared_ptr<Shared> shared = shared_;
    auto *picker = new Picker{[shared](std::vector<std::string> paths) {
        if (!paths.empty())
            post(shared, [paths](Model &m) { m.accept(paths); });
    }};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER,
                           const_cast<SDL_DialogFileFilter *>(filters));
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 1);
    SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, true);
    const std::string title = message.empty() ? t("pickInstallerMessage") : message,
                      accept = t("install");
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, accept.c_str());
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, picked, picker, props);
    SDL_DestroyProperties(props);
}
#endif

void Model::import_picked(std::function<Outcome(const ProgressFn &, std::vector<std::string> *)> materialize) {
    if (importing_)
        return;
    Progress p;
    p.phase = Progress::Copying;
    importing_ = p;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> cancel = cancel_;
    run_worker([=](const std::shared_ptr<Shared> &shared) {
        std::vector<std::string> paths;
        const Outcome o = materialize(
            [&](const Progress &now) {
                post(shared, [now](Model &m) {
                    if (m.importing_)
                        m.importing_ = now;
                });
                return !cancel->load();
            },
            &paths);
        post(shared, [o, paths](Model &m) {
            m.importing_.reset();
            if (!o.ok) {
                if (!o.cancelled)
                    m.set_notice(o.error);
                return;
            }
            m.accept(paths);
        });
    });
}

namespace {

void open_folder(const std::string &dir) {
    if (!Model::can_reveal())
        return;
    std::string url = "file://";
    for (unsigned char c : dir) {
        if (isalnum(c) || strchr("/-_.~", c))
            url.push_back(char(c));
        else {
            char hex[4];
            snprintf(hex, sizeof hex, "%%%02X", c);
            url += hex;
        }
    }
    if (!SDL_OpenURL(url.c_str()))
        fprintf(stderr, "[launcher] could not open %s: %s\n", dir.c_str(), SDL_GetError());
}

} // namespace

// The folder the game is in: a file manager opens folders, it does not
// select a file in one as the Finder does.
void Model::reveal_selected() {
    const Entry *e = selected();
    if (!e)
        return;
    if (!e->exe.empty())
        open_folder(parent_dir(e->exe));
    else if (!e->root.empty())
        open_folder(e->root);
}

void Model::reveal_saves() {
    if (profile_dir_.empty())
        return;
    mkdirs(profile_dir_);
    open_folder(profile_dir_);
}

bool Model::can_reveal() {
#ifdef __ANDROID__
    return false;
#else
    return true;
#endif
}

} // namespace launcher::native
