/* native/tap_attack.c - the battle's "strike from a cell" mode (the
 * launcher's "Touch-friendly battle"), on while RECOMP_TOUCH_BATTLE_CELLS=1:
 * the host sets it from host.touch/battle_cells, on by default on an iPhone or
 * iPad and off elsewhere.
 * Modelled on Might & Magic Dynasty's battle: a click or tap on an enemy the
 * active stack can reach in melee only selects it, and the cells the stack can
 * strike it from light up; a click on one of them walks there and strikes, a
 * click on the enemy again strikes from the cheapest of them, a click anywhere
 * else cancels without acting. Shift-click on a lit cell (or, on a touch
 * screen, a finger held on it and lifted, then a tap) only walks there.
 *
 * How the game does a melee attack (base game, notes in the user's
 * research notes tap-attack): every frame the combat screen's Update picks what is
 * under the cursor (0074de10, into P = screen+0x444) and lets the action
 * function 0074e470(S = screen+0x43c, P) build the command the next left
 * button release sends (S+0x50). For an enemy, the side to strike from is the
 * candidate cell (00c951d0: 8, 12 or 16 around it, counterclockwise from +X)
 * whose sector holds the angle P+4, the cursor's direction from the enemy's
 * centre. While a cell is selected, the wrapper of 0074e470 below rewrites
 * the pick to "that enemy, at the centre angle of that cell's sector": the
 * game itself then checks the path, builds CMoveAttackCreatureCmd, turns the
 * sword cursor and writes the damage hint, and the release on the cell sends
 * it. The release handler (0074bf70) is wrapped for the select and cancel
 * clicks, which it swallows, and the grid's state pass (0080bac0) for the
 * lit cells, drawn with the game's own selection materials.
 *
 * The candidate cells are the game's: each of the 00c951d0 cells for which a
 * trial 00cc6ce0 builds a command (the only check the game has; it finds the
 * path within this turn's movement). The command's path gives the cost.
 *
 * Also here, on for any touch (RECOMP_HOMM5_PARK_CURSOR=0 turns it off): the
 * cursor a finger left on the field is parked when a new turn starts. A touch
 * screen shows no cursor, but the game still hovers whatever stands where the
 * last tap was: a stack that walked onto that cell got its move range shaded,
 * its damage hint and, in the mode above, a tap's meaning. While the cursor
 * has not moved since the last touch put it there, the new turn's frames skip
 * the pick and the action (0074de10, 0074e470), leaving the state Update
 * resets every frame: nothing under the cursor, nothing to send. The next
 * touch (or a mouse that moves) ends it. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x86.h"

#ifdef RECOMP_OVERRIDE_HEADER
#include RECOMP_OVERRIDE_HEADER
#endif
#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif

/* The base game's addresses; Hammers of Fate's and Tribes of the East's are
 * in their addresses.h. A game whose addresses.h names none builds only the
 * stubs at the end. */
#ifndef RECOMP_ADDRESS_HEADER
#define HOMM5_TAP_SCREEN_VTABLE 0x00fa78dc /* NUI::CCombatArenaScreen, the full object */
#define HOMM5_TAP_FN_ACTION fn_0074e470    /* the action for what is under the cursor */
#define HOMM5_TAP_FN_CLICK fn_0074bf70     /* win_left_button_up */
#define HOMM5_TAP_FN_PICK fn_0074de10      /* what is under the cursor, into P, RET 8 */
#define HOMM5_TAP_CURSOR_X 0x0114da0c      /* NUI::CCursor's position, float client pixels */
#define HOMM5_TAP_CURSOR_Y 0x0114da10
#define HOMM5_TAP_FN_GRID fn_0080bac0      /* the grid cells' states, every update */
#define HOMM5_TAP_SET_CELL 0x0080c390      /* thiscall(grid, x, y, size, state), RET 0x10 */
#define HOMM5_TAP_ATTACK_CELLS 0x00c951d0  /* ECX vector out, EDX attacker, push target */
#define HOMM5_TAP_MOVE_ATTACK 0x00cc6ce0   /* builds CMoveAttackCreatureCmd, RET 0x28 */
#define HOMM5_TAP_MOVE_ATTACK_VTABLE 0x01019844
#define HOMM5_TAP_RELEASE 0x005eaca0 /* an object whose last reference went */
#define HOMM5_TAP_FREE 0x005e40c0    /* operator delete (cdecl) */
#endif

#if defined(HOMM5_TAP_SCREEN_VTABLE)
#define TAP_SUPPORTED 1
#else
#define TAP_SUPPORTED 0
#endif

enum {
    TAP_IDLE = 0,
    TAP_SELECT = 1, /* an enemy chosen: its strike cells lit */
    TAP_MOVE = 2,   /* a lit cell chosen to walk to without striking */
};

/* RECOMP_TOUCH_* (runtime/native_seam.h) */
enum {
    TOUCH_BATTLE_DIRECT = 128,
    TOUCH_GESTURE_HOLD = 1,
};

static int tap_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_TOUCH_BATTLE_CELLS");
        on = v && *v == '1';
    }
    return on;
}

static int g_state;              /* TAP_*; the host thread reads it */
static volatile int g_hold;      /* a finger rested on the field and lifted */

/* Where the last touch put the cursor (native/touch.c, under the baton), and
 * how many touches have placed it. */
static int g_placed;
static float g_placed_x, g_placed_y;
static uint32_t g_placed_count;

static int park_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_HOMM5_PARK_CURSOR");
        on = !v || *v != '0';
    }
    return on;
}

#if TAP_SUPPORTED

/* The combat screen's fields and the interfaces' slots that the expansions
 * moved: the base game's here, theirs in their addresses.h. */
#ifndef HOMM5_TAP_SCR_COMBAT
#define HOMM5_TAP_SCR_COMBAT 0x3d8
#define HOMM5_TAP_SCR_ACTIVE 0x3e4
#define HOMM5_TAP_SCR_PHASE 0x430
#define HOMM5_TAP_SCR_S 0x43c
#define HOMM5_TAP_SCR_P 0x444
#define HOMM5_TAP_SCR_GRID 0x4f0
#define HOMM5_TAP_SCR_SHIFT 0x4c
#define HOMM5_TAP_SCR_CTRL 0x54
#define HOMM5_TAP_COMBAT_CELL_TO_WORLD 0x78
#define HOMM5_TAP_UNIT_CELL 0x3c
#define HOMM5_TAP_UNIT_SIZE 0x40
#endif

enum {
    /* The combat screen (the full object, ECX of the wrapped functions). */
    SCR_COMBAT = HOMM5_TAP_SCR_COMBAT, /* NWorld::ICombat */
    SCR_ACTIVE = HOMM5_TAP_SCR_ACTIVE, /* the stack whose turn it is; cleared as its command goes */
    SCR_PHASE = HOMM5_TAP_SCR_PHASE,   /* 0x1000 while the armies are placed */
    SCR_S = HOMM5_TAP_SCR_S,           /* the action state S */
    SCR_P = HOMM5_TAP_SCR_P,           /* the pick P */
    SCR_GRID = HOMM5_TAP_SCR_GRID,     /* the grid's cells, ECX of 0080bac0 */
    SCR_SHIFT = HOMM5_TAP_SCR_SHIFT,   /* key bindings: a byte behind the pointer at +4 */
    SCR_CTRL = HOMM5_TAP_SCR_CTRL,
    /* S */
    S_KIND = 0x30,       /* 2 move, 3 melee, 4/5 shoot ... */
    S_ATTACKER = 0x34,
    S_TARGET = 0x38,
    S_FROM_X = 0x40,     /* melee: the cell the attacker strikes from */
    S_FROM_Y = 0x44,
    S_COMMAND = 0x50,
    /* P */
    P_UNIT = 0x00,       /* ICombatBase under the cursor, holding a reference */
    P_ANGLE = 0x04,      /* int degrees, world XY, counterclockwise from +X */
    P_OBSTACLE = 0x08,
    P_BUILDING = 0x0c,
    P_CELL_X = 0x10,
    P_CELL_Y = 0x14,
    P_BAR = 0x24,        /* byte: picked on the turn order bar, not the field */
    /* ICombat vtable */
    COMBAT_CELL_TO_WORLD = HOMM5_TAP_COMBAT_CELL_TO_WORLD, /* (float out[3], const int cell[2]): the centre, on the ground */
    /* ICombatBase vtable */
    UNIT_CELL = HOMM5_TAP_UNIT_CELL, /* (int out[2]); a 2x2 stack's is its max-x, max-y corner */
    UNIT_SIZE = HOMM5_TAP_UNIT_SIZE, /* 1 or 2 */
    /* CMoveAttackCreatureCmd */
    CMD_REFS = 0x08,
    CMD_TICK = 0x0c,
    CMD_PATH = 0x14,     /* vector of float x, y, z: the cells' centres */
    /* grid cell states (0080bee0's materials) */
    CELL_TARGET = 2,     /* gold */
    CELL_CHOSEN = 3,     /* gold */
    CELL_AIMABLE = 5,    /* blue */
    MAX_CANDIDATES = 16,
};

typedef struct {
    int32_t x, y;  /* the attacker's cell (its anchor) when it strikes from here */
    int index;     /* in 00c951d0's list, which gives the sector */
    int cost;      /* the path's: 2 a straight step, 3 a diagonal one */
} Candidate;

static uint32_t g_scr, g_att, g_target, g_tick;
static int g_att_size, g_target_size;
static int32_t g_att_x, g_att_y, g_target_x, g_target_y;
static Candidate g_cand[MAX_CANDIDATES];
static int g_ncand, g_n, g_best;
static int g_hover = -1;    /* the candidate this frame strikes from, or -1 */
static int g_armed;         /* this frame built the strike we want */
static int g_move = -1;     /* TAP_MOVE: the candidate to walk to */
static int g_parked;        /* this turn's frames see nothing under the cursor */
static uint32_t g_turn;     /* the stack whose turn the parking last looked at */
static uint32_t g_turn_placed; /* g_placed_count when that turn started */

void HOMM5_TAP_FN_ACTION(X86 *c);
void HOMM5_TAP_FN_CLICK(X86 *c);
void HOMM5_TAP_FN_PICK(X86 *c);
void HOMM5_TAP_FN_GRID(X86 *c);
uint32_t homm5_guest_call_regs(X86 *c, uint32_t fn, uint32_t ecx, uint32_t edx,
                               const uint32_t *args, int nargs, uint32_t stack_top);

static int trace(void) {
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_TRACE_TAP_ATTACK") != NULL;
    return on;
}

/* Guest scratch under the current ESP: data above `top`, the calls' stack below. */
static uint32_t scratch(X86 *c) {
    return (c->r[R_ESP] - 0x400u) & ~15u;
}

static uint32_t call(X86 *c, uint32_t fn, uint32_t ecx, uint32_t edx, const uint32_t *args,
                     int nargs, uint32_t top) {
    return homm5_guest_call_regs(c, fn, ecx, edx, args, nargs, top);
}

/* A virtual base's control words: alive while non-negative, then the references. */
static uint32_t object_base(uint32_t p) {
    return p + 4u + rd32(rd32(p + 4u) + 4u);
}

static int alive(uint32_t p) {
    return p && (int32_t)rd32(object_base(p) + 4u) >= 0;
}

static void ref(uint32_t p) {
    const uint32_t refs = object_base(p) + 8u;
    wr32(refs, rd32(refs) + 1u);
}

static void release(X86 *c, uint32_t p) {
    const uint32_t base = object_base(p), refs = base + 8u;
    wr32(refs, rd32(refs) - 1u);
    if (!rd32(refs))
        call(c, HOMM5_TAP_RELEASE, base, 0, NULL, 0, scratch(c));
}

static int unit_size(X86 *c, uint32_t u) {
    return (int)call(c, rd32(rd32(u) + UNIT_SIZE), u, 0, NULL, 0, scratch(c));
}

static void unit_cell(X86 *c, uint32_t u, int32_t *x, int32_t *y) {
    const uint32_t top = scratch(c), out = top;
    wr32(out, 0);
    wr32(out + 4u, 0);
    const uint32_t args[1] = {out};
    call(c, rd32(rd32(u) + UNIT_CELL), u, 0, args, 1, top);
    *x = (int32_t)rd32(out);
    *y = (int32_t)rd32(out + 4u);
}

static int key_held(uint32_t scr, uint32_t binding) {
    const uint32_t p = rd32(scr + binding + 4u);
    return p && rd8(p);
}

/* The centre of candidate i's sector in 007690b0's split of the circle. */
static int32_t sector_angle(int i, int n) {
    const float step = 360.0f / (float)n;
    const float v = (float)i * step + (((n / 4) & 1) ? step * 0.5f : 0.0f);
    return (int32_t)floorf(v + 0.5f);
}

static void reset(X86 *c) {
    if (g_target)
        release(c, g_target);
    if (trace() && g_state != TAP_IDLE)
        fprintf(stderr, "[tap-attack] idle\n");
    g_target = 0;
    g_att = 0;
    g_ncand = 0;
    g_hover = -1;
    g_armed = 0;
    g_move = -1;
    __atomic_store_n(&g_state, TAP_IDLE, __ATOMIC_RELEASE);
}

/* Cells between two world points (2 units a cell) as the game counts a move:
 * 2 a straight step, 3 a diagonal one. */
static int octile(float dx, float dy) {
    const int ax = (int)floorf(fabsf(dx) * 0.5f + 0.5f), ay = (int)floorf(fabsf(dy) * 0.5f + 0.5f);
    const int lo = ax < ay ? ax : ay, hi = ax < ay ? ay : ax;
    return 2 * (hi - lo) + 3 * lo;
}

/* A command's path cost from the attacker's cell centre (sx, sy). A walker's
 * path lists each cell, a flier's only where it rises and lands, and a strike
 * from where it stands has the one cell, so the sum runs from the start. */
static int path_cost(uint32_t cmd, float sx, float sy) {
    const uint32_t begin = rd32(cmd + CMD_PATH), end = rd32(cmd + CMD_PATH + 4u);
    int cost = 0;
    float x = sx, y = sy;
    for (uint32_t p = begin; p + 12u <= end; p += 12u) {
        const float nx = rdf32(p), ny = rdf32(p + 4u);
        cost += octile(nx - x, ny - y);
        x = nx;
        y = ny;
    }
    return cost;
}

static int candidate_at(int32_t x, int32_t y) {
    for (int i = 0; i < g_ncand; ++i)
        if (g_cand[i].x == x && g_cand[i].y == y)
            return i;
    /* A 2x2 attacker: any cell of the square it would stand on, the cheapest. */
    int best = -1;
    if (g_att_size >= 2)
        for (int i = 0; i < g_ncand; ++i)
            if (x <= g_cand[i].x && x >= g_cand[i].x - 1 && y <= g_cand[i].y &&
                y >= g_cand[i].y - 1 && (best < 0 || g_cand[i].cost < g_cand[best].cost))
                best = i;
    return best;
}

/* The click chose an enemy the hover made a melee attack on: its strike cells. */
static int enter_select(X86 *c, uint32_t scr) {
    const uint32_t S = scr + SCR_S;
    const uint32_t att = rd32(S + S_ATTACKER), target = rd32(S + S_TARGET);
    const uint32_t ready = rd32(S + S_COMMAND);
    if (!alive(att) || !alive(target) || att == target || !ready ||
        rd32(ready) != HOMM5_TAP_MOVE_ATTACK_VTABLE)
        return 0;
    const uint32_t combat = rd32(scr + SCR_COMBAT);
    const uint32_t tick = rd32(ready + CMD_TICK);
    const uint32_t ctrl = key_held(scr, SCR_CTRL) ? 1u : 0u;
    const int32_t game_x = (int32_t)rd32(S + S_FROM_X), game_y = (int32_t)rd32(S + S_FROM_Y);

    const uint32_t top = scratch(c), vec = top, cell = top + 0x10u, world = top + 0x20u;
    /* Where the attacker stands, in the world: the paths' start. */
    unit_cell(c, att, &g_att_x, &g_att_y);
    wr32(cell, (uint32_t)g_att_x);
    wr32(cell + 4u, (uint32_t)g_att_y);
    {
        const uint32_t args[2] = {world, cell};
        call(c, rd32(rd32(combat) + COMBAT_CELL_TO_WORLD), combat, 0, args, 2, top);
    }
    const float sx = rdf32(world), sy = rdf32(world + 4u);
    wr32(vec, 0);
    wr32(vec + 4u, 0);
    wr32(vec + 8u, 0);
    {
        const uint32_t args[1] = {target};
        call(c, HOMM5_TAP_ATTACK_CELLS, vec, att, args, 1, top);
    }
    const uint32_t begin = rd32(vec), end = rd32(vec + 4u);
    const int n = begin && end > begin ? (int)((end - begin) / 8u) : 0;
    int found = 0, best = -1, game = -1;
    for (int i = 0; i < n && i < MAX_CANDIDATES; ++i) {
        const int32_t x = (int32_t)rd32(begin + 8u * (uint32_t)i);
        const int32_t y = (int32_t)rd32(begin + 8u * (uint32_t)i + 4u);
        wr32(cell, (uint32_t)x);
        wr32(cell + 4u, (uint32_t)y);
        /* tick, combat, attacker, target, prediction (none), from, ctrl, 0,
         * within this turn's move, no shared path search */
        const uint32_t args[10] = {tick, combat, att, target, 0, cell, ctrl, 0, 0, 0};
        const uint32_t cmd = call(c, HOMM5_TAP_MOVE_ATTACK, 0, 0, args, 10, top);
        if (!cmd)
            continue;
        Candidate *k = &g_cand[found];
        k->x = x;
        k->y = y;
        k->index = i;
        k->cost = path_cost(cmd, sx, sy);
        if (!rd32(cmd + CMD_REFS))
            call(c, HOMM5_TAP_RELEASE, cmd, 0, NULL, 0, top);
        if (x == game_x && y == game_y)
            game = found;
        /* the cheapest; on a tie the side the game chose for the click */
        if (best < 0 || k->cost < g_cand[best].cost ||
            (k->cost == g_cand[best].cost && found == game))
            best = found;
        ++found;
    }
    if (begin) {
        const uint32_t args[1] = {begin};
        call(c, HOMM5_TAP_FREE, 0, 0, args, 1, top);
    }
    if (!found)
        return 0;
    if (g_target)
        release(c, g_target);
    ref(target);
    g_scr = scr;
    g_att = att;
    g_target = target;
    g_tick = tick;
    g_ncand = found;
    g_n = n;
    g_best = best;
    g_hover = -1;
    g_armed = 0;
    g_move = -1;
    g_att_size = unit_size(c, att);
    g_target_size = unit_size(c, target);
    unit_cell(c, target, &g_target_x, &g_target_y);
    __atomic_store_n(&g_state, TAP_SELECT, __ATOMIC_RELEASE);
    if (trace()) {
        fprintf(stderr, "[tap-attack] select: target %08x at %d,%d (size %d), %d of %d cells:",
                target, g_target_x, g_target_y, g_target_size, found, n);
        for (int i = 0; i < found; ++i)
            fprintf(stderr, " %d,%d#%d/%d%s", g_cand[i].x, g_cand[i].y, g_cand[i].index,
                    g_cand[i].cost, i == best ? "*" : "");
        fprintf(stderr, "\n");
    }
    return 1;
}

/* Returns from a thiscall without running it: RET 4 * nargs. */
static void skip_call(X86 *c, uint32_t nargs) {
    const uint32_t esp = c->r[R_ESP];
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 4u + 4u * nargs;
    recomp_return(c);
}

/* Swallows a release: the handler's RET 4 without running it. */
static void skip_click(X86 *c) {
    skip_call(c, 1);
}

/* The cursor is where the last touch left it: no finger or mouse since. */
static int cursor_left_by_touch(void) {
    return g_placed && rdf32(HOMM5_TAP_CURSOR_X) == g_placed_x &&
           rdf32(HOMM5_TAP_CURSOR_Y) == g_placed_y;
}

/* Whether this frame of the battle sees nothing under the cursor. A turn
 * starts parked when the cursor is still where a touch left it; a new touch
 * or the pointer moving ends it. A touch writes the cursor and the place it
 * left it alike, so only its count tells a new one: where the game's own
 * refresh from GetCursorPos lands on the same pixel (an iPad, 2x), the
 * cursor stays where the touch put it and would have kept the turn parked. */
static int parked(uint32_t scr) {
    if (!park_enabled())
        return 0;
    const uint32_t active = rd32(scr + SCR_ACTIVE);
    const uint32_t placed = __atomic_load_n(&g_placed_count, __ATOMIC_ACQUIRE);
    if (active != g_turn) {
        g_turn = active;
        g_turn_placed = placed;
        g_parked = active && cursor_left_by_touch();
        if (trace() && g_parked)
            fprintf(stderr, "[tap-attack] new turn: cursor parked\n");
    } else if (g_parked && (placed != g_turn_placed || !cursor_left_by_touch())) {
        g_parked = 0;
        if (trace())
            fprintf(stderr, "[tap-attack] cursor unparked\n");
    }
    return g_parked;
}

/* 0074de10(ECX screen, P, POINT), RET 8: the pick, every frame of a turn. */
void homm5_tap_pick(X86 *c) {
    const uint32_t scr = c->r[R_ECX];
    if (rd32(scr) == HOMM5_TAP_SCREEN_VTABLE && rd32(scr + SCR_PHASE) != 0x1000u && parked(scr)) {
        skip_call(c, 2);
        return;
    }
    HOMM5_TAP_FN_PICK(c);
}

/* 0074e470(ECX screen, S, P), RET 8: the action for the pick, every frame. */
void homm5_tap_action(X86 *c) {
    const uint32_t scr = c->r[R_ECX], esp = c->r[R_ESP];
    if (rd32(scr) == HOMM5_TAP_SCREEN_VTABLE && g_parked && park_enabled()) {
        skip_call(c, 2); /* nothing picked: no move to a cell either */
        return;
    }
    if (!tap_enabled() || rd32(scr) != HOMM5_TAP_SCREEN_VTABLE) {
        HOMM5_TAP_FN_ACTION(c);
        return;
    }
    const uint32_t S = rd32(esp + 4u), P = rd32(esp + 8u);
    const int hold = __atomic_exchange_n(&g_hold, 0, __ATOMIC_ACQ_REL);
    if (g_state != TAP_IDLE &&
        (scr != g_scr || rd32(S + S_ATTACKER) != g_att || !alive(g_target)))
        reset(c); /* another turn, another battle, or the enemy is gone */
    g_armed = 0;
    int want = -1;
    if (g_state == TAP_SELECT && !key_held(scr, SCR_SHIFT) && !rd8(P + P_BAR) &&
        !rd32(P + P_OBSTACLE) && !rd32(P + P_BUILDING)) {
        const uint32_t unit = rd32(P + P_UNIT);
        if (unit == g_target)
            want = g_best;
        else if (!unit || unit == g_att)
            want = candidate_at((int32_t)rd32(P + P_CELL_X), (int32_t)rd32(P + P_CELL_Y));
        if (hold && want >= 0 && unit != g_target) {
            /* A finger rested on a lit cell: walk there, no strike. */
            g_move = want;
            want = -1;
            __atomic_store_n(&g_state, TAP_MOVE, __ATOMIC_RELEASE);
            if (trace())
                fprintf(stderr, "[tap-attack] move only to %d,%d\n", g_cand[g_move].x,
                        g_cand[g_move].y);
        }
    }
    g_hover = want;
    if (want >= 0) {
        const uint32_t was = rd32(P + P_UNIT);
        if (was != g_target) {
            ref(g_target);
            wr32(P + P_UNIT, g_target);
            if (was)
                release(c, was);
        }
        wr32(P + P_ANGLE, (uint32_t)sector_angle(g_cand[want].index, g_n));
    }
    HOMM5_TAP_FN_ACTION(c);
    if (want >= 0) {
        g_armed = rd32(S + S_KIND) == 3 && rd32(S + S_TARGET) == g_target &&
                  (int32_t)rd32(S + S_FROM_X) == g_cand[want].x &&
                  (int32_t)rd32(S + S_FROM_Y) == g_cand[want].y;
        if (trace() && !g_armed)
            fprintf(stderr, "[tap-attack] cell %d,%d: the game built kind %u from %d,%d\n",
                    g_cand[want].x, g_cand[want].y, rd32(S + S_KIND),
                    (int32_t)rd32(S + S_FROM_X), (int32_t)rd32(S + S_FROM_Y));
    }
}

/* The release as the game handles it. A command it sent ends the turn, so the
 * same stack acting again (good morale) starts a new one for the parking. */
static void click_orig(X86 *c, uint32_t scr) {
    HOMM5_TAP_FN_CLICK(c);
    if (rd32(scr) == HOMM5_TAP_SCREEN_VTABLE && !rd32(scr + SCR_ACTIVE))
        g_turn = 0;
}

/* 0074bf70(ECX screen, msg), RET 4: the left button's release on the field. */
void homm5_tap_click(X86 *c) {
    const uint32_t scr = c->r[R_ECX];
    if (!tap_enabled() || rd32(scr) != HOMM5_TAP_SCREEN_VTABLE ||
        rd32(scr + SCR_PHASE) == 0x1000u) {
        click_orig(c, scr);
        return;
    }
    const uint32_t S = scr + SCR_S, P = scr + SCR_P;
    const uint32_t kind = rd32(S + S_KIND);
    /* Tribes of the East also strikes a town's building in melee: the game's
     * own click does that, with no cell to choose. */
    const int melee = kind == 3 && !rd8(P + P_BAR) && !rd32(P + P_BUILDING);
    switch (g_state) {
    case TAP_IDLE:
        /* Shift or Ctrl: the game's own click (Ctrl makes a shooter strike). */
        if (melee && !key_held(scr, SCR_SHIFT) && !key_held(scr, SCR_CTRL) &&
            enter_select(c, scr)) {
            skip_click(c);
            return;
        }
        click_orig(c, scr);
        return;
    case TAP_SELECT: {
        if (g_armed) { /* the strike from the chosen cell is ready: send it */
            if (trace())
                fprintf(stderr, "[tap-attack] strike from %d,%d\n", g_cand[g_hover].x,
                        g_cand[g_hover].y);
            click_orig(c, scr);
            reset(c);
            return;
        }
        const int lit = candidate_at((int32_t)rd32(P + P_CELL_X), (int32_t)rd32(P + P_CELL_Y));
        if (kind == 2 && lit >= 0 && key_held(scr, SCR_SHIFT)) { /* walk only */
            click_orig(c, scr);
            reset(c);
            return;
        }
        const uint32_t other = rd32(S + S_TARGET);
        reset(c);
        if (melee && other && enter_select(c, scr)) { /* another enemy */
            skip_click(c);
            return;
        }
        skip_click(c); /* anywhere else: cancel, no action */
        return;
    }
    case TAP_MOVE: {
        /* the move the game built goes to that cell (S's, where a 2x2 stack's
         * anchor lands), or the cursor is on one of the cells it covers */
        const int ok = g_move >= 0 && kind == 2 &&
                       (((int32_t)rd32(S + S_FROM_X) == g_cand[g_move].x &&
                         (int32_t)rd32(S + S_FROM_Y) == g_cand[g_move].y) ||
                        candidate_at((int32_t)rd32(P + P_CELL_X),
                                     (int32_t)rd32(P + P_CELL_Y)) == g_move);
        reset(c);
        if (ok)
            click_orig(c, scr);
        else
            skip_click(c);
        return;
    }
    }
    click_orig(c, scr);
}

/* 0080bac0(ECX grid, combat, sceneA, sceneB, S), RET 0x10: every update sets
 * each cell's state anew; the chosen enemy and its strike cells go on top. */
void homm5_tap_grid(X86 *c) {
    const uint32_t grid = c->r[R_ECX];
    HOMM5_TAP_FN_GRID(c);
    const int state = g_state;
    if (state == TAP_IDLE || grid != g_scr + SCR_GRID || rd32(g_scr + SCR_ACTIVE) == 0)
        return; /* nothing chosen, or the command went and the turn is over */
    const uint32_t top = scratch(c);
    for (int i = 0; i < g_ncand; ++i) {
        if (state == TAP_MOVE && i != g_move)
            continue;
        const uint32_t s = i == g_hover || i == g_move ? CELL_CHOSEN : CELL_AIMABLE;
        const uint32_t args[4] = {(uint32_t)g_cand[i].x, (uint32_t)g_cand[i].y,
                                  (uint32_t)g_att_size, s};
        call(c, HOMM5_TAP_SET_CELL, grid, 0, args, 4, top);
    }
    const uint32_t args[4] = {(uint32_t)g_target_x, (uint32_t)g_target_y,
                              (uint32_t)g_target_size, CELL_TARGET};
    call(c, HOMM5_TAP_SET_CELL, grid, 0, args, 4, top);
}

#endif /* TAP_SUPPORTED */

/* For native/touch.c's recomp_touch_context, from the host thread: while an
 * enemy is chosen, or the cursor shows a melee attack (the sword cursors
 * 0x12..0x19) that a tap would choose one with, a battlefield tap is a click
 * at once rather than a preview the next tap confirms. */
int homm5_tap_context(uint32_t cursor) {
    if (!TAP_SUPPORTED || !tap_enabled())
        return 0;
    if (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE) != TAP_IDLE)
        return TOUCH_BATTLE_DIRECT;
    return cursor >= 0x12 && cursor <= 0x19 ? TOUCH_BATTLE_DIRECT : 0;
}

/* native/touch.c: a touch put the cursor at (x, y), client pixels. */
void homm5_tap_pointer_placed(float x, float y) {
    g_placed = 1;
    g_placed_x = x;
    g_placed_y = y;
    __atomic_add_fetch(&g_placed_count, 1u, __ATOMIC_RELEASE);
}

/* runtime/native_seam.h: a finger rested on the battlefield and lifted. The
 * next action frame turns it into "walk only" when it rests on a lit cell. */
void recomp_touch_gesture(int gesture) {
    if (gesture == TOUCH_GESTURE_HOLD && TAP_SUPPORTED &&
        __atomic_load_n(&g_state, __ATOMIC_ACQUIRE) == TAP_SELECT)
        __atomic_store_n(&g_hold, 1, __ATOMIC_RELEASE);
}
