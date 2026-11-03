/* native/hof_screen.c - Hammers of Fate's interface on screens wider than 4:3
 * (design notes ultrawide-plan.md, section 6). Hammers of Fate only.
 *
 * The 2.1 exe has a CVirtualScreenController (vtable 01032b04) but no
 * wide-screen code: SetScreenSize (00e500d0) stores the screen size in
 * 010d4e2c/30 and every conversion between the 1024x768 interface and the
 * screen reads it with the virtual size compiled in:
 *   screen x = (x - origin x) * screen width * (1/1024)
 *   screen y = (y - origin y) * screen height / 768
 *   x = (1024 / screen width) * screen x + origin x       (and so on for y)
 * (00e437e0 .. 00e43d50, 00e43ed0 .. 00e43f60; origin at 012875bc/c0). On a
 * 21:9 screen x is scaled 3.36 and y 1.875: the interface is stretched.
 *
 * Those functions take the screen width from the global, so this gives them
 * an effective width of height * 4/3 (the same scale across as down) and moves
 * the origin by the margin that centres the 1024-wide interface:
 *   origin x = game's origin x - (width - effective width) / 2 * 1024 / effective
 * The screen-size getters (00e500b0, 00e437c0) answer the effective size too,
 * which the interface's text and glyph scaling take (see size_mode), and
 * SetOrigin/GetOrigin (00e50100/00e50130) keep the game's own origin apart
 * from the margin.
 *
 * The 3D scene (design notes ultrawide-plan.md 7.2) projects with GetVirtualSize
 * (00e437a0, a constant 1024x768): its renderer 007bfd10 asks for it at
 * 007bfd98 and hands it to the camera (vtable +0x34 -> 009fffa0 -> 009ffda0),
 * so on a wide screen the picture was 4:3 stretched across. That one call now
 * gets the screen's own aspect, S3 = {1024 + 2m, 768} with m the margin
 * above; Hor+ (native/horplus.c) then widens the view from 4:3. The pick and
 * the world -> screen projection invert the scene's matrix with a size they
 * are given and work in interface units (the cursor comes from 00e43ca0):
 * given the 1024x768 or S3 size, they now work in S3 with x shifted by m, so
 * a click lands where the picture shows it and a label sits over its object.
 * The spellbook's own 3D (007f1040, picked from 007f1900) gets S3 the same.
 *
 * Screens that Tribes of the East aligns to the whole screen span it here too:
 * the wide-screen pak stretches their root (EPA_MARGIN) and native/
 * ui_bounds.c widens a stretched root's parent to hof_screen_span. The game
 * has no reposition_interfaces to lay the screens already made out again
 * after a mode change: hof_screen_mode_changes counts the changes and
 * native/phone_hud.c lays each root out again as it is next drawn.
 *
 * RECOMP_HOMM5_WIDE_UI=0 keeps the stretch; RECOMP_TRACE_WIDE=1 logs the
 * mapping. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "x86.h"

enum {
    SCREEN_W = 0x010d4e2c, /* float, what the conversions read */
    SCREEN_H = 0x010d4e30,
    ORIGIN_X = 0x012875bc, /* float */
    ORIGIN_Y = 0x012875c0,
};

static int g_known;          /* SetScreenSize has run */
static float g_real_w, g_real_h;
static float g_margin_x;     /* virtual units the origin is moved by */
static float g_game_origin_x;
static int g_mode_changes;   /* SetScreenSize with another size than before */

static int env_flag(const char *name, int fallback) {
    const char *v = getenv(name);
    return v && *v ? v[0] != '0' : fallback;
}

static int wide_ui(void) {
    static int on = -1;
    if (on < 0)
        on = env_flag("RECOMP_HOMM5_WIDE_UI", 1);
    return on;
}

static void ret_args(X86 *c, uint32_t arg_bytes) {
    const uint32_t esp = c->r[R_ESP];
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 4u + arg_bytes;
    recomp_return(c);
}

/* 00e500d0: SetScreenSize(int width, int height), vtable slot, RET 8. */
void hof_set_screen_size(X86 *c) {
    const uint32_t esp = c->r[R_ESP];
    const float w = (float)(int32_t)rd32(esp + 4u), h = (float)(int32_t)rd32(esp + 8u);
    if (!g_known)
        g_game_origin_x = rdf32(ORIGIN_X);
    else if (w != g_real_w || h != g_real_h)
        ++g_mode_changes;
    g_known = 1;
    g_real_w = w;
    g_real_h = h;
    float eff = w;
    g_margin_x = 0.0f;
    if (wide_ui() && h > 0.0f && w * 3.0f > h * 4.0f) {
        eff = h * (1024.0f / 768.0f);
        g_margin_x = (w - eff) * 0.5f * (1024.0f / eff);
    }
    wrf32(SCREEN_W, eff);
    wrf32(SCREEN_H, h);
    wrf32(ORIGIN_X, g_game_origin_x - g_margin_x);
    static float told_w = -1, told_h = -1;
    if (getenv("RECOMP_TRACE_WIDE") && (w != told_w || h != told_h)) {
        told_w = w;
        told_h = h;
        fprintf(stderr, "[wide] screen %.0fx%.0f: interface %.0f wide, origin x %.1f\n", w, h,
                eff, g_game_origin_x - g_margin_x);
    }
    ret_args(c, 8u);
}

/* The screen's own size in pixels (the client area), as SetScreenSize was
 * given it: native/touch_native.c's points are client pixels. Zero before. */
int hof_real_screen_size(float *w, float *h) {
    *w = g_real_w;
    *h = g_real_h;
    return g_known;
}

/* HOMM5_SCREEN_MODE_CHANGES for native/phone_hud.c. */
int hof_screen_mode_changes(void) {
    return g_mode_changes;
}

/* HOMM5_SCREEN_SPAN for native/ui_bounds.c: the real screen in interface
 * units, from -m to 1024 + m. */
void hof_screen_span(float *x, float *w) {
    *x = -g_margin_x;
    *w = 1024.0f + 2.0f * g_margin_x;
}

static float real_w(void) {
    return g_known ? g_real_w : rdf32(SCREEN_W);
}
static float real_h(void) {
    return g_known ? g_real_h : rdf32(SCREEN_H);
}

/* Which size each getter answers. The interface sizes itself from them: text
 * boxes are measured in pixels (00eef1f0/00ef33e0, float getter 00e437c0) and
 * the fonts scale their glyphs by the int getter (00e500b0) over 1024x768, so
 * both must give the effective width the conversions use, or text is drawn
 * 3440/1024 wide on 1440/768 high frames (measured 2026-10-19).
 * RECOMP_HOF_SCREEN_SIZE: "eff" (default: both effective), "ui" (float
 * effective, int real), "real" (both real), to compare. */
static int size_mode(void) { /* 0 ui, 1 eff, 2 real */
    static int mode = -1;
    if (mode < 0) {
        const char *v = getenv("RECOMP_HOF_SCREEN_SIZE");
        mode = !v || !*v ? 1 : v[0] == 'u' ? 0 : v[0] == 'r' ? 2 : 1;
    }
    return mode;
}
/* From the globals the conversions read, so a panel that native/phone_hud.c
 * draws larger (it scales them for the panel) gets its glyphs larger too. */
static float effective_w(void) {
    return g_known ? rdf32(SCREEN_W) : real_w();
}
static float effective_h(void) {
    return g_known ? rdf32(SCREEN_H) : real_h();
}

/* 00e500b0: GetScreenSize(int *width, int *height), vtable slot, RET 8. */
void hof_get_screen_size(X86 *c) {
    const uint32_t esp = c->r[R_ESP];
    const uint32_t pw = rd32(esp + 4u), ph = rd32(esp + 8u);
    wr32(pw, (uint32_t)(int32_t)(size_mode() == 1 ? effective_w() : real_w()));
    wr32(ph, (uint32_t)(int32_t)(size_mode() == 2 ? real_h() : effective_h()));
    c->r[R_EAX] = ph;
    ret_args(c, 8u);
}

/* 00e437c0: the screen size as floats (fastcall: ECX the out pair), returns ECX. */
void hof_screen_size_f(X86 *c) {
    const uint32_t out = c->r[R_ECX];
    wrf32(out, size_mode() == 2 ? real_w() : effective_w());
    wrf32(out + 4u, size_mode() == 2 ? real_h() : effective_h());
    c->r[R_EAX] = out;
    ret_args(c, 0u);
}

/* 00e50100: SetOrigin(int x, int y), vtable slot, RET 8. */
void hof_set_origin(X86 *c) {
    const uint32_t esp = c->r[R_ESP];
    g_game_origin_x = (float)(int32_t)rd32(esp + 4u);
    wrf32(ORIGIN_X, g_game_origin_x - g_margin_x);
    wrf32(ORIGIN_Y, (float)(int32_t)rd32(esp + 8u));
    ret_args(c, 8u);
}

/* 00e50130: GetOrigin(int *x, int *y), vtable slot, RET 8. */
void hof_get_origin(X86 *c) {
    const uint32_t esp = c->r[R_ESP];
    const uint32_t px = rd32(esp + 4u), py = rd32(esp + 8u);
    wr32(px, (uint32_t)(int32_t)(rdf32(ORIGIN_X) + g_margin_x));
    wr32(py, (uint32_t)(int32_t)rdf32(ORIGIN_Y));
    c->r[R_EAX] = py;
    ret_args(c, 8u);
}

/* ---- the 3D scene's size, pick and labels ------------------------------- */

enum {
    RET_SCENE_SIZE = 0x007bfd9e, /* 007bfd10's GetVirtualSize call, for its camera */
    RET_BOOK_CAMERA = 0x007f1055, /* 007f1040's, for the spellbook's camera */
    RET_BOOK_PICK_SIZE = 0x007f194b, /* 007f1900's, the size it picks the book in */
    RET_BOOK_PICK = 0x007f195e,  /* 007f1900's pick against the spellbook's view */
};

void fn_009feb50(X86 *c);
void fn_009feed0(X86 *c);

/* native/ui_bounds.c: a root has been stretched across the screen. The
 * game's 3D windows then measure the picture across their own (now
 * screen-wide) rectangle, as on a 4:3 screen, and the corrections below,
 * made for a 1024-wide root, would move the pick and the labels twice. */
extern int homm5_ui_roots_stretched;
int homm5_horplus(void); /* native/horplus.c: the 3D widens its view (a map or battle is open) */

/* RECOMP_TRACE_WIDE=2: the first picks and labels. */
static int trace_scene(void) {
    static int left = -1;
    if (left < 0) {
        const char *v = getenv("RECOMP_TRACE_WIDE");
        left = v && v[0] == '2' ? 300 : 0;
    }
    return left > 0 && left--;
}

static float scene_width(void) {
    return 1024.0f + 2.0f * g_margin_x;
}

/* A size the scene's matrix was built for (or the plain interface size the
 * game passes beside it), on a wide screen. */
static int scene_sized(uint32_t size) {
    if (!(g_margin_x > 0.0f) || !size)
        return 0;
    const float w = rdf32(size), h = rdf32(size + 4u);
    return h == 768.0f && (w == 1024.0f || w == scene_width());
}

/* 00e437a0: GetVirtualSize (fastcall: ECX the out pair; returns ECX). The
 * scene's camera and the spellbook's (a 3D book drawn over the whole screen,
 * its camera and pick built by 007f1040) get the screen's shape: at 4:3 the
 * book was stretched across a wide screen and its pick missed the close
 * button. Hor+ (native/horplus.c) keeps the book's proportions. */
void hof_virtual_size(X86 *c) {
    const uint32_t out = c->r[R_ECX];
    const uint32_t ret = rd32(c->r[R_ESP]);
    const int book = ret == RET_BOOK_CAMERA || ret == RET_BOOK_PICK_SIZE;
    const int scene = ret == RET_SCENE_SIZE || (book && homm5_horplus());
    wrf32(out, scene ? scene_width() : 1024.0f);
    wrf32(out + 4u, 768.0f);
    c->r[R_EAX] = out;
    ret_args(c, 0u);
}

/* 009feb50: screen point -> ray (fastcall: ECX out origin, EDX out direction;
 * matrix stack, size, point; RET 0xC). The point is in interface units. */
void hof_pick_ray(X86 *c) {
    const uint32_t esp = c->r[R_ESP];
    const uint32_t size = rd32(esp + 8u), point = rd32(esp + 12u);
    /* The book's point is the cursor's, in the 1024 units centred on the
     * screen, whatever the roots do. */
    const int book = rd32(esp) == RET_BOOK_PICK;
    const int shift = point && scene_sized(size) && (book || !homm5_ui_roots_stretched);
    if (trace_scene() && point && size)
        fprintf(stderr, "[wide] pick from %08x: size %.0fx%.0f point %.1f,%.1f%s\n", rd32(esp),
                rdf32(size), rdf32(size + 4u), rdf32(point), rdf32(point + 4u),
                shift ? " (shifted)" : "");
    if (!shift) {
        fn_009feb50(c);
        return;
    }
    const uint32_t w = rd32(size), x = rd32(point);
    wrf32(size, scene_width());
    wrf32(point, rdf32(point) + g_margin_x);
    fn_009feb50(c);
    wr32(size, w);
    wr32(point, x);
}

/* 009feed0: world point -> screen (fastcall: ECX the point, EDX the matrix
 * stack; size, out x/y, out z, z min, z max; RET 0x14; AL = visible). */
void hof_world_to_screen(X86 *c) {
    const uint32_t esp = c->r[R_ESP];
    const uint32_t ret = rd32(esp);
    const uint32_t size = rd32(esp + 4u), out = rd32(esp + 8u);
    const int shift = scene_sized(size) && !homm5_ui_roots_stretched;
    const float in_w = size ? rdf32(size) : 0.0f;
    if (!shift) {
        fn_009feed0(c);
    } else {
        const uint32_t w = rd32(size);
        wrf32(size, scene_width());
        fn_009feed0(c);
        wr32(size, w);
        if (out)
            wrf32(out, rdf32(out) - g_margin_x);
    }
    if (trace_scene() && out)
        fprintf(stderr, "[wide] label from %08x: size %.0f -> %.1f,%.1f%s\n", ret, in_w,
                rdf32(out), rdf32(out + 4u), shift ? " (shifted)" : "");
}

/* ---- sizes converted as points --------------------------------------------
 * The point conversions subtract the origin, which the vanilla game keeps at
 * 0, so some callers convert a size with them: the cursor turns its texture
 * size {w, h} into pixels with 00e43900 (007278e0). With the centring origin
 * those sizes come out (w + margin) * scale: the cursor was huge and wide.
 * For the call sites in size_sites[] (return addresses) the origin is the
 * game's own for the call. The inverse conversions (pixels -> interface)
 * add the origin and are wrapped the same way. */
static const uint32_t size_sites[] = {
    /* Audited statically 2026-10-19 (every call of the point conversions;
     * design notes ultrawide-plan.md). Pairs that subtract two converted values
     * (00e3b160/00e3b17d, 00e4fab7/00e4fac6) cancel the origin and stay out. */
    0x0072794c, /* 007278e0: the cursor's texture size (00e43900) */
    0x0088e758, /* 0088e650: an offset added to absolute rects (00e439f0) */
    0x00eef805, /* 00eef680: the caret's height (00e43b60, y; harmless) */
    0x00e96e30, /* 00e96d20: a local y offset added to a clip corner (00e43ae0) */
    0x00e98ac3, /* 00e98a10: the mouse's distance into a text box (00e43ae0) */
    0x00ea4bd5, /* 00ea4b30: a local y offset added to a rect corner (00e43ae0) */
    /* text wrap widths (00e43b30) */
    0x00e3e0df, 0x00e3e191, 0x00e3c64e, 0x00e3cacd, 0x00e4f020, 0x00e9690c,
    0x00e9694a, 0x00e969b2, 0x00e97330, 0x00e992a1, 0x00e99761, 0x00e9a8f2,
    0x00ea4731, 0x00eee5f8, 0x00eee8e1,
    /* inverse: measured text sizes back to interface units (00e43b90 x,
     * 00e43c40 int pairs; 00e43bc0 y entries are harmless while origin y is 0) */
    0x00eee617, /* eee370: a text width compared with its box */
    0x00ea7fe7, /* ea7fc0 ITextView::GetTextSize: the map's hover info box (007518e0) */
    0x00e4f25e, /* CForegroundTextString::GetTextSize */
    0x00e4f292, /* CForegroundTextString::Draw: the text's clip rect size */
    0x00e4f07a, /* e4f060: a text width */
    0x00eeec5d, 0x00eef552, /* line heights (y) */
};

void fn_00e43900(X86 *c);
void fn_00e439f0(X86 *c);
void fn_00e43ae0(X86 *c);
void fn_00e43b30(X86 *c);
void fn_00e43b90(X86 *c);
void fn_00e43bf0(X86 *c);
void fn_00e43c40(X86 *c);

static int converts_size(X86 *c) {
    const uint32_t ret = rd32(c->r[R_ESP]);
    for (size_t i = 0; i < sizeof size_sites / sizeof size_sites[0]; ++i)
        if (size_sites[i] == ret)
            return 1;
    return 0;
}

static void call_without_origin(X86 *c, void (*original)(X86 *)) {
    if (!(g_margin_x != 0.0f) || !converts_size(c)) {
        original(c);
        return;
    }
    const uint32_t ox = rd32(ORIGIN_X);
    wrf32(ORIGIN_X, g_game_origin_x);
    original(c);
    wr32(ORIGIN_X, ox);
}

/* 00e43900 / 00e439f0: a point in place (fastcall ECX), 00e43ae0: a point to
 * another (ECX in, EDX out), 00e43b30: x as an int (float arg, RET 4). */
void hof_point_to_screen(X86 *c) {
    call_without_origin(c, fn_00e43900);
}
void hof_point_to_screen2(X86 *c) {
    call_without_origin(c, fn_00e439f0);
}
void hof_point_to_screen3(X86 *c) {
    call_without_origin(c, fn_00e43ae0);
}
void hof_x_to_screen(X86 *c) {
    call_without_origin(c, fn_00e43b30);
}

/* 00e43b90: an x back to interface units (float arg, RET 4, int in EAX);
 * 00e43bf0 / 00e43c40: a point back (ECX in, EDX out; floats / ints). */
void hof_x_from_screen(X86 *c) {
    call_without_origin(c, fn_00e43b90);
}
void hof_point_from_screen(X86 *c) {
    call_without_origin(c, fn_00e43bf0);
}
void hof_point_from_screen_i(X86 *c) {
    call_without_origin(c, fn_00e43c40);
}

/* ---- the adventure minimap ----------------------------------------------
 * NUI::CAdvMinimap draws its picture (008333f0) and reads the mouse
 * (00833760 buttons, 00833910 moves) at its description's Position
 * (desc +0x88/+0x8c, desc = window +0xfc) plus a 25-unit frame, as if the
 * window sat at that offset from the screen's corner; it never reads where
 * the window was laid out (+0x64/+0x68). Anchored to the bottom left of a
 * wide screen, the frame moves and the picture stayed at the top left. Each
 * runs with the laid-out corner in the Position, which the layout reads as
 * the anchor offset, so it is put back after. ECX is the minimap's +0x1b0
 * part in all three. */
enum {
    MINIMAP_PART = 0x1b0,
    WINDOW_DESC = 0xfc,
    WINDOW_X = 0x64,
    WINDOW_Y = 0x68,
    DESC_POS_X = 0x88,
    DESC_POS_Y = 0x8c,
};

void fn_008333f0(X86 *c);
void fn_00833760(X86 *c);
void fn_00833910(X86 *c);

static void at_laid_out_corner(X86 *c, void (*original)(X86 *)) {
    const uint32_t window = c->r[R_ECX] - MINIMAP_PART;
    const uint32_t desc = rd32(window + WINDOW_DESC);
    if (!desc) {
        original(c);
        return;
    }
    const uint32_t x = rd32(desc + DESC_POS_X), y = rd32(desc + DESC_POS_Y);
    wr32(desc + DESC_POS_X, rd32(window + WINDOW_X));
    wr32(desc + DESC_POS_Y, rd32(window + WINDOW_Y));
    original(c);
    wr32(desc + DESC_POS_X, x);
    wr32(desc + DESC_POS_Y, y);
}

void hof_minimap_draw(X86 *c) {
    at_laid_out_corner(c, fn_008333f0);
}
void hof_minimap_button(X86 *c) {
    at_laid_out_corner(c, fn_00833760);
}
void hof_minimap_move(X86 *c) {
    at_laid_out_corner(c, fn_00833910);
}

/* ---- a battle's stack counters ------------------------------------------
 * 007518e0 places a counter over its creature (the creature's label minus
 * half the counter) through the counter's IWindow (its sub-object at
 * object + 4 + the offset its second vtable names; RET 0x18). The position
 * is from the 1024-wide interface's own left edge, which the counter keeps
 * while only placed. When its battle screen is laid out again (as it is when
 * the spellbook closes) the counter goes through the anchor axis, which adds
 * the left edge of a root stretched across the screen (and on a phone the
 * safe area's): the counters slid left by that much. native/ui_bounds.c asks
 * here and lays a counter out from 0, as it was placed. */
void fn_007518e0(X86 *c);

static uint32_t g_counters[256];
static int g_counter_next;

void hof_counter_place(X86 *c) {
    const uint32_t obj = rd32(c->r[R_ESP] + 4u);
    if (obj) {
        const uint32_t sub = obj + 4u + rd32(rd32(obj + 4u) + 8u);
        int known = 0;
        for (int i = 0; i < 256 && !known; ++i)
            known = g_counters[i] == sub;
        if (!known) {
            g_counters[g_counter_next] = sub;
            g_counter_next = (g_counter_next + 1) % 256;
        }
    }
    fn_007518e0(c);
}

/* Whether `win` (a CWindow, as SetParentRect has it) is a counter: its
 * IWindow sub-object sits a little way into it. */
int hof_counter_window(uint32_t win) {
    static const uint32_t parts[] = {0u, 0x10u, 0x14u, 0xf8u, 0x10cu};
    for (int i = 0; i < 256; ++i) {
        if (!g_counters[i])
            continue;
        for (size_t k = 0; k < sizeof parts / sizeof parts[0]; ++k)
            if (g_counters[i] == win + parts[k]) {
                static int told;
                if (getenv("RECOMP_TRACE_HUD") && told < 4) {
                    ++told;
                    fprintf(stderr, "[wide] counter %08x (IWindow at +%x) laid out from 0\n", win, parts[k]);
                }
                return 1;
            }
    }
    return 0;
}
