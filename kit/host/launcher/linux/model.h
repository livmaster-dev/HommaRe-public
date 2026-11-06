// model.h - the native launcher's state on Linux (LauncherModel in
// apple/LauncherModel.swift): which of the games launcher-games.json lists
// are on this computer and whether each is the supported build, the folders
// the player picked or installed into, the settings the game reads when it
// starts, and installs from GOG installers. No game files ship with the app:
// the art and the executables are read from the player's own copy.
//
// Its methods run on the main thread. Searches and installs run on threads of
// their own and hand their results back, which drain() applies.
#pragma once
#include "config.h"
#include "detect.h"
#include "installer.h"
#include "skin.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct SDL_Window;

namespace launcher::native {

struct Entry {
    GameSpec spec;
    GameState state;
    std::string root, exe;
    std::shared_ptr<const Image> art;
    // False while the port of this game is not part of the app.
    bool playable_here = false;
    bool can_play() const {
        return playable_here && state.kind == GameState::Ready;
    }
    bool interrupted() const {
        return state.kind == GameState::Interrupted;
    }
};

// Whether the main button installs: the game is not here, or not whole.
bool installs(const Entry &e);

class Model {
  public:
    // `modules` is the app's own game module, then any others it carries,
    // comma-separated. `state_file` keeps the remembered folders, the last
    // game and the language.
    Model(Config config, const std::string &modules, const std::string &known_exe,
          const std::string &profile_dir, const std::string &manifest_dir,
          const std::string &state_file);
    ~Model();

    // Applies what searches, installs and dialogs handed back; true when
    // anything did.
    bool drain();
    // `wake` is called from any thread when something was handed back (the
    // bridge wakes its event loop with it).
    void set_wake(std::function<void()> wake);
    // The window the file dialogs belong to.
    SDL_Window *window = nullptr;

    // Set by the bridge: the player pressed Play or closed the launcher;
    // after each search (the test hooks).
    std::function<void(const Entry &)> on_play;
    std::function<void()> on_quit, on_scanned;

    const Config &config() const {
        return config_;
    }
    const std::vector<Entry> &entries() const {
        return entries_;
    }
    const Entry *selected() const;
    const std::string &selected_id() const {
        return selected_id_;
    }
    void select(const std::string &id);
    // The next or previous game, round the list.
    void step(int by);
    bool scanning() const {
        return scanning_;
    }
    // A search has finished at least once.
    bool has_scanned() const {
        return has_scanned_;
    }
    const std::optional<Progress> &importing() const {
        return importing_;
    }
    // The game's own textures from an install on this computer.
    const std::shared_ptr<const Skin> &skin() const {
        return skin_;
    }
    std::string title() const;
    const std::string &profile_dir() const {
        return profile_dir_;
    }
    // Something to tell the player (an alert); also logged.
    const std::string &notice() const {
        return notice_;
    }
    void set_notice(const std::string &text);
    void clear_notice() {
        notice_.clear();
    }
    // Folders the player picked or installed into, newest first.
    std::vector<std::string> remembered_roots() const;
    // Where installs go: RECOMP_LAUNCHER_GAMES_DIR, else <user data>/Games.
    static std::string import_base();
    // The remembered folders that still exist, once each.
    static std::vector<std::string> live_roots(const std::vector<std::string> &paths);

    // The settings (mods/settings_file.h), read by the game when it starts.
    int frame_limit() const {
        return frame_limit_;
    }
    int energy_saver() const {
        return energy_saver_;
    }
    int idle_frames() const {
        return idle_frames_;
    }
    int ui_bounds() const {
        return ui_bounds_;
    }
    bool gpu_skin() const {
        return gpu_skin_;
    }
    bool skip_intro() const {
        return skip_intro_;
    }
    void set_frame_limit(int value);
    void set_energy_saver(int value);
    void set_idle_frames(int value);
    void set_ui_bounds(int value);
    void set_gpu_skin(bool on);
    void set_skip_intro(bool on);
    // The choice in Settings: a language code, or "" for the system's.
    void set_language(const std::string &code);

    void refresh();
    void play();
    void quit();
    // Files or folders the player picked or dropped: a folder (or a game's
    // executable) inside an installed game is played where it is; the GOG
    // installers found in the rest are installed one after another.
    void accept(const std::vector<std::string> &paths);
    void install(const std::string &setup);
    void apply_patch(const std::string &patch, bool quiet = false);
    // Picks up an install the selected game's folder says is unfinished.
    void resume_install();
    void cancel_import();
    void pick_folder();
    void pick_installer(const std::string &message = "");
    void reveal_selected();
    void reveal_saves();
    // Whether this platform has a file manager to show a folder in (not on
    // Android, where the game and the saves are in the app's own storage).
    static bool can_reveal();

  private:
    struct Shared;
    using Task = std::function<void(Model &)>;
    static void post(const std::shared_ptr<Shared> &shared, Task task);
    void apply(const std::map<std::string, Found> &found);
    // Android: what the system's picker handed back, made into paths on a
    // worker (a copy shows as one), then accepted (android_files.h).
    void import_picked(std::function<Outcome(const ProgressFn &, std::vector<std::string> *)> materialize);
    std::vector<std::string> candidate_roots() const;
    void remember(const std::string &root);
    void install_next();
    // Runs `work` on a thread of its own; it hands results back with post().
    void run_worker(std::function<void(const std::shared_ptr<Shared> &)> work);
    bool store(const char *key, int value);
    void save_state() const;

    Config config_;
    std::string this_module_;
    std::vector<std::string> modules_;
    std::string known_exe_, profile_dir_, manifest_dir_, state_file_;
    std::vector<Entry> entries_;
    std::string selected_id_;
    bool scanning_ = false, has_scanned_ = false;
    std::optional<Progress> importing_;
    std::shared_ptr<const Skin> skin_;
    std::string notice_;
    int frame_limit_ = 2, energy_saver_ = 0, idle_frames_ = 0, ui_bounds_ = 0;
    bool gpu_skin_ = true, skip_intro_ = false;

    // launcher.json: the remembered folders and the last game.
    std::vector<std::string> roots_;
    std::string last_game_;
    // GOG installers waiting for the one being installed; updates queued with
    // installers say nothing when there is nothing to do.
    std::vector<std::string> install_queue_, quiet_patches_;

    std::shared_ptr<Shared> shared_;
    // The install under way stops when this is set.
    std::shared_ptr<std::atomic<bool>> cancel_;
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<Worker> workers_;
};

} // namespace launcher::native
