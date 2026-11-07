// native_screen_mac.mm - platform_ui_native_screen on the Mac (sdl/platform_ui.h):
// a view the game opens (the game repository's native-screens/, module
// RecompScreens) over the game window's content view. The module takes the
// view's mouse; the host keeps the game's keys from the game while it shows
// (sdl/main.cpp).
#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>
#include <dlfcn.h>

#include "platform_ui.h"

#if defined(RECOMP_NATIVE_SCREENS)
namespace {
using ScreensShow = void (*)(void *, const char *, void (*)(const char *));
using ScreensHide = void (*)(void);
#if defined(RECOMP_GAME_MODULE)
// A game module (an expansion in the multi-game app) carries no copy of the
// views: the app's own serves it, as on iOS (sdl/platform_ui_ios.mm).
ScreensShow screens_show() {
    static ScreensShow f = (ScreensShow)dlsym(RTLD_MAIN_ONLY, "recomp_native_screens_show");
    return f;
}
ScreensHide screens_hide() {
    static ScreensHide f = (ScreensHide)dlsym(RTLD_MAIN_ONLY, "recomp_native_screens_hide");
    return f;
}
#else
extern "C" void recomp_native_screens_show(void *root_view, const char *state,
                                           void (*command)(const char *));
extern "C" void recomp_native_screens_hide(void);
ScreensShow screens_show() {
    return recomp_native_screens_show;
}
ScreensHide screens_hide() {
    return recomp_native_screens_hide;
}
#endif
} // namespace
#endif

void platform_ui_native_screen(SDL_Window *window, const char *state,
                               void (*command)(const char *)) {
#if defined(RECOMP_NATIVE_SCREENS)
    if (!state) {
        if (ScreensHide hide = screens_hide())
            hide();
        return;
    }
    NSWindow *nswindow = (__bridge NSWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    NSView *root = nswindow.contentView;
    ScreensShow show = screens_show();
    if (root && show)
        show((__bridge void *)root, state, command);
#else
    (void)window;
    (void)state;
    (void)command;
#endif
}
