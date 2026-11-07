/* native/touch.c - Heroes V's touch adapters for the host (runtime/native_seam.h).
 * Addresses: design notes ios-touch-plan.md section 13.
 *
 * Placement: the game draws its own cursor at the float pair NUI::CCursor
 * keeps in client pixels (0x0114da0c/10). Each frame CCursor refreshes that
 * pair from GetCursorPos, or, in its relative mode, integrates DirectInput
 * axes into it and warps the system cursor there (FUN_0070ddb0). A touch
 * writes the pair itself, so the next frame hit-tests the finger's point in
 * either mode; the host then drops the DirectInput motion the jump would
 * otherwise replay.
 *
 * Hover: every frame the top screen asks 0070cf80 for the tooltip delay in ms
 * and its tooltip timer shows a tooltip once the cursor has rested that long
 * (00ea0fc0; any cursor motion restarts it). The replacement below answers
 * from the host's touch state. The time keeps accumulating while the answer
 * is "never", so switching to "now" shows the tooltip on the next frame.
 *
 * Context: the top screen of the screen stack and the cursor the game chose
 * by hit-testing tell the host whether a finger is on the adventure map, the
 * battlefield or the interface. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "x86.h"

/* The game's own addresses: the base game's below, an expansion's from its
 * games/<id>/native/addresses.h (game.toml [translate] native addresses,
 * which the build passes to the native sources alone as RECOMP_ADDRESS_HEADER).
 * RECOMP_OVERRIDE_HEADER declares the replacements defined here. */
#ifdef RECOMP_OVERRIDE_HEADER
#include RECOMP_OVERRIDE_HEADER
#endif
#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif
#ifndef HOMM5_CURSOR_OBJECT
#define HOMM5_CURSOR_OBJECT 0x0114da18
#define HOMM5_CURSOR_VTABLE 0x00fa1e50
#define HOMM5_CURSOR_X 0x0114da0c
#define HOMM5_CURSOR_Y 0x0114da10
#define HOMM5_CURSOR_ID 0x0114da60
#define HOMM5_HOVER_TIMEOUT 0x010ce164
#define HOMM5_SCREEN_STACK 0x0114b2e4
#define HOMM5_VT_ADVENTURE 0x00fab2ac
#define HOMM5_VT_COMBAT 0x00fa79c8
#define HOMM5_SUB_ADVENTURE 0x7b4
#define HOMM5_SUB_COMBAT 0x658
#define HOMM5_SCREEN_WINDOWS 0xa0
#define HOMM5_WINDOWS_MODAL 0x1f4
#endif
#ifndef HOMM5_FN_CURSOR_DRAW
#define HOMM5_FN_CURSOR_DRAW fn_0070dae0 /* NUI::CCursor::Draw (vtable slot 2) */
#define HOMM5_CURSOR_HW 0x0114d9f8       /* its hardware-cursor flag (byte) */
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

enum {
    CURSOR_OBJECT = HOMM5_CURSOR_OBJECT, /* NUI::CCursor, a static singleton */
    CURSOR_VTABLE = HOMM5_CURSOR_VTABLE, /* its vtable once constructed */
    CURSOR_X = HOMM5_CURSOR_X,
    CURSOR_Y = HOMM5_CURSOR_Y,
    CURSOR_ID = HOMM5_CURSOR_ID,             /* the cursor shown now (enum index, Cursors.xdb) */
    CURSOR_HW = HOMM5_CURSOR_HW,             /* set: the system shows the cursor (ui_use_hwcursor) */
    HOVER_TIMEOUT = HOMM5_HOVER_TIMEOUT,     /* Options.Tooltips.HoverTimeout, seconds */
    SCREEN_STACK = HOMM5_SCREEN_STACK,       /* std::list<IScreen*>: the sentinel node */
    VT_ADVENTURE = HOMM5_VT_ADVENTURE,       /* IScreen subobject vtables of the screens */
    VT_COMBAT = HOMM5_VT_COMBAT,
    SUB_ADVENTURE = HOMM5_SUB_ADVENTURE,     /* where that subobject sits in the screen object */
    SUB_COMBAT = HOMM5_SUB_COMBAT,
    SCREEN_WINDOWS = HOMM5_SCREEN_WINDOWS,   /* screen object: its CWindowScreen, 0xf8 bytes in */
    WINDOWS_MODAL = HOMM5_WINDOWS_MODAL,     /* CWindowScreen: the modal window, if one is open */
};

/* Cursors that mean interface rather than the map or the field under it. */
enum {
    CUR_ARROW = 0x00,
    CUR_INVISIBLE = 0x04,
    CUR_CORNER = 0x1d,
    CUR_FINGER = 0x31, /* over a button or the minimap */
};

/* 100000 s: far beyond any hover, and still inside an int32 of ms once the
 * right-click path adds one to it. */
#define HOVER_NEVER_MS 100000000

static volatile int g_hover = -1; /* RECOMP_TOUCH_HOVER_*, from the host */

/* native/camera.c: the map's camera has drawn a frame lately (the map is up,
 * not still loading). Every game links it. */
int homm5_map_camera_live(void);
int homm5_zoom_camera_live(void); /* the same for a battle's */

void recomp_touch_hover(int state) {
    g_hover = state;
}

/* Settings: the game writes global.cfg (the chosen profile) and the profile's
 * user.cfg only on a clean exit or from its options screen (006bfe80), and an
 * iPad app is closed from the app switcher instead. Going to the background,
 * the host asks for a save; the game's own thread does it in its next frame
 * (the hover getter runs every frame), and the host waits for that. */
/* A request is a number; the game's thread answers the latest one it saw.
 * Requests that pile up while one save is under way (a scene's and the app's
 * notification, a module's and its launcher's) are answered by one save, and
 * no request resets the answer another one is waiting for. */
static volatile uint32_t g_save_asked, g_save_answered;
void homm5_save_settings(X86 *c); /* native/save.cpp */

void recomp_app_background(void) {
    int waited = 0;
    const uint32_t ask = __atomic_add_fetch(&g_save_asked, 1u, __ATOMIC_ACQ_REL);
    while ((int32_t)(__atomic_load_n(&g_save_answered, __ATOMIC_ACQUIRE) - ask) < 0 && waited < 3000) {
        struct timespec ts = {0, 10 * 1000000};
        nanosleep(&ts, NULL);
        waited += 10;
    }
    fprintf(stderr, "[homm5] settings %s before going to the background\n",
            (int32_t)(__atomic_load_n(&g_save_answered, __ATOMIC_ACQUIRE) - ask) >= 0
                ? "saved"
                : "not saved (the game did not reach a frame)");
}

static void nt_frame(X86 *c);           /* native/touch_native.c, below */
static int homm5_native_hover(void);
void homm5_hotkeys_frame(X86 *c); /* native/hotkeys.c: the panel's buttons and language */

void homm5_hover_timeout(X86 *c) {
    nt_frame(c);
    homm5_hotkeys_frame(c);
    const uint32_t asked = __atomic_load_n(&g_save_asked, __ATOMIC_ACQUIRE);
    if (asked != __atomic_load_n(&g_save_answered, __ATOMIC_ACQUIRE)) {
        homm5_save_settings(c);
        __atomic_store_n(&g_save_answered, asked, __ATOMIC_RELEASE);
    }
    const int native = homm5_native_hover();
    const int state = native >= 0 ? native : g_hover;
    int32_t ms;
    if (state == 0)
        ms = 0;
    else if (state > 0)
        ms = HOVER_NEVER_MS;
    else
        ms = recomp_sse_cvtt_i32(rdf32(HOVER_TIMEOUT) * 1000.0f);
    c->r[0] = (uint32_t)ms;
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u;
    recomp_return(c);
}

/* A guest word, or zero for an address outside the arena (a pointer the game
 * has not set up yet, read from the host thread). */
static uint32_t peek32(uint32_t addr) {
    uint32_t v;
    if (!g_mem || addr < 0x1000u || addr > (uint32_t)GUEST_SIZE - 4u)
        return 0;
    memcpy(&v, g_mem + addr, sizeof v);
    return v;
}

/* The top screen's IScreen subobject, or zero. */
static uint32_t top_screen(void) {
    const uint32_t sentinel = peek32(SCREEN_STACK);
    if (!sentinel || peek32(sentinel) == sentinel)
        return 0; /* no screen */
    const uint32_t last = peek32(sentinel + 4u);
    return last ? peek32(last + 8u) : 0;
}

/* The modal window open over a screen (a message box, a tutorial page), or zero. */
static uint32_t screen_modal(uint32_t screen, uint32_t sub) {
    const uint32_t windows = peek32(screen - sub + SCREEN_WINDOWS);
    return windows > 0xf8u ? peek32(windows - 0xf8u + WINDOWS_MODAL) : 0;
}

/* What the cursor drags (NUI::CCursor +0x4c, beside the cursor's id at +0x48
 * in all three games): a counted object whose +4 is negative once the drag is
 * over (CCursor's "is dragging" at 0070e1f0 in the base game). Zero when
 * nothing is dragged. */
static uint32_t dragged_object(void) {
    const uint32_t obj = peek32(CURSOR_ID + 4u);
    return obj && (int32_t)peek32(obj + 4u) >= 0 ? obj : 0;
}

/* For native/phone_hud.c: whether `win` (a CWindow) is the icon of what the
 * cursor drags (an artifact's, a stack's, a hero's), laid out at the cursor.
 * The drag object's +0xc points into the window, at its draw part (+0x110
 * for a stack's CWindowSimple), not at its start. */
int homm5_is_drag_icon(uint32_t win) {
    const uint32_t obj = dragged_object();
    const uint32_t icon = obj ? peek32(obj + 12u) : 0;
    return win && icon >= win && icon - win < 0x200u;
}

/* An object's class name from its MSVC RTTI (vtable[-1] -> the complete object
 * locator, +0xc -> the type descriptor, its name at +8: ".?AVCName@NUI@@"),
 * or "" when the words do not lead to one. */
static void rtti_name(uint32_t obj, char *name, size_t size) {
    name[0] = 0;
    const uint32_t vt = peek32(obj);
    const uint32_t col = vt ? peek32(vt - 4u) : 0;
    const uint32_t td = col ? peek32(col + 12u) : 0;
    /* In 64 bits: a stray word near the top of the range (0xffffffff, which
     * the scan in drag_client below meets) wrapped `td + 8u` to a small number
     * and the copy read 4 GiB past the arena. */
    if (td < 0x1000u || (uint64_t)td + 8u + size > (uint64_t)(uint32_t)GUEST_SIZE)
        return;
    memcpy(name, g_mem + td + 8u, size - 1);
    name[size - 1] = 0;
    if (name[0] != '.' || name[1] != '?')
        name[0] = 0;
}

/* The client of a drag (what the game drags: NUI::CTownScreenDragNDropClient-
 * Heroes, ...Creatures and the like), found through the screen's drag-and-drop
 * state: a NUI::CDragNDropBase the screen holds, whose +0x1c is the active
 * NUI::CDragNDropHelper; that helper's +0x20 is the drag object the cursor
 * holds and its +8 the client (measured in Tribes of the East's town screen,
 * the base at screen-0x370). The base is looked for around the top screen's
 * IScreen subobject, so no screen's layout is needed. Zero when not found. */
static uint32_t drag_client(uint32_t obj) {
    const uint32_t screen = top_screen();
    char name[64];
    for (int32_t off = -0x1000; screen && off < 0x1000; off += 4) {
        const uint32_t base = peek32(screen + (uint32_t)off);
        if (base < 0x1000u || base > (uint32_t)GUEST_SIZE - 0x40u)
            continue;
        rtti_name(base, name, sizeof name);
        if (strcmp(name, ".?AVCDragNDropBase@NUI@@") != 0)
            continue;
        const uint32_t helper = peek32(base + 0x1cu);
        if (helper && peek32(helper + 0x20u) == obj)
            return peek32(helper + 8u);
    }
    return 0;
}

/* Whether the drag is one a held Shift splits on the drop: an army's stack, in
 * a town, a hero's screen, a meeting or a garrison (the game opens its
 * SplitStack window: the drop reads the screen's "shift" binding, e.g. the
 * hero screen's at 008512bc in the base game). Every drag's object is a
 * NUI::CDragNDropHelperShared; its icon window (+0xc) tells an artifact's (the
 * inventory's DragIcon, a CWindowMSButton) from a stack's or a hero's (the
 * plain UI/Common/Creatures/DragIcon, a CWindowSimple: UI/HeroScreen3 and
 * UI/TownScreen hold them), and a town's hero, which nothing splits, is told
 * by its client's class. Decided once per drag. */
static int drag_splits(void) {
    static uint32_t known_obj;
    static int known;
    const uint32_t obj = dragged_object();
    const uint32_t icon = obj ? peek32(obj + 12u) : 0;
    if (!icon) {
        known_obj = 0; /* decided again next drag: the screen may be another by then */
        return 0;
    }
    if (obj != known_obj) {
        char icon_name[96], client_name[96];
        rtti_name(icon, icon_name, sizeof icon_name);
        const uint32_t client = drag_client(obj);
        client_name[0] = 0;
        if (client)
            rtti_name(client, client_name, sizeof client_name);
        known_obj = obj;
        known = strstr(icon_name, "?AVCWindowSimple@@") && !strstr(client_name, "Heroes");
        if (getenv("RECOMP_TRACE_TOUCH"))
            fprintf(stderr, "[touch] dragging %s %s: %s\n", icon_name[0] ? icon_name : "(no RTTI)",
                    client_name[0] ? client_name : "(client unknown)",
                    known ? "a split may follow" : "no split");
    }
    return known;
}

/* The root window of the map's or a battle's screen when it is on top (in
 * native/phone_layout.c's terms: its children at +0xa4), or zero: the
 * screen's CWindowScreen is 0xf8 before the pointer it keeps. For
 * native/hotkeys.c, which looks for the game's own buttons there. */
uint32_t homm5_top_root(void) {
    const uint32_t screen = top_screen();
    const uint32_t vt = screen ? peek32(screen) : 0;
    const uint32_t sub = vt == VT_ADVENTURE ? SUB_ADVENTURE : vt == VT_COMBAT ? SUB_COMBAT : 0;
    const uint32_t windows = sub ? peek32(screen - sub + SCREEN_WINDOWS) : 0;
    return windows > 0xf8u ? windows - 0xf8u : 0;
}

/* What the top screen is: 1 the adventure map, 2 a battle, 0 anything else
 * (a town, a menu, a loading screen). For native/hotkeys.c: a screen's key
 * section stays on under a town's, and a map's camera may have drawn its
 * last frame a moment ago. */
int homm5_top_screen_kind(void) {
    const uint32_t screen = top_screen();
    const uint32_t vt = screen ? peek32(screen) : 0;
    return vt == VT_ADVENTURE ? 1 : vt == VT_COMBAT ? 2 : 0;
}

int homm5_ui_under_cursor(void); /* native/phone_hud.c */
int homm5_tap_context(uint32_t cursor); /* native/tap_attack.c */
void homm5_tap_pointer_placed(float x, float y);

int recomp_touch_context(void) {
    static int trace = -1;
    static uint32_t traced_vt, traced_cursor, traced_modal;
    static int traced_flags = -1;
    const uint32_t screen = top_screen();
    const uint32_t vt = screen ? peek32(screen) : 0;
    const uint32_t cursor = peek32(CURSOR_ID);
    uint32_t modal = 0;
    int flags = 0;
    /* Over the map the game shows the arrow too where the hero cannot go, so
     * only a button (the finger) or an open modal window means interface. */
    if (vt == VT_ADVENTURE) {
        modal = screen_modal(screen, SUB_ADVENTURE);
        const int drawn = homm5_map_camera_live();
        if (!modal && drawn)
            flags = 4; /* RECOMP_TOUCH_MAP_SCREEN */
        if (!modal && drawn && cursor != CUR_FINGER && cursor != CUR_INVISIBLE &&
            cursor != CUR_CORNER)
            flags |= 1; /* RECOMP_TOUCH_WORLD */
    } else if (vt == VT_COMBAT) {
        /* On the battlefield the game keeps the plain arrow over its buttons
         * too (measured: the start-battle button), so only its action
         * cursors (move, attack, shoot, cast...) mean the field. */
        modal = screen_modal(screen, SUB_COMBAT);
        if (!modal && cursor != CUR_ARROW && cursor != CUR_FINGER && cursor != CUR_INVISIBLE &&
            cursor != CUR_CORNER)
            flags = 2; /* RECOMP_TOUCH_BATTLE */
        /* The "strike from a cell" mode: a tap that chooses or strikes clicks
         * at once (native/tap_attack.c). */
        if (!modal)
            flags |= homm5_tap_context(cursor);
        /* The camera's zoom anywhere but over a list or a text field (the
         * battle's log): the game's own wheel zooms over the field and its
         * buttons alike. Not by the cursor's look: over a cell a unit can move
         * to the base game shows the plain arrow and Tribes of the East the
         * finger it shows over buttons (both measured). */
        if (!modal && cursor != CUR_INVISIBLE && !(homm5_ui_under_cursor() & (8 | 16)) &&
            homm5_zoom_camera_live())
            flags |= 32; /* RECOMP_TOUCH_ZOOM_VIEW */
    }
    if (trace < 0)
        trace = getenv("RECOMP_TRACE_TOUCH") != NULL;
    if (trace && (vt != traced_vt || cursor != traced_cursor || modal != traced_modal ||
                  flags != traced_flags)) {
        traced_flags = flags;
        fprintf(stderr, "[touch-ctx] screen %08x cursor %02x modal %08x -> %d\n", vt, cursor,
                modal, flags);
        traced_vt = vt;
        traced_cursor = cursor;
        traced_modal = modal;
    }
    /* A text field or a list under the cursor (native/phone_hud.c): the
     * keyboard, a drag that scrolls. */
    flags |= homm5_ui_under_cursor();
    if (drag_splits())
        flags |= 256; /* RECOMP_TOUCH_DRAG_SPLIT */
    return flags;
}

/* The battle's action cursors, the ones 00750710 shows the damage hint
 * (cursor_prediction) beside: attack, cast, shoot, shoot with the penalty
 * (the broken arrow), ability, and the eight melee directions. */
static int battle_hint_cursor(uint32_t id) {
    return id == 0x01 || id == 0x02 || id == 0x05 || id == 0x06 || id == 0x08 ||
           (id >= 0x12 && id <= 0x19);
}

/* Whether the battle cursor is drawn over the hardware one: on an iPhone or
 * iPad by default, RECOMP_HOMM5_BATTLE_CURSOR=0/1 to choose. */
static int battle_cursor_wanted(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_HOMM5_BATTLE_CURSOR");
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        on = !v || *v != '0';
#else
        on = v && *v == '1';
#endif
    }
    return on;
}

void HOMM5_FN_CURSOR_DRAW(X86 *c);

/* NUI::CCursor::Draw (thiscall, no stack arguments). With the hardware cursor
 * (the iOS default) it leaves the cursor to the system, and a touch screen
 * shows none: the battle's damage hint stood beside an empty spot, without
 * the picture that says which side the attack comes from or whether a shot
 * is at the penalty. While the battlefield shows an action's cursor, and so
 * its hint, Draw takes the software path: the game's own texture where a Mac
 * shows it, which the hint is already placed against. */
void homm5_cursor_draw(X86 *c) {
    const uint8_t hw = rd8(CURSOR_HW);
    const uint32_t screen = hw && battle_cursor_wanted() ? top_screen() : 0;
    if (screen && peek32(screen) == VT_COMBAT && !screen_modal(screen, SUB_COMBAT) &&
        battle_hint_cursor(rd32(CURSOR_ID))) {
        wr8(CURSOR_HW, 0);
        HOMM5_FN_CURSOR_DRAW(c);
        wr8(CURSOR_HW, hw);
        return;
    }
    HOMM5_FN_CURSOR_DRAW(c);
}

int recomp_pointer_place(int32_t x, int32_t y, int32_t width, int32_t height) {
    uint32_t vtable;
    float fx, fy;
    if (!g_mem || width <= 0 || height <= 0)
        return 0;
    memcpy(&vtable, g_mem + CURSOR_OBJECT, sizeof vtable);
    if (vtable != CURSOR_VTABLE)
        return 0; /* not constructed yet: the host's own placement stands */
    if (x < 0)
        x = 0;
    else if (x >= width)
        x = width - 1;
    if (y < 0)
        y = 0;
    else if (y >= height)
        y = height - 1;
    fx = (float)x;
    fy = (float)y;
    memcpy(g_mem + CURSOR_X, &fx, sizeof fx);
    memcpy(g_mem + CURSOR_Y, &fy, sizeof fy);
    homm5_tap_pointer_placed(fx, fy); /* a battle parks it between turns */
    return 1;
}

uint32_t homm5_guest_call_regs(X86 *c, uint32_t fn, uint32_t ecx, uint32_t edx,
                               const uint32_t *args, int nargs, uint32_t stack_top);
#include "touch_native.c"
