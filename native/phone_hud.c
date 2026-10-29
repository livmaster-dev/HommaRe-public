/* native/phone_hud.c - Heroes V's interface inside a phone's safe area, its
 * map, battle and town panels drawn larger there (design notes ios-phone-hud-plan.md),
 * shared by the three games. Addresses come from the game's
 * RECOMP_OVERRIDE_HEADER and RECOMP_ADDRESS_HEADER (games/<id>/native/), the
 * base game's being the defaults below, as in native/horplus.c.
 *
 * The base game and Tribes of the East place each root WindowScreen with the
 * same function (00e74f30, 00e734a0 there): a root against an edge is placed
 * on a virtual screen narrowed by the safe area and moved in through the
 * origin (hud_place). The base game's native/widescreen.c calls it from its own
 * placement hook; Tribes of the East's hook is homm5_hud_screen_place here
 * (HOMM5_HUD_PLACE_HOOK).
 *
 * Hammers of Fate has no root placement (HOMM5_HUD_ROOT_RECT): its interface
 * is 1024x768 units centred on the screen, with fixed virtual sizes in the
 * conversions (native/hof_screen.c). The safe area is applied where its
 * windows are anchored (native/ui_bounds.c, homm5_safe_span), a root keeps the
 * plain mapping, and a panel is placed in the root's own units from the safe
 * span's left edge (Hud x0). The windows a root holds as it is described are
 * read after 00e7f630 (homm5_hud_root_described). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native_seam.h"
#include "x86.h"
#include "platform/os.h"

#ifdef RECOMP_OVERRIDE_HEADER
#include RECOMP_OVERRIDE_HEADER
#endif
#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif

/* The base game's addresses (an expansion's addresses.h names its own). */
#ifndef HOMM5_SCREEN_STACK
#define HOMM5_SCREEN_STACK 0x0114b2e4
#define HOMM5_VT_ADVENTURE 0x00fab2ac
#define HOMM5_VT_COMBAT 0x00fa79c8
#define HOMM5_SUB_ADVENTURE 0x7b4
#define HOMM5_SUB_COMBAT 0x658
#endif
#ifndef HOMM5_VT_TOWN
#define HOMM5_VT_TOWN 0x00fc0d80   /* NUI::CTownScreen's IScreen, at +0x4b8 */
#define HOMM5_SUB_TOWN 0x4b8
#define HOMM5_VT_LOADING 0x00fb3c68 /* NUI::CPWLScreenInterface's, at +0x1cc */
#define HOMM5_SUB_LOADING 0x1cc
#define HOMM5_VT_HIRE 0x00fc07a4   /* NUI::CHireScreen's (FortScreen), at +0x280 */
#define HOMM5_SUB_HIRE 0x280
#define HOMM5_VT_BUILD 0x00fc1e3c  /* NUI::CTownBuildScreen's (TownBuild), at +0x26c */
#define HOMM5_SUB_BUILD 0x26c
#define HOMM5_VT_HERO 0x00fb48d8   /* NUI::CHeroScreen2's (HeroScreen2), at +0x7d4 */
#define HOMM5_SUB_HERO 0x7d4
#endif
#ifndef HOMM5_SCREEN_WINDOWS
#define HOMM5_SCREEN_WINDOWS 0xa0 /* a game screen: its root WindowScreen */
#endif
#ifndef HOMM5_SCREEN_DESC
#define HOMM5_SCREEN_DESC 0xc8 /* a root WindowScreen: its shared description */
#endif
#ifndef HOMM5_VT_LOADING_MAIN
#define HOMM5_VT_LOADING_MAIN 0x00fb3c00 /* NUI::CPWLScreenInterface */
#define HOMM5_VT_MS_BUTTON 0x0102bb54    /* CWindowMSButton (by its RTTI) */
#define HOMM5_VT_EDIT_LINE 0x0102fc14    /* CWindowEditLine */
#define HOMM5_VT_EDIT_BOX 0x01033eb4     /* CWindowEditBox */
#define HOMM5_VT_LIST 0x0102bd58         /* CWindowListCtrl */
#define HOMM5_VT_SCROLLER 0x01030a30     /* CWindowScrollableContainer */
#define HOMM5_VT_TEXT_VIEW 0x01031588    /* CWindowTextView */
#endif
/* The conversions' globals (floats; CVirtualScreenController). Hammers of
 * Fate has no virtual size to read: its conversions have 1024x768 built in. */
#if !defined(HOMM5_UI_REAL_W) && !defined(HOMM5_HUD_ROOT_RECT)
#define HOMM5_UI_VIRTUAL_W 0x010d2ce4 /* the virtual screen every conversion reads */
#define HOMM5_UI_VIRTUAL_H 0x010d2ce8
#define HOMM5_UI_REAL_W 0x010d2cf4    /* the backbuffer */
#define HOMM5_UI_REAL_H 0x010d2cf8
#define HOMM5_UI_ORIGIN_X 0x012a6654  /* the virtual point at the screen's corner */
#define HOMM5_UI_ORIGIN_Y 0x012a6658
#endif
#ifndef HOMM5_FN_WINDOW_DRAW
#define HOMM5_FN_WINDOW_DRAW fn_00e3dd70     /* CWindow::Draw */
#define HOMM5_FN_WINDOW_CONTAINS fn_00e3bd00 /* whether a point is a window's */
#define HOMM5_FN_SIMPLE_DRAW fn_00e45ad0     /* a CWindowSimple's clipped draw */
#define HOMM5_FN_TEXT_LAYOUT fn_00e453f0     /* a window text's layout */
#define HOMM5_ADDR_TEXT_LAYOUT 0x00e453f0u
#define HOMM5_FN_LOADING_SCREEN fn_0083be20  /* CPWLScreenInterface's constructor */
#define HOMM5_FN_SCREEN_MOUSE_0 fn_00e3c9b0  /* a WindowScreen's mouse handlers */
#define HOMM5_FN_SCREEN_MOUSE_1 fn_00e3ca70
#define HOMM5_FN_SCREEN_MOUSE_2 fn_00e3cb00
#define HOMM5_FN_SCREEN_MOUSE_3 fn_00e3cbc0
#define HOMM5_FN_SCREEN_MOUSE_4 fn_00e3cc80
#define HOMM5_FN_SCREEN_MOUSE_5 fn_00e3cd10
#define HOMM5_FN_SCREEN_MOUSE_6 fn_00e3cdd0
#define HOMM5_FN_TOOLTIP_TIMER fn_00ea0fc0   /* the tooltip controller's timer */
#define HOMM5_FN_TOOLTIP_MOVE fn_00ea0f00    /* and its mouse move */
#define HOMM5_FN_SCREEN_PLACE fn_00e74f30    /* a root WindowScreen's placement */
#define HOMM5_ADDR_SCREEN_PLACE 0x00e74f30u
#endif

enum {
    SCREEN_STACK = HOMM5_SCREEN_STACK, /* std::list<IScreen*>: the sentinel node */
    VT_ADVENTURE = HOMM5_VT_ADVENTURE, /* IScreen subobject vtables (native/touch.c) */
    VT_COMBAT = HOMM5_VT_COMBAT,
    SUB_ADVENTURE = HOMM5_SUB_ADVENTURE, /* the screen object, back from that subobject */
    SUB_COMBAT = HOMM5_SUB_COMBAT,
    VT_TOWN = HOMM5_VT_TOWN,
    SUB_TOWN = HOMM5_SUB_TOWN,
    VT_LOADING = HOMM5_VT_LOADING,
    VT_HIRE = HOMM5_VT_HIRE,
    SUB_HIRE = HOMM5_SUB_HIRE,
    VT_BUILD = HOMM5_VT_BUILD,
    SUB_BUILD = HOMM5_SUB_BUILD,
    VT_HERO = HOMM5_VT_HERO,
    SUB_HERO = HOMM5_SUB_HERO,
    LOADING_VTABLE = HOMM5_VT_LOADING_MAIN,
    MS_BUTTON_VTABLE = HOMM5_VT_MS_BUTTON,
    SUB_LOADING = HOMM5_SUB_LOADING,
#ifdef HOMM5_UI_VIRTUAL_W
    VIRTUAL_W = HOMM5_UI_VIRTUAL_W,
    VIRTUAL_H = HOMM5_UI_VIRTUAL_H,
#endif
    REAL_W = HOMM5_UI_REAL_W,
    REAL_H = HOMM5_UI_REAL_H,
    ORIGIN_X = HOMM5_UI_ORIGIN_X,
    ORIGIN_Y = HOMM5_UI_ORIGIN_Y,
    SCREEN_ROOT = HOMM5_SCREEN_WINDOWS, /* a game screen: its root WindowScreen (placement's ECX) */
    SCREEN_DESC = HOMM5_SCREEN_DESC, /* WindowScreen: its shared description */
    DESC_HOR_ALIGN = 0xfc,
    DESC_VER_ALIGN = 0x100,
    ALIGN_LOW_END = 0,
    ALIGN_CENTER = 1,
    ALIGN_HIGH_END = 2,
    ALIGN_SCREEN = 3,
};

#ifdef HOMM5_FN_SCREEN_PLACE
void HOMM5_FN_SCREEN_PLACE(X86 *c);
static void hud_place(X86 *c, uint32_t window);
#endif
static int trace_hud(void);
static uint32_t g_roots[7]; /* the map's, the battle's, the town's, the loading screen's, the
                              * hire screen's, the build screen's and the hero screen's */

/* Whether an alignment puts a root against the screen's edge: whole-screen
 * roots and those at the far end (the near end is centred, above). */
static uint32_t loading_root(void);

static int edge_align(uint32_t a) {
    return a == ALIGN_HIGH_END || a == ALIGN_SCREEN;
}

/* The roots placed against an edge, with their descriptions: the safe area
 * moves them in, and their drawing and mouse follow (native/widescreen.c,
 * the interface section). A root that went away and whose memory now holds
 * something else no longer has its description at +0xc8. */
static struct {
    uint32_t root, desc;
} g_edge[64];

void homm5_hud_note_edge_root(uint32_t root, uint32_t desc) {
    static int next;
    for (int i = 0; i < 64; ++i)
        if (g_edge[i].root == root) {
            g_edge[i].desc = desc;
            return;
        }
    g_edge[next].root = root;
    g_edge[next].desc = desc;
    next = (next + 1) % 64;
}

/* What tells a noted root from other memory there later: its description,
 * or in Hammers of Fate (whose root may get its description only after it is
 * laid out) its window's vtable while it has no parent. */
static uint32_t edge_key(uint32_t root) {
#ifdef HOMM5_HUD_ROOT_RECT
    return rd32(root - 0xf8u + 0x28u) ? 0 : rd32(root - 0xf8u); /* ROOT_MAIN, WINDOW_PARENT */
#else
    return rd32(root + SCREEN_DESC);
#endif
}

static uint32_t edge_root_desc(uint32_t root) {
    if (!root)
        return 0;
    for (int i = 0; i < 64; ++i)
        if (g_edge[i].root == root)
            return g_edge[i].desc && edge_key(root) == g_edge[i].desc ? g_edge[i].desc : 0;
    return 0;
}

static int env_flag(const char *name, int fallback) {
    const char *v = getenv(name);
    return v && *v ? v[0] != '0' : fallback;
}

/* RECOMP_HOMM5_WIDE_UI=0: the interface as the game lays it out. */
static int wide_ui(void) {
    static int on = -1;
    if (on < 0)
        on = env_flag("RECOMP_HOMM5_WIDE_UI", 1);
    return on;
}

/* ---- The interface inside the safe area, and larger on a phone -----------
 * Every conversion between the interface's virtual pixels and the screen's
 * real ones reads the same globals,
 *   real = (virtual - origin) * (real size / virtual size),
 * and none of them takes a window: they run while a window draws (CWindow::
 * Draw, 00e3dd70, for each window and its children) and while a mouse message
 * is handled (a WindowScreen's handlers convert its position with 00e38700).
 * A root WindowScreen against an edge of the screen gets its own mapping for
 * both, and for its placement (00e74f30):
 *  - inside the safe area: placed on a virtual screen narrowed by the insets
 *    (the side with the camera housing, the host's host_display_safe_insets),
 *    then moved in by them through the origin. Only on a phone.
 *  - except the loading screen (PWLScreen), whose picture fills the screen;
 *  - the map's, the battle's and the town's interface larger
 *    (AdventureScreen-Heavy, CombatScreen-Heavy, TownScreen, one root each at
 *    game screen + 0xa0): the root keeps
 *    the plain scale, and each of its children that stands against an edge (a
 *    panel: the resource bar, the jog dial and each of its buttons, the hero
 *    list, the turn order bar) is drawn k times larger around the root's edge
 *    or edge centre it stands by, so the jog dial's base and its buttons grow
 *    around one corner together. The minimap keeps its size (the whole map in
 *    it, and at k half the screen's height), and so does what stands free: the
 *    dialogs the game opens over the map are children of the same root.
 * The mouse follows through the one test every window's hit test ends in
 * (00e3bd00, "is this point mine"): the point is carried into the root's
 * frame when the game worked it out with the plain mapping (as it does when
 * it asks whether the cursor is over the interface, 007c15a0), and into a
 * panel's frame for the windows of a panel.
 * Text follows: a text view lays itself out again when the real size it was
 * laid out for changes, and the fonts' pixel size comes from the mapping.
 * RECOMP_HOMM5_MAP_UI_SCALE / RECOMP_HOMM5_BATTLE_UI_SCALE /
 * RECOMP_HOMM5_TOWN_UI_SCALE set k (1.75, 1.4 and 1.4 on a phone, 1 elsewhere); RECOMP_HOMM5_SAFE_INSETS=l,t,r,b (fractions of
 * the picture) stands in for the host's safe area; RECOMP_TRACE_HUD=1 logs
 * the placements and the panels. */

void HOMM5_FN_WINDOW_DRAW(X86 *c);
#ifndef HOMM5_HUD_ROOT_RECT
void HOMM5_FN_SCREEN_MOUSE_0(X86 *c);
void HOMM5_FN_SCREEN_MOUSE_1(X86 *c);
void HOMM5_FN_SCREEN_MOUSE_2(X86 *c);
void HOMM5_FN_SCREEN_MOUSE_3(X86 *c);
void HOMM5_FN_SCREEN_MOUSE_4(X86 *c);
void HOMM5_FN_SCREEN_MOUSE_5(X86 *c);
void HOMM5_FN_SCREEN_MOUSE_6(X86 *c);
#endif
void HOMM5_FN_WINDOW_CONTAINS(X86 *c);
void HOMM5_FN_SIMPLE_DRAW(X86 *c);
void HOMM5_FN_TEXT_LAYOUT(X86 *c);
void HOMM5_FN_LOADING_SCREEN(X86 *c);
#ifndef HOMM5_HUD_ROOT_RECT
void HOMM5_FN_TOOLTIP_TIMER(X86 *c);
void HOMM5_FN_TOOLTIP_MOVE(X86 *c);
#endif
#ifdef HOMM5_SCREEN_SPAN
void HOMM5_SCREEN_SPAN(float *x, float *w); /* native/hof_screen.c: the screen in interface units */
#endif
#ifdef HOMM5_HUD_ROOT_RECT
void homm5_safe_span(float *x, float *w); /* native/ui_bounds.c: the safe area's part of it */
void homm5_safe_sides(float *l, float *r);
#endif
void homm5_guest_thiscall(X86 *c, uint32_t fn, uint32_t self); /* native/save.cpp */
void homm5_guest_thiscall_rect(X86 *c, uint32_t fn, uint32_t self, const float r[4]);
/* native/phone_layout.c: a phone's rows of buttons instead of the jog dials. */
int homm5_phone_layout_epoch(void);
int homm5_phone_layout_draw(uint32_t win);
uint32_t homm5_phone_layout_strip(uint32_t win);
void homm5_phone_layout_restore(uint32_t win, uint32_t bg);
enum { LAYOUT_HIDE = 1, LAYOUT_BARE = 2 }; /* phone_layout.c's PL_HIDE, PL_BARE */
/* native/native_screens.c: non-zero when `part` (00e3dd70's ECX) is the root
 * of a screen the host draws natively; that file reads the screen then. */
int homm5_native_screens_draw(X86 *c, uint32_t part);

enum {
    HUD_NONE = 0,
    HUD_MAP = 1,
    HUD_BATTLE = 2,
    HUD_EDGE = 3, /* any other root against an edge: the safe area only */
    HUD_TOWN = 4,
    HUD_FULL = 5, /* the loading screen: its picture fills the whole screen */
    HUD_DIALOG = 6, /* the town's hire and build screens, the hero's: only their corner dial */
    /* From a root WindowScreen as placement gets it: */
    ROOT_DRAW = 0x14, /* the draw part 00e3dd70 gets */
    ROOT_MAIN = 0xf8, /* back to its window (the CWindowScreen its mouse handlers get) */
    ROOT_TIPS = 8,    /* its tooltip controller */
    /* A window (the CWindow its parent's list of children holds): */
    WINDOW_PART = 0x10c,  /* its draw part, also ECX of 00e3bd00 */
    WINDOW_PARENT = 0x28, /* its parent window */
    WINDOW_CHILDREN = 0xa4, /* std::vector of its child windows */
    SIMPLE_DRAW = 0x110,    /* a CWindowSimple's draw with its clip: ECX of 00e45ad0 */
    PART_TEXT = -0x9c,      /* a draw part: the window's text, laid out by 00e453f0 */
    WINDOW_X = 0x64,      /* float position in its parent */
    WINDOW_Y = 0x68,
    WINDOW_PLACED = 0x70, /* its placement: */
    PLACED_RECT = 0x54,   /* float x1, y1, x2, y2 as laid out */
    /* The minimap's frame in virtual pixels (Minimap-heavy). */
    /* The town's inner screens' dial as tools/phone_ui_mod.py frames it. */
    DIAL_FRAME_W = 271,
    DIAL_FRAME_H = 167,
    MINIMAP_X = 2,
    MINIMAP_W = 219,
    MINIMAP_H = 221,
    /* The town's two dials as tools/phone_ui_mod.py frames them (FRAME_W x
     * FRAME_H), against the bottom corners at MARGIN. */
    MINIMAP_ART_MARGIN = 16, /* from the window's edges to its frame's picture */
    TOWN_FRAME_W = 208,
    TOWN_FRAME_H = 334,
    FRAME_MARGIN = 6,
};

typedef struct {
    float vw, vh, rw, rh, ox, oy;
} Mapping;

static Mapping mapping_now(void) {
#ifdef HOMM5_UI_VIRTUAL_W
    Mapping m = {rdf32(VIRTUAL_W), rdf32(VIRTUAL_H), rdf32(REAL_W),
                 rdf32(REAL_H),    rdf32(ORIGIN_X),  rdf32(ORIGIN_Y)};
#else
    Mapping m = {1024.0f, 768.0f, rdf32(REAL_W), rdf32(REAL_H), rdf32(ORIGIN_X), rdf32(ORIGIN_Y)};
#endif
    return m;
}

static void mapping_set(const Mapping *m) {
#ifdef HOMM5_UI_VIRTUAL_W
    wrf32(VIRTUAL_W, m->vw);
    wrf32(VIRTUAL_H, m->vh);
#endif
    wrf32(REAL_W, m->rw);
    wrf32(REAL_H, m->rh);
    wrf32(ORIGIN_X, m->ox);
    wrf32(ORIGIN_Y, m->oy);
}

/* Mappings of ours nest: a root may be placed or handed the mouse while
 * another root draws (a save loads under the loading screen's drawing). The
 * plain mapping is the one in force before the outermost of ours, and what
 * the game itself writes meanwhile (a new screen size) is kept, not undone. */
static int g_mapped; /* mappings of ours in force */
static Mapping g_plain_saved;
/* The mappings ours wrote since the outermost began, for its end. */
static Mapping g_written[16];
static int g_written_count;
static float g_text_k; /* the scale of the panel being drawn (texts, below) */

static Mapping plain_mapping(void) {
    return g_mapped ? g_plain_saved : mapping_now();
}

/* A scope of ours as its native frame holds it, and as the list of those in
 * force keeps it: the guest thread (its CPU) and where its stack stood. */
typedef struct {
    Mapping back, set;
    X86 *cpu;
    uint32_t esp;
    float text_k;   /* the panel scale when it began */
    uint32_t serial; /* 0: not in the list (full) */
} Scope;

enum { MAPPING_FIELDS = sizeof(Mapping) / sizeof(float), SCOPES_MAX = 32 };
static Scope g_scopes[SCOPES_MAX];
static int g_scope_count;

static void scope_remove(int i) {
    memmove(&g_scopes[i], &g_scopes[i + 1], (size_t)(g_scope_count - i - 1) * sizeof g_scopes[0]);
    --g_scope_count;
}

static void scope_undo(const Scope *sc);

/* Scopes whose guest frame is gone. A guest exception the game catches
 * further out resumes there by longjmp (kit/runtime/seh.cpp), past the native
 * code that would have ended them: a save loaded from a click unwinds the
 * click's handler so, and its mapping (the origin moved in by the safe area)
 * stayed in force for everything after - a battle's stack counters went off
 * the screen and the spellbook took clicks a safe area's width to the right.
 * On one thread a frame that still runs sits above (a higher ESP than) the
 * code it calls, so a scope of this thread entered at or below the ESP of the
 * code running now has ended. */
static void scopes_unwound(X86 *c) {
    const uint32_t esp = c->r[R_ESP];
    for (int i = g_scope_count - 1; i >= 0; --i) {
        if (g_scopes[i].cpu != c || g_scopes[i].esp > esp)
            continue;
        const Scope dead = g_scopes[i];
        scope_remove(i);
        g_text_k = dead.text_k;
        scope_undo(&dead);
        static int told;
        if (told < 8) {
            ++told;
            fprintf(stderr, "[hud] a mapping scope unwound by a guest exception ended (origin %.1f,%.1f)\n",
                    dead.set.ox, dead.set.oy);
        }
    }
}

static void scope_enter(X86 *c, Scope *sc, const Mapping *m) {
    static uint32_t serial;
    scopes_unwound(c);
    sc->back = mapping_now();
    if (g_mapped++ == 0) {
        g_plain_saved = sc->back;
        g_written_count = 0;
    }
    if (g_written_count < 16)
        g_written[g_written_count++] = *m;
    sc->set = *m;
    sc->cpu = c;
    sc->esp = c->r[R_ESP];
    sc->text_k = g_text_k;
    sc->serial = 0;
    if (g_scope_count < SCOPES_MAX) {
        sc->serial = ++serial ? serial : ++serial;
        g_scopes[g_scope_count++] = *sc;
    }
    mapping_set(m);
}

/* Ends this scope, and first any of its thread begun inside it that a guest
 * exception caught inside it left behind. */
static void scope_leave(X86 *c, const Scope *sc) {
    if (sc->serial) {
        int at = -1;
        for (int i = 0; i < g_scope_count && at < 0; ++i)
            if (g_scopes[i].serial == sc->serial)
                at = i;
        for (int i = g_scope_count - 1; at >= 0 && i > at; --i) {
            if (g_scopes[i].cpu != c)
                continue;
            const Scope dead = g_scopes[i];
            scope_remove(i);
            g_text_k = dead.text_k;
            scope_undo(&dead);
        }
        if (at >= 0)
            scope_remove(at);
    }
    scope_undo(sc);
}

/* Each field still as this scope set it goes back; one the game wrote
 * meanwhile (a new screen size) stays. Scopes need not end in the order they
 * began: while a save loads, the loading screen draws inside its scope and
 * the new screens are placed inside theirs, and one may end while the other's
 * mapping is in force. So the outermost end also puts back the plain value of
 * every field still holding one of ours: left there, a root's narrowed
 * virtual screen became everyone's. */
static void scope_undo(const Scope *sc) {
    const Mapping now = mapping_now();
    Mapping out = now;
    float *o = &out.vw;
    const float *n = &now.vw, *set = &sc->set.vw, *back = &sc->back.vw;
    for (int i = 0; i < MAPPING_FIELDS; ++i)
        if (n[i] == set[i])
            o[i] = back[i];
    if (--g_mapped == 0) {
        const float *plain = &g_plain_saved.vw;
        int stray = 0;
        for (int i = 0; i < MAPPING_FIELDS; ++i) {
            if (o[i] == plain[i])
                continue;
            for (int k = 0; k < g_written_count; ++k)
                if (o[i] == (&g_written[k].vw)[i]) {
                    o[i] = plain[i];
                    stray = 1;
                    break;
                }
        }
        static int told;
        if (stray && told < 8) {
            ++told;
            fprintf(stderr,
                    "[hud] scopes ended out of order; mapping put back to virtual %.0fx%.0f, "
                    "origin %.1f,%.1f (was %.0fx%.0f, %.1f,%.1f)\n",
                    out.vw, out.vh, out.ox, out.oy, now.vw, now.vh, now.ox, now.oy);
        }
    }
    if (memcmp(&out, &now, sizeof now))
        mapping_set(&out);
}

static int trace_hud(void) {
    static int on = -1;
    if (on < 0)
        on = env_flag("RECOMP_TRACE_HUD", 0);
    return on;
}

/* A phone's panel scale: `tuned` on the phone it was tuned on (an iPhone 15
 * Pro Max sideways, 430 points tall), less on a taller picture for the same
 * size in the hand (a foldable iPhone's closed screen, 466), never more on a
 * smaller one; none where the window is not laid out for a phone (an iPad, a
 * foldable opened) or the scale would be barely larger. */
static float phone_scale(float tuned) {
    if (!host_phone_layout())
        return 1.0f;
    float w, h;
    if (!host_display_picture_points(&w, &h) || h <= 0.0f)
        return tuned;
    float k = tuned * 430.0f / h;
    if (k > tuned)
        k = tuned;
    return k < 1.15f ? 1.0f : k;
}

/* Whether the scales read so far are stale: the display changed (a
 * foldable folded or opened, a rotation; host_display_generation). */
static int scales_stale(int *seen) {
    const int gen = host_display_generation();
    if (gen == *seen)
        return 0;
    *seen = gen;
    return 1;
}

static float hud_scale(int kind) {
    static float k[7] = {1, -1, -1, 1, -1, 1, -1};
    static int seen = -1;
    if (scales_stale(&seen)) {
        static const float unread[7] = {1, -1, -1, 1, -1, 1, -1};
        memcpy(k, unread, sizeof k);
    }
    if (k[kind] < 0) {
        const char *v = getenv(kind == HUD_MAP                           ? "RECOMP_HOMM5_MAP_UI_SCALE"
                               : kind == HUD_TOWN || kind == HUD_DIALOG ? "RECOMP_HOMM5_TOWN_UI_SCALE"
                                                                        : "RECOMP_HOMM5_BATTLE_UI_SCALE");
        float f = v && *v ? (float)atof(v) : phone_scale(kind == HUD_MAP ? 1.75f : 1.4f);
        if (!(f >= 0.5f && f <= 3.0f))
            f = 1.0f;
        k[kind] = f;
    }
    return k[kind];
}

/* The safe area's insets in virtual pixels of the plain mapping `m`. */
static void hud_insets(const Mapping *m, float in[4]) {
    float f[4] = {0, 0, 0, 0};
    const char *v = getenv("RECOMP_HOMM5_SAFE_INSETS");
    if (v && *v)
        sscanf(v, "%f,%f,%f,%f", &f[0], &f[1], &f[2], &f[3]);
    else if (host_display_phone())
        host_display_safe_insets(&f[0], &f[1], &f[2], &f[3]);
    in[0] = f[0] * m->vw;
    in[1] = f[1] * m->vh;
    in[2] = f[2] * m->vw;
    in[3] = f[3] * m->vh;
}

/* The map's and the battle's roots, from the screen stack: read as the
 * outermost window (the desktop all screens hang from) starts to draw. */
static void find_roots(void) {
    memset(g_roots, 0, sizeof g_roots);
    const uint32_t sentinel = rd32(SCREEN_STACK);
    int guard = 64;
    for (uint32_t node = sentinel ? rd32(sentinel) : 0; node && node != sentinel && guard--;
         node = rd32(node)) {
        const uint32_t screen = rd32(node + 8u);
        const uint32_t vt = screen ? rd32(screen) : 0;
        if (vt == VT_ADVENTURE)
            g_roots[0] = rd32(screen - SUB_ADVENTURE + SCREEN_ROOT);
        if (vt == VT_COMBAT)
            g_roots[1] = rd32(screen - SUB_COMBAT + SCREEN_ROOT);
        if (vt == VT_TOWN)
            g_roots[2] = rd32(screen - SUB_TOWN + SCREEN_ROOT);
        if (vt == VT_LOADING)
            g_roots[3] = rd32(screen - SUB_LOADING + SCREEN_ROOT);
        if (vt == VT_HIRE)
            g_roots[4] = rd32(screen - SUB_HIRE + SCREEN_ROOT);
        if (vt == VT_BUILD)
            g_roots[5] = rd32(screen - SUB_BUILD + SCREEN_ROOT);
        if (vt == VT_HERO)
            g_roots[6] = rd32(screen - SUB_HERO + SCREEN_ROOT);
    }
    if (!g_roots[3])
        g_roots[3] = loading_root();
}

/* Which interface `root` is: the map's, the battle's (wherever their screens
 * are in the stack: a root is placed before its screen is pushed), another
 * root against an edge, or none. */
static int hud_kind(uint32_t root) {
    if (!root)
        return HUD_NONE;
    if (root == loading_root())
        return HUD_FULL;
    const uint32_t sentinel = rd32(SCREEN_STACK);
    int guard = 64;
    for (uint32_t node = sentinel ? rd32(sentinel) : 0; node && node != sentinel && guard--;
         node = rd32(node)) {
        const uint32_t screen = rd32(node + 8u);
        const uint32_t vt = screen ? rd32(screen) : 0;
        if (vt == VT_ADVENTURE && rd32(screen - SUB_ADVENTURE + SCREEN_ROOT) == root)
            return HUD_MAP;
        if (vt == VT_COMBAT && rd32(screen - SUB_COMBAT + SCREEN_ROOT) == root)
            return HUD_BATTLE;
        if (vt == VT_TOWN && rd32(screen - SUB_TOWN + SCREEN_ROOT) == root)
            return HUD_TOWN;
        if ((vt == VT_HIRE && rd32(screen - SUB_HIRE + SCREEN_ROOT) == root) ||
            (vt == VT_BUILD && rd32(screen - SUB_BUILD + SCREEN_ROOT) == root) ||
            (vt == VT_HERO && rd32(screen - SUB_HERO + SCREEN_ROOT) == root))
            return HUD_DIALOG;
        if (vt == VT_LOADING && rd32(screen - SUB_LOADING + SCREEN_ROOT) == root)
            return HUD_FULL;
    }
    const uint32_t desc = edge_root_desc(root);
#ifndef HOMM5_HUD_ROOT_RECT
    /* Across the screen but centred down: only the dialogue scene is laid out
     * so (tools/widescreen_mod.py --wide-dialogues). Its parchments reach the
     * screen's edges, under a phone's camera housing too, and its words stay
     * in the middle: placed over the whole screen, like the loading screen.
     * (Hammers of Fate stretches it over the whole screen anyway.) */
    if (desc && rd32(desc + DESC_HOR_ALIGN) == ALIGN_SCREEN &&
        rd32(desc + DESC_VER_ALIGN) == ALIGN_CENTER)
        return HUD_FULL;
#endif
    return desc ? HUD_EDGE : HUD_NONE;
}

/* A root's layout: its rectangle inside the safe area (w x h, from x0,y0 in
 * the root's units: 0,0 where the root is placed inside it), the origin that
 * puts that rectangle's corner at the safe area's, and k, the scale of its
 * panels. */
typedef struct {
    uint32_t root;
    int kind;
    float k, w, h, ox, oy, x0, y0;
    Mapping plain; /* the mapping everything else is drawn with */
} Hud;

static int window_rect(uint32_t win, float r[4]);

static int hud_for(uint32_t root, Hud *h) {
    h->root = root;
    h->kind = hud_kind(root);
    if (!h->kind || h->kind == HUD_FULL)
        return 0;
    h->plain = plain_mapping();
    h->k = hud_scale(h->kind);
    h->x0 = h->y0 = 0.0f;
#ifdef HOMM5_HUD_ROOT_RECT
    /* No placement to narrow: the root keeps the plain mapping, and the safe
     * area is in its edge windows' anchors already (native/ui_bounds.c). Its
     * panels go by the safe span when the root spans the screen (the
     * wide-screen pak stretches the map's, the battle's and the town's), else
     * by the root's own 1024 units. */
    float rr[4], sx, sw, fx, fw;
    if (h->k == 1.0f && h->kind != HUD_EDGE)
        return 0;
    if (!window_rect(root - ROOT_MAIN, rr)) { /* not laid out by a parent: the game's 1024x768 */
        rr[0] = rr[1] = 0.0f;
        rr[2] = 1024.0f;
        rr[3] = 768.0f;
    }
    HOMM5_SCREEN_SPAN(&fx, &fw);
    homm5_safe_span(&sx, &sw);
    /* A root against the edges (a town's inner screens: their corner dial)
     * reaches the safe area's edges too, like a screen-wide one. */
    const int spans = (fabsf(rr[0] - fx) < 0.5f && fabsf(rr[2] - rr[0] - fw) < 0.5f) ||
                      h->kind == HUD_EDGE || h->kind == HUD_DIALOG;
    h->x0 = spans ? sx : rr[0];
    h->w = spans ? sw : rr[2] - rr[0];
    h->y0 = rr[1];
    h->h = rr[3] - rr[1];
    h->ox = h->plain.ox;
    h->oy = h->plain.oy;
    return h->w > 0.0f && h->h > 0.0f;
#endif
    float in[4];
    hud_insets(&h->plain, in);
    if (h->kind == HUD_EDGE) { /* insets only across the axes it meets an edge on */
        const uint32_t desc = edge_root_desc(root);
        if (!edge_align(rd32(desc + DESC_HOR_ALIGN)))
            in[0] = in[2] = 0;
        if (!edge_align(rd32(desc + DESC_VER_ALIGN)))
            in[1] = in[3] = 0;
        /* A screen drawn for 1024 units across (the options, with their scroll
         * at the edge) is never narrowed below them: on a screen not much
         * wider than 4:3 (a foldable's closed one) the safe area's sides are
         * cut down to what is left over, in their proportion. */
        const float room = h->plain.vw - 1024.0f, across = in[0] + in[2];
        if (across > 0.0f && across > room) {
            const float k = room > 0.0f ? room / across : 0.0f;
            in[0] *= k;
            in[2] *= k;
        }
    }
    if (h->k == 1.0f && in[0] == 0 && in[1] == 0 && in[2] == 0 && in[3] == 0)
        return 0;
    h->w = h->plain.vw - in[0] - in[2];
    h->h = h->plain.vh - in[1] - in[3];
    h->ox = h->plain.ox - in[0];
    h->oy = h->plain.oy - in[1];
    return 1;
}

/* The root's own mapping, for drawing and the mouse: the plain scale, moved
 * in by the safe area. */
static Mapping hud_mapping(const Hud *h) {
    Mapping m = h->plain;
    m.ox = h->ox;
    m.oy = h->oy;
    return m;
}

/* A window's rectangle in its parent's virtual pixels, as laid out; zero when
 * its layout is not the one this was written for. */
static int window_rect(uint32_t win, float r[4]) {
    /* Anywhere in the arena: the heap goes on above 0x10000000 (game.toml
     * heap_extra_base). */
    const uint32_t placed = rd32(win + WINDOW_PLACED);
    if (placed < 0x10000u || placed > (uint32_t)GUEST_SIZE - 0x100u)
        return 0;
    for (int i = 0; i < 4; ++i)
        r[i] = rdf32(placed + PLACED_RECT + 4u * (uint32_t)i);
    /* The placement's rectangle starts where the window stands. */
    if (!(fabsf(r[0] - rdf32(win + WINDOW_X)) < 0.5f && fabsf(r[1] - rdf32(win + WINDOW_Y)) < 0.5f))
        return 0;
    return r[2] > r[0] && r[3] > r[1] && r[2] - r[0] < 4096.0f && r[3] - r[1] < 4096.0f;
}

/* A child of the root's rectangle in the interface's frame: from its x0,y0
 * (the same as window_rect where the root is placed inside the safe area). */
static int child_rect(const Hud *h, uint32_t win, float r[4]) {
    if (!window_rect(win, r))
        return 0;
    r[0] -= h->x0;
    r[2] -= h->x0;
    r[1] -= h->y0;
    r[3] -= h->y0;
    return 1;
}

/* The screen's own centre across, in the interface's frame. */
static float screen_centre_x(const Hud *h) {
#ifdef HOMM5_HUD_ROOT_RECT
    float x, w;
    HOMM5_SCREEN_SPAN(&x, &w);
    return x + w * 0.5f - h->x0;
#else
    return h->plain.vw * 0.5f + h->ox;
#endif
}

/* A panel: the point of the root it grows around (a), the point of the root
 * where that one is shown (t: a itself, or the screen's own centre for a
 * panel in the middle of an edge, which keeps it centred whatever the safe
 * area does), and its scale; in the interface's frame (Hud x0, y0 at 0,0). */
typedef struct {
    float ax, ay, tx, ty, k;
} Panel;

static float clampf_(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/* The corners of buttons everywhere (the town's, its inner screens', the
 * hero's) are as large as the map's. */
static float corner_scale(void) {
    return hud_scale(HUD_MAP);
}

static float bar_scale(void) {
    static float k = -1;
    static int seen = -1;
    if (scales_stale(&seen))
        k = -1;
    if (k < 0) {
        const char *v = getenv("RECOMP_HOMM5_BAR_UI_SCALE");
        k = v && *v ? (float)atof(v) : 1.0f; /* as on the screens over it (the tavern's) */
        if (!(k >= 0.5f && k <= 3.0f))
            k = 1.0f;
    }
    return k;
}

static int panel_of(const Hud *h, uint32_t win, Panel *p, int depth);

/* The windows of a root the game places itself (a dragged creature's icon,
 * the cursor's damage forecast): laid out at 0,0 and moved by code, they
 * are never panels, wherever they go. Read as the root is placed. A button
 * hidden then is at 0,0 too (a battle's start and next creature, shown while
 * placing the troops) but stays where its file puts it: not one of these. */
static struct {
    uint32_t root, win;
} g_free[64];

/* All of a root's windows as it is placed: the windows the game makes later
 * (a battle's stack counters, which follow the creatures) are not panels.
 * The last few roots placed are kept. */
enum { ROOT_CHILDREN_MAX = 256, KNOWN_ROOTS = 32 };
static struct {
    uint32_t root;
    int n;
    uint32_t win[ROOT_CHILDREN_MAX];
} g_known[KNOWN_ROOTS];
static int g_known_next;

static void note_free_windows(uint32_t root) {
    int found = 0;
    for (int i = 0; i < 64; ++i)
        if (g_free[i].root == root)
            g_free[i].root = g_free[i].win = 0;
    int slot = -1;
    for (int i = 0; i < KNOWN_ROOTS && slot < 0; ++i)
        if (g_known[i].root == root)
            slot = i;
    if (slot < 0) {
        slot = g_known_next;
        g_known_next = (g_known_next + 1) % KNOWN_ROOTS;
    }
    g_known[slot].root = root;
    g_known[slot].n = 0;
    const uint32_t list = root - ROOT_MAIN + WINDOW_CHILDREN;
    const uint32_t b = rd32(list), e = rd32(list + 4);
    for (uint32_t at = b; at < e && at < b + 4 * ROOT_CHILDREN_MAX; at += 4) {
        const uint32_t win = rd32(at);
        if (win)
            g_known[slot].win[g_known[slot].n++] = win;
        /* By its position: a hidden window may have no rectangle laid out. */
        if (!win || rdf32(win + WINDOW_X) != 0.0f || rdf32(win + WINDOW_Y) != 0.0f ||
            rd32(win) == MS_BUTTON_VTABLE)
            continue;
        int i = 0;
        while (i < 64 && g_free[i].root)
            ++i;
        if (i == 64) { /* full of roots gone by: the oldest place goes */
            static int next;
            i = next;
            next = (next + 1) % 64;
        }
        g_free[i].root = root;
        g_free[i].win = win;
        ++found;
    }
    if (trace_hud())
        fprintf(stderr, "[hud] root %08x: %d windows placed by the game\n", root, found);
}

/* 1 when `win` was among the root's windows as it was placed, 0 when not,
 * -1 when the root's windows were never read. */
static int known_window(uint32_t root, uint32_t win) {
    for (int i = 0; i < KNOWN_ROOTS; ++i)
        if (g_known[i].root == root) {
            for (int j = 0; j < g_known[i].n; ++j)
                if (g_known[i].win[j] == win)
                    return 1;
            return 0;
        }
    return -1;
}

static int free_window(uint32_t root, uint32_t win) {
    for (int i = 0; i < 64; ++i)
        if (g_free[i].root == root && g_free[i].win == win)
            return 1;
    return 0;
}

/* A small window lying on a larger one goes with it: on a panel (the resource
 * bar's and the hero list's fold buttons) as that panel, on another window
 * (the minimap's) as it is. 1 a panel, -1 another window, 0 none. */
static int panel_under(const Hud *h, uint32_t win, const float r[4], Panel *p) {
    const uint32_t list = h->root - ROOT_MAIN + WINDOW_CHILDREN;
    const uint32_t b = rd32(list), e = rd32(list + 4);
    const float cx = (r[0] + r[2]) * 0.5f, cy = (r[1] + r[3]) * 0.5f;
    for (uint32_t at = b; at < e && at < b + 4 * ROOT_CHILDREN_MAX; at += 4) {
        const uint32_t other = rd32(at);
        float o[4];
        if (!other || other == win || !child_rect(h, other, o) || free_window(h->root, other))
            continue;
        if (o[2] - o[0] <= 64.0f && o[3] - o[1] <= 64.0f)
            continue;
        if (cx >= o[0] - 8 && cx <= o[2] + 8 && cy >= o[1] - 8 && cy <= o[3] + 8)
            return panel_of(h, other, p, 1) ? 1 : -1;
    }
    return 0;
}

/* The minimap's rectangle among the root's windows, if it is there. */
static void minimap_rect(const Hud *h, float m[4]) {
    const uint32_t list = h->root - ROOT_MAIN + WINDOW_CHILDREN;
    const uint32_t b = rd32(list), e = rd32(list + 4);
    for (uint32_t at = b; at < e && at < b + 4 * ROOT_CHILDREN_MAX; at += 4) {
        float o[4];
        const uint32_t other = rd32(at);
        if (other && child_rect(h, other, o) && fabsf(o[2] - o[0] - MINIMAP_W) < 6.0f &&
            fabsf(o[3] - o[1] - MINIMAP_H) < 6.0f && o[0] < h->w * 0.5f && o[1] > h->h * 0.5f) {
            memcpy(m, o, sizeof o);
            return;
        }
    }
}

/* Whether window `win`, a child of interface `h`'s root, is a panel. */
static int panel_of(const Hud *h, uint32_t win, Panel *p, int depth) {
    float r[4];
    if (!child_rect(h, win, r) || free_window(h->root, win))
        return 0;
    /* The dial of the town's inner screens (build, hire, the tavern...), made
     * an L frame by tools/phone_ui_mod.py, known by its size: in the screen's
     * corner at the town's scale, on whatever screen holds it. */
    if (fabsf(r[2] - r[0] - DIAL_FRAME_W) < 1.0f && fabsf(r[3] - r[1] - DIAL_FRAME_H) < 1.0f) {
        p->k = corner_scale();
        if (p->k == 1.0f)
            return 0;
        p->ax = r[2];
        p->ay = r[3];
        p->tx = h->w - 6.0f * p->k;
        p->ty = h->h - 6.0f * p->k;
        return 1;
    }
    if ((h->kind != HUD_MAP && h->kind != HUD_BATTLE && h->kind != HUD_TOWN) || h->k == 1.0f ||
        known_window(h->root, win) == 0)
        return 0;
    const float W = h->w, H = h->h, cx = (r[0] + r[2]) * 0.5f, cy = (r[1] + r[3]) * 0.5f;
    if (h->kind == HUD_MAP && cx < W * 0.5f && cy > H * 0.5f &&
        fabsf(r[2] - r[0] - MINIMAP_W) < 6.0f && fabsf(r[3] - r[1] - MINIMAP_H) < 6.0f)
        return 0; /* the minimap */
    if (fabsf(cx - W * 0.5f) < W * 0.05f && fabsf(cy - H * 0.5f) < H * 0.05f)
        return 0; /* free-standing, as the game's dialogs are */
    if (!depth && r[2] - r[0] <= 64.0f && r[3] - r[1] <= 64.0f) {
        const int under = panel_under(h, win, r, p);
        if (under)
            return under > 0;
    }
    /* Grows around the middle of an edge only when it is centred on it, else
     * around the nearer end. */
    p->ax = fabsf(cx - W * 0.5f) < W * 0.05f ? W * 0.5f : r[0] < W - r[2] ? 0.0f : W;
    p->ay = fabsf(cy - H * 0.5f) < H * 0.05f ? H * 0.5f : r[1] < H - r[3] ? 0.0f : H;
    p->k = h->k;
    p->tx = p->ax;
    p->ty = p->ay;
    /* A battle keeps its left side as it is: the turn order, the placement
     * bar (at the phone's scale they cover the field's near rows). */
    if (h->kind == HUD_BATTLE && p->ax == 0.0f)
        return 0;
    if (h->kind == HUD_BATTLE && p->ax == W) /* its buttons: as large as the map's */
        p->k = corner_scale();
    if (trace_hud() && p->ax == 0.0f && p->ay == H) {
        static uint32_t told[16];
        int known = 0;
        for (int i = 0; i < 16; ++i)
            known |= told[i] == win;
        if (!known) {
            for (int i = 0; i < 16; ++i)
                if (!told[i]) {
                    told[i] = win;
                    break;
                }
            fprintf(stderr, "[hud] kind %d bottom-left window %08x: %.1f,%.1f..%.1f,%.1f of %.0fx%.0f\n",
                    h->kind, win, r[0], r[1], r[2], r[3], W, H);
        }
    }
    if (h->kind == HUD_MAP && p->ax == 0.0f && p->ay == H && r[0] > 150.0f && r[0] < 400.0f &&
        r[3] - r[1] > 100.0f) { /* as wide as its heroes and towns: one column for one */
        /* The hero and town list beside the minimap: from the minimap's
         * bottom to its top, right beside it. */
        float m[4] = {MINIMAP_X, H - MINIMAP_H, MINIMAP_X + MINIMAP_W, H};
        minimap_rect(h, m);
        m[1] += MINIMAP_ART_MARGIN; /* its frame's picture stands inside its window */
        m[2] -= MINIMAP_ART_MARGIN;
        m[3] -= MINIMAP_ART_MARGIN;
        p->k = clampf_((m[3] - m[1]) / (r[3] - r[1]), 0.5f, 3.0f);
        p->ax = r[0];
        p->ay = r[3];
        p->tx = m[2] + 6.0f;
        p->ty = m[3];
        return 1;
    }
    if (h->kind == HUD_TOWN && p->ay == H) {
        const float kc = corner_scale();
        if (fabsf(r[2] - r[0] - TOWN_FRAME_W) < 1.0f && fabsf(r[3] - r[1] - TOWN_FRAME_H) < 1.0f) {
            p->k = kc; /* a dial's frame: as large as the map's corners */
            return p->k != 1.0f;
        }
        /* The rest of the bottom (ListBase: the heroes and their armies, laid
         * out from the left): in the middle of the room between the two
         * frames, no wider than it. */
        const float L = kc * (FRAME_MARGIN + TOWN_FRAME_W + FRAME_MARGIN), R = W - L;
        if (R > L)
            p->k = clampf_(fminf(p->k, (R - L) / (r[2] - r[0])), 0.5f, 3.0f);
        p->ax = (r[0] + r[2]) * 0.5f;
        p->tx = (L + R) * 0.5f;
        return 1;
    }
    if (p->ay == 0.0f && p->ax == W * 0.5f) {
        /* The top's middle (the resource bar, a town's name): its own scale
         * (by default none: the same bar on the screens opened over the map
         * and the town keeps the plain size), centred on the screen whatever
         * the safe area does, no wider than the room across (a foldable's
         * closed screen is nearly 4:3). */
        p->k = bar_scale();
        const float across = r[2] - r[0];
        if (across > 0.0f && p->k * across > W - 12.0f)
            p->k = fmaxf(1.0f, (W - 12.0f) / across);
        p->tx = screen_centre_x(h);
    }
    return p->k != 1.0f;
}

/* A panel's mapping: k times the root's scale, its anchor where the root's
 * mapping shows its target. */
static Mapping panel_mapping(const Hud *h, const Panel *p) {
    Mapping m = h->plain;
    m.rw *= p->k;
    m.rh *= p->k;
    m.ox = h->x0 + p->ax - (p->tx - (h->ox - h->x0)) / p->k;
    m.oy = h->y0 + p->ay - (p->ty - (h->oy - h->y0)) / p->k;
    return m;
}

/* The interfaces drawn in this frame and the last, for the mouse between two
 * frames: the hit test asks about windows, and the window's root is found
 * among these. */
static Hud g_huds[2][16];
static int g_hud_count[2], g_hud_frame;

static void note_hud(const Hud *h) {
    const int f = g_hud_frame;
    for (int i = 0; i < g_hud_count[f]; ++i)
        if (g_huds[f][i].root == h->root)
            return;
    if (g_hud_count[f] < 16)
        g_huds[f][g_hud_count[f]++] = *h;
}

static const Hud *hud_of_window(uint32_t win) {
    for (int age = 0; age < 2; ++age) {
        const int f = age ? !g_hud_frame : g_hud_frame;
        for (int i = 0; i < g_hud_count[f]; ++i)
            if (g_huds[f][i].root - ROOT_MAIN == win)
                return &g_huds[f][i];
    }
    return NULL;
}

#ifdef HOMM5_FN_SCREEN_PLACE
/* What each root was last placed for, so a root placed before its screen
 * reached the stack (or before the safe area changed) is placed again. */
static struct {
    uint32_t root;
    float w, h;
    int epoch; /* homm5_phone_layout_epoch's, as it was placed */
} g_placed[32];

static int placed_for(const Hud *h) {
    for (int i = 0; i < 32; ++i)
        if (g_placed[i].root == h->root)
            return g_placed[i].w == h->w && g_placed[i].h == h->h &&
                   g_placed[i].epoch == homm5_phone_layout_epoch();
    return 0;
}

static int placed_inset(uint32_t root) {
    for (int i = 0; i < 32; ++i)
        if (g_placed[i].root == root)
            return g_placed[i].w != 0;
    return 0;
}

/* Placed with the phone's rows on and now off, or the other way round (a
 * foldable folded or opened): a root never placed here was placed before
 * any change. */
static int placed_before_flip(uint32_t root) {
    const int now = homm5_phone_layout_epoch();
    for (int i = 0; i < 32; ++i)
        if (g_placed[i].root == root)
            return g_placed[i].epoch != now;
    return now != 0;
}

static void remember_placed(uint32_t root, const Hud *h) {
    static int next;
    int slot = -1;
    for (int i = 0; i < 32 && slot < 0; ++i)
        if (g_placed[i].root == root)
            slot = i;
    if (slot < 0) {
        slot = next;
        next = (next + 1) % 32;
    }
    g_placed[slot].root = root;
    g_placed[slot].w = h ? h->w : 0;
    g_placed[slot].h = h ? h->h : 0;
    g_placed[slot].epoch = homm5_phone_layout_epoch();
}

/* The root placement (00e74f30) for a root: the base game's native/
 * widescreen.c has already centred an untagged one. */
static void hud_place(X86 *c, uint32_t root) {
    Hud h;
    if (!hud_for(root, &h)) {
        remember_placed(root, NULL);
        HOMM5_FN_SCREEN_PLACE(c);
        return;
    }
    Mapping m = {h.w, h.h, h.w * h.plain.rw / h.plain.vw, h.h * h.plain.rh / h.plain.vh, h.ox,
                 h.oy};
    Scope sc;
    scope_enter(c, &sc, &m);
    HOMM5_FN_SCREEN_PLACE(c);
    note_free_windows(root);
    scope_leave(c, &sc);
    remember_placed(root, &h);
    if (trace_hud())
        fprintf(stderr, "[hud] %s root %08x placed: %.0fx%.0f from %.1f,%.1f, panels at %.2f\n",
                h.kind == HUD_MAP      ? "map"
                : h.kind == HUD_BATTLE ? "battle"
                : h.kind == HUD_TOWN   ? "town"
                : h.kind == HUD_DIALOG ? "town dialog"
                                       : "edge",
                root, h.w,
                h.h, h.ox, h.oy, h.k);
}

/* The base game's placement hook (native/widescreen.c) hands its roots here. */
void homm5_hud_place(X86 *c, uint32_t root) {
    hud_place(c, root);
}

#ifdef HOMM5_HUD_PLACE_HOOK
/* A root WindowScreen's placement (thiscall: ECX the window; no stack
 * arguments) in a game that aligns its roots itself (Tribes of the East):
 * the safe area and the panels' scale only, the alignment left to it. */
void homm5_hud_screen_place(X86 *c) {
    const uint32_t root = c->r[R_ECX];
    const uint32_t desc = rd32(root + SCREEN_DESC);
    if (!wide_ui() || !desc) {
        HOMM5_FN_SCREEN_PLACE(c);
        return;
    }
    if (edge_align(rd32(desc + DESC_HOR_ALIGN)) || edge_align(rd32(desc + DESC_VER_ALIGN)))
        homm5_hud_note_edge_root(root, desc);
    hud_place(c, root);
}
#endif
#endif /* HOMM5_FN_SCREEN_PLACE */

#ifdef HOMM5_FN_ROOT_DESCRIBE
void HOMM5_FN_ROOT_DESCRIBE(X86 *c);

/* A root WindowScreen takes its description and lays its windows out
 * (Hammers of Fate's 00e7f630, thiscall: ECX the window; the description and
 * a flag on the stack): its windows as it is laid out, for the panels
 * (note_free_windows), in a game with no placement of its own to hook. Every
 * root counts as one against an edge (the game aligns none itself): the
 * shared corner dial of a screen (the tavern's, the hire screen's) is then
 * known on any of them, as in the base game. */
void homm5_hud_root_described(X86 *c) {
    const uint32_t root = c->r[R_ECX];
    HOMM5_FN_ROOT_DESCRIBE(c);
    if (wide_ui() && hud_scale(HUD_MAP) != 1.0f && !rd32(root - ROOT_MAIN + WINDOW_PARENT)) {
        note_free_windows(root);
        homm5_hud_note_edge_root(root, edge_key(root));
    }
}

/* For native/ui_bounds.c: whether `win` is one of its root's windows the
 * game places itself (laid out at 0,0 as the root was described: a dragged
 * creature's icon, which follows the finger). The safe area is not its. */
int homm5_hud_free_window(uint32_t win) {
    const uint32_t parent = rd32(win + WINDOW_PARENT);
    return parent && !rd32(parent + WINDOW_PARENT) && free_window(parent + ROOT_MAIN, win);
}

#endif

/* The interface being drawn, if any. */
static int g_draw_depth; /* CWindow::Draw calls under way */
static int g_root_depth; /* the depth its root was drawn at */
static Hud g_drawing;
static int g_drawing_on;

/* Where each draw under way began (its thread and guest stack): one a guest
 * exception unwound (scopes_unwound) never counts itself out. */
enum { DRAWS_MAX = 64 };
static X86 *g_draw_cpu[DRAWS_MAX];
static uint32_t g_draw_esp[DRAWS_MAX];

static void draw_plain(X86 *c) {
    if (g_draw_depth >= 0 && g_draw_depth < DRAWS_MAX) {
        g_draw_cpu[g_draw_depth] = c;
        g_draw_esp[g_draw_depth] = c->r[R_ESP];
    }
    ++g_draw_depth;
    HOMM5_FN_WINDOW_DRAW(c);
    --g_draw_depth;
}

/* Draws of this thread that began at or below the ESP running now have ended,
 * and with them the interface they drew. */
static void draws_unwound(X86 *c) {
    while (g_draw_depth > 0 && g_draw_depth <= DRAWS_MAX && g_draw_cpu[g_draw_depth - 1] == c &&
           g_draw_esp[g_draw_depth - 1] <= c->r[R_ESP])
        --g_draw_depth;
    if (g_drawing_on && g_draw_depth <= g_root_depth)
        g_drawing_on = 0;
}

static void draw_mapped(X86 *c, const Mapping *m) {
    Scope sc;
    scope_enter(c, &sc, m);
    draw_plain(c);
    scope_leave(c, &sc);
}

/* Texts. A window's text is laid out once (00e453f0: its glyphs' pixel size
 * from the mapping in force, the block measured in real pixels) and then only
 * placed, so a text laid out with the plain mapping stays small in a panel and
 * its alignment slips. Each layout's scale is noted, and a panel's texts are
 * laid out again at the panel's scale as they are drawn. */
static float g_text_k = 1.0f;
static struct {
    uint32_t text;
    float k;
} g_texts[1024];

static float *text_slot(uint32_t text, int add) {
    uint32_t i = (text >> 4) * 2654435761u >> 22;
    for (int n = 0; n < 1024; ++n, i = (i + 1) & 1023) {
        if (g_texts[i].text == text)
            return &g_texts[i].k;
        if (!g_texts[i].text) {
            if (!add)
                return NULL;
            g_texts[i].text = text;
            return &g_texts[i].k;
        }
    }
    return NULL; /* full: laid out again each time, harmless */
}

/* 00e453f0 (thiscall: ECX the text; no arguments). */
void homm5_text_layout(X86 *c) {
    float *k = text_slot(c->r[R_ECX], g_text_k != 1.0f);
    if (k)
        *k = g_text_k;
    HOMM5_FN_TEXT_LAYOUT(c);
}

static void text_at_scale(X86 *c, uint32_t part) {
    const uint32_t text = rd32(part + (uint32_t)PART_TEXT);
    if (text < 0x10000u || text > (uint32_t)GUEST_SIZE - 0x100u || !rd32(text + 0x64))
        return;
    const float *k = text_slot(text, 0);
    if ((k ? *k : 1.0f) != g_text_k)
        homm5_guest_thiscall(c, HOMM5_ADDR_TEXT_LAYOUT, text);
}

/* Whether a battle is in its placement phase: its placement bar (600 x 165,
 * the only window of that size) was drawn in the last moments. A window is
 * drawn only while it is shown. native/camera.c moves the camera for it. */
static volatile int64_t g_placing_ms;

static int64_t now_ms_(void) {
    return (int64_t)(os_monotonic_ns() / 1000000);
}

static void note_placement_bar(uint32_t win) {
    float r[4];
    if (window_rect(win, r) && fabsf(r[2] - r[0] - 600.0f) < 2.0f &&
        fabsf(r[3] - r[1] - 165.0f) < 2.0f)
        g_placing_ms = now_ms_();
}

int homm5_battle_placing(void) {
    return g_placing_ms && now_ms_() - g_placing_ms < 400;
}

/* A window the game positions itself, from the screen's picture (a dragged
 * creature's icon, a battle's stack counters over the creatures): drawn with
 * the plain mapping, or the interface's safe area and scale would move it off
 * what it follows. Those laid out at 0,0 as the root was placed, and the small
 * ones made after it; a dialog made later is larger and stays in the frame. */
static int follows_picture(const Hud *h, uint32_t win) {
    float r[4];
    if (h->kind != HUD_MAP && h->kind != HUD_TOWN && h->kind != HUD_BATTLE)
        return 0; /* the arrows of a build screen's tree are placed in its frame */
    if (!window_rect(win, r) || r[2] - r[0] > 128.0f || r[3] - r[1] > 128.0f)
        return 0; /* a panel or a whole screen's layer, even one standing at 0,0 */
    if (free_window(h->root, win))
        return 1;
    /* Made after the root was placed: in a battle, the stack counters; on
     * the map, the resource bar's numbers, which go with the bar. */
    return h->kind == HUD_BATTLE && known_window(h->root, win) == 0;
}

#ifdef HOMM5_ADDR_SET_PARENT_RECT
/* The safe area's sides as the screens were laid out: when the phone turns
 * and the camera housing changes sides, the edge windows' anchors
 * (native/ui_bounds.c) are out of date. The game has no placement to run
 * again, so the screens in the stack get their 1024x768 parent rectangle
 * again (SetParentRect lays out each window under them). */
static void relayout_if_turned(X86 *c) {
    static float laid_l = -1.0f, laid_r = -1.0f;
    static int laid_epoch;
    static float laid_w = -1.0f;
    float l, r;
    homm5_safe_sides(&l, &r);
    /* The phone's rows turned on or off (native/phone_layout.c) as well, or
     * the screen's width changed: a device reset to a new screen (a foldable
     * opened; sdl/main.cpp follow_screen) lays nothing out again here. */
    const int epoch = homm5_phone_layout_epoch();
    float span_w = 0.0f;
#ifdef HOMM5_SCREEN_SPAN
    {
        float span_x;
        HOMM5_SCREEN_SPAN(&span_x, &span_w);
    }
#endif
    if (laid_l < 0.0f || (l == laid_l && r == laid_r && epoch == laid_epoch && span_w == laid_w)) {
        laid_l = l;
        laid_r = r;
        laid_epoch = epoch;
        laid_w = span_w;
        return;
    }
    laid_l = l;
    laid_r = r;
    laid_epoch = epoch;
    laid_w = span_w;
    static const float screen[4] = {0.0f, 0.0f, 1024.0f, 768.0f};
    for (int i = 0; i < 7; ++i) { /* the loading screen's picture fills the screen */
        if (!g_roots[i] || i == 3)
            continue;
        if (trace_hud())
            fprintf(stderr, "[hud] safe area %.3f,%.3f: laying root %08x out again\n", l, r,
                    g_roots[i]);
        homm5_guest_thiscall_rect(c, HOMM5_ADDR_SET_PARENT_RECT, g_roots[i] - ROOT_MAIN, screen);
    }
}
#endif

#ifdef HOMM5_SCREEN_MODE_CHANGES
int HOMM5_SCREEN_MODE_CHANGES(void);
static uint32_t loading_root(void);
/* After a change of screen mode (native/hof_screen.c counts them) the screens
 * already made still span the old screen: the game has no
 * reposition_interfaces, so each root is laid out again the first time it is
 * drawn after the change, as it was made (SetParentRect with the 1024x768
 * rectangle). A root that is drawn is alive; one made after the change gets
 * the same layout again, which changes nothing. */
static void relayout_after_mode_change(X86 *c, uint32_t root) {
    static int laid_for;
    static uint32_t done[128];
    static int n_done;
    const int changes = HOMM5_SCREEN_MODE_CHANGES();
    if (changes != laid_for) {
        laid_for = changes;
        n_done = 0;
    }
    if (!changes || rd32(root - ROOT_MAIN + 0x28u) || root == loading_root())
        return; /* no change yet, not a root, or the loading screen (fills the screen) */
    for (int i = 0; i < n_done; ++i)
        if (done[i] == root)
            return;
    if (n_done == (int)(sizeof done / sizeof done[0]))
        return;
    done[n_done++] = root;
    if (trace_hud())
        fprintf(stderr, "[hud] screen mode changed: laying root %08x out again\n", root);
    static const float screen[4] = {0.0f, 0.0f, 1024.0f, 768.0f};
    homm5_guest_thiscall_rect(c, HOMM5_ADDR_SET_PARENT_RECT, root - ROOT_MAIN, screen);
}
#endif

/* 00e3dd70: CWindow::Draw (thiscall: ECX the window's draw part; one stack
 * argument, the canvas). */
static void window_draw(X86 *c) {
    const uint32_t part = c->r[R_ECX];
    if (!wide_ui()) {
        HOMM5_FN_WINDOW_DRAW(c);
        return;
    }
    draws_unwound(c);
    scopes_unwound(c);
    if (g_draw_depth == 0) { /* a frame's interface begins */
        find_roots();
#ifdef HOMM5_ADDR_SET_PARENT_RECT
        relayout_if_turned(c);
#endif
        g_hud_frame = !g_hud_frame;
        g_hud_count[g_hud_frame] = 0;
    }
#ifdef HOMM5_SCREEN_MODE_CHANGES
    relayout_after_mode_change(c, part - ROOT_DRAW);
#endif
    const uint32_t root = part - ROOT_DRAW;
    if (!g_drawing_on && root &&
        (root == g_roots[0] || root == g_roots[1] || root == g_roots[2] ||
         root == g_roots[3] || root == g_roots[4] || root == g_roots[5] || root == g_roots[6] ||
         edge_root_desc(root))) {
        Hud h;
        if (!hud_for(root, &h)) {
#ifdef HOMM5_FN_SCREEN_PLACE
            /* Placed inside the safe area before its screen was known (the
             * loading screen is placed before it is pushed): placed again
             * over the whole screen. */
            if (placed_inset(root) || placed_before_flip(root)) {
                if (trace_hud())
                    fprintf(stderr, "[hud] placing root %08x again, plain\n", root);
                homm5_guest_thiscall(c, HOMM5_ADDR_SCREEN_PLACE, root);
                remember_placed(root, NULL);
            }
#endif
            draw_plain(c);
            return;
        }
#ifdef HOMM5_FN_SCREEN_PLACE
        if (!placed_for(&h)) {
            if (trace_hud())
                fprintf(stderr, "[hud] placing root %08x again\n", root);
            homm5_guest_thiscall(c, HOMM5_ADDR_SCREEN_PLACE, root);
            remember_placed(root, &h); /* once, whatever the call did */
        }
#endif
        note_hud(&h);
        g_drawing = h;
        g_drawing_on = 1;
        g_root_depth = g_draw_depth;
        const Mapping m = hud_mapping(&h);
        draw_mapped(c, &m);
        g_drawing_on = 0;
        return;
    }
    Panel pn;
    if (g_drawing_on && g_draw_depth == g_root_depth + 1 && g_drawing.kind == HUD_BATTLE)
        note_placement_bar(part - WINDOW_PART);
    if (g_drawing_on && g_draw_depth == g_root_depth + 1 && follows_picture(&g_drawing, part - WINDOW_PART)) {
        draw_mapped(c, &g_drawing.plain);
        return;
    }
    if (g_drawing_on && g_draw_depth == g_root_depth + 1 &&
        panel_of(&g_drawing, part - WINDOW_PART, &pn, 0)) {
        const Mapping m = panel_mapping(&g_drawing, &pn);
        const float back = g_text_k;
        Scope sc;
        scope_enter(c, &sc, &m);
        g_text_k = pn.k;
        text_at_scale(c, part);
        draw_plain(c);
        g_text_k = back;
        scope_leave(c, &sc);
        return;
    }
    if (g_drawing_on)
        text_at_scale(c, part);
    draw_plain(c);
}

/* 00e45ad0: a CWindowSimple's draw (thiscall: ECX its part at +0x110; the
 * canvas on the stack): it clips the canvas to its rectangle, converted with
 * the mapping in force, before CWindow::Draw runs. A panel's mapping goes in
 * here already, or the panel is clipped to its plain size. */
void homm5_simple_draw(X86 *c) {
    Panel pn;
    if (wide_ui() && g_drawing_on && g_draw_depth == g_root_depth + 1 &&
        follows_picture(&g_drawing, c->r[R_ECX] - SIMPLE_DRAW)) {
        Scope sc;
        scope_enter(c, &sc, &g_drawing.plain);
        HOMM5_FN_SIMPLE_DRAW(c);
        scope_leave(c, &sc);
        return;
    }
    if (wide_ui() && g_drawing_on && g_draw_depth == g_root_depth + 1 &&
        panel_of(&g_drawing, c->r[R_ECX] - SIMPLE_DRAW, &pn, 0)) {
        const Mapping m = panel_mapping(&g_drawing, &pn);
        const float back = g_text_k;
        Scope sc;
        scope_enter(c, &sc, &m);
        g_text_k = pn.k;
        HOMM5_FN_SIMPLE_DRAW(c);
        g_text_k = back;
        scope_leave(c, &sc);
        return;
    }
    HOMM5_FN_SIMPLE_DRAW(c);
}

/* 0083be20: NUI::CPWLScreenInterface's constructor (thiscall: ECX the
 * object), the loading screen. It is drawn while a game loads without being
 * in the screen stack, so it is known from here. */
static uint32_t g_loading;

void homm5_loading_screen(X86 *c) {
    const uint32_t self = c->r[R_ECX];
    HOMM5_FN_LOADING_SCREEN(c);
    g_loading = self;
}

static uint32_t loading_root(void) {
    return g_loading && rd32(g_loading) == LOADING_VTABLE ? rd32(g_loading + SCREEN_ROOT) : 0;
}

/* CWindow::Draw with a phone's rows of buttons (native/phone_layout.c): a jog
 * dial's disc is not drawn (RET 4 at once), a dial made a frame is drawn
 * without its background. Nor is the root of a screen the host draws natively
 * (native/native_screens.c). */
void homm5_window_draw(X86 *c) {
    const uint32_t win = c->r[R_ECX] - WINDOW_PART;
    const int how = homm5_native_screens_draw(c, c->r[R_ECX]) ? LAYOUT_HIDE
                                                              : homm5_phone_layout_draw(win);
    if (how == LAYOUT_HIDE) {
        const uint32_t esp = c->r[R_ESP];
        c->eip = rd32(esp);
        c->r[R_ESP] = esp + 8u;
        recomp_return(c);
        return;
    }
    if (how == LAYOUT_BARE) {
        const uint32_t bg = homm5_phone_layout_strip(win);
        window_draw(c);
        homm5_phone_layout_restore(win, bg);
        return;
    }
    window_draw(c);
}

/* 00e3bd00: whether a point is the window's (thiscall: ECX the window's
 * draw part, the point on the stack; RET 4). Every window class's own test
 * comes here through an adjustor thunk. */
static void window_contains(X86 *c) {
    if (!wide_ui()) {
        HOMM5_FN_WINDOW_CONTAINS(c);
        return;
    }
    /* The interface the window belongs to, and the child of its root it is
     * in (none when the window is the root itself). */
    const uint32_t win = c->r[R_ECX] - WINDOW_PART;
    const Hud *h = hud_of_window(win);
    uint32_t child = 0;
    for (uint32_t up = win, guard = 16; !h && up && guard--;) {
        const uint32_t parent = rd32(up + WINDOW_PARENT);
        if ((h = hud_of_window(parent)) != NULL)
            child = up;
        up = parent;
    }
    if (!h) {
        HOMM5_FN_WINDOW_CONTAINS(c);
        return;
    }
    const uint32_t pt = rd32(c->r[R_ESP] + 4);
    const float x = rdf32(pt), y = rdf32(pt + 4), gx = rdf32(ORIGIN_X), gy = rdf32(ORIGIN_Y);
    float lx = x, ly = y;
    if (fabsf(gx - h->ox) < 0.01f && fabsf(gy - h->oy) < 0.01f) {
        /* worked out in the root's frame (its own handlers) */
    } else if (fabsf(gx - h->plain.ox) < 0.01f && fabsf(gy - h->plain.oy) < 0.01f) {
        lx += h->ox - h->plain.ox; /* with the plain mapping: carried into the root's frame */
        ly += h->oy - h->plain.oy;
    } else {
        HOMM5_FN_WINDOW_CONTAINS(c); /* in some other frame (a panel's, while it draws): as it is */
        return;
    }
    Panel pn;
    if (child && follows_picture(h, child)) {
        lx -= h->ox - h->plain.ox; /* drawn with the plain mapping: tested with it */
        ly -= h->oy - h->plain.oy;
    } else if (child && panel_of(h, child, &pn, 0)) {
        lx = h->x0 + pn.ax + (lx - h->x0 - pn.tx) / pn.k;
        ly = h->y0 + pn.ay + (ly - h->y0 - pn.ty) / pn.k;
    }
    wrf32(pt, lx);
    wrf32(pt + 4, ly);
    HOMM5_FN_WINDOW_CONTAINS(c);
    wrf32(pt, x);
    wrf32(pt + 4, y);
}

/* What lies under the cursor, for the host's touch gestures: the game asks
 * every frame whether the cursor is over its windows (007c15a0), down to the
 * one it is on, so a text field or a list that answers yes was under it just
 * now. native/touch.c adds RECOMP_TOUCH_TEXT and RECOMP_TOUCH_LIST from here. */
enum {
    VT_EDIT_LINE = HOMM5_VT_EDIT_LINE,
    VT_EDIT_BOX = HOMM5_VT_EDIT_BOX,
    VT_LIST = HOMM5_VT_LIST,
    VT_SCROLLER = HOMM5_VT_SCROLLER,
    VT_TEXT_VIEW = HOMM5_VT_TEXT_VIEW,
};
static volatile int64_t g_text_hit_ms, g_list_hit_ms;
static volatile uint32_t g_list_hit; /* the innermost list or scroller the cursor was on */

int homm5_ui_under_cursor(void) {
    const int64_t now = now_ms_();
    return (g_text_hit_ms && now - g_text_hit_ms < 300 ? 8 : 0) |
           (g_list_hit_ms && now - g_list_hit_ms < 300 ? 16 : 0);
}

/* 00e3bd00: whether a point is the window's. */
void homm5_window_contains(X86 *c) {
    const uint32_t win = c->r[R_ECX] - WINDOW_PART;
    window_contains(c);
    if ((c->r[R_EAX] & 0xff) && homm5_phone_layout_draw(win) == LAYOUT_HIDE)
        c->r[R_EAX] &= ~0xffu; /* a hidden disc is under no finger */
    if (!(c->r[R_EAX] & 0xff))
        return;
    const uint32_t vt = rd32(win);
    if (vt == VT_EDIT_LINE || vt == VT_EDIT_BOX)
        g_text_hit_ms = now_ms_();
    else if (vt == VT_LIST || vt == VT_SCROLLER || vt == VT_TEXT_VIEW)
        g_list_hit_ms = now_ms_();
    if (vt == VT_LIST || vt == VT_SCROLLER) /* asked from the outside in: the last is innermost */
        g_list_hit = win;
}

/* A list or scroller (CWindowScrollableContainerBase and what derives from
 * it: the list control, a combo box's drop-down) scrolls with the slider of
 * its scroll bar: CWindowScrollBar at +0x154, its CWindowSlider at +0x118. The
 * slider keeps min, max (less the page), page and position (+0x10c..+0x118,
 * interface units) and gathers the game's wheel in a float at +0x13c, which
 * its step each frame adds to the position as that many pages (clamped, the
 * content moved to -position). With its description (+0x108) giving two or
 * more segments (+0xec), a list moves a segment a frame while that is non-zero.
 * Same offsets in Hammers of Fate (research 2026-10-21). */
enum {
    LIST_SCROLL_BAR = 0x154,
    BAR_SLIDER = 0x118,
    SLIDER_MIN = 0x10c,
    SLIDER_MAX = 0x110,
    SLIDER_PAGE = 0x114,
    SLIDER_POS = 0x118,
    SLIDER_DESC = 0x108,
    DESC_SEGMENTS = 0xec,
    SLIDER_WHEEL = 0x13c,
};

/* native_seam.h: a trackpad's scroll over the interface, `dy` pixels of a
 * width x height picture the content moves (down positive). The list the
 * cursor was over just now takes it in its slider's wheel, as the pages it
 * makes, so the content follows the fingers; a list in segments gets one
 * segment's worth at a time. */
int recomp_ui_scroll(float x, float y, float dy, int width, int height) {
    (void)x;
    (void)y;
    (void)width;
    const uint32_t win = g_list_hit;
    if (!win || !g_list_hit_ms || now_ms_() - g_list_hit_ms > 300 || height <= 0)
        return 0;
    const uint32_t vt = rd32(win);
    if (vt != VT_LIST && vt != VT_SCROLLER)
        return 0;
    const uint32_t bar = rd32(win + LIST_SCROLL_BAR);
    const uint32_t slider = bar ? rd32(bar + BAR_SLIDER) : 0;
    if (!slider)
        return 0;
    const float lo = rdf32(slider + SLIDER_MIN), hi = rdf32(slider + SLIDER_MAX);
    const float page = rdf32(slider + SLIDER_PAGE);
    if (!(page > 1.0f && page < 100000.0f) || !(hi >= lo) || hi - lo > 1000000.0f)
        return 0;
    if (hi <= lo)
        return 1; /* everything shows: nothing to move, nor to give the wheel */
    /* Picture pixels to interface units, down; the content down is the
     * position less. */
    const Mapping m = plain_mapping();
    const float units = dy * (m.rh > 0.0f ? m.vh / m.rh : 768.0f / (float)height);
    const uint32_t desc = rd32(slider + SLIDER_DESC);
    const uint32_t segments = desc ? rd32(desc + DESC_SEGMENTS) : 0;
    static uint32_t seg_slider;
    static float seg_left;
    if (segments >= 2 && segments < 100000) {
        const float seg = (hi - lo) / (float)(segments - 1);
        if (seg_slider != slider) {
            seg_slider = slider;
            seg_left = 0.0f;
        }
        seg_left -= units;
        if (seg > 0.0f && (seg_left >= seg || seg_left <= -seg)) {
            const float step = seg_left > 0.0f ? 1.0f : -1.0f;
            seg_left -= step * seg;
            wrf32(slider + SLIDER_WHEEL, step);
        }
        return 1;
    }
    wrf32(slider + SLIDER_WHEEL, rdf32(slider + SLIDER_WHEEL) - units / page);
    return 1;
}

#ifndef HOMM5_HUD_ROOT_RECT /* there a root keeps the plain mapping */
/* A WindowScreen's mouse handlers (thiscall: ECX its window, the message on
 * the stack): the message's position is converted in the root's frame. */
static void hud_mouse(X86 *c, void (*orig)(X86 *)) {
    Hud h;
    if (!wide_ui() || !hud_for(c->r[R_ECX] + ROOT_MAIN, &h)) {
        orig(c);
        return;
    }
    const Mapping m = hud_mapping(&h);
    Scope sc;
    scope_enter(c, &sc, &m);
    orig(c);
    scope_leave(c, &sc);
}

void homm5_screen_mouse_0(X86 *c) { hud_mouse(c, HOMM5_FN_SCREEN_MOUSE_0); }
void homm5_screen_mouse_1(X86 *c) { hud_mouse(c, HOMM5_FN_SCREEN_MOUSE_1); }
void homm5_screen_mouse_2(X86 *c) { hud_mouse(c, HOMM5_FN_SCREEN_MOUSE_2); }
void homm5_screen_mouse_3(X86 *c) { hud_mouse(c, HOMM5_FN_SCREEN_MOUSE_3); }
void homm5_screen_mouse_4(X86 *c) { hud_mouse(c, HOMM5_FN_SCREEN_MOUSE_4); }
void homm5_screen_mouse_5(X86 *c) { hud_mouse(c, HOMM5_FN_SCREEN_MOUSE_5); }
void homm5_screen_mouse_6(X86 *c) { hud_mouse(c, HOMM5_FN_SCREEN_MOUSE_6); }

/* The tooltip controller's timer and mouse move (thiscall: ECX the
 * controller): where a tooltip goes is worked out in the root's frame. */
static void hud_tips(X86 *c, void (*orig)(X86 *)) {
    Hud h;
    if (!wide_ui() || !hud_for(c->r[R_ECX] - ROOT_TIPS, &h)) {
        orig(c);
        return;
    }
    const Mapping m = hud_mapping(&h);
    Scope sc;
    scope_enter(c, &sc, &m);
    orig(c);
    scope_leave(c, &sc);
}

void homm5_tooltip_timer(X86 *c) { hud_tips(c, HOMM5_FN_TOOLTIP_TIMER); }
void homm5_tooltip_move(X86 *c) { hud_tips(c, HOMM5_FN_TOOLTIP_MOVE); }
#endif
