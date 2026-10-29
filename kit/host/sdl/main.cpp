// main.cpp - the windowed host for the recompiled game, on SDL3.
//
// The guest owns the main thread. That is not a shortcut: the game never
// blocks in GetMessage, it drains its queue with PeekMessageA and spins on
// GetTickCount, so there is no point at which an event loop could take over.
// Instead the host rides on the guest's own clock reads, exactly as the
// headless host does - host/boot.cpp calls back about once a
// millisecond - and that callback is where SDL gets its turn:
//
//     boot_run() -> run_entry() -> ... guest frame loop ...
//                       -> GetTickCount -> boot tick -> pump() -> SDL events
//
// pump() drains the SDL event queue, translates each event into both of the
// input paths the game reads (the DirectInput device state in input.cpp and
// the Win32 message queue). Sealed frames are presented by a dedicated worker.
//
// Nothing here draws the game itself. present_thread.cpp owns the swapchain
// and d3d_render.cpp owns the Direct3D scene; this file owns the window, the
// events and the lifetime.
#include "../../mods/display_settings.h"
#include "game_config.h"
#include "../../runtime/layout.h"
#include "../../platform/os.h"
#include "../audio.h"
#include "../audio/sink.h"
#include "../audio_capture.h"
#include "../boot.h"
#include "../../runtime/display_seam.h"
#include "../d3d_render.h"
#include "../game_modules.h"
#include "../game_path.h"
#include "../launcher/launcher_sdl.h"
#include "../launcher/native_launcher.h"
#include "../gpu/gpu_factory.h"
#include "../input.h"
#include "../input_gate.h"
#include "../text_overrides.h"
#include "../page_overlay.h"
#include "../server_prompt.h"
#include "../ui_mods.h"
#include "../input_touch.h"
#include "../input_trackpad.h"
#include "../controls/controls_host.h"
#include "../../mods/controls_settings.h"
#include "../controls/gamepad_sdl.h"
#include "../../mods/mods_internal.h"
#include "../../mods/settings_file.h"
#include "../midi.h"
#include "../present.h"
#include "../../runtime/display_seam.h"
#include "../window_presentation.h"
#include "keymap.h"
#include "platform_ui.h"
#include "version.h"
#include "../../dx/dx.h"
#include "../../dx/host_d9.h"
#include "../../runtime/loader.h"
#include "../../runtime/mods_seam.h"
#include "../../runtime/native_seam.h"
#include "../../runtime/win32.h"

#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include "trackpad_mac.h"
#endif
#endif
#if defined(RECOMP_GAME_MODULE)
// A game module is entered through recomp_game_main by the app that loaded
// it; that app owns the process entry point and SDL's UIKit/AppKit setup.
#define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL_main.h> // on iOS this supplies the UIKit entry point and renames main
#ifdef __EMSCRIPTEN__
#include "../gpu/d3d9_backend.h"
#include <emscripten/emscripten.h>
#include <emscripten/wasmfs.h>
#include <thread>
#endif
#include <SDL3/SDL_vulkan.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Win32 messages this host delivers.
enum {
    WM_PAINT_ = 0x000f,
    WM_CLOSE_ = 0x0010,
    WM_ACTIVATE_ = 0x0006,
    WM_SETFOCUS_ = 0x0007,
    WM_KILLFOCUS_ = 0x0008,
    WM_ACTIVATEAPP_ = 0x001c,
    WM_KEYDOWN_ = 0x0100,
    WM_KEYUP_ = 0x0101,
    WM_CHAR_ = 0x0102,
    WM_SYSKEYDOWN_ = 0x0104,
    WM_SYSKEYUP_ = 0x0105,
    WM_MOUSEMOVE_ = 0x0200,
    WM_LBUTTONDOWN_ = 0x0201,
    WM_LBUTTONUP_ = 0x0202,
    WM_RBUTTONDOWN_ = 0x0204,
    WM_RBUTTONUP_ = 0x0205,
    WM_MBUTTONDOWN_ = 0x0207,
    WM_MBUTTONUP_ = 0x0208,
    WM_MOUSEWHEEL_ = 0x020a,
};

namespace {

SDL_Window *g_window = nullptr;
void *g_surface = nullptr; // the native surface the presenter draws into
std::unique_ptr<gpu::Device> g_gpu;
bool g_close_requested = false; // the user asked to close
bool g_guest_activated = false; // WM_ACTIVATEAPP(1) has been delivered
bool g_focused = false;
int g_mode_w = 640, g_mode_h = 480;
bool g_mode_dirty = false;
int g_window_mode = 0, g_wanted_window_mode = 0;
bool g_fullscreen_transition = false;
HostRect g_pointer_confinement; // window points; main thread only
bool g_pointer_sample_valid = false;
int32_t g_pointer_sample_x = 0, g_pointer_sample_y = 0;
int g_pointer_sample_w = 0, g_pointer_sample_h = 0;
bool g_borderless_frame_dirty = true;
int g_windowed_x = 0, g_windowed_y = 0, g_windowed_w = 0, g_windowed_h = 0;
uint32_t g_pending_mode_w = 0, g_pending_mode_h = 0;
uint8_t g_buttons = 0; // for the MK_ bits in a mouse message

uint32_t make_lparam(int32_t x, int32_t y) {
    return ((uint32_t)(y & 0xffff) << 16) | (uint32_t)(x & 0xffff);
}
// The wParam every mouse message carries: which buttons are down and whether
// Shift or Control is held. A game that reads it and finds Shift never set
// cannot tell a shift-click from a click.
int32_t g_cursor_x = 0, g_cursor_y = 0;

// The modifiers AS THE GUEST KNOWS THEM: a consumed Shift is not in here,
// so it cannot appear in a mouse message the guest does see.
uint32_t mouse_wparam() {
    return host_mouse_wparam(g_buttons, host_guest_modifiers());
}
void post(uint32_t msg, uint32_t wparam, uint32_t lparam) {
    // Mouse input is addressed to a point, not to a window: the runtime
    // hit-tests it, activates what it lands on, and converts the position into
    // that window's client coordinates - which is what Windows does and what
    // any windowed UI reads. Posting it to the main window instead delivered
    // every click to whichever window happened to be created first, carrying a
    // screen position the recipient read as its own client one.
    if (msg >= 0x0100 && msg <= 0x0109) {
        host_post_key_message(msg, wparam, lparam);
        return;
    }
    if (msg >= 0x200 && msg <= 0x209) {
        host_post_client_mouse_message(host_main_window(), msg, wparam, int16_t(lparam),
                                       int16_t(lparam >> 16));
        return;
    }
    uint32_t hwnd = host_main_window();
    if (hwnd)
        host_post_message(hwnd, msg, wparam, lparam);
}

uint32_t current_modifier_flags() {
    return host_modifier_flags_from_sdl(SDL_GetModState());
}

// The window for a guest mode on the screen this window is on or opening on,
// so a 640x480 mode is not a postage stamp on a 5K display and a 1920x1080
// mode does not open a window larger than a laptop's screen.
HostWindowSize window_size_for(int gw, int gh) {
    SDL_Rect visible = {0, 0, 0, 0};
    SDL_DisplayID display = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
    if (!SDL_GetDisplayUsableBounds(display, &visible))
        visible.w = visible.h = 0;
    const float density =
        g_window ? SDL_GetWindowPixelDensity(g_window) : SDL_GetDisplayContentScale(display);
    return host_window_size_for(gw, gh, visible.w, visible.h, density > 0 ? density : 1.0);
}

// Publish display pixels, not SDL window coordinates (points on Retina).
// Use the desktop mode even while fullscreen: a notch-safe window or a lower
// selected game mode must not hide the display's own resolution from menus.
// SDL video queries stay on the main thread; guest threads read only this cache.
std::atomic<int> g_screen_w{0}, g_screen_h{0};
// The display less the strip a full-screen window leaves clear at the top (a
// MacBook's notch): what a full-screen game actually gets. Equal to the
// display where there is no such strip.
std::atomic<int> g_fullscreen_w{0}, g_fullscreen_h{0};
// The window's safe area for the game (host_display_safe_insets). Sixteen
// bytes are no lock-free atomic on x86-64 without cmpxchg16b, where
// std::atomic would call libatomic, which a desktop may not have installed;
// it changes with the window only, so a mutex costs nothing.
struct SafeInsets {
    float l = 0, t = 0, r = 0, b = 0;
};
SafeInsets g_safe_insets;
std::mutex g_safe_insets_m;
SafeInsets safe_insets() {
    std::lock_guard<std::mutex> held(g_safe_insets_m);
    return g_safe_insets;
}
// The window's form, phone-sized or not (host_display_phone), the game's
// picture in points (host_display_picture_points) and the count of display
// changes (host_display_generation); kept on the main thread.
std::atomic<bool> g_phone_form{false};
struct PicturePoints {
    float w = 0, h = 0;
};
std::atomic<PicturePoints> g_picture_points{};
std::atomic<int> g_display_generation{0};
// Whether the app is in the foreground, and when it last came back (ns): a
// screen lock or app switch makes the system hand the scene transient sizes
// that are not the screen the player is looking at (follow_screen).
std::atomic<bool> g_app_active{true};
std::atomic<uint64_t> g_foreground_ns{0};
extern "C" __attribute__((weak)) double host_mac_safe_top_points(void);
void note_screen_size() {
    int w = 0, h = 0;
    SDL_DisplayID display = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
    // A touch device's window is the screen it is shown on, as large as the
    // system makes its scene now (a foldable's closed or opened screen):
    // the display mode SDL keeps from the UIScreen it started on is neither.
    if (g_window && platform_ui_touch_device())
        SDL_GetWindowSizeInPixels(g_window, &w, &h);
    if (w <= 0 || h <= 0) {
        if (const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(display)) {
            const double density = mode->pixel_density > 0 ? mode->pixel_density : 1.0;
            w = int(std::lround(mode->w * density));
            h = int(std::lround(mode->h * density));
        }
    }
    if ((w <= 0 || h <= 0) && g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN)) {
        SDL_GetWindowSizeInPixels(g_window, &w, &h);
    }
    if (w <= 0 || h <= 0) {
        SDL_Rect bounds = {0, 0, 0, 0};
        if (SDL_GetDisplayBounds(display, &bounds)) {
            const double density = g_window ? SDL_GetWindowPixelDensity(g_window) : 1.0;
            w = int(std::lround(bounds.w * density));
            h = int(std::lround(bounds.h * density));
        }
    }
    if (w <= 0 || h <= 0)
        return;
    int fh = h;
    if (host_mac_safe_top_points) {
        SDL_DisplayID d = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
        const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(d);
        const double density = m && m->pixel_density > 0 ? m->pixel_density : 1.0;
        const int strip = int(std::lround(host_mac_safe_top_points() * density));
        if (strip > 0 && strip < h)
            fh = h - strip;
    }
    if (w != g_screen_w.load() || h != g_screen_h.load() || fh != g_fullscreen_h.load()) {
        fprintf(stderr, "[host] display pixel size: %dx%d (full screen %dx%d)\n", w, h, w, fh);
        g_display_generation.fetch_add(1);
    }
    g_screen_w.store(w);
    g_screen_h.store(h);
    g_fullscreen_w.store(w);
    g_fullscreen_h.store(fh);
}
extern "C" int host_display_fullscreen_size(int *w, int *h) {
    const int fw = g_fullscreen_w.load(), fh = g_fullscreen_h.load();
    if (!w || !h || fw <= 0 || fh <= 0)
        return 0;
    *w = fw;
    *h = fh;
    return 1;
}

// The window's size in points and in drawable pixels.
void window_sizes(int *bw, int *bh, int *dw, int *dh) {
    *bw = *bh = *dw = *dh = 0;
    if (!g_window)
        return;
    SDL_GetWindowSize(g_window, bw, bh);
    SDL_GetWindowSizeInPixels(g_window, dw, dh);
}

// The presenter's game rectangle for a drawable of dw x dh (present.h): in
// landscape always the whole drawable, as it has always been; in portrait the
// rectangle the presenter composes the game into, when it fits this drawable.
HostGameRect game_rect_for(int dw, int dh) {
    const HostGameRect whole = {0, 0, dw, dh};
    if (dh <= dw)
        return whole;
    const HostGameRect r = host_present_current_game_rect();
    if (r.w <= 0 || r.h <= 0 || r.x < 0 || r.y < 0 || r.x + r.w > dw || r.y + r.h > dh)
        return whole;
    return r;
}

// Decode window points into game-rectangle pixels only: drawable pixels, less
// the rectangle's origin, with the rectangle's size as the extent (the whole
// drawable in landscape). Layout selection, capture, drag ownership and guest
// motion are applied later under the guest baton.
void view_point_to_drawable(double px, double py, int32_t *out_x, int32_t *out_y, int *width,
                            int *height) {
    *out_x = 0;
    *out_y = 0;
    *width = 0;
    *height = 0;
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    if (bw <= 0 || bh <= 0)
        return;
    const HostGameRect rect = game_rect_for(dw, dh);
    int32_t x, y;
    if (!g_pointer_confinement.empty()) {
        x = host_confined_pointer_pixel(px, g_pointer_confinement.x, g_pointer_confinement.w, dw);
        y = host_confined_pointer_pixel(py, g_pointer_confinement.y, g_pointer_confinement.h, dh);
    } else {
        x = (int32_t)floor(px / bw * dw);
        y = (int32_t)floor(py / bh * dh);
    }
    host_present_point_to_game(rect, x, y, out_x, out_y);
    *width = rect.w;
    *height = rect.h;
}

// The game rectangle in window points, for the paths that work in points.
// `whole` says it is the entire window (landscape).
struct PointRect {
    double x, y, w, h;
    bool whole;
};
PointRect game_rect_points() {
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    const HostGameRect r = game_rect_for(dw, dh);
    if (bw <= 0 || dw <= 0 || (r.x == 0 && r.y == 0 && r.w == dw && r.h == dh))
        return {0, 0, double(bw), double(bh), true};
    const double scale = double(dw) / bw;
    return {r.x / scale, r.y / scale, r.w / scale, r.h / scale, false};
}

// ---------------------------------------------------------------------------
// Input is DECODED when the event arrives and APPLIED when the baton is ours.
//
// The runtime services this host from inside a scheduler idle slice, with its
// lock down and another guest thread free to hold the baton and run guest code
// for the whole slice. Applying input there would deliver to the guest, and
// dispatch mod callbacks, concurrently with that thread's hooks - callbacks
// that capture a view of guest memory and touch the settings, heap and event
// registries. So an event that arrives while parked is decoded into the queue
// below and applied from pump(), which runs on a thread that holds the baton.
// ---------------------------------------------------------------------------
struct PendingInput {
    enum Kind {
        MOTION,
        BUTTON,
        WHEEL,
        KEY,
        MODIFIERS,
        FOCUS,
        RELEASE_CAPTURE,
        PLACE,
        CAMERA,
        LIST_SCROLL,
        GESTURE
    } kind;
    int32_t x = 0, y = 0, dz = 0;
    double dx = 0, dy = 0; // Drawable deltas preserve subpixel motion in the queue.
    int drawable_w = 0, drawable_h = 0;
    int button = 0;
    bool down = false;
    bool inside = false, edge = false;
    uint16_t key = 0;
    uint32_t character = 0;
    uint32_t flags = 0;
};
// PendingInput::flags on a BUTTON: the touch mapper's wheel-button pan.
constexpr uint32_t kPendingTouchPan = 1;
std::vector<PendingInput> g_pending_input;
// The queue is FILLED on the host thread inside an idle slice, with the
// scheduler's lock down, and DRAINED by whichever guest thread holds the baton
// - the tick, or the scheduler itself when nothing else can run. Those are two
// different threads running at the same time, so the queue needs its own lock.
// It is held only across the push and the swap, never across an apply: a mod
// callback must not run with a host lock held.
std::mutex g_pending_input_m;

void apply_input(const PendingInput &e);
void apply_focus(bool focused, uint32_t modifier_flags);

// True when the event was held back rather than applied.
void deliver_pending_input();

bool queue_or_apply(const PendingInput &e) {
#ifdef __EMSCRIPTEN__
    // The browser's main thread handles events; the game applies them.
    const bool queue = true;
#else
    const bool queue = sched_in_idle_slice();
    if (!queue) {
        // Input still waiting from an idle slice goes first: applying this one
        // now would let it overtake them. A focus loss queued during a slice
        // and the gain applied right after it delivered WM_ACTIVATEAPP(1)
        // before (0), and NFS Carbon stayed paused on a black screen.
        bool behind = false;
        {
            std::lock_guard<std::mutex> held(g_pending_input_m);
            if (!g_pending_input.empty()) {
                g_pending_input.push_back(e);
                behind = true;
            }
        }
        if (behind) {
            deliver_pending_input();
            return true;
        }
    }
#endif
    if (queue) {
        {
            std::lock_guard<std::mutex> held(g_pending_input_m);
            g_pending_input.push_back(e);
        }
        // Announced AFTER the input is in the queue, because a thread woken by
        // this goes straight to the emptiness check - and OUTSIDE the queue
        // lock, because the scheduler asks input_pending() while holding its
        // own mutex. Taking the two in the other order here would be a
        // lock-ordering inversion between them.
        sched_input_arrived();
        return true;
    }
    apply_input(e);
    return false;
}

// Called from pump(), which the guest reaches through its own clock read, so
// the calling thread holds the baton.
void deliver_pending_input() {
    std::vector<PendingInput> batch;
    {
        std::lock_guard<std::mutex> held(g_pending_input_m);
        if (g_pending_input.empty())
            return;
        batch.swap(g_pending_input);
    }
    // A press and its release must not be applied in the same guest turn, or a
    // guest that polls its buttons never sees the button down at all. The rule
    // is in input_gate.cpp, where it is tested without a window.
    std::vector<HostInputStep> steps;
    steps.reserve(batch.size());
    for (const PendingInput &e : batch)
        steps.push_back(
            {uint8_t(e.kind == PendingInput::BUTTON), uint8_t(e.button), uint8_t(e.down ? 1 : 0)});
    const uint32_t limit = host_input_batch_limit(steps.data(), uint32_t(steps.size()));
    // Applied outside the lock, in arrival order. Order is the whole point: a
    // focus loss sits in this queue among the presses it must follow, so
    // replaying a press after it cannot leave a key stuck down.
    for (uint32_t i = 0; i < limit; ++i)
        apply_input(batch[i]);
    if (limit >= batch.size())
        return;
    {
        std::lock_guard<std::mutex> held(g_pending_input_m);
        g_pending_input.insert(g_pending_input.begin(), batch.begin() + limit, batch.end());
    }
    // The deferred remainder is input the guest has not seen yet, so the
    // scheduler must come back for it rather than park until something else
    // arrives.
    sched_input_arrived();
}

// ---------------------------------------------------------------------------
// The platform half of pointer capture. The policy and the arithmetic are in
// input_gate.cpp, where they can be tested without a window; what is left here
// is what only a window can do: hide and confine the associated system cursor.
// Window positions remain the motion source in both capture states.
// ---------------------------------------------------------------------------
bool g_pointer_hidden = false;
// Test80 (test report: the arrow sat in the middle of the picture while racing with the controller):
// the OS cursor also hides itself while the pointer is idle - 2.5 s after the last real mouse or trackpad
// motion, or at once on controller input - and comes back on the next motion or click. Not while the
// settings page (which is navigated with a real cursor) is up. g_os_cursor_hidden is what SDL was told.
bool g_idle_hidden = false, g_os_cursor_hidden = false;
uint64_t g_pointer_active_ms = 0;
void reconcile_os_cursor() {
    const bool hide = g_pointer_hidden || g_idle_hidden;
    if (hide == g_os_cursor_hidden)
        return;
    if (hide)
        SDL_HideCursor();
    else
        SDL_ShowCursor();
    g_os_cursor_hidden = hide;
}
std::atomic<bool> g_escape_held{false};
std::atomic<bool> g_platform_capture_requested{false};
void update_platform_pointer_capture();

// Host-side state transitions, stamped on the frame-timings clock so they
// can be laid beside the presenter's acknowledgement trace.
static void trace_state(const char *what) {
    static const bool trace = recomp_env("TRACE_POINTER") != nullptr;
    if (trace)
        fprintf(stderr, "[state %.3f] %s\n", double(os_monotonic_ns()) / 1e9, what);
}
void apply_pointer_capture(bool want) {
    // No pointer to capture on a touch platform: fingers are placed absolutely.
    want = want && platform_ui_pointer_capture_supported();
    host_pointer_capture(want);
    // Queue draining can run on a guest worker. All native cursor/window work
    // belongs to the main thread, which observes this at the next pump.
    g_platform_capture_requested = want;
    if (SDL_IsMainThread())
        update_platform_pointer_capture();
}

void update_idle_cursor() {
    const bool idle = SDL_GetTicks() - g_pointer_active_ms > 2500;
    // Only once the game shows its own picture: over a launcher or settings form it draws no cursor of its own.
    // Test128: a Direct3D 9 game (NFSMW) never has a DirectDraw primary, so the arrow never hid; the GPU renderer
    // running (the device took the game's draws) counts as the game's picture too (RC5 launcher log, 2 Oct 2026).
    const bool primary = ddraw_gdi_primary_active() || host_d9_active() != 0, focus = g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS),
               page = host_page_visible() || mods_controls_editing();
    const bool want = idle && primary && focus && !page && !g_close_requested;
    if (want != g_idle_hidden) {
        g_idle_hidden = want;
        reconcile_os_cursor();
        // Test128 (an RC4 test session: the arrow stayed on screen): each change and why, once.
        printf("[host] cursor: %s (idle %d, game picture %d, window focus %d, settings page %d)\n",
               want ? "hidden" : "shown", int(idle), int(primary), int(focus), int(page));
        fflush(stdout);
    } else if (!want && idle) {
        // Idle but kept visible: say why at most once a minute.
        static uint64_t last_ms = 0;
        const uint64_t now = SDL_GetTicks();
        if (!last_ms || now - last_ms > 60000) {
            last_ms = now;
            printf("[host] cursor: idle but shown (game picture %d, window focus %d, settings page %d, closing %d)\n",
                   int(primary), int(focus), int(page), int(g_close_requested));
            fflush(stdout);
        }
    }
}

// Minimized, hidden or fully covered by other windows: the energy policy
// (mods/display_settings.cpp) drops the frame limit while nobody sees it.
static std::atomic<int> g_window_unseen{0};
extern "C" int host_window_unseen(void) {
    return g_window_unseen.load(std::memory_order_relaxed);
}

bool window_minimized_or_hidden() {
    const SDL_WindowFlags flags = g_window ? SDL_GetWindowFlags(g_window) : 0;
    return (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
}
bool window_focused() {
    return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}
bool window_fullscreen() {
    return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
}

// Capture is for a window that is focused, showing, and not displaying the mod
// settings page - the page is navigated with a real cursor. It is not tied to
// what the guest is doing: an FMV reads the mouse exactly as gameplay does.
bool pointer_capture_wanted() {
    if (!platform_ui_pointer_capture_supported())
        return false;
    // Nothing to stand in for the system pointer until the guest has a display
    // surface of its own: while it is still showing plain windows - a launcher,
    // a settings form - it draws no cursor, so hiding the host one would leave
    // the player with nothing to aim.
    if (!ddraw_gdi_primary_active() && host_d9_active() == 0)
        return false;
    if (!g_focused || !g_window || !window_focused() || g_escape_held)
        return false;
    if (window_minimized_or_hidden())
        return false;
    if (host_page_visible() || mods_controls_editing())
        return false;
    return true;
}

void update_platform_pointer_capture() {
    const bool want = g_platform_capture_requested && pointer_capture_wanted() &&
                      !g_close_requested && !g_fullscreen_transition;
    if (want && !g_pointer_hidden) {
        trace_state("capture ON: hiding the OS cursor");
        g_pointer_hidden = true;
        reconcile_os_cursor();
    }
    if (!want && g_pointer_hidden) {
        trace_state("capture OFF: showing the OS cursor");
        g_pointer_hidden = false;
        reconcile_os_cursor();
    }

    // A regular window confines too; hiding alone lets the OS pointer leave
    // the frame. Dragging into the resize margin releases capture so the
    // window's resize edges remain reachable.
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    HostRect rect = host_pointer_confinement_wanted(want, g_window_mode) && g_window
                        ? host_pointer_confinement_rect({0, 0, double(bw), double(bh)})
                        : HostRect{};
    if (g_window && (rect.x != g_pointer_confinement.x || rect.y != g_pointer_confinement.y ||
                     rect.w != g_pointer_confinement.w || rect.h != g_pointer_confinement.h)) {
        if (rect.empty())
            SDL_SetWindowMouseRect(g_window, nullptr);
        else {
            SDL_Rect clip{int(rect.x), int(rect.y), int(rect.w), int(rect.h)};
            SDL_SetWindowMouseRect(g_window, &clip);
        }
        fprintf(stderr, "[host] pointer confinement: %.1f,%.1f %.1fx%.1f points (window mode %d)\n",
                rect.x, rect.y, rect.w, rect.h, g_window_mode);
        g_pointer_confinement = rect;
        g_pointer_sample_valid = false;
    }
}

bool window_has_resize_edges() {
    return g_window_mode == 0 && !g_fullscreen_transition && !window_fullscreen();
}

bool input_pending() {
    std::lock_guard<std::mutex> held(g_pending_input_m);
    return !g_pending_input.empty();
}
void drain_input() {
    deliver_pending_input();
}

void apply_motion(int32_t x, int32_t y, double drawable_dx, double drawable_dy) {
    if (host_gate_menu_pointer(x, y, -1, false, 0)) {
        int32_t gx, gy; // still where the pointer is, for when the menu closes
        host_gate_window_pointer(x, y, &gx, &gy);
        return;
    }
    HitResult hit;
    const bool delivered = host_gate_window_motion(x, y, drawable_dx, drawable_dy, &hit);
    static const bool trace = recomp_env("TRACE_POINTER") != nullptr;
    static double last_trace = 0;
    const double now = (double(os_monotonic_ns()) / 1e9);
    if (trace && now - last_trace >= 0.1) {
        last_trace = now;
        const auto guest =
            host_guest_pointer_resolve(g_mem, GUEST_SIZE, RECOMP_HOOK_MOUSE_DEVICE_PTR);
        fprintf(stderr,
                "[pointer-game] drawable %d,%d hit %d at %d,%d delivered %d guest %d,%d bounds "
                "%d,%d,%d,%d\n",
                x, y, int(hit.kind), hit.gx, hit.gy, delivered, guest.x, guest.y, guest.left,
                guest.top, guest.right, guest.bottom);
    }
    if (host_pointer_captured() && g_buttons && g_window && window_has_resize_edges()) {
        double px, py;
        host_pointer_drawable_position(&px, &py);
        int bw, bh, dw, dh;
        window_sizes(&bw, &bh, &dw, &dh);
        const double margin = 8 * (bw > 0 ? double(dw) / bw : 1.0);
        // The pointer is in game-rectangle pixels.
        const HostGameRect game = game_rect_for(dw, dh);
        if (host_pointer_at_resize_edge(px, py, game.w, game.h, margin))
            apply_pointer_capture(false);
    }
    {
        static int last_kind = -99;
        static bool last_delivered = false;
        if (int(hit.kind) != last_kind || delivered != last_delivered) {
            char what[96];
            snprintf(what, sizeof what, "hit kind %d -> %d, delivered %d -> %d", last_kind,
                     int(hit.kind), int(last_delivered), int(delivered));
            trace_state(what);
            last_kind = int(hit.kind);
            last_delivered = delivered;
        }
    }
    if (!delivered)
        return;
    x = hit.gx;
    y = hit.gy;
    g_cursor_x = x;
    g_cursor_y = y;
    post(WM_MOUSEMOVE_, mouse_wparam(), make_lparam(x, y));
}

// Where the pointer is right now, in window points.
bool pointer_in_window_points(double *px, double *py) {
    float gx, gy;
    SDL_GetGlobalMouseState(&gx, &gy);
    int wx, wy;
    if (!g_window || !SDL_GetWindowPosition(g_window, &wx, &wy))
        return false;
    *px = gx - wx;
    *py = gy - wy;
    return true;
}

void sample_captured_pointer() {
    if (g_pointer_confinement.empty())
        return;
    double px, py;
    if (!pointer_in_window_points(&px, &py))
        return;
    PendingInput e;
    e.kind = PendingInput::MOTION;
    view_point_to_drawable(px, py, &e.x, &e.y, &e.drawable_w, &e.drawable_h);
    if (g_pointer_sample_valid && e.x == g_pointer_sample_x && e.y == g_pointer_sample_y &&
        e.drawable_w == g_pointer_sample_w && e.drawable_h == g_pointer_sample_h)
        return;
    g_pointer_sample_valid = true;
    g_pointer_sample_x = e.x;
    g_pointer_sample_y = e.y;
    g_pointer_sample_w = e.drawable_w;
    g_pointer_sample_h = e.drawable_h;
    queue_or_apply(e);
}

// The strips the system keeps along the window's edges (a status bar), in
// points: a hardware pointer stops at their inner side.
void system_strip_insets(double *top, double *bottom) {
    *top = *bottom = 0;
    int w = 0, h = 0;
    SDL_Rect safe{};
    if (!g_window || !SDL_GetWindowSafeArea(g_window, &safe))
        return;
    SDL_GetWindowSize(g_window, &w, &h);
    *top = safe.y > 0 ? safe.y : 0;
    const int below = h - (safe.y + safe.h);
    *bottom = below > 0 ? below : 0;
}

void handle_mouse_move(const SDL_MouseMotionEvent &motion) {
    // A real pointer move (not one the touch path or the binding itself
    // synthesized, which both arrive as SDL_TOUCH_MOUSEID): feed the mapped
    // binding's Cursor stick mode. A touch's own placement reaches it through
    // the kTouchPlaceEvent handler below instead, in window points.
    if (motion.which != SDL_TOUCH_MOUSEID)
        controls::host_pointer_moved(motion.x, motion.y);
    PendingInput e;
    e.kind = PendingInput::MOTION;
    // A pointer resting against a system strip means the edge behind it.
    double strip_top = 0, strip_bottom = 0;
    system_strip_insets(&strip_top, &strip_bottom);
    static PointerStripLatch strip_latch;
    // Only a strip the game image reaches: in portrait the image starts below it.
    const PointRect game = game_rect_points();
    const double strip = game.whole ? strip_top : std::max(0.0, strip_top - game.y);
    const float y = (float)strip_latch.apply(motion.y, strip);
    view_point_to_drawable(motion.x, y, &e.x, &e.y, &e.drawable_w, &e.drawable_h);
    static const bool trace = recomp_env("TRACE_POINTER") != nullptr;
    static double last_trace = 0;
    const double now = (double(os_monotonic_ns()) / 1e9);
    // Every event along the top strip, where an edge scroll is decided; one in
    // ten elsewhere.
    if (trace && (now - last_trace >= 0.1 || motion.y < strip_top + 32)) {
        last_trace = now;
        fprintf(stderr,
                "[pointer] event %.1f,%.1f drawable %d,%d/%d,%d clip %d delta %.1f,%.1f which %u\n",
                motion.x, motion.y, e.x, e.y, e.drawable_w, e.drawable_h,
                !g_pointer_confinement.empty(), motion.xrel, motion.yrel, (unsigned)motion.which);
    }
    if (!g_pointer_confinement.empty())
        sample_captured_pointer();
    else
        queue_or_apply(e);
}

void apply_button(int button, bool down, int32_t x, int32_t y, bool inside, bool edge) {
    if (down && (!inside || edge)) {
        apply_pointer_capture(false);
        return;
    }
    if (host_gate_menu_pointer(x, y, button, down, 0))
        return;
    // A click inside the window is how the pointer is taken back after it was
    // released - by focus loss, or by the settings page closing.
    int32_t dx, dy;
    auto hit = host_gate_window_pointer(x, y, &dx, &dy);
    if (hit.kind == HitResult::HIT_NONE) {
        if (down)
            return;
        // An outside release must still release a guest button held during
        // an edge drag, even though the outside position has no hit owner.
        hit.gx = g_cursor_x;
        hit.gy = g_cursor_y;
    }
    if (!host_pointer_captured() &&
        host_pointer_can_capture(pointer_capture_wanted(), inside, down, g_escape_held,
                                 host_page_visible() || mods_controls_editing()))
        apply_pointer_capture(true);
    x = hit.gx;
    y = hit.gy;
    if (!down && !(g_buttons & ~(1u << button)))
        host_gate_end_drag();
    static const bool trace_buttons = recomp_env("TRACE_POINTER") != nullptr;
    const bool consumed = host_gate_button(button, down, x, y);
    if (trace_buttons)
        fprintf(stderr, "[pointer-button] button %d %s at %d,%d consumed %d captured %d\n", button,
                down ? "down" : "up", x, y, int(consumed), int(host_pointer_captured()));
    if (consumed)
        return;
    if (down)
        host_gate_begin_drag(&hit);
    g_cursor_x = x;
    g_cursor_y = y;
    // Absolute placement alone does not move Populous's integrated cursor.
    host_input_motion(x, y, dx, dy);
    post(WM_MOUSEMOVE_, mouse_wparam(), make_lparam(x, y));
    if (down)
        g_buttons |= (uint8_t)(1u << button);
    else
        g_buttons &= (uint8_t)~(1u << button);
    host_input_button(button, down);
    static const uint32_t msgs[3][2] = {
        {WM_LBUTTONUP_, WM_LBUTTONDOWN_},
        {WM_RBUTTONUP_, WM_RBUTTONDOWN_},
        {WM_MBUTTONUP_, WM_MBUTTONDOWN_},
    };
    if (button < 3)
        post(msgs[button][down ? 1 : 0], mouse_wparam(), make_lparam(x, y));
}

void handle_button(const SDL_MouseButtonEvent &event, int button, bool down) {
    PendingInput e;
    e.kind = PendingInput::BUTTON;
    e.button = button;
    e.down = down;
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    e.inside = event.x >= 0 && event.y >= 0 && event.x < bw && event.y < bh;
    e.edge = window_has_resize_edges() && host_pointer_at_resize_edge(event.x, event.y, bw, bh, 8);
    // A wheel-button drag the touch mapper made is a pan: apply_input scales it.
    if (event.which == SDL_TOUCH_MOUSEID && button == 2)
        e.flags = kPendingTouchPan;
    view_point_to_drawable(event.x, event.y, &e.x, &e.y, &e.drawable_w, &e.drawable_h);
    queue_or_apply(e);
}

// The key paths live in input_gate.cpp, which owns the decision, the delivery
// and the two kinds of key state they need. What is left here is the SDL
// event decoding.
void apply_wheel(int32_t dz, int32_t x, int32_t y);

// A trackpad's scroll over the interface: the list under the pointer follows
// it (recomp_ui_scroll); where the game takes none (a town, a screen without a
// list), the game's wheel as SDL made it, kTrackpadListWheelPerPoint a point.
void apply_list_scroll(int32_t x, int32_t y, double dy_drawable, double dy_points) {
    static double left = 0; // wheel steps not sent yet
    if (host_gate_ui_scroll(x, y, dy_drawable)) {
        left = 0;
        return;
    }
    left += dy_points * kTrackpadListWheelPerPoint;
    const int whole = (int)left; // toward zero
    if (!whole)
        return;
    left -= whole;
    apply_wheel(whole, x, y);
}

void apply_wheel(int32_t dz, int32_t x, int32_t y) {
    if (host_gate_menu_pointer(x, y, -1, false, dz))
        return;
    if (host_gate_wheel(dz))
        return;
    int32_t dx, dy;
    auto hit = host_gate_window_pointer(x, y, &dx, &dy);
    if (hit.kind == HitResult::HIT_NONE)
        return;
    x = hit.gx;
    y = hit.gy;
    host_input_motion(x, y, dx, dy);
    host_input_wheel(dz);
    post(WM_MOUSEWHEEL_, ((uint32_t)dz << 16) | mouse_wparam(), make_lparam(x, y));
}

void apply_key(uint16_t key, bool down, uint32_t character, uint32_t flags) {
    // Every key event carries the modifier flags, and this is the only place
    // a modifier held across a focus change can be noticed. A diff, so it
    // emits nothing when the state already agrees.
    host_gate_sync_modifiers(flags);
    host_key_event(key, down, character);
}

// A modifier key is an ordinary key event to SDL; the host diffs modifier
// state as a whole, so it arrives as the flags after the change.
bool is_modifier_scancode(SDL_Scancode sc) {
    return sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT || sc == SDL_SCANCODE_LCTRL ||
           sc == SDL_SCANCODE_RCTRL || sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT ||
           sc == SDL_SCANCODE_LGUI || sc == SDL_SCANCODE_RGUI || sc == SDL_SCANCODE_CAPSLOCK;
}

void handle_key(const SDL_KeyboardEvent &event, bool down) {
    // Opt-in diagnostics use the same complete-frame probe as the smoke host.
    // SDL input runs on the guest thread, so the request is ordered with draws.
    // Keep the normal F8 binding untouched outside a diagnostic launch.
    if (event.scancode == SDL_SCANCODE_F8 && recomp_env("D3D9_PROBE_HOTKEY")) {
        if (down && !event.repeat) {
            static unsigned capture = 0;
            char tag[48];
            snprintf(tag, sizeof tag, "interactive-%03u", ++capture);
            fprintf(stderr, "[host] capturing graphics diagnostic %s\n", tag);
            host_d9_probe_next_frame(tag);
        }
        return;
    }
    if (is_modifier_scancode(event.scancode)) {
        PendingInput e;
        e.kind = PendingInput::MODIFIERS;
        e.flags = host_modifier_flags_from_sdl(event.mod);
        queue_or_apply(e);
        return;
    }
    if (event.scancode == SDL_SCANCODE_ESCAPE) {
        g_escape_held = down;
        if (down) {
            // Release before queuing guest input: it must work even while the
            // scheduler cannot grant the guest baton.
            g_platform_capture_requested = false;
            update_platform_pointer_capture();
            PendingInput release;
            release.kind = PendingInput::RELEASE_CAPTURE;
            queue_or_apply(release);
        }
    }
    const uint16_t key = host_keycode_from_scancode(event.scancode);
    if (key == 0xffff)
        return;
    PendingInput e;
    e.kind = PendingInput::KEY;
    e.key = key;
    e.down = down;
    // The character the key produces with no modifier held, which is what a
    // WM_CHAR for it carries; keycodes above the Unicode range are not characters.
    // In the user's layout and with Shift/Caps applied: a Russian layout types Cyrillic.
    const SDL_Keycode plain = SDL_GetKeyFromScancode(
        event.scancode, (SDL_Keymod)(event.mod & (SDL_KMOD_SHIFT | SDL_KMOD_CAPS)), false);
    e.character = plain < 0x40000000 && plain >= 0x20 ? (uint32_t)plain : 0u;
    e.flags = host_modifier_flags_from_sdl(event.mod);
    queue_or_apply(e);
}

// Called from the event pump, including inside an idle slice. It decides that
// the focus changed and records WHEN, relative to the input around it; nothing
// guest-visible happens here.
void note_focus(bool focused) {
    // RECOMP_ASSUME_FOCUS=1: unattended runs (a window that opens behind
    // another app never gains focus) keep the guest believing it is in front,
    // so a game that pauses in the background still plays and presents.
    static const bool assume_focus = [] {
        const char *v = recomp_env("ASSUME_FOCUS");
        return v && *v && *v != '0';
    }();
    if (assume_focus)
        focused = true;
    if (focused == g_focused && g_guest_activated)
        return;
    g_focused = focused;
    g_guest_activated = true;
    PendingInput e;
    e.kind = PendingInput::FOCUS;
    e.down = focused;
    // Read now, not at apply time: what is held at the moment focus returns is
    // what the guest missed, and by the time the queue drains it may differ.
    e.flags = focused ? current_modifier_flags() : 0u;
    queue_or_apply(e);
}

void apply_input(const PendingInput &e) {
    if (e.kind == PendingInput::MOTION || e.kind == PendingInput::BUTTON ||
        e.kind == PendingInput::WHEEL)
        host_gate_fallback_layout(e.drawable_w, e.drawable_h);
    switch (e.kind) {
    case PendingInput::MOTION:
        apply_motion(e.x, e.y, e.dx, e.dy);
        break;
    case PendingInput::BUTTON:
        if (e.flags & kPendingTouchPan) {
            // In queue order, so the pan's own motion is scaled and nothing after it.
            static const double gain = [] {
                const char *v = recomp_env("TOUCH_PAN_GAIN");
                return v && *v ? atof(v) : (double)RECOMP_TOUCH_PAN_GAIN;
            }();
            host_input_set_relative_gain(e.down ? gain : 1.0);
        }
        apply_button(e.button, e.down, e.x, e.y, e.inside, e.edge);
        break;
    case PendingInput::WHEEL:
        apply_wheel(e.dz, e.x, e.y);
        break;
    case PendingInput::KEY:
        apply_key(e.key, e.down, e.character, e.flags);
        break;
    case PendingInput::MODIFIERS:
        host_modifier_event(e.flags);
        break;
    case PendingInput::FOCUS:
        apply_focus(e.down, e.flags);
        break;
    case PendingInput::RELEASE_CAPTURE:
        apply_pointer_capture(false);
        break;
    case PendingInput::PLACE:
        host_gate_pointer_place(e.x, e.y);
        break;
    case PendingInput::CAMERA: // a camera gesture: phase in button, zoom and turn in dx, dy
        host_gate_camera_gesture(e.button, e.x, e.y, e.dx, e.dy);
        break;
    case PendingInput::LIST_SCROLL: // drawable pixels in dy, the same in window points in dx
        apply_list_scroll(e.x, e.y, e.dy, e.dx);
        break;
    case PendingInput::GESTURE: // RECOMP_TOUCH_GESTURE_* in button
        recomp_touch_gesture(e.button);
        break;
    }
}

// The whole of what a focus change does to the guest, including the two paths
// that reach a mod callback: the modifier resync on gain and the release on
// loss. Runs under the baton, from the queue, in arrival order with the key
// and button events around it - which is the point.
void apply_focus(bool focused, uint32_t modifier_flags) {
    if (!host_main_window())
        return; // the guest has no window yet
    // The state now, not when the event was noted: a stale loss delivered after
    // the gain would leave the guest believing it is in the background.
    focused = g_focused;
    recomp_media_focus(focused ? 1 : 0);
    post(WM_ACTIVATEAPP_, focused ? 1 : 0, 0);
    post(WM_ACTIVATE_, focused ? 1 : 0, 0);
    post(focused ? WM_SETFOCUS_ : WM_KILLFOCUS_, 0, 0);
    if (focused)
        post(WM_PAINT_, 0, 0);
    // Whatever is held right now became held while another application had
    // the focus, so no event announced it and the gate's state says it is up.
    if (focused)
        host_gate_sync_modifiers(modifier_flags);
    if (!focused) {
        // The pointer goes back to the user BEFORE the state is cleared:
        // host_gate_release_all drops the capture flag, and the platform
        // effects are keyed off that flag.
        apply_pointer_capture(false);
        // Nothing that was down can be seen coming up while another
        // application has the focus, so it all comes up now - including the
        // host's own copy of the button and key state.
        host_gate_release_all();
        g_buttons = 0;
    }
}

void post_drawable_size();

// The Mac trackpad moves the 3D map's camera (input_trackpad.h) through the
// same game adapter as the iPad's fingers. The launcher's choice,
// host.input/trackpad_camera: 0 a vertical scroll changes the height, 1 as on
// a tablet (scroll pans, pinch zooms, turn rotates), 2 the game's own wheel.
// RECOMP_TRACKPAD_CAMERA overrides it, RECOMP_TRACKPAD_ZOOM is the points of
// scroll per doubling of the distance, RECOMP_TRACKPAD_INVERT a list of
// height, zoom, rotate.
TrackpadMapper g_trackpad;
// Where an unattended run's trackpad gestures have the pointer.
bool g_trackpad_test_cursor = false;
SDL_FPoint g_trackpad_test_at{};

void push_trackpad_actions(const std::vector<TrackpadAction> &actions) {
    for (const TrackpadAction &a : actions) {
        if (a.scroll != 0) { // a list's scroll, in window points
            PendingInput p;
            p.kind = PendingInput::LIST_SCROLL;
            view_point_to_drawable(a.x, a.y, &p.x, &p.y, &p.drawable_w, &p.drawable_h);
            int bw, bh, dw, dh;
            window_sizes(&bw, &bh, &dw, &dh);
            p.dy = bh > 0 ? a.scroll * dh / bh : a.scroll;
            p.dx = a.scroll;
            queue_or_apply(p);
            continue;
        }
        PendingInput p;
        p.kind = PendingInput::CAMERA;
        view_point_to_drawable(a.x, a.y, &p.x, &p.y, &p.drawable_w, &p.drawable_h);
        p.button = a.phase;
        p.dx = a.zoom;
        p.dy = a.turn;
        queue_or_apply(p);
    }
}

[[maybe_unused]] bool on_trackpad(const TrackpadEvent &e) {
    if (!g_window)
        return false;
    TrackpadView v;
    float mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    int w = 0, h = 0;
    SDL_GetWindowSize(g_window, &w, &h);
    if (g_trackpad_test_cursor) { // an unattended run's (RECOMP_AUTO_KEYS trackpad)
        mx = g_trackpad_test_at.x;
        my = g_trackpad_test_at.y;
    }
    v.cursor_x = mx;
    v.cursor_y = my;
    v.width = w;
    v.height = h;
    // Only a gesture's start asks the game what lies under its cursor.
    if (e.phase == TrackpadEvent::Began && !e.momentum) {
        const int context = recomp_touch_context();
        v.world = (context & RECOMP_TOUCH_WORLD) != 0;
        v.zoom_view = (context & RECOMP_TOUCH_ZOOM_VIEW) != 0;
    }
    std::vector<TrackpadAction> actions;
    const bool taken = g_trackpad.event(e, v, &actions);
    push_trackpad_actions(actions);
    return taken;
}

// Once the game's window is up (the launcher has had its say by then).
void install_trackpad() {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    static bool done = false;
    if (done || !g_window)
        return;
    done = true;
    if (!recomp_camera_gesture(-1, 0, 0, 1, 0, 0, 0))
        return; // the game has no camera adapter: its wheel as it was
    int64_t mode = TrackpadMapper::Height;
    mods_settings_file_get("host.input/trackpad_camera", &mode);
    if (const char *v = recomp_env("TRACKPAD_CAMERA"); v && *v)
        mode = atoi(v);
    g_trackpad.set_mode((int)mode);
    if (const char *v = recomp_env("TRACKPAD_ZOOM"); v && *v)
        g_trackpad.set_points_per_doubling(atof(v));
    const char *inv = recomp_env("TRACKPAD_INVERT");
    const std::string invert = inv ? inv : "";
    auto has = [&](const char *w) { return invert.find(w) != std::string::npos; };
    g_trackpad.set_invert(has("height"), has("zoom"), has("rotate"));
    // Off leaves the camera to the game's wheel, but a list still follows a
    // precise scroll (input_trackpad.h).
    const bool on = trackpad_mac_install(g_window, on_trackpad);
    fprintf(stderr, "[host] trackpad camera: %s%s\n",
            g_trackpad.mode() == TrackpadMapper::Tablet ? "tablet"
            : g_trackpad.mode() == TrackpadMapper::Off  ? "the game's wheel (lists follow)"
                                                        : "height",
            on ? "" : " (not installed)");
#endif
}

TouchMapper g_touch = [] {
    TouchMapper t;
    t.set_hover(RECOMP_TOUCH_HOVER != 0); // game.toml [touch] gestures
    // Directions for a camera that answers the other way round: game.toml
    // [touch] invert, or RECOMP_TOUCH_INVERT, a list of panx, pany, zoom, rotate.
    const char *inv = recomp_env("TOUCH_INVERT");
    const std::string invert = inv ? inv : RECOMP_TOUCH_INVERT;
    auto has = [&](const char *w) { return invert.find(w) != std::string::npos; };
    t.set_invert(has("panx"), has("pany"), has("zoom"), has("rotate"));
    // A game that moves its map camera from gestures itself.
    t.set_camera_gestures(recomp_camera_gesture(-1, 0, 0, 1, 0, 0, 0) != 0);
    return t;
}();

// The game's hover delay follows the fingers (runtime/native_seam.h).
void sync_touch_hover() {
    static int sent = RECOMP_TOUCH_HOVER_GAME;
    const int state = g_touch.hover_state();
    if (state != sent) {
        recomp_touch_hover(state);
        sent = state;
    }
}

// A touch names a place. The motion event moves the host's idea of the
// pointer; the PLACE event that follows writes the game's own cursor there
// (host_gate_pointer_place) so the click after it hit-tests where the finger
// is, with no convergence to wait for. All three travel the SDL queue in order.
constexpr Sint32 kTouchPlaceEvent = 0x70756c63; // 'pulc'
void push_touch_action_now(const TouchAction &a);

void push_touch_actions(const std::vector<TouchAction> &actions) {
    for (const TouchAction &a : actions)
        push_touch_action_now(a);
}

void push_touch_action_now(const TouchAction &a) {
    const SDL_WindowID ours = g_window ? SDL_GetWindowID(g_window) : 0;
    {
        SDL_Event e{};
        e.common.timestamp = SDL_GetTicksNS();
        switch (a.kind) {
        case TouchAction::Motion: {
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.windowID = ours;
            e.motion.which = SDL_TOUCH_MOUSEID;
            e.motion.x = (float)a.x;
            e.motion.y = (float)a.y;
            SDL_PushEvent(&e);
            if (!a.place)
                return;
            SDL_Event place{};
            place.type = SDL_EVENT_USER;
            place.user.windowID = ours;
            place.user.code = kTouchPlaceEvent;
            place.user.data1 = (void *)(intptr_t)lround(a.x * 16);
            place.user.data2 = (void *)(intptr_t)lround(a.y * 16);
            SDL_PushEvent(&place);
            return;
        }
        case TouchAction::Button:
            e.type = a.down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.windowID = ours;
            e.button.which = SDL_TOUCH_MOUSEID;
            e.button.button = a.button == 1   ? SDL_BUTTON_RIGHT
                              : a.button == 2 ? SDL_BUTTON_MIDDLE
                                              : SDL_BUTTON_LEFT;
            e.button.down = a.down;
            e.button.clicks = 1;
            e.button.x = (float)a.x;
            e.button.y = (float)a.y;
            break;
        case TouchAction::Key:
            e.type = a.down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.windowID = ours;
            e.key.scancode = (SDL_Scancode)a.scancode;
            e.key.key = SDL_GetKeyFromScancode((SDL_Scancode)a.scancode, SDL_KMOD_NONE, false);
            e.key.down = a.down;
            break;
        case TouchAction::Camera: {
            // In order with the clicks around it, mapped as a placement is.
            PendingInput p;
            p.kind = PendingInput::CAMERA;
            view_point_to_drawable(a.x, a.y, &p.x, &p.y, &p.drawable_w, &p.drawable_h);
            p.button = a.phase;
            p.dx = a.zoom;
            p.dy = a.turn;
            queue_or_apply(p);
            return;
        }
        case TouchAction::Gesture: {
            PendingInput p;
            p.kind = PendingInput::GESTURE;
            p.button = a.gesture;
            queue_or_apply(p);
            return;
        }
        case TouchAction::Wheel:
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.windowID = ours;
            e.wheel.mouse_x = (float)a.x;
            e.wheel.mouse_y = (float)a.y;
            e.wheel.y = (float)a.wheel;
            // handle_event() flips a FLIPPED wheel's sign; NORMAL passes a.wheel through as-is.
            e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
            break;
        }
        SDL_PushEvent(&e);
    }
}

void push_touch_key(int scancode, bool down) {
    TouchAction a;
    a.kind = TouchAction::Key;
    a.scancode = scancode;
    a.down = down;
    push_touch_action_now(a);
}

// The on-screen controls' hooks.
void toggle_system_keyboard() {
    if (!g_window)
        return;
    if (SDL_TextInputActive(g_window))
        SDL_StopTextInput(g_window);
    else
        SDL_StartTextInput(g_window);
}
void open_settings_page() {
    (void)mods_page_open(nullptr);
}

// Focus loss or backgrounding: every finger is gone, every key and modifier up.
void touch_release_all() {
    std::vector<TouchAction> actions;
    g_touch.cancel_all(&actions);
    push_touch_actions(actions);
    controls::host_release_all();
}

// A finger's position in drawable pixels, where the controls are laid out.
void finger_to_drawable(const SDL_TouchFingerEvent &f, double *px, double *py) {
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    *px = f.x * dw;
    *py = f.y * dh;
}

TouchPoint touch_point(const SDL_TouchFingerEvent &f) {
    int w = 0, h = 0;
    if (g_window)
        SDL_GetWindowSize(g_window, &w, &h);
    // The gesture mapper's edges are the game image's (portrait: the image's
    // rectangle inside the window).
    const PointRect game = game_rect_points();
    if (game.whole) {
        g_touch.set_bounds(w, h);
        g_touch.set_origin(0, 0);
    } else {
        g_touch.set_bounds(game.w, game.h);
        g_touch.set_origin(game.x, game.y);
    }
    // The strips the system keeps (a status bar, a gesture zone) never deliver
    // a finger, so a finger "on" that edge arrives at the strip's inner side.
    // Only the part of a strip that overlaps the game image counts.
    SDL_Rect safe{0, 0, w, h};
    if (g_window && SDL_GetWindowSafeArea(g_window, &safe)) {
        double l = safe.x, t = safe.y;
        double r = w - (safe.x + safe.w), b = h - (safe.y + safe.h);
        if (!game.whole) {
            l = safe.x - game.x;
            t = safe.y - game.y;
            r = (game.x + game.w) - (safe.x + safe.w);
            b = (game.y + game.h) - (safe.y + safe.h);
        }
        g_touch.set_edge_insets(l > 0 ? l : 0, t > 0 ? t : 0, r > 0 ? r : 0, b > 0 ? b : 0);
        static bool logged = false;
        if (!logged) {
            logged = true;
            fprintf(stderr, "[touch] window %dx%d points, safe area insets %g,%g,%g,%g\n", w, h, l,
                    t, r, b);
        }
    }
    return {(int64_t)f.fingerID, f.x * w, f.y * h};
}

// A window point in drawable pixels, where the on-screen controls and the
// editor are laid out (unlike view_point_to_drawable, which lands in the
// game image's own coordinates).
void window_point_to_drawable(double px, double py, double *dx, double *dy) {
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    *dx = bw > 0 ? px / bw * dw : 0;
    *dy = bh > 0 ? py / bh * dh : 0;
}

// While the layout editor is open it owns the keyboard, the mouse and the
// wheel: nothing here reaches the game, so the simulation sees frozen input
// for as long as the editor is up (the F10 page freezes it the same way, by
// consuming keys). Fingers go to the editor through host_finger_*, which the
// normal path already calls first. True: the event is spent.
bool handle_editor_event(const SDL_Event &event) {
    if (!controls::host_editing())
        return false;
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        double dx, dy;
        window_point_to_drawable(event.motion.x, event.motion.y, &dx, &dy);
        controls::host_editor_pointer(dx, dy, 0);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (event.button.button != SDL_BUTTON_LEFT)
            return true;
        double dx, dy;
        window_point_to_drawable(event.button.x, event.button.y, &dx, &dy);
        controls::host_editor_pointer(dx, dy, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 1 : -1);
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        controls::host_editor_wheel(
            event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y);
        return true;
    case SDL_EVENT_TEXT_INPUT:
        controls::host_editor_text(event.text.text);
        return true;
    case SDL_EVENT_KEY_UP:
        // No key-up is ever swallowed, Escape's included: a key the player was
        // holding when the editor opened has to be released to the game, and
        // Escape also has host state behind it (g_escape_held, the pointer
        // capture) that only its key-up clears. The editor takes key presses
        // alone, so an Escape release with no press it knows about is the one
        // the game and the host both still need.
        return false;
    case SDL_EVENT_KEY_DOWN:
        if (event.key.scancode == SDL_SCANCODE_ESCAPE)
            controls::host_editor_escape(); // Escape is Done
        else if (event.key.scancode == SDL_SCANCODE_RETURN ||
                 event.key.scancode == SDL_SCANCODE_KP_ENTER)
            controls::host_editor_text_done();
        return true;
    default:
        return false;
    }
}

// SDL's text input follows the editor's rename prompt.
void update_editor_text_input() {
    static bool on = false;
    const bool want = controls::host_editor_text_wanted();
    if (want == on || !g_window)
        return;
    on = want;
    if (want)
        SDL_StartTextInput(g_window);
    else
        SDL_StopTextInput(g_window);
}

// SDL's text input follows a host prompt (the ubi-gs server): on a phone or a
// tablet that is what raises the system keyboard; on a desktop it is what
// makes SDL deliver the typed text (SDL_EVENT_TEXT_INPUT) the prompt reads.
void update_prompt_text_input() {
    static bool on = false;
    const bool want = host_page_prompt_wants_text();
    if (want == on || !g_window)
        return;
    on = want;
    if (want)
        SDL_StartTextInput(g_window);
    else
        SDL_StopTextInput(g_window);
}

// Test128 (RC3 and Development 61 test sessions): when the Mac sleeps, the TV's HDMI audio device
// disappears and comes back on wake as a new device (#15 removed, #39 'LG TV' added). The output - the Dolby Digital
// bitstream or PCM - stayed on the vanished device, so the game was silent until it was restarted, and the game's
// streaming sound kept restarting its buffer (thousands of times). The playback device that was the default when it
// disappeared is remembered by name; when a device of that name comes back, the output reopens once the link has
// settled (the same reopen as after an HDR switch: host_audio_restart_output). Other devices (a controller's
// speaker) coming and going change nothing.
namespace {
std::map<SDL_AudioDeviceID, std::string> g_audio_device_names;
std::string g_audio_default_name, g_audio_lost_name;
std::string audio_device_name(SDL_AudioDeviceID id) {
    const char *n = SDL_GetAudioDeviceName(id);
    return n ? n : "";
}
void note_audio_device_event(uint32_t type, SDL_AudioDeviceID which) {
    if (type == SDL_EVENT_AUDIO_DEVICE_REMOVED) {
        auto it = g_audio_device_names.find(which);
        if (it != g_audio_device_names.end()) {
            if (!it->second.empty() && it->second == g_audio_default_name) {
                g_audio_lost_name = it->second;
                printf("[host] audio: the output device '%s' went away; the output reopens when it is back\n",
                       g_audio_lost_name.c_str());
                fflush(stdout);
            }
            g_audio_device_names.erase(it);
        }
        return;
    }
    const std::string name = audio_device_name(which);
    if (!name.empty())
        g_audio_device_names[which] = name;
    if (type == SDL_EVENT_AUDIO_DEVICE_ADDED && !g_audio_lost_name.empty() && name == g_audio_lost_name) {
        printf("[host] audio: the output device '%s' is back (#%u); the output reopens once it is the system "
               "output again\n", name.c_str(), unsigned(which));
        fflush(stdout);
        g_audio_lost_name.clear();
        // review (2 Oct): no resume across the loss and no fixed delay alone - the reopen builds a fresh session
        // (the Dolby sink resolves the default device, its streams, the AC-3 format and hog mode anew, and a new
        // IOProc). It waits until macOS has made the returned device the system output again (on wake it briefly
        // shows the built-in speakers), checked every 250 ms for at most 15 s, then 1 s for the HDMI link.
        std::thread([name] {
            bool back = false;
            for (int i = 0; i < 60 && !back; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                back = audio_device_name(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) == name;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            if (!back)
                printf("[host] audio: '%s' is not the system output after 15 s; reopening on the current output\n",
                       name.c_str());
            host_audio_restart_output("the output device came back (sleep/wake or the TV reconnected)");
        }).detach();
        // The returned device is the output from here on. Not the momentary system default: on wake that is the
        // built-in speakers, and remembering it lost the second sleep (an RC4 test session).
        g_audio_default_name = name;
        return;
    }
    const std::string def = audio_device_name(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    if (!def.empty() && g_audio_lost_name.empty())
        g_audio_default_name = def;
}
// Test135 (the review's RC5 follow-up): when the macOS audio service (coreaudiod) restarts, every device, stream, hog
// claim and IOProc the process held is gone, like after a sleep. The same fresh reopen, after 2 s for the service to
// enumerate the devices again. The listener runs on a CoreAudio thread; the work runs on its own thread.
void audio_service_restarted() {
    std::thread([] {
        printf("[host] audio: the macOS audio service restarted; the output reopens\n");
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        host_audio_restart_output("the macOS audio service restarted");
    }).detach();
}
} // namespace


// One SDL event, translated into both of the input paths the game reads: the
// DirectInput device state, and the Win32 message queue.
void handle_event(const SDL_Event &event) {
    if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND || event.type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        g_app_active.store(false);
    } else if (event.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        g_foreground_ns.store(os_monotonic_ns());
        g_app_active.store(true);
    }
    if (platform_ui_handle_lifecycle(event))
        return;
    if (handle_editor_event(event))
        return;
    const SDL_WindowID ours = g_window ? SDL_GetWindowID(g_window) : 0;
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (event.motion.xrel != 0.0f || event.motion.yrel != 0.0f)
            g_pointer_active_ms = SDL_GetTicks();
        if (event.motion.which != SDL_TOUCH_MOUSEID) {
            g_touch.pointer_used(); // a mouse, trackpad or pencil: the game's hover delay
            sync_touch_hover();
        }
        if (event.motion.windowID == ours || !g_pointer_confinement.empty())
            handle_mouse_move(event.motion);
        break;
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: {
        static const bool trace = recomp_env("TRACE_POINTER") != nullptr;
        if (trace)
            trace_state(event.type == SDL_EVENT_WINDOW_MOUSE_ENTER ? "pointer entered the window"
                                                                   : "pointer left the window");
        break;
    }
    case SDL_EVENT_WINDOW_OCCLUDED:
        trace_state("window OCCLUDED");
        g_window_unseen.store(1, std::memory_order_relaxed);
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        trace_state("window exposed");
        g_window_unseen.store(0, std::memory_order_relaxed);
        break;
    case SDL_EVENT_WINDOW_HIDDEN:
        trace_state("window HIDDEN");
        g_window_unseen.store(1, std::memory_order_relaxed);
        break;
    case SDL_EVENT_WINDOW_SHOWN:
        trace_state("window shown");
        g_window_unseen.store(0, std::memory_order_relaxed);
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
        trace_state("window MINIMIZED");
        g_window_unseen.store(1, std::memory_order_relaxed);
        break;
    case SDL_EVENT_WINDOW_RESTORED:
        trace_state("window restored");
        g_window_unseen.store(0, std::memory_order_relaxed);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        g_pointer_active_ms = SDL_GetTicks();
        if (event.button.windowID == ours) {
            const int button = event.button.button == SDL_BUTTON_LEFT     ? 0
                               : event.button.button == SDL_BUTTON_RIGHT  ? 1
                               : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                          : -1;
            if (button >= 0)
                handle_button(event.button, button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (event.wheel.windowID == ours) {
            PendingInput e;
            e.kind = PendingInput::WHEEL;
            const double steps =
                event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            e.dz = (int32_t)lround(steps * 120.0);
            // WM_MOUSEWHEEL carries the position of the wheel event itself.
            view_point_to_drawable(event.wheel.mouse_x, event.wheel.mouse_y, &e.x, &e.y,
                                   &e.drawable_w, &e.drawable_h);
            queue_or_apply(e);
        }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        // Ctrl+V or Command-V pastes into a host prompt (the ubi-gs server's
        // address); the clipboard is read here, on the window's thread.
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_V &&
            (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) && host_page_prompt_open()) {
            if (char *clip = SDL_GetClipboardText()) {
                (void)host_page_prompt_text(clip);
                SDL_free(clip);
            }
            break;
        }
        // Command combinations belong to the application, not the game:
        // Command-Q quits through SDL_EVENT_QUIT and Command-H hides.
        if (event.key.mod & SDL_KMOD_GUI)
            break;
        handle_key(event.key, event.type == SDL_EVENT_KEY_DOWN);
        break;
    case SDL_EVENT_TEXT_INPUT:
        // Typed text (and a phone's keyboard's) goes to a host prompt; the
        // game takes its characters from the key events instead.
        (void)host_page_prompt_text(event.text.text);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        trace_state("focus LOST");
        g_platform_capture_requested = false;
        update_platform_pointer_capture();
        g_escape_held = false;
        touch_release_all();
        {
            std::vector<TrackpadAction> actions;
            g_trackpad.cancel(&actions);
            push_trackpad_actions(actions);
        }
        note_focus(false);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        trace_state("focus gained");
        note_focus(true);
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
        note_screen_size();
        post_drawable_size();
        // The confinement is the window's rectangle: a window the game's new
        // display mode made larger would otherwise keep the pointer inside the
        // old size until capture is taken again (Escape).
        update_platform_pointer_capture();
        break;
    case SDL_EVENT_AUDIO_DEVICE_ADDED:
    case SDL_EVENT_AUDIO_DEVICE_REMOVED:
    case SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED:
        // A TV/receiver waking or eARC renegotiating moves the game's stream
        // to another route without telling the mixer; record where it went.
        if (!event.adevice.recording) {
            sdl_audio_log_route(event.type == SDL_EVENT_AUDIO_DEVICE_ADDED     ? "device added"
                                : event.type == SDL_EVENT_AUDIO_DEVICE_REMOVED ? "device removed"
                                                                               : "format changed",
                                event.adevice.which);
            note_audio_device_event(event.type, event.adevice.which);
        }
        break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        note_screen_size();
        g_borderless_frame_dirty = true;
        post_drawable_size();
        break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        // The green button or the View menu: the player chose fullscreen, so
        // it becomes the setting instead of being undone on the next frame.
        // A touch platform's window is always fullscreen and has no
        // "windowed" mode to save - platform_ui_pointer_capture_supported()
        // is false there - so a rotation raising this same event (no player
        // choice behind it) never rewrites the saved window-mode setting.
        if (!g_fullscreen_transition && platform_ui_pointer_capture_supported()) {
            g_wanted_window_mode = 2;
            (void)mods_display_set(DISPLAY_WINDOW, 2);
        }
        g_fullscreen_transition = false;
        g_window_mode = 2;
        note_screen_size();
        post_drawable_size();
        update_platform_pointer_capture();
        break;
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        if (!g_fullscreen_transition && g_wanted_window_mode == 2 &&
            platform_ui_pointer_capture_supported()) {
            g_wanted_window_mode = 0;
            (void)mods_display_set(DISPLAY_WINDOW, 0);
        }
        g_fullscreen_transition = false;
        g_window_mode = 0;
        note_screen_size();
        post_drawable_size();
        update_platform_pointer_capture();
        break;
    case SDL_EVENT_USER:
        if (event.user.code == kTouchPlaceEvent) {
            const double px = double((intptr_t)event.user.data1) / 16.0;
            const double py = double((intptr_t)event.user.data2) / 16.0;
            controls::host_pointer_moved(px, py);
            PendingInput e;
            e.kind = PendingInput::PLACE;
            view_point_to_drawable(px, py, &e.x, &e.y, &e.drawable_w, &e.drawable_h);
            queue_or_apply(e);
        }
        break;
    case SDL_EVENT_FINGER_CANCELED: {
        // The system took the finger (a gesture, a call): whatever it held lets go.
        const int64_t finger = (int64_t)event.tfinger.fingerID;
        if (controls::host_finger_cancel(finger))
            break;
        std::vector<TouchAction> actions;
        g_touch.set_context(recomp_touch_context());
        g_touch.finger_cancel(finger, &actions);
        push_touch_actions(actions);
        break;
    }
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION: {
        std::vector<TouchAction> actions;
        const uint64_t now = SDL_GetTicksNS();
        const int64_t finger = (int64_t)event.tfinger.fingerID;
        host_input_touch_activity();
        // A finger the on-screen controls own never reaches the gesture mapper.
        double px = 0, py = 0;
        finger_to_drawable(event.tfinger, &px, &py);
        g_touch.set_context(recomp_touch_context());
        if (event.type == SDL_EVENT_FINGER_DOWN) {
            if (controls::host_finger_down(finger, px, py, now))
                break;
            g_touch.finger_down(touch_point(event.tfinger), now, &actions);
        } else if (event.type == SDL_EVENT_FINGER_UP) {
            if (controls::host_finger_up(finger, now))
                break;
            g_touch.finger_up(touch_point(event.tfinger), now, &actions);
        } else {
            if (controls::host_finger_motion(finger, px, py, now))
                break;
            g_touch.finger_motion(touch_point(event.tfinger), now, &actions);
        }
        push_touch_actions(actions);
        sync_touch_hover();
        break;
    }
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
            (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (event.gaxis.value > 12000 || event.gaxis.value < -12000)))
            g_pointer_active_ms = 0;   // playing with the controller: hide the idle cursor now
        // The settings page, when it is up, is driven by the pad too. Its keys go through the
        // same filter the keyboard's do; the game meanwhile sees a pad with nothing held.
        // Open or close the settings page from the controller: the DualSense touchpad click (the game
        // has no use for it), or Start while Back/View is held on any pad. Toggled before the pad state
        // is published, so the game never sees that Start press.
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            SDL_Gamepad *pad = SDL_GetGamepadFromID(event.gbutton.which);
            const bool touchpad = event.gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD;
            const bool chord = event.gbutton.button == SDL_GAMEPAD_BUTTON_START && pad &&
                               SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
            if (touchpad || chord) {
                (void)mods_input_key(0x44, 0, true); // F10
                (void)mods_input_key(0x44, 0, false);
                controls::gamepad_mask_until_release(event.gbutton.button);
                controls::gamepad_handle_event(event);
                break;
            }
        }
        // A release goes only where its press went.
        static bool sent[256] = {};
        bool down = false;
        const int dik = controls::gamepad_menu_key(event, &down);
        if (dik && down && host_page_visible()) {
            sent[dik] = true;
            // The page's press stays the page's: when it closes the page (East = Escape), the game
            // must not see the button still held.
            if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                controls::gamepad_mask_until_release(event.gbutton.button);
            // A host prompt (the ubi-gs server) before the settings page, as the keyboard's.
            if (!host_page_prompt_key(uint8_t(dik), true))
                (void)mods_input_key(uint8_t(dik), 0, true);
        } else if (dik && !down && sent[dik]) {
            sent[dik] = false;
            if (!host_page_prompt_key(uint8_t(dik), false))
                (void)mods_input_key(uint8_t(dik), 0, false);
        }
        controls::gamepad_handle_event(event);
        break;
    }
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_QUIT:
        // The guest closes itself: WM_CLOSE runs its own shutdown path, and
        // the window stays up until it is done so the last frame does not
        // vanish mid-teardown.
        if (!g_close_requested)
            fprintf(stderr, "[host] %s: asking the game to close\n",
                    event.type == SDL_EVENT_QUIT ? "quit requested" : "window close requested");
        g_close_requested = true;
        // A guest thread waiting on a host prompt gets its "no" now, so it
        // can take part in the shutdown.
        host_page_prompt_cancel();
        break;
    default:
        break;
    }
}

// Unattended runs (diagnostics, not a player feature).
//   RECOMP_AUTO_KEYS="20:Escape,25:Return,40:click:640:480" presses a key
//   (SDL scancode name) or clicks the left button at window coordinates (in
//   points; both at most 1 = fractions of the window) the
//   given number of seconds after start, through the same SDL events a real
//   press makes, so an intro can be skipped or a menu walked without anyone at
//   the keyboard. Each press is released RECOMP_AUTO_KEYS_HOLD seconds later
//   (default 0.1, a quick human tap).
//   RECOMP_MAX_SECONDS=S asks the game to close after S seconds, as closing
//   the window does (the headless host reads the same variable). A game busy
//   loading pumps no events and never sees that request, so a watchdog thread
//   ends the process outright 20 s later: an unattended run never leaves a
//   window behind.
//   "30:trackpad:scroll-y:-300" (or scroll-x, magnify, rotate; then the
//   total: points, scale change, degrees; ":X:Y" puts the game's cursor
//   there first, in window points) is a trackpad gesture through the same
//   handler AppKit's events reach (sdl/trackpad_mac.h), a step every 16 ms.
//   "30:text:127.0.0.1:8080" types the rest of the item (no commas) as an
//   SDL text-input event would deliver it, into a host prompt.
struct AutoInput {
    double at;
    bool click, down;
    SDL_Scancode scancode;
    float x, y;
    // A trackpad step: kind (trackpad_mac_test), phase, momentum, amount; -1 none.
    int trackpad = -1, phase = 0, momentum = 0;
    double amount = 0;
    bool place = false; // put the game's cursor at x, y (trackpad gestures)
    std::string text;   // typed text; non-empty for a text item
};

// The trackpad gesture of a RECOMP_AUTO_KEYS item, as the timed steps a
// trackpad sends: scrolls with a began-changed-ended run and a fading inertia
// tail, pinches and turns with began-changed-ended.
[[maybe_unused]] bool parse_auto_trackpad(double at, const std::string &what,
                                          std::vector<AutoInput> *out) {
    char kind_name[16] = {};
    double total = 0;
    float x = 0, y = 0;
    const int n = sscanf(what.c_str(), "trackpad:%15[a-z-]:%lf:%f:%f", kind_name, &total, &x, &y);
    if (n < 2)
        return false;
    const std::string k = kind_name;
    const int kind = k == "scroll-y" ? 0 : k == "scroll-x" ? 1 : k == "magnify" ? 2 : k == "rotate" ? 3 : -1;
    if (kind < 0)
        return false;
    AutoInput base{at, false, true, SDL_SCANCODE_UNKNOWN, x, y};
    if (n == 4) {
        AutoInput place = base;
        place.place = true;
        out->push_back(place);
        base.at += 0.4; // a few frames for the game's cursor to show what it is over
    }
    const int steps = 20;
    const double per = total / steps, dt = 0.016;
    auto step = [&](int phase, int momentum, double amount) {
        AutoInput a = base;
        a.trackpad = kind;
        a.phase = phase;
        a.momentum = momentum;
        a.amount = amount;
        out->push_back(a);
        base.at += dt;
    };
    if (kind <= 1) {
        step(128, 0, 0);
        for (int i = 0; i < steps; ++i)
            step(i == 0 ? 1 : 2, 0, per);
        step(4, 0, 0);
        double m = per;
        for (int i = 0; i < 10; ++i) {
            m *= 0.8;
            step(0, i == 0 ? 1 : 2, m);
        }
        step(0, 3, 0);
    } else {
        step(1, 0, 0);
        for (int i = 0; i < steps; ++i)
            step(2, 0, per);
        step(4, 0, 0);
    }
    return true;
}

std::vector<AutoInput> parse_auto_input(const char *spec) {
    std::vector<AutoInput> out;
    std::string text = spec ? spec : "";
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos)
            end = text.size();
        std::string item = text.substr(start, end - start);
        start = end + 1;
        size_t colon = item.find(':');
        if (colon == std::string::npos)
            continue;
        AutoInput a{atof(item.c_str()), false, true, SDL_SCANCODE_UNKNOWN, 0.0f, 0.0f};
        std::string what = item.substr(colon + 1);
        if (what.compare(0, 9, "trackpad:") == 0) {
            if (!parse_auto_trackpad(a.at, what, &out))
                fprintf(stderr, "[host] RECOMP_AUTO_KEYS: bad trackpad item %s\n", what.c_str());
            continue;
        }
        if (what.compare(0, 5, "text:") == 0) {
            a.text = what.substr(5);
            if (!a.text.empty())
                out.push_back(a); // no release
            continue;
        }
        if (what.compare(0, 6, "click:") == 0) {
            a.click = true;
            if (sscanf(what.c_str() + 6, "%f:%f", &a.x, &a.y) != 2)
                continue;
        } else {
            a.scancode = SDL_GetScancodeFromName(what.c_str());
            if (a.scancode == SDL_SCANCODE_UNKNOWN) {
                fprintf(stderr, "[host] RECOMP_AUTO_KEYS: unknown key %s\n", what.c_str());
                continue;
            }
        }
        out.push_back(a);
        AutoInput up = a;
        static const double hold = [] {
            const char *v = recomp_env("AUTO_KEYS_HOLD");
            return v ? atof(v) : 0.1;
        }();
        up.at += hold;
        up.down = false;
        out.push_back(up);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const AutoInput &l, const AutoInput &r) { return l.at < r.at; });
    return out;
}

void auto_input_tick() {
    static const std::vector<AutoInput> script = parse_auto_input(recomp_env("AUTO_KEYS"));
    static const double max_seconds = [] {
        const char *v = recomp_env("MAX_SECONDS");
        return v ? atof(v) : 0.0;
    }();
    static size_t next = 0;
    static bool quit_sent = false;
    if (script.empty() && max_seconds <= 0.0)
        return;
    static const bool watchdog = [] {
        if (max_seconds > 0.0)
            std::thread([] {
                std::this_thread::sleep_for(std::chrono::duration<double>(max_seconds + 20.0));
                fprintf(stderr, "[host] RECOMP_MAX_SECONDS=%g: the game did not close; "
                                "ending the process\n", max_seconds);
                fflush(stderr);
                std::_Exit(6);
            }).detach();
        return true;
    }();
    (void)watchdog;
    double now = (double)SDL_GetTicks() / 1000.0;
    const SDL_WindowID ours = g_window ? SDL_GetWindowID(g_window) : 0;
    while (ours && next < script.size() && script[next].at <= now) {
        AutoInput a = script[next++]; // a copy: fractional clicks become points below
        SDL_Event e{};
        if (!a.text.empty()) {
            // What SDL_EVENT_TEXT_INPUT does with it (handle_event), without
            // an event whose text SDL would have to own.
            const bool taken = host_page_prompt_text(a.text.c_str());
            fprintf(stderr, "[host] auto input at %.1f s: text \"%s\"%s\n", now, a.text.c_str(),
                    taken ? "" : " (no prompt open; dropped)");
            continue;
        }
        if (a.place || a.trackpad >= 0) {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
            if (a.place) {
                SDL_Event place{};
                place.type = SDL_EVENT_USER;
                place.user.windowID = ours;
                place.user.code = kTouchPlaceEvent;
                place.user.data1 = (void *)(intptr_t)lround(a.x * 16);
                place.user.data2 = (void *)(intptr_t)lround(a.y * 16);
                SDL_PushEvent(&place);
                g_trackpad_test_at = {a.x, a.y};
                g_trackpad_test_cursor = true;
                fprintf(stderr, "[host] auto input at %.1f s: trackpad pointer %.0f,%.0f\n", now,
                        a.x, a.y);
            } else {
                trackpad_mac_test(a.trackpad, a.phase, a.momentum, a.amount);
            }
#endif
            continue;
        }
        if (a.click) {
            // Both coordinates at most 1: fractions of the window, so a route
            // holds whatever size the window opened at on whichever screen.
            if (a.x <= 1.0f && a.y <= 1.0f && g_window) {
                int ww = 0, wh = 0;
                SDL_GetWindowSize(g_window, &ww, &wh);
                a.x *= (float)ww;
                a.y *= (float)wh;
            }
            SDL_Event move{};
            move.type = SDL_EVENT_MOUSE_MOTION;
            move.motion.windowID = ours;
            move.motion.x = a.x;
            move.motion.y = a.y;
            SDL_PushEvent(&move);
            // Then place the game's own cursor there, as a touch does: a game
            // that steers its cursor from relative motion (Heroes V) would
            // otherwise take the click wherever its cursor had got to.
            SDL_Event place{};
            place.type = SDL_EVENT_USER;
            place.user.windowID = ours;
            place.user.code = kTouchPlaceEvent;
            place.user.data1 = (void *)(intptr_t)lround(a.x * 16);
            place.user.data2 = (void *)(intptr_t)lround(a.y * 16);
            SDL_PushEvent(&place);
            e.type = a.down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.windowID = ours;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = a.down;
            e.button.clicks = 1;
            e.button.x = a.x;
            e.button.y = a.y;
        } else {
            e.type = a.down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.windowID = ours;
            e.key.scancode = a.scancode;
            e.key.key = SDL_GetKeyFromScancode(a.scancode, SDL_KMOD_NONE, false);
            e.key.down = a.down;
        }
        if (a.down)
            fprintf(stderr, "[host] auto input at %.1f s: %s\n", now,
                    a.click ? "click" : SDL_GetScancodeName(a.scancode));
        SDL_PushEvent(&e);
    }
    if (max_seconds > 0.0 && now >= max_seconds && !quit_sent) {
        quit_sent = true;
        fprintf(stderr, "[host] RECOMP_MAX_SECONDS=%g reached\n", max_seconds);
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }
}

// Drains the event queue, waiting up to `seconds` for the first one. Zero
// means take what is already there and return; a real wait is a genuine sleep
// inside SDL, which is what stops the host spinning while the guest waits.
// Returns true when something changed the input state.
int service(double seconds) {
    uint32_t before = host_input_notify_count();
    size_t queued_before = 0;
    {
        std::lock_guard<std::mutex> held(g_pending_input_m);
        queued_before = g_pending_input.size();
    }
    auto_input_tick();
    SDL_Event event;
    bool first = true;
    for (;;) {
        bool got;
        if (first && seconds > 0.0)
            got = SDL_WaitEventTimeout(&event, int(seconds * 1000.0));
        else
            got = SDL_PollEvent(&event);
        first = false;
        if (!got)
            break;
        handle_event(event);
    }
    // Input that was only QUEUED still counts as input having arrived: the
    // guest should re-check its condition and come back through the tick,
    // which is where the queue is applied.
    {
        std::lock_guard<std::mutex> held(g_pending_input_m);
        if (g_pending_input.size() != queued_before)
            return 1;
    }
    return host_idle_wait_result(before, host_input_notify_count());
}

// The drawable, its scale and its safe area in drawable pixels, for the
// on-screen controls.
controls::Screen controls_screen() {
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    controls::Screen s;
    s.dw = dw;
    s.dh = dh;
    s.scale = bw > 0 ? double(dw) / bw : 1.0;
    s.safe = {0, 0, dw, dh};
    SDL_Rect safe;
    if (g_window && SDL_GetWindowSafeArea(g_window, &safe))
        s.safe = {int(lround(safe.x * s.scale)), int(lround(safe.y * s.scale)),
                  int(lround(safe.w * s.scale)), int(lround(safe.h * s.scale))};
    // Portrait pins the game image below the top strip.
    host_present_set_safe_top(s.safe.y);
    return s;
}

// The window's form: phone-sized on a phone when its shorter side is under
// 600 points (the on-screen controls' tablet threshold, controls/
// layout_store.cpp form_for), else a tablet's, as a foldable iPhone's opened
// screen is. Followed every turn: the window changes size as it folds.
void note_phone_form() {
    int w = 0, h = 0;
    if (g_window)
        SDL_GetWindowSize(g_window, &w, &h);
    const bool phone = platform_ui_phone() && w > 0 && h > 0 && std::min(w, h) < 600;
    if (phone != g_phone_form.load()) {
        g_phone_form.store(phone);
        g_display_generation.fetch_add(1);
        fprintf(stderr, "[host] window %dx%d pt: %s form\n", w, h, phone ? "phone" : "tablet");
    }
    const PointRect game = game_rect_points();
    const PicturePoints p = {float(std::max(game.w, game.h)), float(std::min(game.w, game.h))};
    const PicturePoints old = g_picture_points.load();
    if (p.w != old.w || p.h != old.h) {
        g_picture_points.store(p);
        g_display_generation.fetch_add(1);
    }
}

// The window's safe area as fractions of the game picture's sides, for the
// game (host_display_safe_insets); read on the game's thread.
void publish_safe_insets() {
    note_phone_form();
    const bool phone = g_phone_form.load();
    int w = 0, h = 0;
    SDL_Rect safe{};
    SafeInsets s;
    const int side = phone ? platform_ui_cutout_side() : 0;
    bool corner = false;
    if (g_window && SDL_GetWindowSafeArea(g_window, &safe)) {
        SDL_GetWindowSize(g_window, &w, &h);
        const PointRect game = game_rect_points();
        if (w > 0 && h > 0 && game.w > 0 && game.h > 0) {
            double l = safe.x - game.x, t = safe.y - game.y;
            double r = (game.x + game.w) - (safe.x + safe.w);
            double b = (game.y + game.h) - (safe.y + safe.h);
            // A phone keeps clear only the side with the camera housing; the
            // other gets a narrow margin for the rounded corner, and the
            // home indicator's strip none (the game defers edge swipes). A
            // tablet-sized window (an iPad, a foldable opened) keeps the
            // system's safe area as it is.
            if (side) {
                static const double far = [] {
                    const char *v = recomp_env("SAFE_FAR_PT");
                    return v && *v ? std::clamp(atof(v), 0.0, 60.0) : 10.0;
                }();
                static double told[4] = {-1, -1, -1, -1};
                if (told[0] != l || told[1] != t || told[2] != r || told[3] != b) {
                    told[0] = l, told[1] = t, told[2] = r, told[3] = b;
                    fprintf(stderr, "[host] system safe area of the picture: %.0f,%.0f,%.0f,%.0f pt of %dx%d\n",
                            l, t, r, b, w, h);
                }
                (side < 0 ? r : l) = far;
                // A camera in a corner (a foldable's closed screen) hides a
                // corner of the picture, not a band down its side: that side
                // keeps the corner's margin only.
                corner = platform_ui_cutout_in_corner(w, h);
                if (corner)
                    l = r = far;
            }
            if (phone)
                b = 0;
            s.l = float(std::clamp(l / game.w, 0.0, 0.25));
            s.t = float(std::clamp(t / game.h, 0.0, 0.25));
            s.r = float(std::clamp(r / game.w, 0.0, 0.25));
            s.b = float(std::clamp(b / game.h, 0.0, 0.25));
        }
    }
    const SafeInsets old = safe_insets();
    if (old.l != s.l || old.t != s.t || old.r != s.r || old.b != s.b) {
        {
            std::lock_guard<std::mutex> held(g_safe_insets_m);
            g_safe_insets = s;
        }
        g_display_generation.fetch_add(1);
        fprintf(stderr, "[host] safe insets of the picture: %.3f,%.3f,%.3f,%.3f (housing %s%s)\n",
                s.l, s.t, s.r, s.b, side < 0 ? "left" : side > 0 ? "right" : "none",
                corner ? ", a camera in a corner" : "");
    }
}

// A command from a native screen's view. RECOMP_NATIVE_SCREEN_SAMPLE only
// logs it: there is no game screen behind the sample.
void native_screen_sample_command(const char *command) {
    fprintf(stderr, "[host] native screen command: %s\n", command);
}

// A screen the game asks the host to draw natively (recomp_screen_state), its
// document passed on to the platform's view whenever it changes.
// RECOMP_NATIVE_SCREEN_SAMPLE=<file> shows that document instead, to work on
// a view without the game's screen.
// A native screen is up (the game's undrawn screen beneath it): the system
// keyboard is the native view's, not the game's text input.
static bool g_native_screen_up = false;

void sync_native_screen() {
    static std::vector<char> buf(16384);
    static unsigned serial = 0;
    static bool &shown = g_native_screen_up;
    static const std::string sample = [] {
        const char *path = recomp_env("NATIVE_SCREEN_SAMPLE");
        std::string text;
        if (path && *path) {
            std::ifstream in(path, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            fprintf(stderr, "[host] native screen sample %s: %zu bytes\n", path, text.size());
        }
        return text;
    }();
    if (!sample.empty()) {
        if (!shown)
            platform_ui_native_screen(g_window, sample.c_str(), native_screen_sample_command);
        shown = true;
        return;
    }
    int n = recomp_screen_state(buf.data(), int(buf.size()), &serial);
    if (n >= int(buf.size())) {
        buf.resize(size_t(n) + 4096);
        n = recomp_screen_state(buf.data(), int(buf.size()), &serial);
    }
    if (n < 0 && shown) {
        fprintf(stderr, "[host] native screen gone\n");
        shown = false;
        // The same document may come back (a screen that stalled the game a
        // moment, a message box closed): it is shown again, not "unchanged".
        serial = 0;
        platform_ui_native_screen(g_window, nullptr, recomp_screen_command);
    } else if (n > 0 && n < int(buf.size())) {
        if (!shown)
            fprintf(stderr, "[host] native screen shown (%d bytes)\n", n);
        shown = true;
        platform_ui_native_screen(g_window, buf.data(), recomp_screen_command);
    }
}

// The housekeeping every turn does once the events are in.
void after_events() {
    publish_safe_insets();
    install_trackpad();
    update_prompt_text_input();
    {
        // Time-based gestures (a long press) fire from the clock, not an event,
        // and deferred clicks go out once the game's cursor has caught up and
        // the game has presented frames that sampled the press.
        std::vector<TouchAction> actions;
        g_touch.frames_presented(host_present_count());
        const int context = recomp_touch_context();
        g_touch.set_context(context);
        g_touch.tick(SDL_GetTicksNS(), &actions);
        push_touch_actions(actions);
        sync_touch_hover();
        // The system keyboard: four fingers toggle it, and it comes up by
        // itself while the cursor rests on one of the game's text fields
        // (RECOMP_TOUCH_TEXT), going once it leaves. The field's line is the
        // text area, which the system keeps above the keyboard.
        {
            static bool text_input = false;
            const bool want = (g_touch.text_input_wanted() || (context & RECOMP_TOUCH_TEXT)) &&
                              !g_native_screen_up;
            if (want != text_input && g_window) {
                text_input = want;
                if (want) {
                    double x = 0, y = 0;
                    g_touch.placed(&x, &y);
                    const PointRect game = game_rect_points();
                    const SDL_Rect area{0, int(game.y + y) - 24, int(game.w), 48};
                    SDL_SetTextInputArea(g_window, &area, 0);
                    SDL_StartTextInput(g_window);
                } else {
                    SDL_StopTextInput(g_window);
                }
            }
        }
        // The camera buttons over the 3D map, unless the launcher turned them off.
        static const bool camera_buttons = [] {
            int64_t on = 1;
            mods_settings_file_get("host.touch/camera_buttons", &on);
            return on != 0;
        }();
        platform_ui_camera_buttons(g_window,
                                   camera_buttons && (context & RECOMP_TOUCH_MAP_SCREEN),
                                   g_phone_form.load(), recomp_camera_step);
        sync_native_screen();
        // The controls follow the hardware keyboard (attached: key layouts
        // hidden), a physical controller (connected: pad layouts hidden and
        // its state fed to the pad) and the settings rows; the view is
        // published only when it changed.
        // The controls going away (a keyboard arriving, unless RECOMP_KEYPAD
        // forces them) lets go of every finger, as the keypad did.
        static const bool force = recomp_env("KEYPAD") != nullptr;
        static bool wanted = false;
        const bool absent = platform_ui_keypad_wanted();
        if (wanted && !(absent || force))
            touch_release_all();
        wanted = absent || force;
        const controls::Screen screen = controls_screen();
        const HostGameRect game = game_rect_for(screen.dw, screen.dh);
        controls::host_set_screen(screen, controls::Rect{game.x, game.y, game.w, game.h},
                                  screen.dh - (screen.safe.y + screen.safe.h));
        controls::gamepad_poll();
        controls::host_set_wanted(absent, controls::gamepad_connected());
        controls::host_pump(SDL_GetTicksNS());
        update_editor_text_input();
    }
    // A shell-launched process does not always come forward on its own, and a
    // window that never gained focus receives no key events at all. Ask again,
    // briefly, rather than once at startup and never afterwards.
    if (g_window && !window_focused() && boot_elapsed() < 3.0)
        SDL_RaiseWindow(g_window);
    note_focus(window_focused());

    // Capture follows the window and the page. Released whenever the window
    // is not focused, is minimised or is showing the settings page; taken back
    // by a click, not automatically.
    if (host_pointer_captured() && !pointer_capture_wanted())
        apply_pointer_capture(false);
    else if (!host_pointer_captured() && window_fullscreen() && pointer_capture_wanted() && !g_close_requested)
        apply_pointer_capture(true);
    update_platform_pointer_capture();
    // Fullscreen chrome can reroute or omit motion events at an edge.
    // Captured input owns the pointer: sample its current position even
    // without an event for our window, and deliver only changes.
    sample_captured_pointer();

    if (g_close_requested && !boot_close_requested())
        boot_request_close("the window was closed");
}

// Borderless is a desktop window: keep the entire drawable inside the usable
// area of its display, below the menu bar and outside the Dock.
void fit_borderless_window() {
    SDL_Rect usable;
    if (!g_window || !SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(g_window), &usable))
        return;
    g_borderless_frame_dirty = false;
    int x, y, w, h;
    SDL_GetWindowPosition(g_window, &x, &y);
    SDL_GetWindowSize(g_window, &w, &h);
    if (x == usable.x && y == usable.y && w == usable.w && h == usable.h)
        return;
    SDL_SetWindowPosition(g_window, usable.x, usable.y);
    SDL_SetWindowSize(g_window, usable.w, usable.h);
    post_drawable_size();
    fprintf(stderr, "[host] borderless usable frame: %d,%d %dx%d points\n", usable.x, usable.y,
            usable.w, usable.h);
}

// Consume a posted setting on the main thread. Fullscreen completes through
// window events; another setting can supersede it meanwhile.
void apply_window_mode() {
    int request = host_display_take_window();
    if (request >= 0)
        g_wanted_window_mode = request;
    if (!g_window || g_fullscreen_transition)
        return;
    if (window_fullscreen()) {
        if (g_wanted_window_mode != 2) {
            g_fullscreen_transition = true;
            update_platform_pointer_capture();
            SDL_SetWindowFullscreen(g_window, false);
        }
        return;
    }
    if (g_window_mode == g_wanted_window_mode) {
        if (g_window_mode == 1 && g_borderless_frame_dirty)
            fit_borderless_window();
        return;
    }
    if (g_window_mode == 0) {
        SDL_GetWindowPosition(g_window, &g_windowed_x, &g_windowed_y);
        SDL_GetWindowSize(g_window, &g_windowed_w, &g_windowed_h);
    }
    if (g_wanted_window_mode == 1) {
        SDL_SetWindowBordered(g_window, false);
        g_window_mode = 1;
        fit_borderless_window();
    } else {
        SDL_SetWindowBordered(g_window, true);
        if (g_windowed_w > 0) {
            SDL_SetWindowSize(g_window, g_windowed_w, g_windowed_h);
            SDL_SetWindowPosition(g_window, g_windowed_x, g_windowed_y);
        }
        g_window_mode = 0;
        if (g_wanted_window_mode == 2) {
            g_fullscreen_transition = true;
            update_platform_pointer_capture();
            // Borderless desktop fullscreen: no display mode change.
            SDL_SetWindowFullscreenMode(g_window, nullptr);
            SDL_SetWindowFullscreen(g_window, true);
        }
    }
    post_drawable_size();
}

// The display mode changed under us, which resizes the window.
void apply_mode_change() {
    if (!g_mode_dirty)
        return;
    g_mode_dirty = false;
    host_pointer_set_mode((int)g_pending_mode_w, (int)g_pending_mode_h);
    g_mode_w = (int)g_pending_mode_w;
    g_mode_h = (int)g_pending_mode_h;
    const HostWindowSize size = window_size_for(g_mode_w, g_mode_h);
    // The minimum first, since a window cannot shrink below the one it had. It
    // is the guest's frame when that fits the screen, so integer scaling always
    // has a whole multiple to take, and never more than the window.
    SDL_SetWindowMinimumSize(g_window, size.min_w, size.min_h);
    if (g_window_mode == 0 && !g_fullscreen_transition) {
        SDL_SetWindowSize(g_window, size.w, size.h);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        post_drawable_size();
    }
    host_set_client_size(host_main_window(), g_mode_w, g_mode_h);
}

// True when this thread may touch the window at all.
bool on_host_thread() {
    if (boot_on_run_thread() && SDL_IsMainThread())
        return true;
    static bool warned = false;
    if (!warned) {
        warned = true;
        fprintf(stderr, "[host] a guest worker thread reached the host; the window is "
                        "serviced only on the main thread\n");
    }
    return false;
}

// One turn of the host's event loop, called from inside guest code.
//
// The tick can arrive on any guest thread: the runtime's cooperative scheduler
// hands the baton to real pthreads and whichever one holds it reads the clock.
// The window belongs to the thread that called boot_run, so a tick on any
// other thread does nothing at all and hands control straight back.
// A touch device's game follows its screen: when the window's size changes (a
// foldable folded or opened) and stays so for a moment, the guest's main
// window gets the new size, landscape, and a game that sizes its back buffer
// by its client area resets its device to it (Heroes V's frame check
// 005fdb20: GetClientRect against the back buffer, then gfx_resolution and
// Reset). Until then the presenter fits the old frame to the new screen. Only
// on a change of the screen: a resolution the player picked stays until the
// next one. RECOMP_FOLLOW_SCREEN=0 leaves the window alone. Under the baton.
void follow_screen() {
    if (!platform_ui_touch_device())
        return;
    static const bool allowed = [] {
        const char *v = recomp_env("FOLLOW_SCREEN");
        return !(v && v[0] == '0');
    }();
    const uint32_t main_window = host_main_window();
    const int w = g_fullscreen_w.load(), h = g_fullscreen_h.load();
    if (!allowed || !main_window || w <= 0 || h <= 0)
        return;
    const int lw = std::max(w, h), lh = std::min(w, h);
    static int known_w = 0, known_h = 0, want_w = 0, want_h = 0;
    static uint64_t since = 0;
    if (!known_w) { // the screen the game started on
        known_w = lw;
        known_h = lh;
        return;
    }
    // Not while the app is away or has just returned: the sizes a locking or
    // switching system hands the scene then are transient, and a game reset to
    // one of them would be saved as the screen's resolution. The size is taken
    // again, settled, a second after the app is back.
    if (!g_app_active.load() || os_monotonic_ns() - g_foreground_ns.load() < 1000000000ull) {
        want_w = want_h = 0;
        return;
    }
    if (lw == known_w && lh == known_h) {
        want_w = want_h = 0;
        return;
    }
    const uint64_t now = os_monotonic_ns();
    if (lw != want_w || lh != want_h) { // settled only once it stops changing
        want_w = lw;
        want_h = lh;
        since = now;
        return;
    }
    if (now - since < 400000000ull)
        return;
    known_w = lw;
    known_h = lh;
    want_w = want_h = 0;
    fprintf(stderr, "[host] the screen is %dx%d now: the game's window follows\n", lw, lh);
    host_set_client_size(main_window, lw, lh);
    // As Windows tells a window it was resized: a game that lays itself out on
    // WM_SIZE does so before its frame check resets the device.
    post(0x0005 /* WM_SIZE */, 0 /* SIZE_RESTORED */, (uint32_t(lh) << 16) | (uint32_t(lw) & 0xffff));
}

void pump() {
    if (!on_host_thread())
        return;
    // First: anything an idle slice decoded but could not apply. This thread
    // reached here through the guest's own clock read, so it holds the baton
    // and a mod callback dispatched from here is serialised against every
    // other guest thread, which is the whole point.
    deliver_pending_input();
    apply_window_mode();
    apply_mode_change();
    follow_screen();
    service(0.0);
    after_events();
    update_idle_cursor();
    host_gate_pointer_tick();
}

// The runtime's idle wait, called on the run thread when the guest is about to
// block. Returns 1 when input arrived, so the caller can re-check its own
// condition at once.
// The runtime's deadline ticker (any thread): end an SDL event wait now, because a
// guest deadline fell due during it. A user event with this code is otherwise ignored.
constexpr Sint32 kSchedWakeEvent = 0x5ced;
void sdl_idle_wake() {
    SDL_Event e{};
    e.type = SDL_EVENT_USER;
    e.user.code = kSchedWakeEvent;
    SDL_PushEvent(&e);
}

int idle_wait(double seconds) {
    if (!on_host_thread())
        return 0;
    if (seconds < 0.0)
        seconds = 0.0;
    apply_window_mode();
    apply_mode_change();
    int changed = service(seconds);
    after_events();
    update_idle_cursor();
    return changed;
}

void on_mode_change(int w, int h, int bpp) {
    (void)bpp;
    if (w <= 0 || h <= 0)
        return;
    g_pending_mode_w = (uint32_t)w;
    g_pending_mode_h = (uint32_t)h;
    g_mode_dirty = true;
}

// The watchdog takes the report lock before calling this and main takes it
// around its own call, so everything below reads a consistent snapshot.
void report(FILE *out, bool abnormal) {
    fprintf(out, "\n== windowed run ==\n");
    fprintf(out, "stopped:            %s\n", boot_stop_reason());
    fprintf(out, "elapsed:            %.1fs\n", boot_elapsed());
    fprintf(out, "presented frames:   %u\n", host_present_count());
    // Per display mode, because a rate averaged over a whole run mixes the
    // menu and the loading screen in with gameplay.
    for (int i = 0; i < host_present_rate_count(); ++i) {
        HostPresentRate r;
        memset(&r, 0, sizeof r);
        host_present_rate(i, &r);
        fprintf(out, "  %4dx%-4d %2dbpp:  %u guest presents; %.1fs at this mode\n", r.w, r.h, r.bpp,
                r.presents, r.seconds);
    }
    // The two lines the display baseline is made of, in the same words the
    // smoke host uses, because display_compare.py parses the text.
    {
        char line[768];
        if (host_stats_gameplay_line(line, sizeof line))
            fprintf(out, "gameplay: %s\n", line);
        if (host_stats_access_line(line, sizeof line))
            fprintf(out, "%s\n", line);
    }
    fprintf(out, "input changes:      %u announced to the DirectInput shim\n",
            host_input_notify_count());
    fprintf(out,
            "Direct3D:           %u draws, %u textures, %u write-backs into the "
            "render target\n",
            host_d3d_total_draws(), host_d3d_total_textures(), host_d3d_total_flushes());
    // What the mixer actually produced, when the run was asked to capture it.
    host_capture_print(out);
    boot_print_dx_objects(out);
    boot_print_exit_code(out);
    boot_print_undeliverable(out);
    fflush(out);
    boot_print_import_stats(out, abnormal);
}

// The native launcher is the app's front door on every interactive start.
// Runs that name the game themselves (--exe, RECOMP_EXE) or drive it
// unattended skip it unless the launcher was asked for; RECOMP_LAUNCHER=0
// skips it always.
// The launcher's settings (LauncherModel.swift) a game module takes over from
// the app that started it, and its startup-video choice: an empty movie list
// with this marker in the profile's video/ (LauncherModel.setSkipIntro).
[[maybe_unused]] static const char *const kLauncherKeys[] = {
    "host.display/frame_limit", "host.display/energy_saver", "host.display/idle_frames",
    "host.display/render_quality", "host.touch/camera_buttons", "host.input/trackpad_camera",
    "host.display/metal_hud", "homm5.perf/gpu_skin", "host.touch/battle_cells",
    "host.ui/phone_interface", "host.audio/silent_mode", "homm5.wide/ui_bounds", "host.ui/native_menus"};
static const char kIntroMarker[] = "<!-- launcher: skip the startup videos -->";
static const char *const kIntroFiles[] = {"video/intro.xml", "video/intro_a1.xml"};

[[maybe_unused]] static bool intro_skipped(const std::string &profile) {
    FILE *f = fopen((profile + "/" + kIntroFiles[0]).c_str(), "rb");
    if (!f)
        return false;
    char buf[512] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return strstr(buf, kIntroMarker) != nullptr;
}

#if defined(RECOMP_GAME_MODULES)
// A multi-game app (RECOMP_GAME_MODULES): the other games' modules sit in the
// app's Frameworks as libgame.<id>.dylib, on Linux and Windows in modules/
// beside the executable as libgame.<id>.so (.dll) (host/game_modules.h,
// tools/assemble_games.py).
static std::string modules_dir() {
    char path[4096];
    if (os_exe_path(path, sizeof path) != 0)
        return "";
    return game_modules_dir(kModuleLayout, path);
}

// This game's id and those of the modules beside it, for the launcher.
static std::string available_modules() {
    std::string list = RECOMP_GAME_ID;
    const std::string dir = modules_dir();
    std::vector<std::string> names;
    if (!dir.empty())
        os_listdir(
            dir.c_str(),
            [](const char *name, void *user) {
                static_cast<std::vector<std::string> *>(user)->push_back(name);
                return 0;
            },
            &names);
    for (const std::string &id : game_module_ids(kModuleLayout, names))
        list += "," + id;
    return list;
}

// Hands the process to another game's module: this app's window and GPU device
// go (the module brings up its own on the same SDL), then the module's
// recomp_game_main runs the chosen executable. Only returns when that game does.
static int run_game_module(const launcher::NativeChoice &choice, const char *argv0) {
    const std::string path = modules_dir() + "/" + game_module_file(kModuleLayout, choice.module);
    void *module = os_dlopen(path.c_str());
    if (!module) {
        fprintf(stderr, RECOMP_APP_NAME ": cannot load %s: %s\n", path.c_str(), os_dlerror());
        return 3;
    }
    using Entry = int (*)(int, char **);
    const Entry entry = reinterpret_cast<Entry>(os_dlsym(module, "recomp_game_main"));
    if (!entry) {
        fprintf(stderr, RECOMP_APP_NAME ": %s has no recomp_game_main\n", path.c_str());
        return 3;
    }
    if (g_surface) {
        gpu::release_window_surface(g_surface);
        g_surface = nullptr;
    }
    g_gpu.reset();
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }
    if (!choice.root.empty())
        os_setenv("RECOMP_GAME_ROOT", choice.root.c_str());
    // The launcher's choices live in this app's profile; the module keeps its
    // own, so they go along (module_take_launcher_choices).
    std::string settings;
    for (const char *key : kLauncherKeys) {
        int64_t v = 0;
        if (mods_settings_file_get(key, &v))
            settings += std::string(key) + "=" + std::to_string(v) + ";";
    }
    os_setenv("RECOMP_LAUNCHER_SETTINGS", settings.c_str());
    os_setenv("RECOMP_LAUNCHER_SKIP_INTRO", intro_skipped(host_layout().profile_dir) ? "1" : "0");
    fprintf(stderr, "[host] %s goes to module %s\n", choice.game.c_str(), path.c_str());
    std::string exe = choice.exe;
    char flag[] = "--exe";
    char *args[] = {const_cast<char *>(argv0), flag, exe.data(), nullptr};
    return entry(3, args);
}
#endif

#if defined(RECOMP_GAME_MODULE)
// A module finds its resources (Resources/games/<app name> in the app that
// loaded it, beside the shared ones) and keeps its own profile, so two games
// in one app never share settings, saves or the emulated registry.
#if defined(RECOMP_GAME_MODULE)
static void module_take_launcher_choices() {
    if (const char *s = recomp_env("LAUNCHER_SETTINGS"); s && *s) {
        const std::string all = s;
        size_t at = 0;
        while (at < all.size()) {
            size_t end = all.find(';', at);
            if (end == std::string::npos)
                end = all.size();
            const std::string rec = all.substr(at, end - at);
            at = end + 1;
            const size_t eq = rec.find('=');
            if (eq != std::string::npos)
                mods_settings_file_set(rec.substr(0, eq).c_str(), atoll(rec.c_str() + eq + 1));
        }
    }
    const char *skip = recomp_env("LAUNCHER_SKIP_INTRO");
    if (!skip || !*skip)
        return;
    const std::string profile = host_layout().profile_dir;
    const bool want = strcmp(skip, "1") == 0;
    if (want == intro_skipped(profile))
        return;
    for (const char *name : kIntroFiles) {
        const std::string path = profile + "/" + name;
        if (want) {
            os_mkdir(profile.c_str());
            os_mkdir((profile + "/video").c_str());
            if (FILE *f = fopen(path.c_str(), "wb")) {
                fprintf(f, "<?xml version=\"1.0\"?>\n%s\n<Base>\n\t<Movies/>\n</Base>\n", kIntroMarker);
                fclose(f);
            }
        } else {
            FILE *f = fopen(path.c_str(), "rb");
            char buf[512] = {};
            if (f) {
                fread(buf, 1, sizeof buf - 1, f);
                fclose(f);
            }
            if (strstr(buf, kIntroMarker))
                remove(path.c_str()); // only the launcher's own empty list
        }
    }
}
#endif

static void module_prepare() {
    char self[4096];
    if (os_module_path(reinterpret_cast<const void *>(&module_prepare), self, sizeof self) == 0) {
        for (const std::string &dir :
             game_module_resource_dirs(kModuleLayout, self, RECOMP_APP_NAME)) {
            OsStat st{};
            if (os_stat(dir.c_str(), &st) == 0 && st.is_dir) {
                os_setenv("RECOMP_RESOURCES_DIR", dir.c_str());
                break;
            }
        }
    }
    if (const char *profile = recomp_env("PROFILE_DIR"); !profile || !*profile) {
        host_migrate_own_user_data(); // the module's own former names
        char buf[4096];
        if (os_user_data_dir(RECOMP_APP_NAME, buf, sizeof buf) == 0)
            os_setenv("RECOMP_PROFILE_DIR", buf);
    }
    os_setenv("RECOMP_MODULE", "1"); // runtime/layout.cpp: no developer checkout
}
#endif

// The settings a player starts with, per device (2026-10-21): written into
// the profile's settings file for every key it does not hold yet, before the
// launcher reads them, so the launcher and the game agree and a choice the
// player made is never touched. -1: no default there (the key's own applies).
// Unattended runs (RECOMP_AUTO_KEYS, _MAX_SECONDS, _MAX_FRAMES: benchmarks,
// tests) and RECOMP_SETTINGS_DEFAULTS=0 leave the profile as it is.
static void seed_default_settings() {
    for (const char *v : {"RECOMP_AUTO_KEYS", "RECOMP_MAX_SECONDS", "RECOMP_MAX_FRAMES"})
        if (const char *e = getenv(v); e && *e)
            return;
    if (const char *e = getenv("RECOMP_SETTINGS_DEFAULTS"); e && e[0] == '0')
        return;
    struct Default {
        const char *key;
        int64_t mac, pad, phone;
    };
    // Encodings: mods/settings_file.h and the launcher (LauncherModel.swift).
    // The desktop column is a Mac's on Apple, a Linux machine's otherwise:
    // 40 fps spares a MacBook's battery, a Linux desktop gets its display's 60.
#ifdef __APPLE__
    const int64_t desktop_frame_limit = 1; // 40 fps
#else
    const int64_t desktop_frame_limit = 2; // 60 fps
#endif
    static const Default table[] = {
        {"host.display/frame_limit", desktop_frame_limit, 1, 1},
        {"host.display/energy_saver", 2, 2, 2},      // never
        {"host.display/render_quality", 4, 3, 4},    // performance; iPad balanced
        {"host.display/idle_frames", 1, 1, 1},       // 30 fps after a minute idle: on
        {"host.display/metal_hud", 0, 0, 0},         // off
        {"homm5.perf/gpu_skin", 1, -1, -1},          // animated models on the GPU
        {"homm5.wide/ui_bounds", 2, -1, -1},         // interface within 16:9
        {"host.input/trackpad_camera", 0, -1, -1},   // two fingers: height
        {"host.touch/camera_buttons", -1, 0, 0},     // hidden
        {"host.touch/battle_cells", -1, 1, 1},       // touch-friendly battle: on
        {"host.audio/silent_mode", -1, 0, 0},        // respect the silent switch
        {"host.ui/phone_interface", -1, 0, 1},       // iPad never, iPhone automatic
        {"host.ui/native_menus", -1, 2, 1},          // native menus: iPhone on, iPad off
    };
    const int device = !platform_ui_touch_device() ? 0 : platform_ui_phone() ? 2 : 1;
    for (const Default &d : table) {
        const int64_t value = device == 0 ? d.mac : device == 1 ? d.pad : d.phone;
        int64_t have = 0;
        if (value >= 0 && !mods_settings_file_get(d.key, &have))
            mods_settings_file_set(d.key, value);
    }
}

static bool native_launcher_wanted(const GamePath &game, bool asked) {
    if (const char *v = recomp_env("LAUNCHER"); v && strcmp(v, "0") == 0)
        return false;
    if (asked)
        return true;
    if (game.source == GamePathSource::Flag || game.source == GamePathSource::Environment)
        return false;
    for (const char *name : {"AUTO_KEYS", "MAX_SECONDS", "MAX_FRAMES", "LAUNCHER_KEYS"})
        if (const char *v = recomp_env(name); v && *v)
            return false;
    return true;
}

// Where saves live, for the launcher's Saves button: the profile's
// Documents/My Games when the game made one, else the profile itself.
static std::string saves_dir() {
    const std::string profile = host_layout().profile_dir;
    OsStat st{};
    const std::string games = profile + "/Documents/My Games";
    return os_stat(games.c_str(), &st) == 0 && st.is_dir ? games : profile;
}

static bool has_required_dirs(const std::string &dir) {
    static const char *const required[] = RECOMP_REQUIRED_DIRS;
    if (!required[0])
        return false;
    for (const char *const *name = required; *name; ++name) {
        OsStat st{};
        if (os_stat((dir + "/" + *name).c_str(), &st) != 0 || !st.is_dir)
            return false;
    }
    return true;
}

// The install root the guest root maps onto (runtime/loader.cpp reads
// RECOMP_GAME_ROOT): the launcher's choice, else the folder above the EXE
// when that, not the EXE's own folder, holds game.toml's required folders - a
// GOG install keeps its EXE in bin/ beside data/.
static void apply_game_root(const std::string &exe, const std::string &chosen_root) {
    if (const char *set = recomp_env("GAME_ROOT"); set && *set)
        return;
    const size_t cut = exe.find_last_of('/');
    if (cut == std::string::npos)
        return;
    const std::string dir = exe.substr(0, cut);
    std::string root = chosen_root;
    if (root.empty()) {
        const size_t up = dir.find_last_of('/');
        if (up != std::string::npos && up > 0 && !has_required_dirs(dir) && has_required_dirs(dir.substr(0, up)))
            root = dir.substr(0, up);
    }
    if (!root.empty() && root != dir && dir.compare(0, root.size() + 1, root + "/") == 0) {
        os_setenv("RECOMP_GAME_ROOT", root.c_str());
        fprintf(stderr, "[host] game root %s\n", root.c_str());
    }
}

#if defined(__APPLE__) && TARGET_OS_IPHONE
// game.toml [ios] cfg_defaults: a touch device's own starting settings, set in
// the game's config files ("setvar key = value") of the imported copy before
// the game reads them. Only those lines change, and only when they differ.
static void apply_cfg_defaults(const std::string &exe) {
    const char *root_env = recomp_env("GAME_ROOT");
    std::string root = root_env && *root_env ? root_env : exe.substr(0, exe.find_last_of('/'));
    const std::string records = RECOMP_IOS_CFG_DEFAULTS;
    size_t at = 0;
    while (at < records.size()) {
        size_t end = records.find(';', at);
        const std::string rec = records.substr(at, end - at);
        at = end == std::string::npos ? records.size() : end + 1;
        const size_t t1 = rec.find('|'), t2 = rec.find('|', t1 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos)
            continue;
        const std::string path = root + "/" + rec.substr(0, t1), key = rec.substr(t1 + 1, t2 - t1 - 1);
        std::string value = rec.substr(t2 + 1);
        if (value == "screen") {
            // The screen's own size, landscape (the mode the D3D9 list offers):
            // the interface is drawn at it, the 3D at the share the render
            // quality asks for (mods_render_fraction), upscaled by MetalFX.
            int dw = 0, dh = 0;
            if (!host_display_fullscreen_size(&dw, &dh) || dw <= 0 || dh <= 0) {
                // Never leave a resolution the game saved earlier standing:
                // the window's own size, else the display's.
                if (g_window)
                    SDL_GetWindowSizeInPixels(g_window, &dw, &dh);
                if (dw <= 0 || dh <= 0) {
                    const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
                    if (mode) {
                        const double density = mode->pixel_density > 0 ? mode->pixel_density : 1.0;
                        dw = int(std::lround(mode->w * density));
                        dh = int(std::lround(mode->h * density));
                    }
                }
                if (dw <= 0 || dh <= 0)
                    continue;
            }
            int32_t w = 0, h = 0;
            host_display_screen_mode(dw, dh, std::min(dw, dh), &w, &h);
            value = std::to_string(w) + "x" + std::to_string(h);
        }
        FILE *f = fopen(path.c_str(), "rb");
        if (!f)
            continue;
        std::string text;
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;)
            text.append(buf, n);
        fclose(f);
        const std::string want = "setvar " + key + " = " + value;
        std::string out;
        bool found = false, changed = false;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find('\n', pos);
            std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? text.size() : nl + 1;
            const bool cr = !line.empty() && line.back() == '\r';
            if (cr)
                line.pop_back();
            if (line.rfind("setvar " + key + " ", 0) == 0) {
                found = true;
                if (line != want) {
                    line = want;
                    changed = true;
                }
            }
            out += line + (cr ? "\r" : "") + (nl == std::string::npos ? "" : "\n");
        }
        if (!found) {
            out += want + "\r\n";
            changed = true;
        }
        if (changed && (f = fopen(path.c_str(), "wb"))) {
            fwrite(out.data(), 1, out.size(), f);
            fclose(f);
            fprintf(stderr, "[ios] %s: %s\n", rec.substr(0, t1).c_str(), want.c_str());
        }
    }
}
#endif

// The .app can be started from anywhere, and the guest's file system is rooted
// at the directory holding the EXE. Walking up from the executable finds the
// checkout whichever way the app was launched.
std::string classic_modes_path() {
    std::string p = host_resource("classic-modes.json");
    return p.empty() ? "tools/recomp/baseline/classic-modes.json" : p;
}

void post_drawable_size() {
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    static int last_w = 0, last_h = 0;
    if (dw <= 0 || dh <= 0 || (last_w == dw && last_h == dh))
        return;
    // A flip between portrait and landscape (a phone rotating) moves every
    // touch control out from under whatever fingers were holding it.
    if (last_w > 0 && last_h > 0 && (last_w > last_h) != (dw > dh))
        touch_release_all();
    last_w = dw;
    last_h = dh;
    host_present_resize(dw, dh);
}

} // namespace

// ---------------------------------------------------------------------------

#ifdef __EMSCRIPTEN__
// ---------------------------------------------------------------------------
// The web. The browser's main thread may not block, and it alone owns the
// canvas, the input events and WebGPU, so the game runs on a thread of its
// own and the main thread drives everything else from requestAnimationFrame:
// events into the input queue, the game's queued Direct3D 9 work onto the
// GPU, and one presenter turn. The page (tools/web) imports the game into the
// origin-private file system and passes its path as --exe.
// ---------------------------------------------------------------------------
namespace {

std::string g_web_exe;
std::atomic<bool> g_web_guest_done{false};
int g_web_dw = 0, g_web_dh = 0;

void web_tick() {
    deliver_pending_input();
}

int web_idle_wait(double seconds) {
    {
        std::lock_guard<std::mutex> held(g_pending_input_m);
        if (!g_pending_input.empty())
            return 1;
    }
    const double wait = std::min(std::max(seconds, 0.0), 0.004);
    if (wait > 0)
        os_sleep_us((uint64_t)(wait * 1e6));
    std::lock_guard<std::mutex> held(g_pending_input_m);
    return g_pending_input.empty() ? 0 : 1;
}

void web_frame() {
    host_d9_frame(true);
    service(0.0);
    after_events();
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    if (dw > 0 && dh > 0 && (dw != g_web_dw || dh != g_web_dh)) {
        g_web_dw = dw;
        g_web_dh = dh;
        host_present_resize(dw, dh);
        host_display_set_screen(dw, dh);
    }
    host_d9_pump();
    host_present_pump();
    host_d9_frame(false);
    if (g_web_guest_done.load())
        emscripten_cancel_main_loop();
}

// The game's thread: the game's files are read from here, never from the
// main thread, which may not wait for the file system.
void web_guest() {
    backend_t opfs = wasmfs_create_opfs_backend();
    if (!opfs || wasmfs_create_directory("/opfs", 0777, opfs) != 0)
        fprintf(stderr, RECOMP_APP_NAME ": the browser's private file system is unavailable\n");
    std::string dir = g_web_exe.substr(0, g_web_exe.find_last_of('/'));
    if (!dir.empty() && os_chdir(dir.c_str()) != 0)
        fprintf(stderr, "[host] could not enter %s\n", dir.c_str());
    BootOptions options;
    options.name = "web";
    options.exe = g_web_exe.c_str();
    options.activate = true;
    options.tick = web_tick;
    sched_set_input_queue(input_pending, drain_input);
    options.idle_wait = web_idle_wait;
    options.report = report;
    options.deadline_seconds = 0.0;
    if (!boot_load(options)) {
        fprintf(stderr, RECOMP_APP_NAME ": %s\n", loader_error());
        g_web_guest_done.store(true);
        return;
    }
    printf(RECOMP_APP_NAME ": %s, entry %08x\n", loader_exe_path().c_str(), loader_entry_point());
    fflush(stdout);
    host_midi_startup(win32_midi_soundfont_path().c_str());
    boot_run();
    g_web_guest_done.store(true);
}

int web_main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--exe=", 6) == 0)
            g_web_exe = argv[i] + 6;
        else if (strcmp(argv[i], "--exe") == 0 && i + 1 < argc)
            g_web_exe = argv[++i];
    }
    if (g_web_exe.empty()) {
        fprintf(stderr, RECOMP_APP_NAME ": the page gave no --exe\n");
        return 2;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, RECOMP_APP_NAME ": SDL_Init failed: %s\n", SDL_GetError());
        return 3;
    }
    g_gpu = gpu::create_default_device();
    if (!g_gpu) {
        fprintf(stderr, RECOMP_APP_NAME ": this browser gave the page no WebGPU device\n");
        return 3;
    }
    g_window = SDL_CreateWindow(RECOMP_GAME_NAME, g_mode_w, g_mode_h,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) {
        fprintf(stderr, RECOMP_APP_NAME ": SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 3;
    }
    g_surface = gpu::native_surface_for_window(g_window);
    fprintf(stderr, "GPU backend: %s\n", gpu::default_backend_name());
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    if (dw <= 0 || dh <= 0) {
        dw = 1280;
        dh = 720;
    }
    g_web_dw = dw;
    g_web_dh = dh;
    host_display_set_screen(dw, dh);

    D3DRenderer *renderer = new D3DRenderer(g_gpu.get());
    if (!renderer->ok())
        return 3;
    D3DRenderer::setShared(renderer);
    host_present_set_device(g_gpu.get());
    host_present_set_sched_probe(recomp_sched_main_totals);
    host_set_idle_wake(sdl_idle_wake);
    mods_display_live_defaults();
    mods_display_default_overlay(platform_ui_default_overlay());
    mods_display_load_modes(classic_modes_path().c_str());
    host_present_on_mode_change(on_mode_change);
    host_input_set_notify(dinput_host_input_changed);
    host_present_start(g_surface, dw, dh);
    if (!host_d9_web_start(g_gpu.get()))
        fprintf(stderr,
                RECOMP_APP_NAME ": no WebGPU Direct3D 9 renderer; the game draws on the CPU\n");

    const char *capture = recomp_env("HOST_AUDIO_CAPTURE");
    if (capture && *capture)
        host_audio_capture_begin(capture);

    std::thread(web_guest).detach();
    emscripten_set_main_loop(web_frame, 0, false);
    return 0; // the runtime stays alive for the main loop and the game's thread
}

} // namespace
#endif

#if defined(RECOMP_GAME_MODULE)
static int host_main(int argc, char **argv);
// The module's one export: by visibility in ELF and Mach-O (sdl/game_module.map),
// by dllexport in PE, which also keeps MinGW from exporting everything else.
#ifdef _WIN32
#define RECOMP_GAME_EXPORT __declspec(dllexport)
#else
#define RECOMP_GAME_EXPORT __attribute__((visibility("default")))
#endif
extern "C" RECOMP_GAME_EXPORT int recomp_game_main(int argc, char **argv) {
    module_prepare();
    module_take_launcher_choices();
    return host_main(argc, argv);
}
#define main host_main
#endif

int main(int argc, char **argv) {
#ifdef __EMSCRIPTEN__
    return web_main(argc, argv);
#endif
    os_attach_parent_console();
#ifdef _WIN32
    // The window and the taskbar take the executable's icon (resource 1).
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
#endif
    // --version and --probe-layout answer before SDL or the GPU come up, so a
    // packaged build can be checked on a machine with neither a display nor
    // the game.
    const char *exe_flag = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--version") == 0) {
            printf(RECOMP_APP_NAME " %s (%s)\n", POP_RECOMP_VERSION, gpu::default_backend_name());
            return 0;
        }
        if (strcmp(argv[i], "--probe-layout") == 0) {
            const HostLayout &l = host_layout();
            printf("resources_dir=%s\nprofile_dir=%s\ndeveloper=%d\n", l.resources_dir.c_str(),
                   l.profile_dir.c_str(), int(l.developer));
            return 0;
        }
        if (strcmp(argv[i], "--exe") == 0 && i + 1 < argc)
            exe_flag = argv[++i];
        else if (strncmp(argv[i], "--exe=", 6) == 0)
            exe_flag = argv[i] + 6;
        else if (strcmp(argv[i], "--launcher") == 0)
            continue; // launcher::requested reads it
        else {
            fprintf(stderr, "usage: " RECOMP_APP_NAME " [--exe <" RECOMP_EXECUTABLE
                            ">] [--launcher] [--version] [--probe-layout]\n");
            return 2;
        }
    }
#ifndef __ANDROID__
    // A renamed app takes its former user-data folder before anything (the
    // launcher, the saved game path, the settings) reads the profile.
    host_migrate_own_user_data();
#endif
    std::string game_error;
#ifdef __ANDROID__
    platform_ui_init_hints(); // stdout and stderr reach logcat from here on
    (void)exe_flag;           // The mobile app always uses its own external data root.
    // SDL's Java glue has initialized the app-specific external files path
    // before SDL_main. Resolve it before host_layout caches a profile path.
    const char *external = SDL_GetAndroidExternalStoragePath();
    GamePath game;
    if (!external || !*external) {
        game_error =
            "Android external files storage is unavailable: " + std::string(SDL_GetError());
    } else {
        const std::string data_root = external;
        recomp_env_apply_file((data_root + "/switches.txt").c_str());
        const char *profile = recomp_env("PROFILE_DIR");
        if (!profile || !*profile)
            os_setenv("RECOMP_PROFILE_DIR", (data_root + "/profile").c_str());
        // No executable path can lead to this app's resources (the process is
        // the system's app_process), so name the data root the activity
        // unpacked the APK's assets into: host_resource("controls") is
        // <data root>/controls. A switches.txt override still wins.
        const char *resources = recomp_env("RESOURCES_DIR");
        if (!resources || !*resources)
            os_setenv("RECOMP_RESOURCES_DIR", data_root.c_str());
        game = game_path_resolve(nullptr, data_root.c_str());
        // Missing data is the launcher's to explain and import.
        if (!game.exe.empty())
            SDL_Log("[android] game data: %s", game.exe.c_str());
    }
#else
    GamePath game = platform_ui_resolve_game(exe_flag, &game_error);
#endif
    if (!game_error.empty()) {
#ifdef __ANDROID__
        // The kit page needs a running presenter and initialized mod rows;
        // before boot, log the actionable path instead of opening a picker.
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", game_error.c_str());
        SDL_Quit();
        platform_ui_process_exit(2);
#else
        // Before SDL_Init on purpose: the message box brings up what it needs,
        // and on a device with no console this is the only place the reason shows.
        fprintf(stderr, RECOMP_APP_NAME ": %s\n", game_error.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, RECOMP_APP_NAME, game_error.c_str(),
                                 nullptr);
#endif
        return 2;
    }
    if (game.source == GamePathSource::Checkout &&
        os_chdir(host_layout().checkout_root.c_str()) != 0)
        fprintf(stderr, "[host] could not enter %s\n", host_layout().checkout_root.c_str());

#if defined(__APPLE__)
    // The launcher's Metal HUD box (host.display/metal_hud; RECOMP_METAL_HUD=0|1
    // overrides it): Apple's performance HUD over the game. Metal loads the HUD
    // only when MTL_HUD_ENABLED is set before its first device, so a change in
    // the launcher shows from the next start (LauncherModel.metalHudPending).
    if (!getenv("MTL_HUD_ENABLED")) {
        int64_t hud = 0;
        mods_settings_file_get("host.display/metal_hud", &hud);
        if (const char *force = recomp_env("METAL_HUD"); force && *force)
            hud = strcmp(force, "0") != 0;
        if (hud) {
            os_setenv("MTL_HUD_ENABLED", "1");
            fprintf(stderr, "[host] Metal HUD on\n");
        }
    }
#endif
    // The launcher's "Touch-friendly battle" (host.touch/battle_cells): a game's
    // own way to choose a target and then the cell to strike it from
    // (runtime/native_seam.h RECOMP_TOUCH_BATTLE_DIRECT). On by default on an
    // iPhone or iPad, off elsewhere. The game's native code reads the variable;
    // one set already (RECOMP_TOUCH_BATTLE_CELLS=0|1) wins.
    if (!getenv("RECOMP_TOUCH_BATTLE_CELLS")) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
        int64_t cells = 1;
#else
        int64_t cells = 0;
#endif
        mods_settings_file_get("host.touch/battle_cells", &cells);
        os_setenv("RECOMP_TOUCH_BATTLE_CELLS", cells ? "1" : "0");
    }
    platform_ui_init_hints();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, RECOMP_APP_NAME ": SDL_Init failed: %s\n", SDL_GetError());
        return 3;
    }
    os_audio_on_service_restart(audio_service_restarted);
    // While the settings page is up it owns the pad; the game sees nothing held.
    controls::gamepad_set_suppressed_query([] { return host_page_visible(); });

    g_gpu = gpu::create_default_device();
    if (!g_gpu) {
        fprintf(stderr, RECOMP_APP_NAME ": no GPU device is available (%s)\n",
                gpu::default_backend_name());
        // The launcher still runs, drawn in software: the game can be
        // imported, and the player is told why it cannot start here.
        int mode = 0;
        const HostWindowSize ws = window_size_for(g_mode_w, g_mode_h);
        g_window =
            platform_ui_create_window(RECOMP_GAME_NAME, ws.w, ws.h, ws.min_w, ws.min_h, 0, &mode);
        if (g_window) {
            auto platform = launcher::make_platform(g_window);
            launcher::RunOptions options;
            options.known = game.exe;
            options.unplayable =
                std::string("This device has no usable ") + gpu::default_backend_name() +
                " graphics, which the game needs" +
                (strcmp(gpu::default_backend_name(), "vulkan") == 0 ? " (Vulkan 1.1)." : ".");
            if (const char *keys = recomp_env("LAUNCHER_KEYS"))
                options.keys = keys;
            if (const char *dump = recomp_env("LAUNCHER_DUMP")) {
                options.dump_path = dump;
                const char *home = getenv("HOME");
                if (!options.dump_path.empty() && options.dump_path[0] != '/' && home && *home)
                    options.dump_path = std::string(home) + "/" + options.dump_path;
            }
            launcher::run(g_window, nullptr, nullptr, *platform, options);
        }
        return 3;
    }

    const HostWindowSize size = window_size_for(g_mode_w, g_mode_h);
    const bool vulkan = strcmp(gpu::default_backend_name(), "vulkan") == 0;
    if (vulkan && !SDL_Vulkan_LoadLibrary(gpu::vulkan_loader_path()))
        fprintf(stderr, RECOMP_APP_NAME ": SDL_Vulkan_LoadLibrary: %s\n", SDL_GetError());
    const SDL_WindowFlags surface_flag = vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_METAL;
    g_window = platform_ui_create_window(RECOMP_GAME_NAME, size.w, size.h, size.min_w, size.min_h,
                                         surface_flag, &g_window_mode);
    if (!g_window) {
        fprintf(stderr, RECOMP_APP_NAME ": SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 3;
    }
    note_screen_size();
    g_surface = gpu::native_surface_for_window(g_window);
    if (!g_surface) {
        fprintf(stderr, RECOMP_APP_NAME ": no %s surface for the window: %s\n",
                gpu::default_backend_name(), SDL_GetError());
        return 3;
    }
    fprintf(stderr, "GPU backend: %s\n", gpu::default_backend_name());
    // The launcher: when the game is missing or not the supported build, or the
    // player asked for it. Mobile shows it briefly even when the game is ready,
    // so a touch can open it.
    std::string chosen_root;
#if !defined(RECOMP_GAME_MODULE)
    // The app that loads a module seeds the settings it hands over.
    seed_default_settings();
#endif
    {
#if defined(RECOMP_GAME_MODULE)
        // The app that loaded this module has shown its launcher already
        // (RECOMP_LAUNCHER, inherited, asked for that one).
        const bool mobile = false, asked = false;
#else
        const bool mobile = !platform_ui_pointer_capture_supported();
        const bool asked = launcher::requested(argc, argv);
#endif
        bool picked = false;
        const std::string native_config = host_resource("launcher-games.json");
        if (launcher::native_available(native_config) && native_launcher_wanted(game, asked)) {
            launcher::NativeChoice choice;
#if defined(RECOMP_GAME_MODULES)
            const std::string modules = available_modules();
#else
            const std::string modules = RECOMP_GAME_ID;
#endif
            const int r = launcher::native_run(g_window, native_config, modules, game.exe,
                                               saves_dir(), &choice, g_gpu.get(), g_surface);
            if (r == 0)
                return 2;
#if defined(RECOMP_GAME_MODULES)
            // RECOMP_LAUNCHER_VIA_MODULE=1 runs this app's own game through its
            // module too, when the app carries one (a check of the module path).
            const char *via = recomp_env("LAUNCHER_VIA_MODULE");
            if (r == 1 && (choice.module != RECOMP_GAME_ID || (via && strcmp(via, "1") == 0)))
                return run_game_module(choice, argv[0]);
#endif
            if (r == 1) {
                if (choice.module != RECOMP_GAME_ID) {
                    fprintf(stderr, RECOMP_APP_NAME ": %s is played by %s, which this app does not have\n",
                            choice.game.c_str(), choice.module.c_str());
                    return 2;
                }
                if (choice.exe != game.exe && !game_path_save(choice.exe))
                    fprintf(stderr, RECOMP_APP_NAME ": could not remember the game path\n");
                game.exe = choice.exe;
                chosen_root = choice.root;
                picked = true;
            }
        }
        if (!picked && (game.exe.empty() || asked || mobile)) {
            auto platform = launcher::make_platform(g_window);
            launcher::RunOptions options;
            options.known = game.exe;
            options.auto_play = !game.exe.empty() && !asked && mobile ? 1.5 : 0;
            // A relative dump path is under the app's home (a device's container).
            if (const char *dump = recomp_env("LAUNCHER_DUMP")) {
                options.dump_path = dump;
                const char *home = getenv("HOME");
                if (!options.dump_path.empty() && options.dump_path[0] != '/' && home && *home)
                    options.dump_path = std::string(home) + "/" + options.dump_path;
            }
            if (const char *keys = recomp_env("LAUNCHER_KEYS"))
                options.keys = keys;
            const std::string chosen =
                game_path_normal(launcher::run(g_window, g_gpu.get(), g_surface, *platform, options));
            if (chosen.empty())
                return 2;
            if (chosen != game.exe && !game_path_save(chosen))
                fprintf(stderr, RECOMP_APP_NAME ": could not remember the game path\n");
            game.exe = chosen;
        }
    }
    std::string exe = game.exe;
    apply_game_root(exe, chosen_root);
#if defined(__APPLE__) && TARGET_OS_IPHONE
    apply_cfg_defaults(exe);
#endif
    {
        // game.toml [texts]: the game's own words changed where they name
        // Windows, from this install's texts, into the profile.
        const char *root_env = recomp_env("GAME_ROOT");
        const std::string root =
            root_env && *root_env ? root_env : exe.substr(0, exe.find_last_of('/'));
        platform_ui_seed_profile(host_layout().profile_dir);
        // game.toml ui_mods: the interface mods (wide screen, the phone's and
        // the touch screen's), made from this install with the recipes the
        // app carries.
        host_ui_mods(root, host_layout().profile_dir, host_resource("ui-mods"), platform_ui_phone_interface());
        host_text_overrides(root, host_layout().profile_dir, RECOMP_TEXTS_PAK, RECOMP_TEXTS_FILES,
                            RECOMP_TEXTS_REPLACE);
    }
    // game.toml [online]: the game's GS server list comes from a server the
    // player names when its online mode asks (server_prompt.h).
    if (*RECOMP_ONLINE_GSCONNECT_HOST)
        server_prompt_install(host_layout().profile_dir, RECOMP_ONLINE_PROMPT_TITLE,
                              RECOMP_ONLINE_PROMPT_TEXT, RECOMP_ONLINE_DEFAULT_SERVER);

    // The renderer first: the presenter shares its device, so a present
    // cannot run ahead of the scene it is showing.
    D3DRenderer *renderer = new D3DRenderer(g_gpu.get());
    if (!renderer->ok())
        return 3;
    D3DRenderer::setShared(renderer);
    host_present_set_device(g_gpu.get());
    host_present_set_sched_probe(recomp_sched_main_totals);
    host_set_idle_wake(sdl_idle_wake);

    fprintf(stderr, "Mouse capture: click inside to capture; hold Escape to release; drag to "
                    "the window edge to resize.\n");
    mods_display_live_defaults();
    mods_display_default_overlay(platform_ui_default_overlay());
    mods_display_load_modes(classic_modes_path().c_str());
    host_present_on_mode_change(on_mode_change);
    // Every change to the input state wakes the guest's DirectInput threads,
    // which wait on an event rather than polling. Installed here rather than
    // referenced from input.cpp, which links none of the shims.
    host_input_set_notify(dinput_host_input_changed);
    // Before the first presented frame, where mods_page_init reads the
    // layout names this registers.
    {
        controls::HostHooks hooks;
        hooks.key = push_touch_key;
        hooks.touch_actions = push_touch_actions;
        hooks.system_keyboard = toggle_system_keyboard;
        hooks.open_settings = open_settings_page;
        controls::host_init(hooks);
    }

    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
    SDL_PumpEvents();
    int bw, bh, dw, dh;
    window_sizes(&bw, &bh, &dw, &dh);
    if (dw <= 0 || dh <= 0) {
        fprintf(stderr, RECOMP_APP_NAME ": the window has no drawable size\n");
        return 3;
    }
    host_present_start(g_surface, dw, dh);
    // Said out loud, because "the keyboard does nothing" and "the window
    // never gained focus" look identical from the outside.
    printf(RECOMP_APP_NAME ": window %dx%d points, %dx%d pixels, focus %s\n", bw, bh, dw, dh,
           window_focused() ? "yes" : "no");
    fflush(stdout);

    BootOptions options;
    options.name = "windowed";
    options.exe = exe.c_str();
    // This host has a real window with real focus, so activation is
    // delivered from the events that carry it rather than synthesised.
    options.activate = false;
    options.tick = pump;
    // The scheduler drains the queue itself when nothing is runnable, so
    // a guest blocked on an event an input message signals is not waiting
    // for a pump that only its own clock read would trigger.
    sched_set_input_queue(input_pending, drain_input);
    options.idle_wait = idle_wait;
    options.report = report;
    // No run deadline: the run ends when the user closes the window. The
    // watchdog still backs the close, so a guest that ignores WM_CLOSE
    // cannot leave a window on screen with nothing behind it.
    options.deadline_seconds = 0.0;
    options.close_unwind_grace = 15.0;
    options.close_watchdog_grace = 30.0;

    if (!boot_load(options)) {
        host_present_stop();
        fprintf(stderr, RECOMP_APP_NAME ": %s\n", loader_error());
        return 2;
    }
    printf(RECOMP_APP_NAME ": %s, entry %08x\n", loader_exe_path().c_str(), loader_entry_point());
    fflush(stdout);

    // The music's synth, built here and not later. midiOutOpen arrives on a
    // guest thread holding the cooperative scheduler baton, which stops
    // every other guest thread until it returns, so a SoundFont parsed
    // there would freeze the game at the moment the music starts.
    host_midi_startup(win32_midi_soundfont_path().c_str());

    // RECOMP_HOST_AUDIO_CAPTURE=<path.wav> writes the mixer's own output for
    // the whole run.
    const char *capture = recomp_env("HOST_AUDIO_CAPTURE");
    if (capture && *capture)
        host_audio_capture_begin(capture);

    boot_run();
    if (!sched_guest_threads_stopped()) {
        fprintf(stderr,
                "presenter: guest workers outlived shutdown; refusing unsafe presenter join\n");
        fflush(nullptr);
        _Exit(4);
    }
    host_present_stop(); // scheduler has stopped; join outside the guest baton
    host_audio_capture_end();
    boot_report_lock();
    report(stdout, false);
    boot_report_unlock();
    // Whatever else happens, the user gets their cursor back.
    apply_pointer_capture(false);
    SDL_HideWindow(g_window);
    gpu::release_window_surface(g_surface);
    SDL_DestroyWindow(g_window);
    // The audio device goes before the SDL it runs on, not in the exit
    // handlers after it: there its stream is destroyed through a freed mutex.
    host_audio_shutdown();
    SDL_Quit();
    platform_ui_process_exit(0);
    return 0;
}

extern "C" int host_display_phone(void) {
    return g_phone_form.load() ? 1 : 0;
}

extern "C" int host_phone_layout(void) {
    // host.ui/phone_interface: 0 never, 1 by the window's form, 2 always (on
    // a touch device). Read once: the launcher sets it before the game runs.
    static const int64_t setting = [] {
        int64_t v = 1;
        mods_settings_file_get("host.ui/phone_interface", &v);
        return v;
    }();
    if (setting == 0)
        return 0;
    if (setting == 2)
        return platform_ui_touch_device() ? 1 : 0;
    return host_display_phone();
}

extern "C" int host_display_generation(void) {
    return g_display_generation.load();
}

extern "C" int host_display_picture_points(float *w, float *h) {
    const PicturePoints p = g_picture_points.load();
    if (!w || !h || p.w <= 0 || p.h <= 0)
        return 0;
    *w = p.w;
    *h = p.h;
    return 1;
}

extern "C" int host_display_safe_insets(float *l, float *t, float *r, float *b) {
    if (!l || !t || !r || !b)
        return 0;
    const SafeInsets s = safe_insets();
    *l = s.l;
    *t = s.t;
    *r = s.r;
    *b = s.b;
    return s.l > 0 || s.t > 0 || s.r > 0 || s.b > 0;
}

extern "C" int host_display_screen_size(int *w, int *h) {
    const int sw = g_screen_w.load(), sh = g_screen_h.load();
    if (!w || !h || sw <= 0 || sh <= 0)
        return 0;
    // Never a portrait screen: the game's image goes above the controls.
    host_landscape_screen_size(sw, sh, w, h);
    return 1;
}
