// launcher_sdl.h - the launcher screen in the app's SDL window, and each
// target's platform (pickers, storage). launcher_sdl.cpp is shared;
// launcher_platform_desktop.cpp is macOS, Windows and Linux,
// launcher_platform_android.cpp is Android, and host/sdl/platform_ui_ios.mm
// holds the iPadOS one.
#pragma once
#include "../gpu/gpu.h"
#include "launcher_ui.h"

#include <SDL3/SDL.h>

#include <memory>
#include <string>

namespace launcher {

struct RunOptions {
    std::string known;    // a folder or executable already resolved ("" if none)
    double auto_play = 0; // seconds a ready game waits before starting (mobile)
    std::string
        dump_path; // RECOMP_LAUNCHER_DUMP: write each drawn frame here (PPM; relative to $HOME)
    // RECOMP_LAUNCHER_KEYS: scripted input for smoke runs, one step every
    // 300 ms: up, down, left, right, next, previous, enter, back, drop:<path>,
    // wait (a step that does nothing).
    std::string keys;
    // Why the game cannot start here (no GPU device); "" when it can.
    std::string unplayable;
};

// Shows the launcher until the player plays or quits. With no `device`
// (a GPU the host cannot use), it draws through SDL's window surface. Returns the game's
// executable, or "" when the player quit. The window keeps its surface; the
// launcher's swapchain is gone when this returns.
std::string run(SDL_Window *window, gpu::Device *device, void *native_surface, Platform &platform,
                const RunOptions &options);

// This target's platform.
std::unique_ptr<Platform> make_platform(SDL_Window *window);

// Puts a canvas in the window: through a swapchain on `device`, or through
// SDL's window surface without one. The native launcher on Linux
// (linux/bridge.cpp) shows its frames with it too.
struct Presenter {
    gpu::Device *device = nullptr;
    gpu::Swapchain chain;
    gpu::Texture texture;
    gpu::Format format = gpu::Format::BGRA8;
    int w = 0, h = 0, tex_w = 0, tex_h = 0;
    SDL_Window *window = nullptr; // software presentation when there is no device

    bool open(void *surface, int width, int height);
    void resize(int width, int height);
    void show(const Canvas &c);
    void close();
};

// Whether the player asked for the launcher at start: --launcher, the
// RECOMP_LAUNCHER switch, or Shift or Alt held.
bool requested(int argc, char **argv);

#ifdef __ANDROID__
// The system's pickers and documents for the native launcher on Android
// (linux/android_files.cpp), through RecompActivity as the canvas
// launcher's platform has them (launcher_platform_android.cpp). `done` runs
// on the activity's thread.
// Any files, several at once (GOG installers: setup_*.exe and its slices).
void android_pick_documents(PickDone done);
// A folder: Picked.uri is its tree.
void android_pick_tree(PickDone done);
// A document's contents as a descriptor the caller owns, or -1.
int android_open_document(const std::string &uri);
// Everything under a picked folder, with each entry's document URI
// (documents[entry.index]); false with *error set when it cannot be read.
bool android_list_tree(const std::string &tree, std::vector<Entry> *entries,
                       std::vector<std::string> *documents, std::string *error);
#endif

} // namespace launcher
