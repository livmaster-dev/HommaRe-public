/* native/touch_native.c - native touch (runtime/native_seam.h
 * recomp_touch_event): the launcher's "Touch: native". Included at the end of
 * native/touch.c, whose helpers it uses. Design notes: the user's research
 * notes native-touch/proposal.md, checked on the game in stage0-results.md.
 *
 * The host hands over a finger's gesture with its point; there is no cursor
 * between gestures (GetCursorPos lands far outside the window, so nothing
 * hovers and no edge scrolls). Here, on the game's own thread (the hover
 * getter, once a frame, before the screen's Update):
 *
 * - The game's cursor is held at the point a gesture is at: NUI::CCursor's
 *   floats are written every frame from the finger's touch until the next
 *   one, or until a click on the map or a battlefield has gone through. The
 *   game's own per-frame work then sees the point - the battlefield's pick,
 *   the map's cursor, and the map's question whether the interface lies under
 *   a release (0079b810 asks 007c2900, which reads CCursor, not the message:
 *   a button over the map takes a release only with the cursor on it).
 * - A click is the WndProc's own events in the game's input queue (005f3140:
 *   type, client x, y, wParam), the same messages a mouse makes: down and up
 *   in one frame, nothing held for frames. Once the queue has been read, a
 *   (-1,-1) move straight to the top window screen (IWindow+0x24, 00e73de0)
 *   takes away the hover the click left, and IScreen+0x6C its tooltip.
 * - A rest shows what the game shows on hover: a move to the point, and the
 *   hover delay at once (homm5_hover_timeout asks homm5_native_hover). What it
 *   showed stays after the lift, until the next touch.
 * - A battlefield's tap first shows the move or attack (the hold at the
 *   point), a second tap on it acts; with the "strike from a cell" mode on
 *   (RECOMP_TOUCH_BATTLE_DIRECT) every tap clicks, as that mode wants.
 * - With no gesture held, a battlefield skips its pick (native/tap_attack.c
 *   asks homm5_native_parks): a cursor far off the field would be clamped to
 *   its edge and the next click would walk there.
 * - Movies and loading screens wait for a key, not a click: a tap presses one.
 *
 * Addresses: the base game's here, an expansion's in its addresses.h
 * (HOMM5_NT_*, found with games/map_addresses.py). */

#include "native_seam.h"

#ifndef RECOMP_ADDRESS_HEADER
#define NT_SUPPORTED 1
#define NT_QUEUE 0x005f3140        /* fastcall(ECX type, EDX x; y, wParam), RET 8 */
#define NT_QUEUE_LIST 0x01133ed4   /* std::list of queued events: the sentinel */
#define NT_CWS_ISCREEN_VT 0x0102e66c /* CWindowScreen's IScreen part (cws + 0xf8) */
#define NT_CLIENT_W 0x010d2cf4     /* the client area, float pixels */
#define NT_CLIENT_H 0x010d2cf8
#define NT_VT_MOVIE 0x00fc3fb8     /* the movie player's screen (intro, cut scenes) */
#define NT_VT_LOADING 0x00fb3c68   /* the loading screen that waits for a key */
#define NT_FN_EDGE_SCROLL fn_0070ded0 /* CCursor::GetEdgeScroll(out, int *id), RET 8 */
#define NT_FN_CURSOR_UPDATE fn_0070ddb0 /* CCursor::Update(flag), RET 4 (vtable +0x1C) */
#define NT_SCR_ACTIVE 0x3e4        /* the battle screen's active stack */
#define NT_ISCREEN_HIDE_TIP 0x6c   /* IScreen: hide the tooltip (00e752b0) */
#elif defined(HOMM5_NT_QUEUE)
#define NT_SUPPORTED 1               /* an expansion: its addresses.h */
#define NT_QUEUE HOMM5_NT_QUEUE
#define NT_QUEUE_LIST HOMM5_NT_QUEUE_LIST
#define NT_CWS_ISCREEN_VT HOMM5_NT_CWS_ISCREEN_VT
#define NT_CLIENT_W HOMM5_UI_REAL_W
#define NT_CLIENT_H HOMM5_UI_REAL_H
#define NT_VT_MOVIE HOMM5_NT_VT_MOVIE
#define NT_VT_LOADING HOMM5_NT_VT_LOADING
#define NT_FN_EDGE_SCROLL HOMM5_NT_FN_EDGE_SCROLL
#define NT_FN_CURSOR_UPDATE HOMM5_NT_FN_CURSOR_UPDATE
#define NT_SCR_ACTIVE HOMM5_TAP_SCR_ACTIVE
#define NT_ISCREEN_HIDE_TIP HOMM5_NT_ISCREEN_HIDE_TIP
#else
#define NT_SUPPORTED 0
#endif

#ifdef HOMM5_NT_REAL_SCREEN_SIZE
int HOMM5_NT_REAL_SCREEN_SIZE(float *w, float *h);
#endif

enum {
    NT_WM_MOVE = 1, /* the queue's event types (re-nui.md 1.1) */
    NT_WM_RDOWN = 2,
    NT_WM_LDOWN = 3,
    NT_WM_RUP = 4,
    NT_WM_LUP = 5,
    NT_WM_LDBL = 7,
    NT_MK_LBUTTON = 1,
    NT_EVENTS = 64,
    NT_POSTS = 32,
    NT_SDL_SPACE = 44, /* SDL scancodes */
    NT_SDL_ESCAPE = 41,
};

/* A battlefield's second tap acts within this of the first, client pixels per
 * 1000 of the picture's height (28 points on a 768-point phone screen). */
#define NT_PREVIEW_SLOP 36.0f

typedef struct {
    int kind, phase, count;
    float x, y; /* client pixels */
} NtEvent;

typedef struct {
    int type;
    int32_t x, y;
    uint32_t wparam;
} NtPost;

/* Filled under the baton by the host's input queue, emptied by the game's
 * thread under it too: never at the same time. */
static NtEvent g_nt_events[NT_EVENTS];
static unsigned g_nt_head, g_nt_count;

static int g_nt_active;      /* a finger touched since a mouse last moved */
static int g_nt_hold;        /* the cursor is held at (g_nt_x, g_nt_y) */
static float g_nt_x, g_nt_y; /* client pixels */
static int g_nt_press;       /* a rest is under way: hover at once */
static int g_nt_shown;       /* a right tap showed a card: it stays until the next touch */
static int g_nt_rdown;       /* a battlefield's right button is down (its card shows while it is) */
static int g_nt_drag;        /* an interface drag holds the left button */
static int g_nt_preview;     /* a battlefield's first tap was here */
static float g_nt_preview_x, g_nt_preview_y;
static int g_nt_aim;         /* a drag over a battlefield aims (no buttons) */
static uint32_t g_nt_turn;   /* the battle's active stack when the cursor was held */
static NtPost g_nt_posts[NT_POSTS]; /* events for the queue next frame */
static int g_nt_nposts;
static int g_nt_leave;       /* frames left before the hover is taken away (-1 none) */
static int g_nt_release;     /* the hold goes with the leave */
static int g_nt_key = -1, g_nt_key_frames; /* a key held for an any-key screen */

int recomp_touch_event(int kind, int phase, float x, float y, int width, int height, int count) {
#if !NT_SUPPORTED
    (void)kind, (void)phase, (void)x, (void)y, (void)width, (void)height, (void)count;
    return 0;
#else
    if (kind == RECOMP_TOUCH_EVENT_QUERY)
        return 1;
    if (g_nt_count >= NT_EVENTS)
        return 1; /* a stalled game: drop the newest rather than block the host */
    float cx = x, cy = y;
#ifdef HOMM5_NT_REAL_SCREEN_SIZE
    float cw = 0.0f, ch = 0.0f; /* Hammers of Fate keeps an effective width in the globals */
    HOMM5_NT_REAL_SCREEN_SIZE(&cw, &ch);
#else
    const float cw = rdf32(NT_CLIENT_W), ch = rdf32(NT_CLIENT_H);
#endif
    if (width > 0 && height > 0 && cw > 0 && ch > 0) {
        cx = x * cw / (float)width;
        cy = y * ch / (float)height;
    }
    NtEvent *e = &g_nt_events[(g_nt_head + g_nt_count) % NT_EVENTS];
    e->kind = kind;
    e->phase = phase;
    e->count = count;
    e->x = cx;
    e->y = cy;
    ++g_nt_count;
    return 1;
#endif
}

#if NT_SUPPORTED

static int nt_trace(void) {
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_TRACE_TOUCH") != NULL;
    return on;
}

/* Native touch is in charge: a finger came after the last mouse. */
static int nt_on(void) {
    return g_nt_active;
}

static void nt_post(int type, float x, float y, uint32_t wparam) {
    if (g_nt_nposts >= NT_POSTS)
        return;
    NtPost *p = &g_nt_posts[g_nt_nposts++];
    p->type = type;
    p->x = (int32_t)x;
    p->y = (int32_t)y;
    p->wparam = wparam;
}

static int nt_queue_length(void) {
    const uint32_t s = peek32(NT_QUEUE_LIST);
    int n = 0;
    for (uint32_t p = s ? peek32(s) : 0; p && p != s && n < 1000; p = peek32(p))
        ++n;
    return n;
}

/* The top screen's window screen (CWindowScreen), from the pointer to its
 * IScreen part the screen object holds (+0xa0 for the map and a battle). Only
 * a pointer into the guest's heaps counts. */
static uint32_t nt_top_cws(void) {
    const uint32_t screen = top_screen();
    for (int32_t off = -0x1000; screen && off < 0x1000; off += 4) {
        const uint32_t p = peek32(screen + (uint32_t)off);
        if (((p >= 0x01400000u && p < 0x0e000000u) || p >= 0x10400000u) &&
            p < (uint32_t)GUEST_SIZE - 0x400u && peek32(p) == NT_CWS_ISCREEN_VT)
            return p - 0xf8u;
    }
    return 0;
}

/* No hover left: the windows forget the pointer, the tooltip goes. */
static void nt_leave_now(X86 *c) {
    const uint32_t cws = nt_top_cws();
    if (!cws)
        return;
    const uint32_t top = (c->r[R_ESP] - 0x800u) & ~15u, pt = top;
    const uint32_t iw = cws + 0x10u + rd32(rd32(cws + 0x10u) + 8u);
    wrf32(pt, -1.0f);
    wrf32(pt + 4u, -1.0f);
    const uint32_t args[2] = {pt, 0};
    homm5_guest_call_regs(c, rd32(rd32(iw) + 0x24u), iw, 0, args, 2, top);
    const uint32_t is = cws + 0xf8u;
    homm5_guest_call_regs(c, rd32(rd32(is) + NT_ISCREEN_HIDE_TIP), is, 0, NULL, 0, top);
}

static uint32_t nt_battle_scr(void) {
    const uint32_t s = top_screen();
    return s && peek32(s) == VT_COMBAT ? s - SUB_COMBAT : 0;
}

static void nt_hold(float x, float y) {
    const uint32_t scr = nt_battle_scr();
    g_nt_turn = scr ? rd32(scr + NT_SCR_ACTIVE) : 0;
    g_nt_hold = 1;
    g_nt_x = x;
    g_nt_y = y;
}

static void nt_click(const NtEvent *e) {
    nt_hold(e->x, e->y);
    if (e->count >= 2) { /* Windows' order: down, up, double click, up */
        nt_post(NT_WM_MOVE, e->x, e->y, 0);
        nt_post(NT_WM_LDBL, e->x, e->y, NT_MK_LBUTTON);
        nt_post(NT_WM_LUP, e->x, e->y, 0);
    } else {
        nt_post(NT_WM_MOVE, e->x, e->y, 0);
        nt_post(NT_WM_LDOWN, e->x, e->y, NT_MK_LBUTTON);
        nt_post(NT_WM_LUP, e->x, e->y, 0);
    }
    g_nt_leave = 2;
    /* On the map the per-frame work reads the cursor: once the click is
     * through, no cursor. A battlefield keeps it until the turn moves on: a
     * click that only chose (an enemy for "strike from a cell") leaves the
     * game showing that choice, its damage and the side it strikes from, at
     * the point. Elsewhere it stays (a text field the keyboard is up for). */
    const uint32_t s = top_screen(), vt = s ? peek32(s) : 0;
    g_nt_release = vt == VT_ADVENTURE;
}

static void nt_key_tap(int scancode) {
    if (g_nt_key >= 0)
        return;
    host_touch_key(scancode, 1);
    g_nt_key = scancode;
    g_nt_key_frames = 4;
}

/* A battlefield shows a creature's card only while the right button is
 * down; a touch that showed one lets it go. */
static void nt_right_up(void) {
    if (g_nt_rdown) {
        g_nt_rdown = 0;
        nt_post(NT_WM_RUP, g_nt_x, g_nt_y, 0);
    }
}

static void nt_apply(X86 *c, const NtEvent *e) {
    const uint32_t s = top_screen(), vt = s ? peek32(s) : 0;
    if (nt_trace())
        fprintf(stderr, "[touch-native] event %d phase %d at %.0f,%.0f count %d screen %08x\n",
                e->kind, e->phase, e->x, e->y, e->count, vt);
    switch (e->kind) {
    case RECOMP_TOUCH_EVENT_POINTER:
        nt_right_up();
        g_nt_active = g_nt_hold = g_nt_press = g_nt_preview = g_nt_aim = g_nt_shown = 0;
        g_nt_drag = 0;
        g_nt_leave = -1;
        return;
    case RECOMP_TOUCH_EVENT_DOWN:
        g_nt_active = 1;
        nt_right_up();
        if (g_nt_press || g_nt_hold || g_nt_shown)
            nt_leave_now(c); /* what the last touch showed goes */
        g_nt_press = g_nt_aim = g_nt_shown = 0;
        nt_hold(e->x, e->y);
        /* A battlefield learns what is under the point from a move: over its
         * buttons the cursor turns into the finger before the tap asks
         * whether it is on the field (else a button's tap was a preview). */
        if (nt_battle_scr())
            nt_post(NT_WM_MOVE, e->x, e->y, 0);
        return;
    case RECOMP_TOUCH_EVENT_TAP: {
        g_nt_active = 1;
        if (vt == NT_VT_MOVIE || vt == NT_VT_LOADING) {
            nt_key_tap(vt == NT_VT_MOVIE ? NT_SDL_ESCAPE : NT_SDL_SPACE);
            return;
        }
        const int ctx = recomp_touch_context();
        if ((ctx & RECOMP_TOUCH_BATTLE) && !(ctx & RECOMP_TOUCH_BATTLE_DIRECT)) {
            const float slop = NT_PREVIEW_SLOP * rdf32(NT_CLIENT_H) / 1000.0f;
            const float dx = e->x - g_nt_preview_x, dy = e->y - g_nt_preview_y;
            if (g_nt_preview && dx * dx + dy * dy <= slop * slop) {
                NtEvent at = *e;
                at.x = g_nt_preview_x;
                at.y = g_nt_preview_y;
                at.count = 1;
                g_nt_preview = 0;
                nt_click(&at);
            } else { /* the first tap: the game shows the move or the attack */
                g_nt_preview = 1;
                g_nt_preview_x = e->x;
                g_nt_preview_y = e->y;
                nt_hold(e->x, e->y);
            }
            return;
        }
        g_nt_preview = 0;
        nt_click(e);
        return;
    }
    case RECOMP_TOUCH_EVENT_PRESS:
        if (e->phase == RECOMP_TOUCH_PHASE_BEGIN || e->phase == RECOMP_TOUCH_PHASE_MOVE) {
            g_nt_press = 1;
            nt_hold(e->x, e->y);
            nt_post(NT_WM_MOVE, e->x, e->y, 0);
            /* On a battlefield a rest is the right button held: the card of
             * the creature under the finger, as long as it rests. */
            if (e->phase == RECOMP_TOUCH_PHASE_BEGIN && nt_battle_scr()) {
                nt_post(NT_WM_RDOWN, e->x, e->y, 0);
                g_nt_rdown = 1;
            }
        } else {
            g_nt_press = 0; /* what it showed stays until the next touch */
            nt_right_up();
        }
        return;
    case RECOMP_TOUCH_EVENT_DRAG:
        if (e->phase == RECOMP_TOUCH_PHASE_BEGIN) {
            const int ctx = recomp_touch_context();
            g_nt_preview = 0;
            nt_hold(e->x, e->y);
            if (ctx & RECOMP_TOUCH_BATTLE) { /* a battlefield: aim, the game shows it */
                g_nt_aim = 1;
                return;
            }
            g_nt_drag = 1;
            nt_post(NT_WM_MOVE, e->x, e->y, 0);
            nt_post(NT_WM_LDOWN, e->x, e->y, NT_MK_LBUTTON);
        } else if (e->phase == RECOMP_TOUCH_PHASE_MOVE) {
            nt_hold(e->x, e->y);
            if (g_nt_drag)
                nt_post(NT_WM_MOVE, e->x, e->y, NT_MK_LBUTTON);
        } else {
            nt_hold(e->x, e->y);
            if (g_nt_aim) { /* an aimed point is a preview: a tap on it acts */
                g_nt_aim = 0;
                g_nt_preview = 1;
                g_nt_preview_x = e->x;
                g_nt_preview_y = e->y;
            }
            if (g_nt_drag) {
                g_nt_drag = 0;
                nt_post(NT_WM_MOVE, e->x, e->y, NT_MK_LBUTTON);
                nt_post(NT_WM_LUP, e->x, e->y, 0);
                g_nt_leave = 2;
                g_nt_release = 0;
            }
        }
        return;
    case RECOMP_TOUCH_EVENT_RIGHT_TAP: /* what a right click shows; it stays */
        g_nt_active = 1;
        g_nt_preview = 0;
        g_nt_shown = 1; /* the card's controller keeps it only while hover is "now" */
        nt_hold(e->x, e->y);
        nt_post(NT_WM_MOVE, e->x, e->y, 0);
        nt_post(NT_WM_RDOWN, e->x, e->y, 0);
        if (nt_battle_scr())
            g_nt_rdown = 1; /* a battlefield's card goes with the button: up at the next touch */
        else
            nt_post(NT_WM_RUP, e->x, e->y, 0);
        return;
    }
}

/* Every frame, on the game's thread, before the screen's Update. */
static void nt_frame(X86 *c) {
    if (g_nt_key >= 0 && --g_nt_key_frames <= 0) {
        host_touch_key(g_nt_key, 0);
        g_nt_key = -1;
    }
    /* What was posted last frame: the game has drawn a frame with the cursor
     * at the point (its pick, its cursor), so the click lands as a mouse's. */
    if (g_nt_nposts) {
        const uint32_t top = (c->r[R_ESP] - 0x800u) & ~15u;
        for (int i = 0; i < g_nt_nposts; ++i) {
            const uint32_t args[2] = {(uint32_t)g_nt_posts[i].y, g_nt_posts[i].wparam};
            homm5_guest_call_regs(c, NT_QUEUE, (uint32_t)g_nt_posts[i].type,
                                  (uint32_t)g_nt_posts[i].x, args, 2, top);
        }
        g_nt_nposts = 0;
    }
    while (g_nt_count) {
        const NtEvent e = g_nt_events[g_nt_head];
        g_nt_head = (g_nt_head + 1) % NT_EVENTS;
        --g_nt_count;
        nt_apply(c, &e);
        /* In a battle what follows a finger's touch waits a frame: the game
         * looks at the new point first, and a tap reads the cursor that shows
         * there (a button's, not the attack the last touch held on the field).
         * Only there: a menu does not ask every frame. */
        if (e.kind == RECOMP_TOUCH_EVENT_DOWN && nt_battle_scr())
            break;
    }
    /* The click has been read (the queue is empty) and a frame has passed:
     * its hover goes, and on the map or a battlefield the cursor with it. */
    if (g_nt_leave > 0 && !g_nt_nposts && nt_queue_length() == 0 && --g_nt_leave == 0) {
        g_nt_leave = -1;
        if (!g_nt_press && !g_nt_drag) {
            nt_leave_now(c);
            if (g_nt_release && !g_nt_preview)
                g_nt_hold = 0;
        }
    }
    /* A battle's turn moved on (the stack acted, another one's turn came):
     * what a finger showed for the last one goes, its preview with it. */
    if (g_nt_hold && !g_nt_drag) {
        const uint32_t scr = nt_battle_scr();
        if (scr && rd32(scr + NT_SCR_ACTIVE) != g_nt_turn) {
            g_nt_hold = g_nt_preview = g_nt_aim = g_nt_press = g_nt_shown = 0;
            nt_right_up();
            nt_leave_now(c);
            if (nt_trace())
                fprintf(stderr, "[touch-native] battle turn moved on: no cursor\n");
        }
    }
    if (g_nt_hold && g_nt_active) {
        wrf32(CURSOR_X, g_nt_x);
        wrf32(CURSOR_Y, g_nt_y);
    }
}

/* homm5_hover_timeout: the hover delay while native touch is in charge, in
 * RECOMP_TOUCH_HOVER_* terms (now while a finger rests, never otherwise), or
 * -1 when it is not. */
static int homm5_native_hover(void) {
    if (!nt_on())
        return -1;
    return g_nt_press || g_nt_shown ? RECOMP_TOUCH_HOVER_NOW : RECOMP_TOUCH_HOVER_NEVER;
}

/* native/tap_attack.c: whether a battlefield's frame should see nothing under
 * the cursor: native touch is in charge and no gesture holds the cursor. */
int homm5_native_parks(void) {
    return nt_on() && !g_nt_hold;
}

void NT_FN_EDGE_SCROLL(X86 *c);

/* 0070ded0 CCursor::GetEdgeScroll(ECX cursor; float out[2], int *cursor_id),
 * RET 8: no edge scrolls while native touch is in charge. A finger near the
 * edge of the picture means what is there, not "scroll"; the map follows a
 * drag instead. */
void homm5_edge_scroll(X86 *c) {
    if (!nt_on()) {
        NT_FN_EDGE_SCROLL(c);
        return;
    }
    const uint32_t esp = c->r[R_ESP], out = rd32(esp + 4u);
    wrf32(out, 0.0f);
    wrf32(out + 4u, 0.0f);
    c->r[R_EAX] = out; /* returns `out`: the callers read the vector through EAX */
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 12u;
    recomp_return(c);
}

void NT_FN_CURSOR_UPDATE(X86 *c);

/* 0070ddb0 CCursor::Update(ECX cursor; flag), RET 4: once a frame, between
 * the screen's Update and the draw, it puts the cursor where GetCursorPos
 * says, and on a touch that is far off the window. While a finger's point is
 * held it goes back there afterwards: the battle's cursor (the side a strike
 * comes from) and its damage hint are drawn at the point, not off the screen.
 * The rest of Update always runs: it lets go of an object the cursor carries
 * once that is gone (vt+0x38; a dragged artifact dropped into a slot), and
 * without it the next frame's drag work called the dead object. */
void homm5_cursor_update(X86 *c) {
    NT_FN_CURSOR_UPDATE(c);
    if (nt_on() && g_nt_hold && g_nt_active) {
        wrf32(CURSOR_X, g_nt_x);
        wrf32(CURSOR_Y, g_nt_y);
    }
}

#else

static void nt_frame(X86 *c) {
    (void)c;
}
static int homm5_native_hover(void) {
    return -1;
}
int homm5_native_parks(void) {
    return 0;
}

#endif /* NT_SUPPORTED */
