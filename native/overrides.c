/* overrides.c - see overrides.h. Each replacement leaves the guest exactly as
 * the translated original does: registers, flags, x87 stack and status word,
 * and the stack slot it used. */
#include <stdio.h>
#include <stdlib.h>

#include "x86.h"
#include "platform/os.h"
#include "funcs.h"
#include "native_check.h"

#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif
#ifndef HOMM5_FLOAT_MAX_ORIG
#define HOMM5_FLOAT_MAX_ORIG fn_00614540
#define HOMM5_FLOAT_MIN_ORIG fn_00614570
#define HOMM5_AABB_ADD_ORIG fn_0066e070
#endif

/* 00614540 / 00614570 (Tribes of the East: 0050e5b0 / 0050e5e0, byte for
 * byte): __stdcall float max(a, b) and min(a, b) on the x87.
 *   PUSH ECX; FLD x; FCOMP y; FNSTSW AX; ECX = second; SHL EAX,23; SAR EAX,31
 *   (C0 as a mask); ECX &= mask; EAX = ~mask & first | ECX; [ESP] = EAX;
 *   FLD [ESP]; POP ECX; RET 8
 * max compares a with b and returns b when a < b (or unordered), min compares
 * b with a and returns b when b < a (or unordered). They are called millions
 * of times a frame by the particle and effect code. The POP leaves ECX the
 * result too. RECOMP_HOMM5_NATIVE_FMINMAX=0 the originals, c check, a
 * alternate (native_check.h). */
static void h5_float_compute(X86 *c, int min) {
    const uint32_t esp = c->r[4];
    const uint32_t a = rd32(esp + 4u), b = rd32(esp + 8u);
    const double fa = (double)rdf32(esp + 4u), fb = (double)rdf32(esp + 8u);
    if (min)
        fcom(c, fb, fa);
    else
        fcom(c, fa, fb);
    const uint32_t mask = (c->fpu_sw & 0x0100u) ? 0xffffffffu : 0u;
    const uint32_t r = (b & mask) | (a & ~mask);
    wr32(esp - 4u, r);
    c->r[0] = r;
    c->r[1] = r;
    c->eflags_cf = 0;
    c->eflags_of = 0;
    c->eflags_zf = (r == 0);
    c->eflags_sf = (r >> 31) & 1u;
    c->eflags_pf = parity8(r);
    fpush(c, (double)rdf32(esp - 4u));
    c->eip = rd32(esp);
    c->r[4] = esp + 12u;
}
static void h5_float_select(X86 *c, int min) {
    static int mode = -1, shown = -1;
    const int m = nc_mode(&mode, &shown, "RECOMP_HOMM5_NATIVE_FMINMAX", "fminmax", 1);
    if (m == NC_ORIGINAL) {
        (min ? HOMM5_FLOAT_MIN_ORIG : HOMM5_FLOAT_MAX_ORIG)(c);
        return;
    }
    const uint32_t ret_exp = c->ret_expect, esp = c->r[4];
    if (m == NC_CHECK) {
        static NcCheck k = {"fminmax", 0, 0, 0};
        if (rd32(esp) != ret_exp) { /* the original would run on past its RET */
            ++k.skipped;
            (min ? HOMM5_FLOAT_MIN_ORIG : HOMM5_FLOAT_MAX_ORIG)(c);
            return;
        }
        const uint32_t slot0 = rd32(esp - 4u);
        X86 n = *c;
        (min ? HOMM5_FLOAT_MIN_ORIG : HOMM5_FLOAT_MAX_ORIG)(c);
        const uint32_t slot_o = rd32(esp - 4u);
        wr32(esp - 4u, slot0);
        h5_float_compute(&n, min);
        const uint32_t slot_n = rd32(esp - 4u);
        wr32(esp - 4u, slot_o);
        char why[160];
        int bad = nc_state_differs(c, &n, why, sizeof why);
        if (!bad && slot_n != slot_o)
            bad = 1, snprintf(why, sizeof why, "stack slot %08x native %08x", slot_o, slot_n);
        nc_report(&k, bad, min ? "min" : "max", why);
        return;
    }
    h5_float_compute(c, min);
    if (c->eip != ret_exp)
        recomp_return(c);
}
void h5_float_max(X86 *c) {
    h5_float_select(c, 0);
}
void h5_float_min(X86 *c) {
    h5_float_select(c, 1);
}

#ifdef HOMM5_AABB_ADD_ORIG
/* 0066e070: grow a box {min x, y, z, max x, y, z} to hold a point (thiscall:
 * ECX the box, the point on the stack, RET 4). It calls the float min/max
 * above six times, and 0066bc50 runs it for every vertex of the animated
 * meshes it bounds each frame: about 9% of the adventure map's CPU time at
 * 3440x1440 (design notes ultrawide-plan.md). The guest leaves EAX the last result,
 * ECX the box, the x87 stack as it was and its status word from the last
 * compare; so does this. RECOMP_HOMM5_NATIVE_AABB=0 runs the original. */
static uint32_t h5_pick(uint32_t cur, uint32_t pt, int max) {
    const float fc = rdf32(cur), fp = rdf32(pt);
    /* min takes the point when it is below (or unordered), max when the box is below. */
    const int take = max ? !(fc >= fp) : !(fp >= fc);
    const uint32_t r = rd32(take ? pt : cur);
    wr32(cur, r);
    return r;
}
void h5_aabb_add(X86 *c) {
    /* RECOMP_HOMM5_NATIVE_AABB=ab alternates the original and this every 10 s
     * and logs each switch, so one run on one view measures both. */
    static int mode = -1; /* 0 original, 1 native, 2 alternate */
    if (mode < 0) {
        const char *v = getenv("RECOMP_HOMM5_NATIVE_AABB");
        mode = !v || !*v ? 1 : v[0] == '0' ? 0 : v[0] == 'a' ? 2 : 1;
    }
    int native = mode;
    if (mode == 2) {
        static int shown = -1;
        native = (int)((os_monotonic_ns() / 10000000000ull) & 1);
        if (native != shown) {
            shown = native;
            fprintf(stderr, "[aabb] %s\n", native ? "native" : "original");
        }
    }
    if (!native) {
        HOMM5_AABB_ADD_ORIG(c);
        return;
    }
    const uint32_t esp = c->r[4], box = c->r[1], pt = rd32(esp + 4u);
    h5_pick(box + 0u, pt + 0u, 0);
    h5_pick(box + 4u, pt + 4u, 0);
    h5_pick(box + 8u, pt + 8u, 0);
    h5_pick(box + 12u, pt + 0u, 1);
    h5_pick(box + 16u, pt + 4u, 1);
    const double before = (double)rdf32(box + 20u), point = (double)rdf32(pt + 8u);
    const uint32_t r = h5_pick(box + 20u, pt + 8u, 1);
    fcom(c, before, point);
    c->r[0] = r;
    c->eflags_cf = 0;
    c->eflags_of = 0;
    c->eflags_zf = (r == 0);
    c->eflags_sf = (r >> 31) & 1u;
    c->eflags_pf = parity8(r);
    c->eip = rd32(esp);
    c->r[4] = esp + 8u;
    recomp_return(c);
}
#endif
