/* hotkeys_cfg.h - the touch panel's commands (native/hotkeys.c): the game's key
 * bindings file read into a table, the commands the panel offers, and the
 * document the host draws them from (runtime/native_seam.h
 * recomp_hotkeys_state). Plain C over text: no guest memory, no files, so the
 * tests (native/tests/hotkeys_test.c) drive it with strings. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Key codes: USB HID usages, which SDL's scancodes are. */
enum {
    HK_KEY_LCTRL = 224,
    HK_KEY_LSHIFT = 225,
    HK_KEY_LALT = 226,
    HK_KEY_RCTRL = 228,
    HK_KEY_RSHIFT = 229,
    HK_KEY_RALT = 230,
};

/* The usage of one of the game's key names ('M', 'F5', 'NUM_PLUS', 'CTRL'),
 * or 0 for a name that is no key (MOUSE_BUTTON0, MOUSE_AXIS_X) or unknown.
 * The plain CTRL, SHIFT and ALT, which the game takes for either side, are
 * the left one. */
int hk_key_code(const char *name);

/* One "bind <action> 'KEY' [+ 'KEY'...]" line: the section it is in ("" for
 * the common one before the first bindsection, which is always active), the
 * action, and its keys, modifiers first, as the panel presses them. */
typedef struct {
    char section[40];
    char action[48];
    char text[32];   /* how a player reads it: "Ctrl+1", "F5" */
    uint8_t mods[3]; /* modifier usages, 0 past the last */
    uint8_t key;     /* the key itself */
} HkBind;

/* Reads a bindings file (CP1251 text: "//" comments, bindsection, bind,
 * bindconfigure) into `out`: the binds whose keys are all keys (no mouse),
 * at most `cap` of them. Returns how many. */
int hk_parse(const char *text, size_t len, HkBind *out, int cap);

/* The first bind of `action` in one of the active sections (in their order),
 * then in the common section; NULL when it has none. */
const HkBind *hk_find(const HkBind *binds, int n, const char *action, const char *const *sections,
                      int nsections);

/* The languages of the panel's words: the game's (native_screens.h NS_*). */
enum { HK_EN, HK_RU, HK_DE, HK_FR, HK_PL };

/* The panel's document for the active sections, JSON:
 *   {"screen": "<the sections, space-separated>", "lang": "ru", "commands": [
 *     {"id": "mana", "label": "...", "keys": "M", "kind": "tap",
 *      "quick": 2, "symbol": "drop.fill", "key": 16, "mods": []}, ...]}
 * `id` names the button's art (touch-ui/camera-panel/btn-<id>), `symbol` the
 * system symbol drawn where there is none. `kind`: "tap" presses the key with
 * its mods; "latch" holds the key until the next click in the game (or a
 * second press); "next_tap_right" makes the next tap a right click; "screen"
 * opens a native view of the game's (the host sends {"do": "panel", "id": ...}
 * to runtime/native_seam.h recomp_screen_command). `quick`:
 * pinned by default, 1 on every screen size, 2 on a large one only, 0 not.
 * A command whose action the bindings lack is left out (Hammers of Fate binds
 * no heroes' mana), and one whose own button the game shows (`onscreen`). Returns the length written (NUL-terminated), the length it
 * needs when that does not fit, or 0 when no command applies. */
int hk_document(const HkBind *binds, int n, const char *const *sections, int nsections, int lang,
                uint32_t onscreen, char *out, int cap);

/* The catalog's commands, in its order: how many, and the names of command
 * i's own buttons in the game's interface (space-separated window <Name>s;
 * NULL for one the game has no button for). Bit i of hk_document's
 * `onscreen` leaves command i out: its button is on the screen already. */
int hk_command_count(void);
const char *hk_command_buttons(int i);
