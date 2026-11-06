/* native/ui_bounds.c - the player's interface bounds on screens wider than
 * the interface (design notes ultrawide-plan.md), shared by the base game and Tribes
 * of the East. Addresses come from the game's RECOMP_OVERRIDE_HEADER
 * (games/<id>/native/overrides.h), the base game's being the defaults, as in
 * native/horplus.c; the header also redirects the axis function to
 * homm5_anchor_axis.
 *
 * On a wide screen the edge-anchored windows of the interface (the minimap,
 * the round menus, the battle's panels) sit at the screen's edges. The player
 * may keep them inside a narrower box centred on the screen: 4:3, 16:9 or
 * 21:9 (the launcher's setting homm5.wide/ui_bounds, 0 the whole screen,
 * 1 21:9, 2 16:9, 3 4:3; RECOMP_HOMM5_UI_BOUNDS wins). Screens, backgrounds
 * and the 3D view keep the whole screen: only a window anchored to the left
 * or right edge of a parent as wide as the virtual screen is anchored to the
 * box's edge instead. RECOMP_TRACE_WIDE=1 logs the first of them.
 *
 * Hammers of Fate lays its interface out on a 1024-wide virtual screen
 * centred on the real one (native/hof_screen.c), so its screen spans
 * HOMM5_SCREEN_SPAN instead, and HOMM5_STRETCH_ROOTS lets a root screen whose
 * own placement stretches across its parent (EPA_MARGIN, what the wide-screen
 * pak gives Tribes of the East's ALIGN_SCREEN screens) span the whole screen:
 * that game has no root alignment of its own.
 *
 * Nor does it place its roots again, so there a phone's safe area is kept
 * here too (homm5_safe_span): the windows anchored to the left or right edge
 * of a screen-wide root are anchored that far in, the camera housing's side
 * clear and the other a narrow margin. When the phone turns and the housing
 * changes sides, native/phone_hud.c lays the screens shown out again. The
 * root itself and what it stretches (the 3D view, backgrounds) keep the
 * whole screen: the game measures its 3D picture by the root.
 * native/phone_hud.c lays the larger panels out in the same span. In the
 * base game and Tribes of the East the safe area is native/phone_hud.c's
 * placement of a root on a narrowed virtual screen. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "native_seam.h"
#include "x86.h"
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#ifdef RECOMP_OVERRIDE_HEADER
#include RECOMP_OVERRIDE_HEADER
#endif
#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif
/* The interface's current virtual width (float), the axis function and the
 * address SetParentRect (00e3b6f0 in the base game, EDI the window
 * throughout) asks it for its x from. */
#ifndef HOMM5_VIRTUAL_W
#define HOMM5_VIRTUAL_W 0x010d2ce4
#endif
#ifndef HOMM5_FN_ANCHOR_AXIS
#define HOMM5_FN_ANCHOR_AXIS fn_00e43650
#endif
#ifndef HOMM5_RET_ANCHOR_X
#define HOMM5_RET_ANCHOR_X 0x00e3b803
#endif
/* And its y, right after (native/phone_layout.c lays the phone's rows out in
 * both; the dragged icon is lifted in y). */
#ifndef HOMM5_RET_ANCHOR_Y
#define HOMM5_RET_ANCHOR_Y 0x00e3b84d
#endif
/* What makes a battle's stack counter (ID_STACKINFO_WINDOW) and adds it to
 * the battle screen; Tribes of the East's is 00739b00. Hammers of Fate has
 * its own HOMM5_COUNTER_WINDOW (native/hof_screen.c). */
#if !defined(HOMM5_COUNTER_WINDOW) && !defined(HOMM5_FN_COUNTER_MAKE)
#define HOMM5_FN_COUNTER_MAKE fn_008333f0
#endif

void homm5_phone_layout_axis(X86 *c, int vertical); /* native/phone_layout.c */

enum {
    /* A child window's anchor along an axis (EPA_*), as the axis takes it. */
    EPA_LOW_END = 0,
    EPA_HIGH_END = 2,
    EPA_MARGIN = 3,
    WINDOW_PARENT = 0x28, /* 0 for a root screen */
};

void HOMM5_FN_ANCHOR_AXIS(X86 *c);
#ifdef HOMM5_SCREEN_SPAN
void HOMM5_SCREEN_SPAN(float *x, float *w);
#endif
#ifdef HOMM5_COUNTER_WINDOW
int HOMM5_COUNTER_WINDOW(uint32_t win);
#endif
#ifdef HOMM5_STRETCH_ROOTS
int homm5_hud_free_window(uint32_t win); /* native/phone_hud.c */
#endif

/* Where the real screen's left edge is in interface units, and its width. */
static void screen_span(float *x, float *w) {
#ifdef HOMM5_SCREEN_SPAN
    HOMM5_SCREEN_SPAN(x, w);
#else
    *x = 0.0f;
    *w = rdf32(HOMM5_VIRTUAL_W);
#endif
}

#ifdef HOMM5_STRETCH_ROOTS
/* A phone's safe area across as fractions of the picture, left and right
 * (RECOMP_HOMM5_SAFE_INSETS=l,t,r,b stands in for the host's, as in
 * native/phone_hud.c): the side with the camera housing kept clear, the other
 * a narrow margin for the rounded corner. */
void homm5_safe_sides(float *l, float *r) {
    float f[4] = {0, 0, 0, 0};
    const char *v = getenv("RECOMP_HOMM5_SAFE_INSETS");
    if (v && *v)
        sscanf(v, "%f,%f,%f,%f", &f[0], &f[1], &f[2], &f[3]);
    else if (host_display_phone())
        host_display_safe_insets(&f[0], &f[1], &f[2], &f[3]);
    *l = f[0] > 0.0f && f[0] < 0.25f ? f[0] : 0.0f;
    *r = f[2] > 0.0f && f[2] < 0.25f ? f[2] : 0.0f;
}

/* The screen's span without the safe area's sides. */
void homm5_safe_span(float *x, float *w) {
    float l, r;
    screen_span(x, w);
    homm5_safe_sides(&l, &r);
    const float full = *w;
    *x += l * full;
    *w -= (l + r) * full;
}
#endif

/* Set once a root has been stretched across the screen: the game's own
 * picture <-> interface math then sees the screen's width again
 * (native/hof_screen.c drops its 4:3 corrections). */
int homm5_ui_roots_stretched;

static int near(float a, float b) {
    return a > b - 0.5f && a < b + 0.5f;
}
int mods_settings_file_get(const char *key, int64_t *out);

/* The aspect of the box, 0 for the whole screen. Read once: the interface is
 * laid out first at start. */
static float ui_bounds(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *v = getenv("RECOMP_HOMM5_UI_BOUNDS");
        int64_t stored = 0;
        if (v && *v)
            mode = atoi(v);
        else
            mode = mods_settings_file_get("homm5.wide/ui_bounds", &stored) ? (int)stored : 0;
    }
    switch (mode) {
    case 1:
        return 64.0f / 27.0f; /* 21:9, as 2560x1080 */
    case 2:
        return 16.0f / 9.0f;
    case 3:
        return 4.0f / 3.0f;
    default:
        return 0.0f;
    }
}

static int trace_wide(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_TRACE_WIDE");
        on = v && *v && v[0] != '0';
    }
    return on;
}

#ifdef HOMM5_COUNTER_WINDOW
/* A window the game placed itself from the interface's left edge (a stack
 * counter's button, a combo box's drop-down list under its field): it stands
 * at its own offset, where one laid out on a stretched root stands at the
 * root's edge plus it. */
static int placed_from_zero(X86 *c) {
    const uint32_t win = c->r[R_EDI];
    const float off = rdf32(c->r[R_ESP] + 12);
    const uint32_t vt = rd32(win);
#ifdef HOMM5_VT_COMBO_LIST
    if (vt == HOMM5_VT_COMBO_LIST && near(rdf32(win + 0x64), off))
        return 1;
#endif
    return vt == HOMM5_VT_MS_BUTTON && off > 1.0f && near(rdf32(win + 0x64), off);
}
#endif

#ifdef HOMM5_FN_COUNTER_MAKE
/* A battle's stack counter in the base game and Tribes of the East: a button
 * anchored to the left of the screen-wide battle screen, at offset 0 when
 * 008333f0 makes it and adds it to the screen. 00833530 then moves it over
 * its creature from the base that first layout gave it, without the axis:
 * kept in the player's box like the panels, every counter slid right by the
 * box's margin for good. A window laid out while a counter is made is one,
 * noted for its later layouts; the box leaves them where the game puts them.
 * A flag rather than a depth: a guest exception that skipped the reset is
 * mended by the next counter. */
void HOMM5_FN_COUNTER_MAKE(X86 *c);
enum { COUNTERS = 256 };
static uint32_t g_counters[COUNTERS];
static int g_counter_next, g_making_counter;

static int known_counter(uint32_t win) {
    for (int i = 0; i < COUNTERS; ++i)
        if (g_counters[i] == win)
            return 1;
    return 0;
}

static void note_counter(uint32_t win) {
    if (known_counter(win))
        return;
    g_counters[g_counter_next] = win;
    g_counter_next = (g_counter_next + 1) % COUNTERS;
}

void homm5_counter_make(X86 *c) {
    g_making_counter = 1;
    HOMM5_FN_COUNTER_MAKE(c);
    g_making_counter = 0;
}

static int counter_window(uint32_t win) {
    if (g_making_counter) {
        note_counter(win);
        return 1;
    }
    return known_counter(win);
}
#endif

/* Windows the game places itself. The message boxes (the next player's turn,
 * the save under way, every question) are centred by code, and a dragged
 * creature's icon follows the cursor: each is laid out at offset 0, then
 * SetPlacement (00e3b9f0, Hammers of Fate's 00e479c0, Tribes of the East's
 * 00e35920) gives it its offset and moves its laid-out position by the
 * change. An offset counted from the screen's left edge, added to the box's
 * edge, stood the box's margin to the right. A window whose x offset the game
 * changes is placed from the screen's edge from then on, the shift its
 * last layout took undone first (its children follow as the game moves it).
 * A HUD window keeps the box: the game never moves one from its file's
 * place. Each window is noted with its vtable and the offset it was last
 * placed at, so another window at the same address later starts afresh (the
 * hire screen's panels took a freed slider's note). */
#ifndef HOMM5_FN_SET_PLACEMENT
#define HOMM5_FN_SET_PLACEMENT fn_00e3b9f0
#endif
void HOMM5_FN_SET_PLACEMENT(X86 *c);

enum {
    WINDOW_X = 0x64,          /* the laid-out position (SetParentRect's out) */
    WINDOW_PLACEMENT = 0x10c, /* the IWindow sub-object SetPlacement is called on */
    PLACE_X = 1,              /* SetPlacement's flag: the x offset given */
    LAID_SLOTS = 4096,
};

typedef struct {
    uint32_t win, vt;
    float off;  /* the x offset of its last layout */
    float dx;   /* how far its last layout stood from the game's own edge */
    int placed; /* the game has moved it */
} Laid;
static Laid g_laid[LAID_SLOTS];

/* The window's note, made when `make`; NULL when there is none. */
static Laid *laid(uint32_t win, int make) {
    const uint32_t vt = rd32(win);
    const uint32_t home = (win >> 4) & (LAID_SLOTS - 1);
    Laid *free_slot = NULL;
    for (uint32_t i = 0; i < 16; ++i) {
        Laid *l = &g_laid[(home + i) & (LAID_SLOTS - 1)];
        if (l->win == win) {
            if (l->vt == vt)
                return l;
            *l = (Laid){win, vt, 0.0f, 0.0f, 0}; /* another window there now */
            return make ? l : NULL;
        }
        if (!l->win && !free_slot)
            free_slot = l;
    }
    if (!make)
        return NULL;
    Laid *l = free_slot ? free_slot : &g_laid[home];
    *l = (Laid){win, vt, 0.0f, 0.0f, 0};
    return l;
}

/* SetPlacement: thiscall (ECX the window's IWindow at +0x10c; x, y, w, h
 * offsets, flags), RET 0x14. */
void homm5_window_place(X86 *c) {
    const uint32_t win = c->r[R_ECX] - WINDOW_PLACEMENT, esp = c->r[R_ESP];
    if (rd32(esp + 20) & PLACE_X) {
        const float off = rdf32(esp + 4);
        Laid *l = laid(win, 0);
        if (!l) {
            /* Not laid out against the screen yet: placed before it is. */
            l = laid(win, 1);
            l->placed = 1;
            if (trace_wide())
                fprintf(stderr, "[wide] window %08x (vt %08x) placed by the game at %.0f\n", win,
                        l->vt, off);
        } else if (!near(off, l->off) || l->placed) {
            if (trace_wide() && !l->placed)
                fprintf(stderr, "[wide] window %08x (vt %08x) placed by the game at %.0f (was %.0f): %.0f back\n",
                        win, l->vt, off, l->off, l->dx);
            if (l->dx != 0.0f)
                wrf32(win + WINDOW_X, rdf32(win + WINDOW_X) - l->dx);
            l->dx = 0.0f;
            l->placed = 1;
        }
        if (l)
            l->off = off;
    }
    HOMM5_FN_SET_PLACEMENT(c);
}

/* The axis (00e43650 in the base game): a child window's position along one
 * axis (fastcall: ECX the anchor, EDX its size (in/out); stack: the parent's
 * position and size, the window's offset, its two margins, and where the
 * position goes; RET 0x18). Centred and stretched windows (backgrounds, the
 * 3D view) are left alone. */
/* The icon of a creature or hero being dragged (UI/Common/Creatures/DragIcon,
 * 72 x 70 as tools/touch_ui_mod.py turns its frame): the game lays it out with
 * its top left at the cursor. On iOS a finger would cover it, so it goes up
 * by its height, above and to the right of the finger, the frame's arrow
 * pointing down at it. Known by its size as its x is laid out, and lifted as
 * its y is. */
static int near(float a, float b);
enum { DRAG_ICON_W = 72, DRAG_ICON_H = 70 };
static uint32_t g_drag_icon;

static int lift_drag_icon(void) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_HOMM5_DRAG_ABOVE");
        on = v && *v ? v[0] != '0' : 1;
    }
    return on;
#else
    const char *v = getenv("RECOMP_HOMM5_DRAG_ABOVE"); /* to try it on a Mac */
    return v && v[0] == '1';
#endif
}

void homm5_anchor_axis(X86 *c) {
    const uint32_t esp = c->r[R_ESP];
    if (rd32(esp) == HOMM5_RET_ANCHOR_Y) {
        homm5_phone_layout_axis(c, 1);
        const uint32_t win = c->r[R_EDI], out = rd32(esp + 0x18);
        const float extent = rdf32(c->r[R_EDX]);
        HOMM5_FN_ANCHOR_AXIS(c);
        if (lift_drag_icon() && win == g_drag_icon && near(extent, DRAG_ICON_H) && out)
            wrf32(out, rdf32(out) - DRAG_ICON_H);
        return;
    }
    if (rd32(esp) != HOMM5_RET_ANCHOR_X) {
        HOMM5_FN_ANCHOR_AXIS(c);
        return;
    }
    /* A phone's rows first: the anchor they give is the one the box below
     * goes by. */
    homm5_phone_layout_axis(c, 0);
    if (lift_drag_icon()) {
        if (near(rdf32(c->r[R_EDX]), DRAG_ICON_W))
            g_drag_icon = c->r[R_EDI];
        else if (g_drag_icon == c->r[R_EDI])
            g_drag_icon = 0;
    }
    const uint32_t anchor = c->r[R_ECX];
    const int root = rd32(c->r[R_EDI] + WINDOW_PARENT) == 0;
    const float parent_x = rdf32(esp + 4), parent_w = rdf32(esp + 8);
    float span_x, span_w;
    screen_span(&span_x, &span_w);
#ifdef HOMM5_STRETCH_ROOTS
    /* A root screen gets the 1024 units the game knows; a stretched one gets
     * the whole screen. */
    if (root) {
        const int stretch = anchor == EPA_MARGIN && span_w > 1024.5f && near(parent_x, 0.0f) &&
                            near(parent_w, 1024.0f);
        static int traced_roots;
        if (trace_wide() && traced_roots < 100) {
            ++traced_roots;
            fprintf(stderr, "[wide] root %08x anchor %u parent %.0f+%.0f size %.0f%s\n",
                    c->r[R_EDI], anchor, parent_x, parent_w, rdf32(c->r[R_EDX]),
                    stretch ? ": stretched across the screen" : "");
        }
        if (stretch) {
            homm5_ui_roots_stretched = 1;
            wrf32(esp + 4, span_x);
            wrf32(esp + 8, span_w);
        }
    }
#endif
#ifdef HOMM5_COUNTER_WINDOW
    /* A stack counter is placed from the interface's own left edge, whatever
     * the root's stretch (native/hof_screen.c). */
    if (!root && anchor == EPA_LOW_END && near(parent_x, span_x) &&
        (HOMM5_COUNTER_WINDOW(c->r[R_EDI]) || placed_from_zero(c))) {
        wrf32(esp + 4, 0.0f);
        wrf32(esp + 8, 1024.0f);
        HOMM5_FN_ANCHOR_AXIS(c);
        return;
    }
#endif
    /* The part of the screen an edge window may use: the safe area's, and
     * within it the player's box. */
    float safe_x = span_x, safe_w = span_w;
#ifdef HOMM5_STRETCH_ROOTS
    homm5_safe_span(&safe_x, &safe_w);
#endif
    const float aspect = ui_bounds();
    const float box = 768.0f * aspect;
    int follows_picture = 0;
#ifdef HOMM5_STRETCH_ROOTS
    /* Only a small one (the dragged icon): a panel laid out at 0,0 (Hammers
     * of Fate's hero screen, 396 x 768) stays in the safe area with its
     * bookmarks. */
    follows_picture = !root && rdf32(c->r[R_EDX]) <= 128.0f && homm5_hud_free_window(c->r[R_EDI]);
#endif
    /* Where the game counts a placed window's offset from: the screen's left
     * edge, in Hammers of Fate too (its message boxes are centred on the
     * whole screen; its stack counters, from the interface's own edge, are
     * the exception above). */
    float game_x = span_x, game_w = span_w;
    const int edge = !root && !follows_picture && (anchor == EPA_LOW_END || anchor == EPA_HIGH_END) &&
                     near(parent_x, span_x) && near(parent_w, span_w);
    /* A window the game placed is laid out at the offset it last gave it. One
     * laid out at another stands where a freed one was, of the same class
     * (most are plain windows): the note is that one's. */
    Laid *note = edge ? laid(c->r[R_EDI], 0) : NULL;
    if (note && note->placed && !near(rdf32(esp + 12), note->off)) {
        static int traced_stale;
        if (trace_wide() && traced_stale < 200) {
            ++traced_stale;
            fprintf(stderr, "[wide] window %08x (vt %08x) at %.0f is not the one placed at %.0f\n",
                    c->r[R_EDI], note->vt, rdf32(esp + 12), note->off);
        }
        note->placed = 0;
        note->dx = 0.0f;
    }
    if (note && note->placed) {
        Laid *l = note;
#ifdef HOMM5_VT_PLACED_FROM_INTERFACE
        /* Hammers of Fate's battle hint (cursor_prediction) is placed under
         * the cursor, in the interface's units: from its own edge. */
        if (l->vt == HOMM5_VT_PLACED_FROM_INTERFACE) {
            game_x = 0.0f;
            game_w = 1024.0f;
        }
#endif
        static int traced_placed;
        if (trace_wide() && traced_placed < 200) {
            ++traced_placed;
            fprintf(stderr, "[wide] window %08x (vt %08x) at %.0f laid out from the game's %.0f+%.0f\n",
                    c->r[R_EDI], l->vt, rdf32(esp + 12), game_x, game_w);
        }
        l->off = rdf32(esp + 12);
        l->dx = 0.0f;
        wrf32(esp + 4, game_x);
        wrf32(esp + 8, game_w);
        HOMM5_FN_ANCHOR_AXIS(c);
        return;
    }
    if (edge) {
        float bx = safe_x, bw = safe_w;
        if (aspect > 0.0f && box < safe_w - 0.5f) {
            bx = safe_x + (safe_w - box) * 0.5f;
            bw = box;
        }
        int keep = 0;
#ifdef HOMM5_FN_COUNTER_MAKE
        keep = anchor == EPA_LOW_END && counter_window(c->r[R_EDI]);
#endif
        if (!keep && (!near(bx, span_x) || !near(bw, span_w))) {
            static int traced;
            if (trace_wide() && traced < 200) {
                ++traced;
                fprintf(stderr, "[wide] window %08x anchored %s to %.0f+%.0f\n", c->r[R_EDI],
                        anchor == EPA_LOW_END ? "left" : "right", bx, bw);
            }
            /* The arguments are a copy SetParentRect pushed for this call. */
            wrf32(esp + 4, bx);
            wrf32(esp + 8, bw);
        }
        /* How far this stands from where the game would place it. */
        const float ux = rdf32(esp + 4), uw = rdf32(esp + 8);
        const float dx = anchor == EPA_LOW_END ? ux - game_x : (ux + uw) - (game_x + game_w);
        Laid *l = laid(c->r[R_EDI], !near(dx, 0.0f));
        if (l) {
            l->off = rdf32(esp + 12);
            l->dx = dx;
        }
    }
    HOMM5_FN_ANCHOR_AXIS(c);
}
