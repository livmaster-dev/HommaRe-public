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

#include "x86.h"
#include "platform/os.h"

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
static volatile int g_save_wanted, g_save_done;
void homm5_save_settings(X86 *c); /* native/save.cpp */

void recomp_app_background(void) {
    int waited = 0;
    g_save_done = 0;
    __atomic_store_n(&g_save_wanted, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_save_done, __ATOMIC_ACQUIRE) && waited < 3000) {
        os_sleep_us(10000);
        waited += 10;
    }
    fprintf(stderr, "[homm5] settings %s before going to the background\n",
            g_save_done ? "saved" : "not saved (the game did not reach a frame)");
}

void homm5_hover_timeout(X86 *c) {
    if (__atomic_exchange_n(&g_save_wanted, 0, __ATOMIC_ACQ_REL)) {
        homm5_save_settings(c);
        __atomic_store_n(&g_save_done, 1, __ATOMIC_RELEASE);
    }
    const int state = g_hover;
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
