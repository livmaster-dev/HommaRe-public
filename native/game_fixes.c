/* native/game_fixes.c - fixes for faults of the original game itself.
 *
 * The Arena (the map building that gives +2 Attack or +2 Defence once): the
 * game's own exe of July 2025 gives nothing. A hero's visit marks the building
 * as visited first and asks the question after, and the answer's command
 * (NWorld::CVisitArenaCmd) changes the stat only for a hero who has NOT visited
 * the building, so it never does. The question is asked only on the first
 * visit, and the box is gone after its OK, so here the command's "has visited"
 * check passes: the stat is changed once, as the building says. */
#include <stdint.h>

#include "x86.h"

#ifdef RECOMP_OVERRIDE_HEADER
#include RECOMP_OVERRIDE_HEADER
#endif

#if defined(HOMM5_ARENA_CMD_ORIG) && defined(HOMM5_HAS_VISITED_ORIG)

void HOMM5_ARENA_CMD_ORIG(X86 *c);
void HOMM5_HAS_VISITED_ORIG(X86 *c);

/* Inside the arena command; guest threads run one at a time. */
static int g_arena_cmd;

/* CVisitArenaCmd's run: fastcall ECX = the command; AL = done. */
void homm5_arena_cmd(X86 *c) {
    const int outer = g_arena_cmd;
    g_arena_cmd = 1;
    HOMM5_ARENA_CMD_ORIG(c);
    g_arena_cmd = outer;
}

/* A hero's "has visited the building": thiscall (hero; id), RET 4; AL. */
void homm5_hero_has_visited(X86 *c) {
    HOMM5_HAS_VISITED_ORIG(c);
    if (g_arena_cmd)
        c->r[R_EAX] &= ~0xffu;
}

#endif
