/* native/net_guard.c - bound the game's network string copies against a
 * hostile ubi-gs server or peer (README "Playing online safely").
 *
 * The GS protocol parser (GameSpy clDataList) reads one list element into a
 * caller's buffer with an unbounded byte copy: the base game's FUN_004432d0,
 *   do { c = *src++; *dst++ = c; } while (c != 0);
 * with no length from the message. The player-info deserializer (0042a5e0,
 * reached from the PLAYERINFO message handler 0042a990) copies seven server
 * strings this way into stack buffers of 12/16/36/36/68/132/132 bytes, so a
 * 12-character first field overflows the first buffer and smashes the frame
 * (past the stack cookie). ubi-gs caps these fields server-side now, but a
 * malicious server, or a peer that speaks the protocol, bypasses that.
 *
 * Beside the unbounded reader the game has a BOUNDED one (00443390): the same
 * list extraction, then strncpy(dst, src, size) with a forced terminator.
 * This replacement stands in for the unbounded reader and forwards every read
 * to the bounded one with the destination's size. An over-long field is cut
 * to fit, terminated inside its buffer, and the message goes on with the
 * shortened text (logged once per site); an absent field still fails as
 * before.
 *
 * The reader writes raw bytes through a char *, so it can never fill a string
 * that grows: each of its 52 calls per game copies into a fixed stack buffer,
 * and all 52 are listed below with that buffer's size. A call from anywhere
 * else (one we both missed, or code that is not the game's) gets a small
 * bound, GS_UNLISTED_SIZE, and is logged once per return address, so it
 * fails safe instead of copying unbounded.
 *
 * A read's site is told apart by its return address on the guest stack: every
 * CALL to the reader has its own. The sizes are the deserializers' buffers,
 * read from the listings and the translation; they are the same in all three
 * games (the three executables are one code base). games/<id>/native/
 * addresses.h gives the expansion's reader and call-site addresses; the base
 * game's are below.
 *
 * RECOMP_HOMM5_NET_GUARD=0 turns it off for a run (the raw copy, to reproduce
 * the overflow); RECOMP_TRACE_NET=1 logs every truncation, not just the first
 * per site. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "x86.h"

/* RECOMP_OVERRIDE_HEADER declares the replacement defined here;
 * RECOMP_ADDRESS_HEADER carries an expansion's addresses (the base game's are
 * the defaults below). The idiom is native/touch.c's. */
#ifdef RECOMP_OVERRIDE_HEADER
#include RECOMP_OVERRIDE_HEADER
#endif
#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif

/* The game's two clDataList string readers: the unbounded one this file
 * stands in for (its translated body, still emitted under its lower-case
 * name), and the bounded one it forwards to. Both are __thiscall with the
 * list in ECX; the unbounded takes (dst, index) and returns on RET 0x8, the
 * bounded (dst, index, size) on RET 0xc. */
#ifndef HOMM5_GS_STR_ORIG
#define HOMM5_GS_STR_ORIG fn_004432d0
#define HOMM5_GS_STR_BOUNDED fn_00443390
#endif

/* The reads: a call site's return address and the byte size of the fixed
 * buffer it copies into. An expansion defines its own list in addresses.h.
 * These are all 52 calls of the base game's reader, by the deserializer that
 * makes them (the message named by the type it parses, else by its id); each
 * size is the room from the buffer to the next local or the stack cookie, or
 * the length the caller zeroes when that is less.
 *   PLAYERINFO 0042a5e0             12/16/36/36/68/132/132
 *   GROUP_INFO member loop 004242d0 36/132/132
 *   MEMBER_JOIN 00424f00            36/132/132
 *   proxy JOINWAITMODULE 0043f450   16/132
 *   PROXY_HANDLER sub 1 00427190    36/36/132 (the module's proxy list)
 *   GAME_STARTED 004237e0           132/132 (host ip, alt ip)
 *   LOBBY_MSG sub 3 00422230        132
 *   UPDATEFRIEND 00428c60           132 (optional trailing string; its own
 *                                        absent-field fallback writes 132)
 *   persistantdata reply 004303d0   36
 *   id readers 00425f20 / 00423390 (also entered as 00425790) / 00425940
 *     / 004260b0                    16/16/36/16
 *   room info 0041fb40, fields 0x10-0x13 (the earlier fields go through the
 *     bounded reader already), into GROUP_INFO's 004242d0 frame and, through
 *     0041ffc0, 00423fa0's (message 0x36)
 *                                   36/36/128/128 (132 room for each of the
 *                                        last two; 128 keeps a margin)
 *   messages 0x17 / 0x18: 00421e30 / 00421fa0, into 00424e60 / 00424dc0
 *                                   129 (the caller zeroes 129)
 *   message 0x0c: 00422500 (two reads), into 00424d40       36
 *   message 0x3b: 004227d0, into 004253d0                   16/129/129
 *   messages 0x1b / 0x0a (loop) / 0x24 / 0x28: 004228d0 / 00422a30 /
 *     00423ab0 / 00423c10, into 00425350 / 00425480 / 00425b70 / 00425bf0
 *                                   16 each
 *   message 0x40 list loop 00425c70 (two reads)              16
 *   0xc8 / 0xc9 (ladderquery) / 0x67 (clanservice): 004276d0 / 00427d60 /
 *     00427f40                      36/33/36 (33: the caller zeroes 33)
 *   message 0x08: 00429650, into 00429bc0                   16/132
 *   message 0x98: 00429760, into 00429c50                   16/36/1024 */
#ifndef HOMM5_GS_STR_SITES
#define HOMM5_GS_STR_SITES                                                                         \
    {0x0042a6a8u, 12}, {0x0042a6bcu, 16}, {0x0042a6d0u, 36}, {0x0042a6e4u, 36}, {0x0042a6f8u, 68}, \
        {0x0042a70cu, 132}, {0x0042a720u, 132}, {0x00424a3cu, 36}, {0x00424a7fu, 132},             \
        {0x00424a9au, 132}, {0x00425020u, 36}, {0x0042505fu, 132}, {0x00425076u, 132},             \
        {0x0043f538u, 16}, {0x0043f54eu, 132}, {0x004272fau, 36}, {0x00427374u, 36},               \
        {0x004273ffu, 132}, {0x004238feu, 132}, {0x00423936u, 132}, {0x0042237au, 132},            \
        {0x00428d6fu, 132}, {0x004304f4u, 36}, {0x00425fcau, 16}, {0x004234a6u, 16},               \
        {0x00425a70u, 36}, {0x00426187u, 16}, {0x0041fcb1u, 36}, {0x0041fcc7u, 36},                \
        {0x0041fcddu, 128}, {0x0041fcf3u, 128}, {0x00421f6du, 129}, {0x004220ddu, 129},            \
        {0x004225fcu, 36}, {0x0042263du, 36}, {0x00422899u, 16}, {0x004228adu, 129},               \
        {0x004228bdu, 129}, {0x004229dfu, 16}, {0x00422b6fu, 16}, {0x00423befu, 16},               \
        {0x00423d4fu, 16}, {0x00425d88u, 16}, {0x00425db8u, 16}, {0x0042778au, 36},                \
        {0x00427e47u, 33}, {0x00428020u, 36}, {0x00429709u, 16}, {0x00429742u, 132},               \
        {0x0042981fu, 16}, {0x00429833u, 36}, {0x0042985au, 1024},
#endif

/* The bound for a call that is not listed above. None of the game's calls
 * reaches it (all 52 are listed), so it only has to fail safe: 16 is the
 * smallest buffer the reader fills but one (PLAYERINFO's 12-byte nickname,
 * which is listed), and no call depends on getting more from it. */
#ifndef GS_UNLISTED_SIZE
#define GS_UNLISTED_SIZE 16u
#endif

void HOMM5_GS_STR_ORIG(X86 *c);
void HOMM5_GS_STR_BOUNDED(X86 *c);

struct gs_site {
    uint32_t ret;  /* the return address that identifies the call site */
    uint16_t size; /* the destination buffer's size in bytes */
};
static const struct gs_site gs_sites[] = {HOMM5_GS_STR_SITES};
/* One "already logged" flag per site, so a hostile peer shows in the log once
 * without flooding it. */
static uint8_t gs_logged[sizeof gs_sites / sizeof gs_sites[0]];

/* Unlisted return addresses seen so far, each named in the log once, when
 * first seen, with its own "cut already logged" flag. Past the table's end the
 * extra addresses share the last flag and are no longer named. */
enum { GS_UNLISTED_MAX = 16 };
static uint32_t gs_unlisted[GS_UNLISTED_MAX];
static uint8_t gs_unlisted_logged[GS_UNLISTED_MAX];
static unsigned gs_unlisted_n;

static uint8_t *gs_unlisted_flag(uint32_t ret) {
    for (unsigned i = 0; i < gs_unlisted_n; ++i)
        if (gs_unlisted[i] == ret)
            return &gs_unlisted_logged[i];
    if (gs_unlisted_n == GS_UNLISTED_MAX)
        return &gs_unlisted_logged[GS_UNLISTED_MAX - 1];
    gs_unlisted[gs_unlisted_n] = ret;
    fprintf(stderr,
            "[net_guard] GS string read from unlisted call site %08x, bounded to %u bytes\n", ret,
            GS_UNLISTED_SIZE);
    return &gs_unlisted_logged[gs_unlisted_n++];
}

static int net_guard_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_HOMM5_NET_GUARD");
        on = v && *v ? v[0] != '0' : 1;
    }
    return on;
}

static int trace_net(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_TRACE_NET");
        on = v && *v ? v[0] != '0' : 0;
    }
    return on;
}

/* The bounded reader forwards a server string into a fixed buffer, truncating
 * an over-long one instead of running off the end of the stack frame. */
void homm5_gs_str_read(X86 *c) {
    const uint32_t esp = c->r[R_ESP];
    const uint32_t ret = rd32(esp);

    if (!net_guard_on()) {
        HOMM5_GS_STR_ORIG(c); /* RECOMP_HOMM5_NET_GUARD=0: the raw copy */
        return;
    }
    uint32_t size = GS_UNLISTED_SIZE;
    uint8_t *logged = NULL;
    for (size_t i = 0; i < sizeof gs_sites / sizeof gs_sites[0]; ++i)
        if (gs_sites[i].ret == ret) {
            size = gs_sites[i].size;
            logged = &gs_logged[i];
            break;
        }
    if (!logged)
        logged = gs_unlisted_flag(ret); /* a call we missed: the small bound */

    /* Forward to the bounded reader. Our frame is [ret, dst, index]; it wants
     * [ret, dst, index, size], so lay that one dword lower: the bounded
     * reader's RET 0xc then lands exactly where the unbounded reader's RET 0x8
     * would (both leave ESP at entry + 12 and jump to ret). The list stays in
     * ECX for both. */
    const uint32_t dst = rd32(esp + 4);
    const uint32_t index = rd32(esp + 8);
    const uint32_t nsp = esp - 4;
    wr32(nsp + 0x0, ret);
    wr32(nsp + 0x4, dst);
    wr32(nsp + 0x8, index);
    wr32(nsp + 0xc, size);
    c->r[R_ESP] = nsp;
    c->ret_expect = ret;
    /* Clear the destination first, so an absent field reads back empty and a
     * field that comes back exactly size - 1 characters long is the one that
     * was cut (below). The original reader leaves the destination untouched
     * when the field is absent; the difference does not reach the game, since
     * the callers drop the message when a read fails. */
    if (dst)
        wr8(dst, 0);
    HOMM5_GS_STR_BOUNDED(c);

    /* The result is a bool in AL (SETcc AL); EAX's upper bytes are left over.
     * The bounded reader answers 0 both for an absent field and for one that
     * did not fit. An absent field stays a failure, as it was. A field that
     * did not fit was cut to size - 1 characters and terminated inside the
     * buffer; report it read, as the unbounded reader would have, so the
     * message goes on with the shortened text. Dropping it instead leaves the
     * game waiting forever (a dropped PLAYERINFO hangs the login at "Logging
     * in..."). Normal play keeps every field short, so a cut is a hostile or
     * broken message: log the first per site (every one under
     * RECOMP_TRACE_NET). */
    if ((c->r[R_EAX] & 0xffu) != 0 || !dst)
        return;
    uint32_t len = 0;
    while (len < size && rd8(dst + len) != 0)
        ++len;
    if (len + 1 != size)
        return; /* absent: the game's own failure path */
    c->r[R_EAX] = (c->r[R_EAX] & 0xffffff00u) | 1u;
    if (trace_net() || !*logged) {
        *logged = 1;
        fprintf(stderr, "[net_guard] over-long GS string at %08x cut to %u bytes\n", ret, size);
    }
}
