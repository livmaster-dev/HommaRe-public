/* phone_layout.c - a phone's corner rows of buttons instead of the jog dials.
 *
 * The game's corner menus are jog dials, a disc with round buttons around it
 * (tools/phone_ui_mod.py tells the layout). On a phone-sized window the
 * buttons go in an L at the screen's corner instead, the map's and the
 * battle's disc hidden and the town's dials made frames without it; on a
 * window that is not phone-sized (an iPad, a foldable iPhone opened) the game
 * lays them out as its files say. The choice follows host_phone_layout as it
 * changes - a foldable folded or opened - and every screen is laid out again
 * then (native/phone_hud.c homm5_phone_layout_epoch).
 *
 * The layout is the game's own, given other arguments: SetParentRect asks the
 * anchor axis (00e43650; native/ui_bounds.c homm5_anchor_axis) for each
 * window's x and then its y, with the anchor, the offset and the size of the
 * window's placement (its description's +0x84). For a window of the table
 * (native/phone_layout_table.h, written by tools/phone_ui_mod.py --c-table)
 * the anchor and the offset are replaced in the call; a frame's size is
 * written in its description, and put back for a tablet-sized window. A
 * window is known by its <Name> (description +0x58) and its parent's - a
 * screen's root has none, and is known by a child only it has. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native_seam.h"
#include "x86.h"

#ifdef RECOMP_OVERRIDE_HEADER
#include RECOMP_OVERRIDE_HEADER
#endif
#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif

/* A row of the table: the window (its name, or where that is NULL the
 * window that has a child `with_child`, whatever its name), its parent (by name, or the parent that has a
 * child `parent_child`), the anchors to use across and down (EPA_*: 0 the
 * near edge, 1 the centre, 2 the far edge; -1 the window's own), the offsets
 * (Position) where set_x / set_y, a size where w is not 0, and flags. */
typedef struct {
    const char *name, *with_child, *parent, *parent_child;
    int hor, ver, x, y, set_x, set_y, w, h, flags;
} PhoneLayout;

enum {
    PL_HIDE = 1, /* not drawn, nor under a finger: a jog dial's disc */
    PL_BARE = 2, /* drawn without its background: a dial made a frame */
};

#include "phone_layout_table.h"

enum {
    WINDOW_PARENT = 0x28,     /* its parent window, 0 for a screen's root */
    WINDOW_BACKGROUND = 0x98, /* the background CWindow::Draw draws first */
    WINDOW_CHILDREN = 0xa4,   /* std::vector of its child windows */
    DESC_NAME = 0x58,         /* the description's <Name>: begin, end, capacity */
    DESC_VISIBLE = 0x70,      /* its <Visible>, as Show/Hide set it */
    DESC_PLACEMENT = 0x84,    /* its <Placement>: */
    PLACEMENT_SIZE = 0x20,    /* float w, h */
    NAME_MAX_ = 64,
    CHILDREN_MAX = 256,
};

static int env_flag(const char *name, int fallback) {
    const char *v = getenv(name);
    return v && *v ? v[0] != '0' : fallback;
}

static int trace_layout(void) {
    static int on = -1;
    if (on < 0)
        on = env_flag("RECOMP_TRACE_PHONE_LAYOUT", 0);
    return on;
}

static int guest_ptr(uint32_t a) {
    return a >= 0x10000u && a < (uint32_t)GUEST_SIZE - 0x100u;
}

/* Whether the buttons go in the phone's rows now. RECOMP_HOMM5_PHONE_LAYOUT=0
 * keeps the game's dials on a phone too. */
static int layout_on(void) {
    static int allowed = -1;
    if (allowed < 0)
        allowed = env_flag("RECOMP_HOMM5_PHONE_LAYOUT", 1);
    return allowed && host_phone_layout();
}

static void discs_shown_again(void);

/* A count that moves on each time layout_on changes: a screen laid out at
 * another count is laid out again (native/phone_hud.c). */
int homm5_phone_layout_epoch(void) {
    static int last = -1, epoch;
    const int on = layout_on();
    if (last >= 0 && on != last) {
        ++epoch;
        if (!on)
            discs_shown_again();
        if (trace_layout())
            fprintf(stderr, "[layout] phone rows %s (epoch %d)\n", on ? "on" : "off", epoch);
    }
    last = on;
    return epoch;
}

/* A window's description: what its vtable's third slot returns, read from the
 * getter's own code (MOV EAX,[ECX+n]; RET) rather than called. 0 when the
 * getter is not that. Kept per vtable. */
static uint32_t window_desc(uint32_t win) {
    static struct {
        uint32_t vt;
        int32_t off; /* -1 not a plain getter */
    } seen[32];
    static int next;
    if (!guest_ptr(win))
        return 0;
    const uint32_t vt = rd32(win);
    if (!guest_ptr(vt))
        return 0;
    int32_t off = -2;
    for (int i = 0; i < 32; ++i)
        if (seen[i].vt == vt) {
            off = seen[i].off;
            break;
        }
    if (off == -2) {
        const uint32_t fn = rd32(vt + 8);
        off = -1;
        if (guest_ptr(fn) && rd8(fn) == 0x8b) {
            if (rd8(fn + 1) == 0x81 && rd8(fn + 6) == 0xc3) /* MOV EAX,[ECX+imm32]; RET */
                off = (int32_t)rd32(fn + 2);
            else if (rd8(fn + 1) == 0x41 && rd8(fn + 3) == 0xc3) /* MOV EAX,[ECX+imm8]; RET */
                off = (int32_t)(int8_t)rd8(fn + 2);
            if (off < 0 || off > 0x1000)
                off = -1;
        }
        seen[next].vt = vt;
        seen[next].off = off;
        next = (next + 1) % 32;
    }
    if (off < 0)
        return 0;
    const uint32_t desc = rd32(win + (uint32_t)off);
    return guest_ptr(desc) ? desc : 0;
}

/* The description's name into `out` (empty when it has none). */
static void desc_name(uint32_t desc, char out[NAME_MAX_]) {
    out[0] = 0;
    if (!desc)
        return;
    const uint32_t b = rd32(desc + DESC_NAME), e = rd32(desc + DESC_NAME + 4);
    if (!guest_ptr(b) || e < b || e - b >= NAME_MAX_)
        return;
    uint32_t n = 0;
    for (; n < e - b; ++n)
        out[n] = (char)rd8(b + n);
    out[n] = 0;
}

/* A window's <Name> for native/native_screens.c ("" when it has none). */
void homm5_window_name(uint32_t win, char *out, size_t cap) {
    char name[NAME_MAX_];
    desc_name(window_desc(win), name);
    snprintf(out, cap, "%s", name);
}

/* Whether a window is shown, as IWindow::IsVisible says (00e3bf30 in the
 * base game, the same code in both expansions): its own flag and every
 * parent's, up to the screen's root. For native/hotkeys.c. */
int homm5_window_shown(uint32_t win) {
    for (int guard = 32; guard-- && guest_ptr(win); win = rd32(win + WINDOW_PARENT)) {
        const uint32_t desc = window_desc(win);
        if (!desc || !rd8(desc + DESC_VISIBLE))
            return 0;
    }
    return 1;
}

/* For native/skill_wheel.c: a window's child of that <Name> (0 when none),
 * and a window's own <Visible> taken down: 1 when it was up. */
uint32_t homm5_window_child(uint32_t win, const char *name) {
    if (!guest_ptr(win))
        return 0;
    const uint32_t b = rd32(win + WINDOW_CHILDREN), e = rd32(win + WINDOW_CHILDREN + 4);
    if (!guest_ptr(b) || e < b || e - b > 4u * CHILDREN_MAX)
        return 0;
    for (uint32_t at = b; at < e; at += 4) {
        const uint32_t child = rd32(at);
        char got[NAME_MAX_];
        desc_name(window_desc(child), got);
        if (strcmp(got, name) == 0)
            return child;
    }
    return 0;
}

int homm5_window_take_shown(uint32_t win) {
    const uint32_t desc = window_desc(win);
    if (!desc || !rd8(desc + DESC_VISIBLE))
        return 0;
    wr8(desc + DESC_VISIBLE, 0);
    return 1;
}

static int named(uint32_t win, const char *name) {
    char got[NAME_MAX_];
    desc_name(window_desc(win), got);
    return strcmp(got, name) == 0;
}

static int has_child(uint32_t win, const char *name) {
    if (!guest_ptr(win))
        return 0;
    const uint32_t b = rd32(win + WINDOW_CHILDREN), e = rd32(win + WINDOW_CHILDREN + 4);
    if (!guest_ptr(b) || e < b || e - b > 4u * CHILDREN_MAX)
        return 0;
    for (uint32_t at = b; at < e; at += 4) {
        const uint32_t child = rd32(at);
        if (child && named(child, name))
            return 1;
    }
    return 0;
}

/* The table's row for window `win` (description `desc`), or NULL. */
static const PhoneLayout *row_of(uint32_t win, uint32_t desc) {
    char name[NAME_MAX_];
    desc_name(desc, name);
    const uint32_t parent = rd32(win + WINDOW_PARENT);
    for (size_t i = 0; i < sizeof PHONE_LAYOUT / sizeof PHONE_LAYOUT[0]; ++i) {
        const PhoneLayout *r = &PHONE_LAYOUT[i];
        if (r->name ? strcmp(name, r->name) != 0 : !has_child(win, r->with_child))
            continue;
        if (r->parent && !(guest_ptr(parent) && named(parent, r->parent)))
            continue;
        if (r->parent_child && !(guest_ptr(parent) && has_child(parent, r->parent_child)))
            continue;
        return r;
    }
    return NULL;
}

/* The sizes of the descriptions a frame's size was written in, as the game
 * had them, to put back where the window is not phone-sized. */
static struct {
    uint32_t desc;
    float w, h;
} g_sizes[32];

static int size_slot(uint32_t desc, int add) {
    for (int i = 0; i < 32; ++i)
        if (g_sizes[i].desc == desc)
            return i;
    if (!add)
        return -1;
    for (int i = 0; i < 32; ++i)
        if (!g_sizes[i].desc) {
            g_sizes[i].desc = desc;
            g_sizes[i].w = rdf32(desc + DESC_PLACEMENT + PLACEMENT_SIZE);
            g_sizes[i].h = rdf32(desc + DESC_PLACEMENT + PLACEMENT_SIZE + 4);
            return i;
        }
    return -1;
}

/* From native/ui_bounds.c homm5_anchor_axis, before the axis runs: EDI the
 * window, ESI its placement, ECX the anchor, EDX the size it is laid out at
 * (in its description), the offset the third stack word. `vertical`: the y
 * call. */
void homm5_phone_layout_axis(X86 *c, int vertical) {
    const uint32_t win = c->r[R_EDI], placement = c->r[R_ESI];
    if (!guest_ptr(win) || !guest_ptr(placement))
        return;
    const uint32_t desc = placement - DESC_PLACEMENT;
    const int on = layout_on();
    if (!on) {
        /* A frame's size back as the game had it. */
        const int slot = size_slot(desc, 0);
        if (slot >= 0)
            wrf32(c->r[R_EDX], vertical ? g_sizes[slot].h : g_sizes[slot].w);
        return;
    }
    const PhoneLayout *r = row_of(win, desc);
    if (!r)
        return;
    const int anchor = vertical ? r->ver : r->hor;
    if (anchor >= 0)
        c->r[R_ECX] = (uint32_t)anchor;
    if (vertical ? r->set_y : r->set_x)
        wrf32(c->r[R_ESP] + 0xc, (float)(vertical ? r->y : r->x));
    if (r->w && size_slot(desc, 1) >= 0)
        wrf32(c->r[R_EDX], (float)(vertical ? r->h : r->w));
    if (trace_layout() && vertical)
        fprintf(stderr, "[layout] window %08x %s%s: anchors %d,%d at %d,%d%s\n", win,
                r->name ? r->name : "with ", r->name ? "" : r->with_child, r->hor, r->ver, r->x, r->y,
                r->w ? " (framed)" : "");
}

/* The jog dials' discs the phone's rows hid for good (their <Visible> off, as
 * the game's Hide puts it): not drawn alone, a disc stayed a window of its
 * screen for everything else - the game's own hit tests and the tooltip
 * controller's search for the window under a point (00e9c5d0 in Tribes of
 * the East) found it over the battlefield beside the rows, and asked it for
 * its tooltip (a crash in 00e9b230 after a right button there). Shown again
 * when the rows go (a foldable opened); a disc the game shows again itself
 * is hidden again at its next draw. */
static struct {
    uint32_t win, desc;
} g_discs[16];

static void disc_hidden(uint32_t win, uint32_t desc) {
    if (!rd8(desc + DESC_VISIBLE))
        return;
    wr8(desc + DESC_VISIBLE, 0);
    int slot = -1;
    for (int i = 0; i < 16; ++i) {
        if (g_discs[i].win == win) {
            slot = i;
            break;
        }
        if (slot < 0 && !g_discs[i].win)
            slot = i;
    }
    if (slot < 0)
        slot = 0;
    g_discs[slot].win = win;
    g_discs[slot].desc = desc;
    if (trace_layout())
        fprintf(stderr, "[layout] disc %08x hidden\n", win);
}

static void discs_shown_again(void) {
    for (int i = 0; i < 16; ++i) {
        const uint32_t win = g_discs[i].win, desc = g_discs[i].desc;
        g_discs[i].win = g_discs[i].desc = 0;
        /* Only a window still there: the same description, a disc's name. */
        if (!win || window_desc(win) != desc || !(named(win, "Jogdial") || named(win, "JogDialBase")))
            continue;
        wr8(desc + DESC_VISIBLE, 1);
        if (trace_layout())
            fprintf(stderr, "[layout] disc %08x shown again\n", win);
    }
}

/* How CWindow::Draw is to draw window `win` now: 0 as it is, PL_HIDE not at
 * all, PL_BARE without its background. */
int homm5_phone_layout_draw(uint32_t win) {
    if (!layout_on())
        return 0;
    const uint32_t desc = window_desc(win);
    if (!desc)
        return 0;
    char name[NAME_MAX_];
    desc_name(desc, name);
    for (size_t i = 0; i < sizeof PHONE_LAYOUT / sizeof PHONE_LAYOUT[0]; ++i) {
        const PhoneLayout *r = &PHONE_LAYOUT[i];
        if (!r->flags)
            continue;
        /* Cheap first: the name, or a frame's size as laid out. */
        if (r->name ? strcmp(name, r->name) != 0
                    : rdf32(desc + DESC_PLACEMENT + PLACEMENT_SIZE) != (float)r->w ||
                          rdf32(desc + DESC_PLACEMENT + PLACEMENT_SIZE + 4) != (float)r->h)
            continue;
        if (row_of(win, desc) == r) {
            if (r->flags & PL_HIDE)
                disc_hidden(win, desc);
            return r->flags & PL_HIDE ? PL_HIDE : PL_BARE;
        }
    }
    return 0;
}

/* A window drawn without its background: its background's slot for the
 * draw, put back by homm5_phone_layout_restore. */
uint32_t homm5_phone_layout_strip(uint32_t win) {
    const uint32_t bg = rd32(win + WINDOW_BACKGROUND);
    wr32(win + WINDOW_BACKGROUND, 0);
    return bg;
}

void homm5_phone_layout_restore(uint32_t win, uint32_t bg) {
    wr32(win + WINDOW_BACKGROUND, bg);
}
