// platform_ui_desktop.cpp - the SDL host on desktop and Android. Desktop
// uses a resizable window and file picker; Android uses fullscreen touch, or
// a resizable window on a desktop-class device (a Chromebook).
#include "platform_ui.h"

#include "../game_path.h"

#if defined(__APPLE__)
#include "../../runtime/layout.h"
#include <filesystem>
#include <fstream>
#include <stdio.h>
#include <string.h>
#endif

#ifdef __ANDROID__
#include "../audio.h"
#include "../present.h"
#include <android/log.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
// RecompActivity, resolved once and held as a global ref: FindClass from the
// SDL thread (where the haptic calls below run) sees only the system class
// loader, not the app's, unless the class is already cached from onCreate's
// thread. The launcher's own JNI (launcher_platform_android.cpp) caches its
// own reference the same way; this file needs its own because the two never
// share a translation unit.
jclass g_haptics_activity = nullptr;

JNIEnv *haptics_env() {
    return static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
}

// A pending exception (a missing class/method, or one the Java side threw)
// would abort the next unrelated JNI call on this thread if left in place.
// Logs it once per call site (`*logged`), so a call made every frame does not
// spam logcat, clears it, and reports whether one was pending.
bool jni_failed(JNIEnv *env, const char *what, bool *logged) {
    if (!env->ExceptionCheck())
        return false;
    if (!*logged) {
        *logged = true;
        __android_log_write(ANDROID_LOG_ERROR, "recomp", what);
        env->ExceptionDescribe();
    }
    env->ExceptionClear();
    return true;
}

// The cached RecompActivity class, or null when it cannot be resolved.
jclass haptics_activity_class(JNIEnv *env) {
    if (!g_haptics_activity) {
        static bool logged = false;
        jclass local = env->FindClass("dev/recompkit/RecompActivity");
        if (jni_failed(env, "[haptics] FindClass(RecompActivity) failed", &logged) || !local)
            return nullptr;
        g_haptics_activity = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
    }
    return g_haptics_activity;
}

// A desktop-class device: a Chromebook, Android's desktop, or a keyboard and
// mouse attached at start (RecompActivity.desktopForm, handed over as
// RECOMP_ANDROID_DESKTOP before SDL_main). It plays as the desktop builds do:
// in a window the player sizes, with the desktop's defaults and interface
// mods and no on-screen controls unless asked for. RECOMP_DEVICE_FORM=desktop
// or touch (switches.txt) overrides it. Read once.
bool android_desktop() {
    static const bool desktop = [] {
        if (const char *form = getenv("RECOMP_DEVICE_FORM"); form && *form)
            return strcmp(form, "desktop") == 0;
        const char *v = getenv("RECOMP_ANDROID_DESKTOP");
        return v && strcmp(v, "1") == 0;
    }();
    return desktop;
}

// An app has no console: the host's stdout and stderr go to logcat (tag
// "recomp"), a line at a time.
void redirect_stdio_to_logcat() {
    static bool done = false;
    int fds[2];
    if (done || pipe(fds) != 0)
        return;
    done = true;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    std::thread([fd = fds[0]]() {
        std::string line;
        char buf[1024];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0)
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    __android_log_write(ANDROID_LOG_INFO, "recomp", line.c_str());
                    line.clear();
                } else
                    line.push_back(buf[i]);
            }
    }).detach();
}
} // namespace
#endif

void platform_ui_init_hints() {
#ifdef __ANDROID__
    redirect_stdio_to_logcat();
    // SDL_HINT_ORIENTATIONS is set per device class in platform_ui_create_window
    // instead of here: reading the display needs SDL_Init, which has not run
    // yet at this point, and SDLActivity only reads the hint once, from
    // Android_CreateWindow.
    // The shared touch mapper generates mouse events itself.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
#endif
    // A click that brings the window forward reaches the game in the same
    // event, rather than being swallowed as the activating click.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
}

GamePath platform_ui_resolve_game(const char *flag, std::string *error) {
    if (error)
        error->clear();
    return game_path_resolve(flag); // an empty result means: show the picker
}

SDL_Window *platform_ui_create_window(const char *title, int window_w, int window_h, int min_w,
                                      int min_h, SDL_WindowFlags surface_flag, int *window_mode) {
#ifdef __ANDROID__
    // App lifecycle events go to watchers even when SDL cannot pump while
    // backgrounded. Match the mobile host's audio/presenter suspension seam.
    static bool watching = false;
    if (!watching) {
        SDL_AddEventWatch(
            [](void *, SDL_Event *event) {
                platform_ui_handle_lifecycle(*event);
                return true;
            },
            nullptr);
        watching = true;
    }
    // A phone rotates live between portrait and landscape; a tablet stays
    // landscape. This has to run here rather than in platform_ui_init_hints:
    // SDL_GetDisplayBounds needs SDL_Init (already done by the time this
    // runs) and, more importantly, SDLActivity.setOrientationBis reads
    // SDL_HINT_ORIENTATIONS exactly once, from Android_CreateWindow - so the
    // hint has to be current right before the SDL_CreateWindow call below.
    // The window is resizable so that call takes its "both orientations
    // allowed" branch: with only Landscape hinted that still resolves to one
    // landscape-only request (SCREEN_ORIENTATION_USER_LANDSCAPE), and with
    // Landscape and Portrait both hinted it asks for
    // SCREEN_ORIENTATION_FULL_USER, a live, sensor-driven orientation - a
    // fixed (non-resizable) window would instead freeze to whichever
    // orientation the window happened to be created in and never rotate
    // again. RecompActivity.onCreate locks a tablet to landscape before SDL
    // ever runs; hinting landscape-only here on top of that keeps SDL from
    // ever asking the system for something wider once its own window exists.
    if (android_desktop()) {
        // A window on a Chromebook's desktop: the system places and sizes it
        // (the manifest's <layout> gives its first size), the player resizes
        // it and the presenter letterboxes the game in it, as on a desktop.
        // No orientation is asked for, so the window stays freely resizable.
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait PortraitUpsideDown");
        if (window_mode)
            *window_mode = 0;
        return SDL_CreateWindow(title, window_w, window_h,
                                surface_flag | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    }
    SDL_Rect bounds = {0, 0, 0, 0};
    SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &bounds);
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    const float smallest_side_pt =
        (float)(bounds.w < bounds.h ? bounds.w : bounds.h) / (scale > 0 ? scale : 1.0f);
    SDL_SetHint(SDL_HINT_ORIENTATIONS, smallest_side_pt < 600
                                           ? "LandscapeLeft LandscapeRight Portrait"
                                           : "LandscapeLeft LandscapeRight");
    if (window_mode)
        *window_mode = 2;
    return SDL_CreateWindow(title, 0, 0,
                            surface_flag | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                SDL_WINDOW_RESIZABLE);
#else
    SDL_Window *w = SDL_CreateWindow(title, window_w, window_h,
                                     surface_flag | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!w)
        return nullptr;
    SDL_SetWindowMinimumSize(w, min_w, min_h);
    SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (window_mode)
        *window_mode = 0;
    return w;
#endif
}

bool platform_ui_handle_lifecycle(const SDL_Event &event) {
#ifdef __ANDROID__
    switch (event.type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
    case SDL_EVENT_TERMINATING:
        host_present_suspend(true);
        host_audio_pause(true);
        return true;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        host_audio_pause(false);
        host_present_suspend(false);
        return true;
    default:
        break;
    }
#else
    (void)event;
#endif
    return false;
}

void platform_ui_camera_buttons(SDL_Window *, bool, bool, void (*)(int)) {}

void platform_ui_native_screen(SDL_Window *, const char *, void (*)(const char *)) {}

#if defined(__APPLE__)
namespace {
bool same_bytes(const std::filesystem::path &a, const std::filesystem::path &b) {
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb)
        return false;
    char ba[65536], bb[65536];
    while (fa && fb) {
        fa.read(ba, sizeof ba);
        fb.read(bb, sizeof bb);
        if (fa.gcount() != fb.gcount() || memcmp(ba, bb, (size_t)fa.gcount()) != 0)
            return false;
    }
    return fa.eof() && fb.eof();
}
} // namespace

// macOS: game.toml [macos] profile_files, the same copy as on iOS without its
// phone/ files (an iPhone's only); a file is copied again when its bytes differ.
void platform_ui_seed_profile(const std::string &profile_dir) {
    namespace fs = std::filesystem;
    const std::string seed = host_resource("profile-seed");
    std::error_code ec;
    if (seed.empty() || profile_dir.empty() || !fs::is_directory(seed, ec))
        return;
    for (auto it = fs::recursive_directory_iterator(seed, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        const fs::path rel = fs::relative(it->path(), seed, ec);
        if (rel.begin() != rel.end() && *rel.begin() == "phone")
            continue;
        const fs::path dest = fs::path(profile_dir) / rel;
        std::error_code e2;
        if (fs::exists(dest, e2) && fs::file_size(dest, e2) == it->file_size(e2) &&
            same_bytes(it->path(), dest))
            continue; // a rebuilt mod can keep its size: the bytes decide
        fs::create_directories(dest.parent_path(), e2);
        fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, e2);
        fprintf(stderr, "[macos] profile file %s: %s\n", rel.c_str(),
                e2 ? e2.message().c_str() : "put in place");
    }
}
#else
void platform_ui_seed_profile(const std::string &) {}
#endif

bool platform_ui_phone_interface() {
    return false;
}

bool platform_ui_keypad_wanted() {
#ifdef __ANDROID__
    return !SDL_HasKeyboard();
#else
    return false;
#endif
}

bool platform_ui_touch_device() {
#ifdef __ANDROID__
    return !android_desktop();
#else
    return false;
#endif
}

bool platform_ui_phone() {
    return false;
}

int platform_ui_cutout_side() {
    return 0;
}

bool platform_ui_cutout_in_corner(int, int) {
    return false;
}

bool platform_ui_pointer_capture_supported() {
#ifdef __ANDROID__
    return false;
#else
    return true;
#endif
}

int platform_ui_default_overlay() {
#ifdef __ANDROID__
    return 0;
#else
    return 2;
#endif
}

void platform_ui_process_exit(int code) {
#ifdef __ANDROID__
    // main has called SDL_Quit and destroyed its window. End the process:
    // minimizing here would leave a dead game in the activity's task.
    SDL_Log("[android] the game exited (%d); ending the app", code);
    exit(code);
#else
    (void)code;
    // main() returns; the process ends the ordinary way.
#endif
}

void platform_ui_haptic_tap() {
#ifdef __ANDROID__
    static bool logged_method = false, logged_call = false;
    JNIEnv *env = haptics_env();
    jclass cls = env ? haptics_activity_class(env) : nullptr;
    if (!cls)
        return;
    jmethodID m = env->GetStaticMethodID(cls, "hapticTap", "()V");
    if (jni_failed(env, "[haptics] GetStaticMethodID(hapticTap) failed", &logged_method) || !m)
        return;
    env->CallStaticVoidMethod(cls, m);
    jni_failed(env, "[haptics] CallStaticVoidMethod(hapticTap) failed", &logged_call);
#endif
    // Desktop: no-op.
}

void platform_ui_device_rumble(uint16_t low, uint16_t high) {
#ifdef __ANDROID__
    static bool logged_method = false, logged_call = false;
    JNIEnv *env = haptics_env();
    jclass cls = env ? haptics_activity_class(env) : nullptr;
    if (!cls)
        return;
    jmethodID m = env->GetStaticMethodID(cls, "deviceRumble", "(II)V");
    if (jni_failed(env, "[haptics] GetStaticMethodID(deviceRumble) failed", &logged_method) || !m)
        return;
    env->CallStaticVoidMethod(cls, m, jint(low), jint(high));
    jni_failed(env, "[haptics] CallStaticVoidMethod(deviceRumble) failed", &logged_call);
#else
    (void)low;
    (void)high;
    // Desktop: no-op.
#endif
}
