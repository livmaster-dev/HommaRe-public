// bridge.cpp - the native launcher's C entry points on Linux
// (native_launcher.h), as apple/LauncherBridge.swift is on Apple platforms: a
// session that owns the model and its screens, draws them on a canvas the
// canvas launcher's presenter shows in the host's window, and reads that
// window's events itself, a frame per recomp_native_launcher_poll.
#include "bridge.h"

#include "../../../runtime/layout.h"
#include "../../../third_party/miniz/miniz.h"
#include "../launcher_sdl.h"
#include "../native_launcher.h"
#include "l10n.h"
#include "model.h"
#include "text.h"
#include "view.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace launcher::native {

namespace {

gpu::Device *g_device = nullptr;
void *g_surface = nullptr;

std::string env(const char *name) {
    const char *v = getenv(name);
    return v ? v : "";
}

double seconds() {
    return double(SDL_GetTicksNS()) / 1e9;
}

struct Session {
    SDL_Window *window = nullptr;
    std::unique_ptr<Model> model;
    std::unique_ptr<View> view;
    Presenter presenter;
    bool presenting = false, unpresentable = false;
    Canvas canvas;
    bool dirty = true;
    int result = 0;
    std::string exe, root, game, module, saved_title;
    uint32_t wake_event = 0;
    std::vector<std::string> dropped;
    // The finger that is pressing (0: none), where it began and how far its
    // scroll has gone, in pixels.
    SDL_FingerID finger = 0;
    float finger_start = 0, finger_y = 0;
    bool scrolled = false;

    // Unattended runs (recomp_native_launcher_begin).
    std::string dump, dump_dir;
    bool autoplay = false, dumped = false, played = false, installed = false;
    double play_at = 0, cancel_at = 0, cancel_after = 0, dump_every = 5, next_dump = 0;
    // Frames are drawn whether or not anything changed until then.
    double redraw_until = 0;
    int dumps = 0;
    std::string install;

    // The window's size in points and its pixels per point.
    void sizes(int *w, int *h, int *pw, int *ph) const {
        SDL_GetWindowSize(window, w, h);
        SDL_GetWindowSizeInPixels(window, pw, ph);
    }
    float density() const {
        int w, h, pw, ph;
        sizes(&w, &h, &pw, &ph);
        return w > 0 ? float(pw) / float(w) : 1;
    }

    void draw() {
        int w, h, pw, ph;
        sizes(&w, &h, &pw, &ph);
        if (pw <= 0 || ph <= 0)
            return;
        if (!presenting) {
            if (unpresentable)
                return;
            presenter.device = g_device;
            presenter.window = window;
            if (!presenter.open(g_surface, pw, ph)) {
                fprintf(stderr, "[launcher] no swapchain for the window: %s\n", SDL_GetError());
                // SDL's window surface, as the canvas launcher has without a GPU.
                presenter.device = nullptr;
                if (!g_device || !presenter.open(nullptr, pw, ph)) {
                    fprintf(stderr, "[launcher] the window cannot be drawn in: %s\n",
                            SDL_GetError());
                    unpresentable = true;
                    return;
                }
            }
            presenting = true;
        }
        presenter.resize(pw, ph);
        canvas.resize(pw, ph, presenter.format == gpu::Format::BGRA8);
        view->draw(canvas, w > 0 ? float(pw) / float(w) : 1);
        presenter.show(canvas);
        dirty = false;
    }

    // The launcher as it looks now, drawn offscreen: at the window's size,
    // or RECOMP_LAUNCHER_DUMP_SIZE=<w>x<h> points; RECOMP_LAUNCHER_DUMP_SCALE
    // pixels a point (the window's by default).
    void snapshot(const std::string &path) {
        int w, h, pw, ph;
        sizes(&w, &h, &pw, &ph);
        float density = w > 0 ? float(pw) / float(w) : 1;
        if (const std::string s = env("RECOMP_LAUNCHER_DUMP_SCALE"); !s.empty())
            density = std::max(0.25f, float(atof(s.c_str())));
        if (const std::string s = env("RECOMP_LAUNCHER_DUMP_SIZE"); !s.empty())
            sscanf(s.c_str(), "%dx%d", &w, &h);
        Canvas snap;
        snap.resize(int(float(w) * density), int(float(h) * density), false);
        view->draw(snap, density);
        size_t size = 0;
        void *png = tdefl_write_image_to_png_file_in_memory_ex(
            snap.pixels(), snap.width(), snap.height(), 4, &size, MZ_DEFAULT_LEVEL, 0);
        FILE *f = png ? fopen((path + ".part").c_str(), "wb") : nullptr;
        if (f) {
            fwrite(png, 1, size, f);
            fclose(f);
            rename((path + ".part").c_str(), path.c_str());
            fprintf(stderr, "[launcher] wrote %s (%dx%d)\n", path.c_str(), snap.width(),
                    snap.height());
        } else {
            fprintf(stderr, "[launcher] could not write %s\n", path.c_str());
        }
        mz_free(png);
        // The window's next frame lays out its own regions again.
        dirty = true;
    }

    // After each search: the test hooks' first picture, install and play.
    void scanned() {
        if (!dump.empty() && !dumped) {
            dumped = true;
            snapshot(dump);
        }
        if (!install.empty() && !installed) {
            installed = true;
            std::vector<std::string> paths;
            for (size_t start = 0; start <= install.size();) {
                const size_t colon = std::min(install.find(':', start), install.size());
                if (colon > start)
                    paths.push_back(install.substr(start, colon - start));
                start = colon + 1;
            }
            model->accept(paths);
            if (cancel_after > 0)
                cancel_at = seconds() + cancel_after;
            return;
        }
        const Entry *e = model->selected();
        if (autoplay && !played && !model->importing() && e && e->can_play()) {
            played = true;
            play_at = seconds() + 1;
        }
    }

    void event(const SDL_Event &e) {
        const float d = density();
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            model->quit();
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_SHOWN:
            dirty = true;
            break;
        // Back from the background or from under the system's picker: the
        // window has a new surface (Android drops it meanwhile), which the
        // swapchain takes over once it is there; draw for a moment until then.
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            redraw_until = seconds() + 2;
            dirty = true;
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            dirty |= view->leave();
            break;
        case SDL_EVENT_MOUSE_MOTION:
            dirty |= view->pointer(e.motion.x * d, e.motion.y * d);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_LEFT)
                dirty |= view->press(e.button.x * d, e.button.y * d);
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT)
                dirty |= view->release(e.button.x * d, e.button.y * d);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            dirty |= view->wheel(-e.wheel.y);
            break;
        // A finger is a pointer that presses where it lands and drags to
        // scroll: the Android host turns SDL's own touch-to-mouse events off
        // (host/sdl/platform_ui_desktop.cpp), so a tap reaches here as one.
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP: {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            const float x = e.tfinger.x * float(pw), y = e.tfinger.y * float(ph);
            if (e.type == SDL_EVENT_FINGER_DOWN) {
                if (finger != 0)
                    break;
                finger = e.tfinger.fingerID;
                finger_y = finger_start = y;
                scrolled = false;
                dirty |= view->pointer(x, y);
                dirty |= view->press(x, y);
            } else if (e.tfinger.fingerID != finger) {
                break;
            } else if (e.type == SDL_EVENT_FINGER_MOTION) {
                // Past a few points of travel the finger scrolls the page
                // and the press it began with no longer clicks.
                const float step = 40.0f * d; // a wheel line (View::wheel)
                if (!scrolled && std::abs(y - finger_start) > step / 2)
                    scrolled = true;
                while (scrolled && std::abs(y - finger_y) >= step) {
                    const int dir = y < finger_y ? 1 : -1;
                    dirty |= view->wheel(float(dir));
                    finger_y -= float(dir) * step;
                }
                dirty |= view->pointer(x, y);
            } else {
                finger = 0;
                dirty |= view->release(scrolled ? -1e6f : x, scrolled ? -1e6f : y);
                dirty |= view->leave();
            }
            break;
        }
        case SDL_EVENT_KEY_DOWN:
            // Android's Back is the launcher's Escape.
            dirty |= view->key(uint32_t(e.key.key == SDLK_AC_BACK ? SDLK_ESCAPE : e.key.key),
                               uint16_t(e.key.mod));
            break;
        // Installers, their folders or a game folder dropped from a file manager.
        case SDL_EVENT_DROP_BEGIN:
        case SDL_EVENT_DROP_POSITION:
            dirty |= view->dragging(true);
            break;
        case SDL_EVENT_DROP_FILE:
            if (e.drop.data)
                dropped.push_back(e.drop.data);
            break;
        case SDL_EVENT_DROP_COMPLETE:
            dirty |= view->dragging(false);
            if (!dropped.empty() && !model->importing())
                model->accept(dropped);
            dropped.clear();
            dirty = true;
            break;
        default:
            break;
        }
    }

    // One turn: the events that came (waiting a little for them), what the
    // work handed back, the hooks' timers, and a frame when anything changed.
    void frame() {
        SDL_Event e;
        const bool busy = model->importing() || model->scanning() || play_at > 0 || cancel_at > 0 ||
                          !dump_dir.empty() || redraw_until > 0;
        if (SDL_WaitEventTimeout(&e, busy ? 50 : 250)) {
            do {
                event(e);
            } while (result == 0 && SDL_PollEvent(&e));
        }
        if (result)
            return;
        dirty |= model->drain();
        const double now = seconds();
        if (redraw_until > 0) {
            dirty = true;
            if (now >= redraw_until)
                redraw_until = 0;
        }
        if (play_at > 0 && now >= play_at) {
            play_at = 0;
            model->play();
        }
        if (cancel_at > 0 && now >= cancel_at) {
            cancel_at = 0;
            model->cancel_import();
        }
        if (!dump_dir.empty() && now >= next_dump) {
            next_dump = now + dump_every;
            char name[32];
            snprintf(name, sizeof name, "/launcher-%03d.png", ++dumps);
            snapshot(dump_dir + name);
        }
        if (result == 0 && dirty)
            draw();
    }
};

std::unique_ptr<Session> g_session;

} // namespace

void present_with(gpu::Device *device, void *native_surface) {
    g_device = device;
    g_surface = native_surface;
}

} // namespace launcher::native

using launcher::native::g_session;
using launcher::native::Session;

extern "C" int recomp_native_launcher_begin(void *native_window, const char *config_path,
                                            const char *modules, const char *known_exe,
                                            const char *profile_dir) {
    using namespace launcher::native;
    std::string text, error;
    Config config;
    if (!native_window || !read_text(config_path ? config_path : "", &text) ||
        !parse_config(text, &config, &error)) {
        fprintf(stderr, "[launcher] %s: %s\n", config_path ? config_path : "",
                error.empty() ? "unreadable" : error.c_str());
        return 0;
    }
    auto s = std::make_unique<Session>();
    s->window = static_cast<SDL_Window *>(native_window);
    // Fonts in the app's resources/fonts first, as the Mac's Resources/Fonts.
    set_font_dir(host_resource("fonts"));
    const std::string config_dir = parent_dir(config_path);
    const std::string state =
        host_layout().profile_dir.empty() ? "" : host_layout().profile_dir + "/launcher.json";
    s->model = std::make_unique<Model>(std::move(config), modules ? modules : "",
                                       known_exe ? known_exe : "", profile_dir ? profile_dir : "",
                                       config_dir + "/launcher-manifests", state);
    s->view = std::make_unique<View>(*s->model);
    Session *session = s.get();
    Model &model = *s->model;
    model.window = s->window;
    s->wake_event = SDL_RegisterEvents(1);
    const uint32_t wake_event = s->wake_event;
    model.set_wake([wake_event] {
        SDL_Event e{};
        e.type = wake_event;
        SDL_PushEvent(&e);
    });
    model.on_play = [session](const Entry &e) {
        session->exe = e.exe;
        session->root = e.root;
        session->game = e.spec.id;
        session->module = e.spec.module;
        session->result = 1;
    };
    model.on_quit = [session] { session->result = -1; };
    model.on_scanned = [session] { session->scanned(); };
    // Unattended runs: RECOMP_LAUNCHER_DUMP writes a picture of the launcher
    // once the search is done, RECOMP_LAUNCHER_AUTOPLAY=1 then presses Play
    // when the selected game is ready. RECOMP_LAUNCHER_INSTALL=<path>[:<path>...]
    // installs what those name (GOG installers, their folders, GOG updates)
    // after the first search (and RECOMP_LAUNCHER_INSTALL_CANCEL=<seconds>
    // cancels it that long after it began); autoplay then waits for the
    // installed game. RECOMP_LAUNCHER_DUMP_DIR=<dir> writes a picture every
    // RECOMP_LAUNCHER_DUMP_EVERY seconds (default 5) while the launcher is up.
    // RECOMP_LAUNCHER_GAMES_DIR=<dir> stands in for the launcher's Games
    // folder and is then the only place it looks (model.cpp).
    s->dump = env("RECOMP_LAUNCHER_DUMP");
    s->autoplay =
        !env("RECOMP_LAUNCHER_AUTOPLAY").empty() && env("RECOMP_LAUNCHER_AUTOPLAY") != "0";
    s->install = env("RECOMP_LAUNCHER_INSTALL");
    s->cancel_after = atof(env("RECOMP_LAUNCHER_INSTALL_CANCEL").c_str());
    s->dump_dir = env("RECOMP_LAUNCHER_DUMP_DIR");
    if (!s->dump_dir.empty()) {
        for (size_t i = s->dump_dir.find('/', 1); i != std::string::npos;
             i = s->dump_dir.find('/', i + 1))
            SDL_CreateDirectory(s->dump_dir.substr(0, i).c_str());
        SDL_CreateDirectory(s->dump_dir.c_str());
        s->dump_every = std::max(1.0, atof(env("RECOMP_LAUNCHER_DUMP_EVERY").c_str()) > 0
                                          ? atof(env("RECOMP_LAUNCHER_DUMP_EVERY").c_str())
                                          : 5.0);
        s->next_dump = seconds() + s->dump_every;
    }
    const char *title = SDL_GetWindowTitle(s->window);
    s->saved_title = title ? title : "";
    SDL_SetWindowTitle(s->window, model.title().c_str());
    for (Face f : {Face::Menu, Face::Label, Face::Body})
        fprintf(stderr, "[launcher] face %d: %s\n", int(f),
                face_file(f).empty() ? "Open Sans (the kit's)" : face_file(f).c_str());
    model.refresh();
    g_session = std::move(s);
    return 1;
}

extern "C" int recomp_native_launcher_poll(void) {
    if (!g_session)
        return -1;
    if (g_session->result == 0)
        g_session->frame();
    return g_session->result;
}

extern "C" const char *recomp_native_launcher_exe(void) {
    return g_session ? g_session->exe.c_str() : "";
}

extern "C" const char *recomp_native_launcher_root(void) {
    return g_session ? g_session->root.c_str() : "";
}

extern "C" const char *recomp_native_launcher_game(void) {
    return g_session ? g_session->game.c_str() : "";
}

extern "C" const char *recomp_native_launcher_module(void) {
    return g_session ? g_session->module.c_str() : "";
}

extern "C" void recomp_native_launcher_end(void) {
    if (!g_session)
        return;
    // The host makes its own swapchain on the window next: this one goes first.
    if (g_session->presenting)
        g_session->presenter.close();
    SDL_SetWindowTitle(g_session->window, g_session->saved_title.c_str());
    g_session.reset();
}
