// platform_ui.h - what the SDL host does differently per platform: hints
// before SDL_Init, where the game comes from, the one window, and the app
// lifecycle. platform_ui_desktop.cpp is macOS, Linux, Windows and Android;
// platform_ui_ios.mm is iPadOS. Everything else in the host is shared.
#pragma once
#include "../game_path.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

// Hints that must be set before SDL_Init. Desktop: click-through focus, no
// screensaver. iOS/Android: landscape, no synthesized touch->mouse events.
void platform_ui_init_hints();

// The game executable to load. Desktop: --exe, RECOMP_EXE, checkout, saved
// path (may be empty: the caller shows the picker). iOS: Documents/game/<exe>,
// seeded from the bundle; empty with *error set when the bundle has no game or
// the copy failed. Android resolves its external files root directly in main.
GamePath platform_ui_resolve_game(const char *flag, std::string *error);

// The one window. Desktop: window_w x window_h points with a min_w x min_h
// minimum, resizable, centered, hidden until shown. iOS/Android: fullscreen.
// *window_mode receives 0 (desktop window) or 2 (fullscreen).
SDL_Window *platform_ui_create_window(const char *title, int window_w, int window_h, int min_w,
                                      int min_h, SDL_WindowFlags surface_flag, int *window_mode);

// True when `e` was an app lifecycle event this platform consumed
// (background: pause audio and presentation; foreground: resume). Desktop:
// always false.
bool platform_ui_handle_lifecycle(const SDL_Event &e);

// Whether the on-screen controls (host/controls/controls_host.h) should show:
// iOS/Android without a hardware keyboard. Desktop: never.
bool platform_ui_keypad_wanted();

// Whether this is a phone or tablet: the on-screen controls may show at all
// (desktop shows them only under RECOMP_KEYPAD), and a device motor can stand
// in for controller rumble. iOS and Android: true. Desktop: false.
bool platform_ui_touch_device();

// Whether the device is a phone, not a tablet: the iPhone idiom on iOS,
// whatever its window's size now (a foldable iPhone's opened screen is a
// tablet's: main.cpp's phone form, runtime/native_seam.h host_display_phone,
// goes by the size as well). Elsewhere: false.
bool platform_ui_phone();

// Which side of a landscape phone holds the camera housing (the Dynamic
// Island or the notch): -1 left, 1 right, 0 none or not known. The system's
// safe area keeps both sides clear alike; only this one needs it. iOS: from
// the window scene's orientation, on an iPhone. Elsewhere: 0. Main thread.
int platform_ui_cutout_side();

// Whether that housing is a camera in a corner (a foldable iPhone's closed
// screen), whose side needs no more than the rounded corner's margin: from
// UIKit's reserved regions where it reports them, else by the window's shape
// (w x h points; a long phone's Dynamic Island sits mid-side).
// RECOMP_CUTOUT_IN_CORNER=0/1 overrides. iOS only; elsewhere false. Main thread.
bool platform_ui_cutout_in_corner(int w_pt, int h_pt);

// Whether this platform has a pointer the host may hide and confine. Desktop:
// yes. iOS/Android: no; fingers are placed absolutely and a captured host would read
// the OS pointer, which touch never moves.
bool platform_ui_pointer_capture_supported();

// The performance overlay mode a fresh profile starts with: 2 (graph) on the
// developer's desktop, 0 on a player's tablet.
int platform_ui_default_overlay();

// The guest has exited and the host has torn down: a desktop process returns
// from main; iOS and Android end the process after SDL_Quit.
void platform_ui_process_exit(int code);

// A light tap tick for an on-screen control press. iOS: UIImpactFeedbackGenerator
// (light style). Android: performHapticFeedback(KEYBOARD_TAP). Desktop: no-op.
void platform_ui_haptic_tap();

// The device's own motor, standing in for game rumble when no controller is
// connected. `low`/`high` are the guest's low/high-frequency motor strengths
// (0..65535, as SDL_GetGamepadRumble takes them); 0,0 stops the motor.
// iOS: a Core Haptics continuous player, intensity max(low, high)/65535.
// Android: Vibrator.vibrate with an amplitude derived the same way. Desktop: no-op.
void platform_ui_device_rumble(uint16_t low, uint16_t high);

// Files the build put under profile-seed/ (game.toml [ios] profile_files, on
// macOS [macos] profile_files) go into the player's profile, where the game
// sees them over its own folder; copied when missing or when the bundled copy
// differs in size. Those under phone/ only on an iPhone, without the prefix.
// iOS and macOS; elsewhere nothing.
void platform_ui_seed_profile(const std::string &profile_dir);

// The launcher's phone interface on (host.ui/phone_interface: on an iPhone
// unless off, on an iPad when "always"): the profile's phone/ files and phone
// mods are for it. Always false off iOS.
bool platform_ui_phone_interface();

// The control panel over the game: the camera's buttons over a 3D map - zoom
// in, zoom out, turn left, turn right - and the commands the game offers on its
// screen (runtime/native_seam.h recomp_hotkeys_state), each with its name, in
// a place clear of the game's interface where the document says where that
// stands ("busy"). Shown while it has either.
struct ControlPanel {
    bool camera = false;       // the camera's buttons
    const char *commands = ""; // the game's document, "" for none
    uint32_t generation = 0;   // changes whenever `commands` does
    const char *active = "";   // ids of commands holding now (a held key), space-separated
    bool phone = false;        // laid out for a phone-sized window (main.cpp's phone form)
    bool keyboard = false;     // a hardware keyboard (unused: the commands stay)
    // The game's picture in window points, which the document's "busy" (the
    // game's interface, as fractions of it) is measured against; 0 for the
    // whole window.
    double pic_x = 0, pic_y = 0, pic_w = 0, pic_h = 0;
};
// A camera button calls `camera` with a RECOMP_CAMERA_* step, a command
// `command` with its id. Call it every turn: it only acts on a change; another
// `phone` builds it again at the other size. iOS: a framed grid of round
// buttons at the left: a phone's at the top, by the camera housing; a larger
// screen's right above the game's bottom-left corner window (the minimap, a
// battle's hero portrait), its buttons the size of the game's round ones.
// They take their touches themselves. Desktop and Android: nothing.
void platform_ui_control_panel(SDL_Window *window, const ControlPanel &panel,
                               void (*camera)(int step), void (*command)(const char *id));

// The ring that counts down a resting drag to a split (TouchMapper::split_ring):
// centred at (x, y) in window points, `progress` 0 to 1 (1: held, a light tap
// tick once). `visible` false hides it. Call it every turn: it only acts on a
// change. iOS: a gold ring around the finger, which takes no touches. Desktop
// and Android: nothing.
void platform_ui_split_ring(SDL_Window *window, bool visible, double x, double y, double progress);

// The multi-game app's launcher hands the app over to a game module, which
// has its own copy of this platform layer: from then on the launcher's copy
// leaves the app's lifecycle events (background, foreground) to it. iOS only;
// elsewhere nothing.
void platform_ui_hand_over_lifecycle();

// The window is about to be destroyed (the multi-game app's launcher window,
// before a game module opens its own). iOS: its SDL view controller stops
// listening for the keyboard. Something in UIKit can keep that controller
// alive past SDL_DestroyWindow, and it then answered the next keyboard
// notification (the app going inactive hides the keyboard) by reading the
// freed window. Elsewhere nothing.
void platform_ui_window_closing(SDL_Window *window);

// The host's side of the lifecycle (main.cpp): the app is in the foreground
// and active (1) or inactive or away (0). The iOS layer reports it from its
// lifecycle watcher.
extern "C" void host_note_app_active(int active);

// A screen of the game drawn natively over it (runtime/native_seam.h,
// recomp_screen_state): `state` is the screen's new JSON document, or null
// when no native screen shows; a command from the view goes to `command`.
// Call it only on a change. iOS: the game's SwiftUI views (the game
// repository's native-screens/), over the whole window, taking every touch.
// Desktop and Android: nothing (the game keeps drawing its own).
void platform_ui_native_screen(SDL_Window *window, const char *state, void (*command)(const char *));
