/* granny.c - native replacements for granny2.dll's hottest leaf functions.
 * Each one leaves the guest exactly as the translated original does - the
 * registers it changes, the stack slots it writes, the x87 stack as it found
 * it - and computes in the same precision: x87 registers are doubles in this
 * runtime, values stored to float memory are rounded to float.
 *
 * RECOMP_GRANNY_NATIVE=1 (default) runs these, 0 the originals, check runs
 * both on every call and logs the first differences (the original's results
 * stay), ab alternates every 10 s. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x86.h"
#include "platform/os.h"
#include "funcs.h"

#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif
#ifndef GRANNY_SPLINE_QUAT_ORIG
/* The base game: its granny2.dll (Hammers of Fate's too) holds these four
 * functions and their constants at the same addresses as Tribes of the
 * East's own build. */
void fn_50011890(X86 *c);
#define GRANNY_SPLINE_QUAT_ORIG fn_50011890
#define GRANNY_SPLINE_ONE 0x5004c0d0u
#define GRANNY_SPLINE_NORM_A 0x5004c478u
#define GRANNY_SPLINE_NORM_B 0x5004c1d0u
void fn_500127b0(X86 *c);
#define GRANNY_KNOT_SEARCH_ORIG fn_500127b0
void fn_50028950(X86 *c);
#define GRANNY_POSE_ACCUMULATE_ORIG fn_50028950
#define GRANNY_POSE_DOT_MIN 0x5004c0ccu
void fn_50028b90(X86 *c);
#define GRANNY_POSE_FINISH_ORIG fn_50028b90
#define GRANNY_NORM_HI 0x5004c6d8u
#define GRANNY_NORM_LO 0x5004c6d4u
#define GRANNY_NORM_POLY_A 0x5004c6d0u
#define GRANNY_NORM_POLY_B 0x5004c6ccu
#endif

/* Every product and sum is rounded on its own, as the translated x87 code
 * does: no fused multiply-adds (clang contracts a*b+c by default). */
#pragma clang fp contract(off)

static int granny_mode(void) {
    static int mode = -1; /* 0 original, 1 native, 2 alternate, 3 check */
    if (mode < 0) {
        const char *v = getenv("RECOMP_GRANNY_NATIVE");
        mode = !v || !*v ? 1 : v[0] == '0' ? 0 : v[0] == 'a' ? 2 : v[0] == 'c' ? 3 : 1;
        fprintf(stderr, "[granny] native mode %d\n", mode);
    }
    if (mode != 2)
        return mode;
    static int shown = -1;
    const int on = (int)((os_monotonic_ns() / 10000000000ull) & 1);
    if (on != shown) {
        shown = on;
        fprintf(stderr, "[granny] %s\n", on ? "native" : "original");
    }
    return on;
}

/* RET as the translation does it: the CALL's expected return address needs no
 * lookup. `ret_exp` is the hint as the function found it on entry. */
static inline void granny_ret(X86 *c, uint32_t esp, uint32_t pop, uint32_t ret_exp) {
    c->eip = rd32(esp);
    c->r[4] = esp + 4u + pop;
    if (c->eip != ret_exp)
        recomp_return(c);
}
static inline uint32_t f_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* The ToE granny2's 50011890 (cdecl: knots, controls, t, out): a quaternion
 * on a quadratic B-spline from three knots and three control quaternions, then
 * one Newton step towards unit length. Called for every animated bone's
 * rotation track each sample: 6.3% of the zoomed-out ToE map's main thread in
 * translated x87 (2026-10-20). Order of operations from its listing:
 *   M0  = f(t - k[-1])            [ESP]     (the pushed ECX slot)
 *   M8  = f(M0 / (k[0] - k[-1]))  [ESP+8]   (the knots argument's slot)
 *   r   = (t - k[-2]) / (k[0] - k[-2])
 *   M10 = f((M8 + r) - r * M8)    [ESP+10]  (t's slot)
 *   w2  = (M0 / (k[1] - k[-1])) * M8;  w1 = M10 - w2;  w0 = C1 - M10
 *   out[i] = f((w0 * c[i-8] + w1 * c[i-4]) + w2 * c[i]),  i = 0..2
 *   y3 = (w0 * c[-5] + w1 * c[-1]) + w2 * c[3];  out[3] = f(y3)
 *   k = (C2 - (((o0*o0 + o1*o1) + o2*o2) + y3*y3)) * C3   (y3 unrounded)
 *   out[i] = f(k * out[i])
 * ECX ends as M0's bits (POP ECX takes the slot FSTP overwrote), EAX as out. */
struct SplineQuat {
    uint32_t m0, m8, m10, out[4];
};
static struct SplineQuat spline_quat_compute(X86 *c, uint32_t esp) {
    /* X: one x87 result (rounded to the precision control's width, NaN to
     * the indefinite with IE); DIV also notes a zero divide; ST: a float store
     * in the rounding control's direction - the translation's own helpers. */
#define X(e) fx87(c, (e))
#define DIV(a, b) fx87(c, fdivz(c, (a), (b)))
#define FST(v) fto_float(c, (v))
    const uint32_t kp = rd32(esp + 4u), cp = rd32(esp + 8u);
    const double t = (double)rdf32(esp + 12u);
    const double k0 = rdf32(kp), km1 = rdf32(kp - 4u), km2 = rdf32(kp - 8u), k1 = rdf32(kp + 4u);
    const double c1 = rdf32(GRANNY_SPLINE_ONE), c2 = rdf32(GRANNY_SPLINE_NORM_A),
                 c3 = rdf32(GRANNY_SPLINE_NORM_B);
    struct SplineQuat q;
    const float m0 = FST(X(t - km1));
    const float m8 = FST(DIV((double)m0, X(k0 - km1)));
    const double r = DIV(X(t - km2), X(k0 - km2));
    const double sum = X((double)m8 + r);
    const double prod = X(r * (double)m8);
    const float m10 = FST(X(sum - prod));
    const double w2 = X(DIV((double)m0, X(k1 - km1)) * (double)m8);
    const double w1 = X((double)m10 - w2);
    const double w0 = X(c1 - (double)m10);
    float o[4];
    for (int i = 0; i < 3; ++i) {
        const double a = X(w0 * (double)rdf32(cp + (uint32_t)(i * 4 - 32)));
        const double b = X(w1 * (double)rdf32(cp + (uint32_t)(i * 4 - 16)));
        const double ab = X(a + b);
        o[i] = FST(X(ab + X(w2 * (double)rdf32(cp + (uint32_t)(i * 4)))));
    }
    const double a3 = X(w0 * (double)rdf32(cp - 20u));
    const double b3 = X(w1 * (double)rdf32(cp - 4u));
    const double y3 = X(X(a3 + b3) + X(w2 * (double)rdf32(cp + 12u)));
    o[3] = FST(y3);
    const double o0 = o[0], o1 = o[1], o2 = o[2];
    double s = X(X(o0 * o0) + X(o1 * o1));
    s = X(s + X(o2 * o2));
    s = X(s + X(y3 * y3));
    const double k = X(X(c2 - s) * c3);
    for (int i = 0; i < 4; ++i)
        q.out[i] = f_bits(FST(X(k * (double)o[i])));
    q.m0 = f_bits(m0);
    q.m8 = f_bits(m8);
    q.m10 = f_bits(m10);
    return q;
#undef X
#undef DIV
#undef FST
}

void granny_spline_quat(X86 *c) {
    const int mode = granny_mode();
    if (mode == 0) {
        GRANNY_SPLINE_QUAT_ORIG(c);
        return;
    }
    const uint32_t esp = c->r[4], op = rd32(esp + 16u), ret_exp = c->ret_expect;
    const struct SplineQuat q = spline_quat_compute(c, esp);
    if (mode == 3) {
        const uint32_t cp = rd32(esp + 8u);
        GRANNY_SPLINE_QUAT_ORIG(c); /* returns: ESP is past the return address */
        static unsigned diffs, calls;
        ++calls;
        const uint32_t got[7] = {rd32(esp - 4u), rd32(esp + 4u), rd32(esp + 12u), rd32(op), rd32(op + 4u),
                                 rd32(op + 8u), rd32(op + 12u)};
        const uint32_t want[7] = {q.m0, q.m8, q.m10, q.out[0], q.out[1], q.out[2], q.out[3]};
        int bad = memcmp(got, want, sizeof got) != 0 || c->r[1] != q.m0 || c->r[0] != op;
        if (bad && diffs++ < 20)
            fprintf(stderr,
                    "[granny] spline_quat differs (call %u): slots %08x %08x %08x out %08x %08x %08x %08x, "
                    "native %08x %08x %08x out %08x %08x %08x %08x; ECX %08x EAX %08x (controls %08x)\n",
                    calls, got[0], got[1], got[2], got[3], got[4], got[5], got[6], want[0], want[1], want[2],
                    want[3], want[4], want[5], want[6], c->r[1], c->r[0], cp);
        if (calls % 1000000u == 0)
            fprintf(stderr, "[granny] spline_quat check: %u calls, %u differ\n", calls, diffs);
        return;
    }
    wr32(esp - 4u, q.m0);
    wr32(esp + 4u, q.m8);
    wr32(esp + 12u, q.m10);
    for (int i = 0; i < 4; ++i)
        wr32(op + (uint32_t)(i * 4), q.out[i]);
    c->r[0] = op;
    c->r[1] = q.m0;
    granny_ret(c, esp, 0, ret_exp);
}

/* The ToE granny2's 500127b0 (cdecl: count, knots, t; returns an int): the
 * knot interval holding t, by bisection with x87 compares - called before
 * every curve sample (2.1% of the zoomed-out ToE map's main thread). Leaves
 * EAX the index + 1, ECX and EDX as its last steps did, ESI and EDI restored
 * (their pushed copies below the stack), the status word's condition codes
 * from the last compare and the flags of the last CMP or ADD. */
static void cmp_flags(X86 *c, uint32_t a, uint32_t b) {
    const uint64_t rf = (uint64_t)a - b;
    const uint32_t r = (uint32_t)rf;
    c->eflags_cf = (uint32_t)((rf >> 32) & 1u);
    c->eflags_of = (((a ^ b) & (a ^ r)) >> 31) & 1u;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_zf = (r == 0);
    c->eflags_sf = (r >> 31) & 1u;
    c->eflags_pf = parity8(r);
}
static uint32_t add_flags(X86 *c, uint32_t a, uint32_t b) {
    const uint64_t rf = (uint64_t)a + b;
    const uint32_t r = (uint32_t)rf;
    c->eflags_cf = (uint32_t)((rf >> 32) & 1u);
    c->eflags_of = ((~(a ^ b) & (a ^ r)) >> 31) & 1u;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_zf = (r == 0);
    c->eflags_sf = (r >> 31) & 1u;
    c->eflags_pf = parity8(r);
    return r;
}
static void knot_search_compute(X86 *c) {
    const uint32_t esp = c->r[4];
    uint32_t ecx = rd32(esp + 4u), edx = c->r[2], edi = 0;
    wr32(esp - 4u, c->r[6]);
    wr32(esp - 8u, c->r[7]);
    cmp_flags(c, ecx, 1u);
    if ((int32_t)ecx > 1) {
        uint32_t esi = rd32(esp + 8u);
        const double t = (double)rdf32(esp + 12u);
        for (;;) {
            uint32_t eax = ecx;
            edx = (uint32_t)((int32_t)eax >> 31);
            eax = (uint32_t)((int32_t)(eax - edx) >> 1);
            const int even = (ecx & 1u) == 0;
            ecx = eax;
            fcom(c, t, (double)rdf32(esi + ecx * 4u));
            const uint32_t ah = (fstsw(c) >> 8) & 0x41u;
            if (!even) {
                if (!ah) {
                    edi += ecx;
                    esi += ecx * 4u;
                }
                ++ecx;
            } else if (!ah) {
                fcom(c, t, (double)rdf32(esi + ecx * 4u + 4u));
                const uint32_t ah2 = (fstsw(c) >> 8) & 0x41u;
                if (!parity8(ah2)) { /* JNP: t below the next knot */
                    edi = add_flags(c, edi, ecx);
                    break;
                }
                edi = edi + ecx + 1u;
                esi = esi + ecx * 4u + 4u;
                --ecx;
            }
            cmp_flags(c, ecx, 1u);
            if ((int32_t)ecx <= 1)
                break;
        }
    }
    c->r[0] = edi + 1u;
    c->r[1] = ecx;
    c->r[2] = edx;
}
void granny_knot_search(X86 *c) {
    const int mode = granny_mode();
    if (mode == 0) {
        GRANNY_KNOT_SEARCH_ORIG(c);
        return;
    }
    if (mode != 3) {
        const uint32_t esp = c->r[4], ret_exp = c->ret_expect;
        knot_search_compute(c);
        granny_ret(c, esp, 0, ret_exp);
        return;
    }
    /* check: the native result into a copy of the context, then the original */
    X86 n = *c;
    const uint32_t esp = c->r[4];
    const uint32_t s4 = rd32(esp - 4u), s8 = rd32(esp - 8u);
    knot_search_compute(&n);
    n.eip = rd32(esp);
    n.r[4] = esp + 4u;
    wr32(esp - 4u, s4);
    wr32(esp - 8u, s8);
    GRANNY_KNOT_SEARCH_ORIG(c);
    static unsigned calls, diffs;
    ++calls;
    const int bad = n.r[0] != c->r[0] || n.r[1] != c->r[1] || n.r[2] != c->r[2] || n.r[4] != c->r[4] ||
                    n.r[6] != c->r[6] || n.r[7] != c->r[7] || n.eip != c->eip ||
                    (n.fpu_sw & 0x4700u) != (c->fpu_sw & 0x4700u) || n.eflags_cf != c->eflags_cf ||
                    n.eflags_zf != c->eflags_zf || n.eflags_sf != c->eflags_sf || n.eflags_of != c->eflags_of ||
                    n.eflags_pf != c->eflags_pf || n.eflags_af != c->eflags_af;
    if (bad && diffs++ < 20)
        fprintf(stderr,
                "[granny] knot_search differs (call %u): EAX %08x/%08x ECX %08x/%08x EDX %08x/%08x sw %04x/%04x "
                "flags c%u z%u s%u o%u p%u a%u / c%u z%u s%u o%u p%u a%u\n",
                calls, n.r[0], c->r[0], n.r[1], c->r[1], n.r[2], c->r[2], n.fpu_sw & 0x4700u,
                c->fpu_sw & 0x4700u, n.eflags_cf, n.eflags_zf, n.eflags_sf, n.eflags_of, n.eflags_pf,
                n.eflags_af, c->eflags_cf, c->eflags_zf, c->eflags_sf, c->eflags_of, c->eflags_pf,
                c->eflags_af);
    if (calls % 1000000u == 0)
        fprintf(stderr, "[granny] knot_search check: %u calls, %u differ\n", calls, diffs);
}

/* The ToE granny2's 50028950 (cdecl: accumulator, bone, weight, skeleton,
 * mode, transform): adds one animation's weighted local transform for a bone
 * into the blend accumulator's slot (0x50 bytes: weight sum, count, flags,
 * position 3, orientation 4, scale/shear 9, frame id). A slot from an earlier
 * frame is started over: copied when the weight is exactly 1, scaled
 * otherwise. Mode 1 negates the orientation's weight, mode 2 negates it when
 * the orientation's dot product with the bone's rest one is below the
 * threshold (the shorter way round). 4.0% of the zoomed-out ToE map's main
 * thread translated. Leaves EAX (the count + 1, or the frame id), ECX (the
 * transform's flags or the weight's bits), EDX (the slot), the callee-saved
 * registers restored with their stack copies, and the condition codes of its
 * compares; the flags it leaves are dead after RET. */
static void pose_accumulate_compute(X86 *c) {
#define X(e) fx87(c, (e))
#define FST(v) fto_float(c, (v))
    const uint32_t E = c->r[4];
    const uint32_t p1 = rd32(E + 4u), p2 = rd32(E + 8u), p4 = rd32(E + 16u), p5 = rd32(E + 20u),
                   p6 = rd32(E + 24u);
    const double w = (double)rdf32(E + 12u);
    wr32(E - 4u, c->r[3]);
    wr32(E - 8u, c->r[5]);
    wr32(E - 12u, c->r[6]);
    uint32_t eax = p5 - 1u - 1u;
    double f1 = w;
    if (p5 == 1u) {
        f1 = -w;
    } else if (p5 == 2u) {
        const uint32_t base = rd32(p4 + 8u) + p2 * 0x9cu;
        double s = X(X((double)rdf32(base + 0x24u) * (double)rdf32(p6 + 0x1cu)) +
                     X((double)rdf32(base + 0x20u) * (double)rdf32(p6 + 0x18u)));
        s = X(s + X((double)rdf32(base + 0x1cu) * (double)rdf32(p6 + 0x14u)));
        s = X(s + X((double)rdf32(base + 0x18u) * (double)rdf32(p6 + 0x10u)));
        fcom(c, s, (double)rdf32(GRANNY_POSE_DOT_MIN));
        const uint32_t sw = fstsw(c);
        eax = ((base + 0x18u) & 0xffff0000u) | sw;
        if (sw & 0x0100u) /* C0: below (or unordered) */
            f1 = -w;
    }
    const uint32_t slots = rd32(p1 + 4u), S = slots + p2 * 0x50u;
    uint32_t ecx;
    if (rd32(S + 0x4cu) == rd32(p1 + 12u)) {
        ecx = rd32(p6);
        wr32(S + 8u, rd32(S + 8u) | ecx);
        for (int i = 1; i <= 3; ++i)
            wrf32(S + 8u + (uint32_t)i * 4u,
                  FST(X(X(w * (double)rdf32(p6 + (uint32_t)i * 4u)) + (double)rdf32(S + 8u + (uint32_t)i * 4u))));
        for (int i = 4; i <= 7; ++i)
            wrf32(S + 8u + (uint32_t)i * 4u,
                  FST(X(X(f1 * (double)rdf32(p6 + (uint32_t)i * 4u)) + (double)rdf32(S + 8u + (uint32_t)i * 4u))));
        for (int i = 8; i <= 16; ++i)
            wrf32(S + 8u + (uint32_t)i * 4u,
                  FST(X(X(w * (double)rdf32(p6 + (uint32_t)i * 4u)) + (double)rdf32(S + 8u + (uint32_t)i * 4u))));
        eax = rd32(S + 4u) + 1u;
        const float sum = FST(X(w + (double)rdf32(S)));
        wr32(S + 4u, eax);
        wrf32(S, sum);
    } else {
        fcom(c, w, (double)rdf32(GRANNY_SPLINE_ONE));
        const uint32_t ah = (fstsw(c) >> 8) & 0x44u;
        if (!parity8(ah)) { /* exactly 1: copied */
            wr32(E - 16u, c->r[7]);
            for (int i = 0; i < 17; ++i)
                wr32(S + 8u + (uint32_t)i * 4u, rd32(p6 + (uint32_t)i * 4u));
        } else {
            wr32(S + 8u, rd32(p6));
            for (int i = 1; i <= 3; ++i)
                wrf32(S + 8u + (uint32_t)i * 4u, FST(X(w * (double)rdf32(p6 + (uint32_t)i * 4u))));
            for (int i = 8; i <= 16; ++i)
                wrf32(S + 8u + (uint32_t)i * 4u, FST(X(w * (double)rdf32(p6 + (uint32_t)i * 4u))));
        }
        for (int i = 4; i <= 7; ++i)
            wrf32(S + 8u + (uint32_t)i * 4u, FST(X(f1 * (double)rdf32(p6 + (uint32_t)i * 4u))));
        ecx = rd32(E + 12u);
        wr32(S, ecx);
        wr32(S + 4u, 1u);
        eax = rd32(p1 + 12u);
        wr32(S + 0x4cu, eax);
    }
    c->r[0] = eax;
    c->r[1] = ecx;
    c->r[2] = S;
#undef X
#undef FST
}
void granny_pose_accumulate(X86 *c) {
    const int mode = granny_mode();
    if (mode == 0) {
        GRANNY_POSE_ACCUMULATE_ORIG(c);
        return;
    }
    const uint32_t E = c->r[4], ret_exp = c->ret_expect;
    if (mode != 3) {
        pose_accumulate_compute(c);
        granny_ret(c, E, 0, ret_exp);
        return;
    }
    /* check: the original first, its results kept; memory rolled back for the
     * native run, then compared and restored. */
    const uint32_t S = rd32(rd32(E + 4u) + 4u) + rd32(E + 8u) * 0x50u;
    uint32_t mem0[20], stk0[4], memo[20], stko[4], memn[20], stkn[4];
    for (int i = 0; i < 20; ++i)
        mem0[i] = rd32(S + (uint32_t)i * 4u);
    for (int i = 0; i < 4; ++i)
        stk0[i] = rd32(E - 16u + (uint32_t)i * 4u);
    X86 n = *c;
    GRANNY_POSE_ACCUMULATE_ORIG(c);
    for (int i = 0; i < 20; ++i)
        memo[i] = rd32(S + (uint32_t)i * 4u), wr32(S + (uint32_t)i * 4u, mem0[i]);
    for (int i = 0; i < 4; ++i)
        stko[i] = rd32(E - 16u + (uint32_t)i * 4u), wr32(E - 16u + (uint32_t)i * 4u, stk0[i]);
    pose_accumulate_compute(&n);
    for (int i = 0; i < 20; ++i)
        memn[i] = rd32(S + (uint32_t)i * 4u), wr32(S + (uint32_t)i * 4u, memo[i]);
    for (int i = 0; i < 4; ++i)
        stkn[i] = rd32(E - 16u + (uint32_t)i * 4u), wr32(E - 16u + (uint32_t)i * 4u, stko[i]);
    static unsigned calls, diffs;
    ++calls;
    const int bad = memcmp(memo, memn, sizeof memo) != 0 || memcmp(stko, stkn, sizeof stko) != 0 ||
                    n.r[0] != c->r[0] || n.r[1] != c->r[1] || n.r[2] != c->r[2] ||
                    (n.fpu_sw & 0x4700u) != (c->fpu_sw & 0x4700u);
    if (bad && diffs++ < 20) {
        fprintf(stderr, "[granny] pose_accumulate differs (call %u): EAX %08x/%08x ECX %08x/%08x EDX %08x/%08x sw %04x/%04x\n",
                calls, n.r[0], c->r[0], n.r[1], c->r[1], n.r[2], c->r[2], n.fpu_sw & 0x4700u, c->fpu_sw & 0x4700u);
        for (int i = 0; i < 20; ++i)
            if (memo[i] != memn[i])
                fprintf(stderr, "[granny]   slot +%02x: %08x native %08x\n", i * 4, memo[i], memn[i]);
        for (int i = 0; i < 4; ++i)
            if (stko[i] != stkn[i])
                fprintf(stderr, "[granny]   stack -%02x: %08x native %08x\n", 16 - i * 4, stko[i], stkn[i]);
    }
    if (calls % 1000000u == 0)
        fprintf(stderr, "[granny] pose_accumulate check: %u calls, %u differ\n", calls, diffs);
}

/* The ToE granny2's 50028b90 (cdecl: accumulator, first bone, bone count,
 * skeleton): finishes the blended local pose of a run of bones. A bone this
 * frame's blend reached is topped up with its rest transform (through the
 * accumulator add above) when its weights fall short of the total, divided by
 * its weight sum (unless one animation at weight exactly 1 set it), and its
 * orientation brought to unit length (1/sqrt outside [lo, hi], a polynomial
 * step inside); a bone it did not reach gets its rest transform copied. 3.5%
 * of the zoomed-out ToE map's main thread translated. Out-of-range bone runs
 * (the error log) and empty runs go to the original. Leaves EAX and ECX the
 * end bone, EDX the last rest-transform offset, the callee-saved registers
 * restored, the stack slots it reuses as the original leaves them (the
 * argument slots hold the loop's index and offset, the pushed ECX the end) and
 * the condition codes of its last compare. */
static void pose_finish_compute(X86 *c) {
#define X(e) fx87(c, (e))
#define FST(v) fto_float(c, (v))
    const uint32_t E = c->r[4];
    const uint32_t p1 = rd32(E + 4u), p2 = rd32(E + 8u), p3 = rd32(E + 12u), p4 = rd32(E + 16u);
    const uint32_t end = p2 + p3;
    const uint32_t ebx0 = c->r[3], ebp0 = c->r[5], esi0 = c->r[6], edi0 = c->r[7];
    wr32(E - 4u, end); /* the pushed ECX's slot, overwritten with the end */
    wr32(E - 8u, ebp0);
    wr32(E - 12u, esi0);
    wr32(E - 16u, edi0);
    wr32(E - 20u, ebx0);
    const uint32_t slots = rd32(p1 + 4u), frame = rd32(p1 + 12u);
    const double one = rdf32(GRANNY_SPLINE_ONE), hi = rdf32(GRANNY_NORM_HI), lo = rdf32(GRANNY_NORM_LO),
                 a3 = rdf32(GRANNY_SPLINE_NORM_A), b6 = rdf32(GRANNY_NORM_POLY_A), c6 = rdf32(GRANNY_NORM_POLY_B);
    uint32_t edx = p2 * 0x9cu;
    for (uint32_t bone = p2; (int32_t)bone < (int32_t)end; ++bone, edx += 0x9cu) {
        const uint32_t S = slots + bone * 0x50u, B = S + 0x14u;
        wr32(E + 12u, bone);
        wr32(E + 8u, edx);
        if (rd32(S + 0x4cu) != frame) {
            const uint32_t src = rd32(p4 + 8u) + edx + 8u;
            for (int i = 0; i < 17; ++i)
                wr32(S + 8u + (uint32_t)i * 4u, rd32(src + (uint32_t)i * 4u));
            continue;
        }
        fcom(c, (double)rdf32(S), (double)rdf32(p1 + 8u));
        if (!parity8((fstsw(c) >> 8) & 0x05u)) { /* below the total: top up with the rest pose */
            const uint32_t sk = rd32(p4 + 8u);
            const float wgt = FST(X((double)rdf32(p1 + 8u) - (double)rdf32(S)));
            const uint32_t F = E - 44u; /* the callee's arguments, as the original pushes them */
            wr32(E - 24u, sk + edx + 8u);
            wr32(E - 28u, 2u);
            wr32(E - 32u, p4);
            wrf32(E - 36u, wgt);
            wr32(E - 40u, bone);
            wr32(E - 44u, p1);
            wr32(E - 48u, 0x50028c2du);
            X86 *cc = c;
            const uint32_t esp_save = cc->r[4];
            cc->r[4] = F - 4u;
            cc->r[3] = B;
            cc->r[5] = S;
            cc->r[6] = p1;
            cc->r[7] = end;
            pose_accumulate_compute(cc);
            cc->r[4] = esp_save;
            (void)F;
        }
        if (rd32(S + 4u) != 1u || rd32(S) != 0x3f800000u) {
            const double q = X(fdivz(c, one, (double)rdf32(S)));
            static const uint8_t offs[] = {0x0c, 0x10, 0x14, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44, 0x48};
            for (unsigned i = 0; i < sizeof offs; ++i)
                wrf32(S + offs[i], FST(X(q * (double)rdf32(S + offs[i]))));
        }
        const double o0 = rdf32(B + 4u), o1 = rdf32(B + 8u), o2 = rdf32(B + 12u), o3 = rdf32(B + 16u);
        double s = X(X(o2 * o2) + X(o1 * o1));
        s = X(s + X(o0 * o0));
        s = X(s + X(o3 * o3));
        const float sf = FST(s);
        wrf32(E + 8u, sf);
        double k;
        fcom(c, (double)sf, hi);
        if (!((fstsw(c) >> 8) & 0x41u)) { /* above hi */
            k = X(fdivz(c, one, X(sqrt(s))));
        } else {
            fcom(c, (double)sf, lo);
            if (!parity8((fstsw(c) >> 8) & 0x05u)) { /* below lo */
                k = X(fdivz(c, one, X(sqrt(s))));
            } else {
                const double a = X(a3 - (double)sf);
                const double b = X(X(a * a) * (double)sf);
                const double d = X(b6 - b);
                k = X(X(a * d) * c6);
            }
        }
        for (int i = 0; i < 4; ++i)
            wrf32(B + 4u + (uint32_t)i * 4u, FST(X(k * (double)rdf32(B + 4u + (uint32_t)i * 4u))));
    }
    wr32(E + 12u, end);
    wr32(E + 8u, edx);
    c->r[0] = end;
    c->r[1] = end;
    c->r[2] = edx;
    c->r[3] = ebx0;
    c->r[5] = ebp0;
    c->r[6] = esi0;
    c->r[7] = edi0;
#undef X
#undef FST
}
static int pose_finish_in_range(X86 *c) {
    const uint32_t E = c->r[4];
    const int32_t first = (int32_t)rd32(E + 8u), count = (int32_t)rd32(rd32(E + 4u)),
                  end = first + (int32_t)rd32(E + 12u);
    return first >= 0 && first < count && end >= 0 && end <= count && first < end;
}
void granny_pose_finish(X86 *c) {
    const int mode = granny_mode();
    if (mode == 0 || !pose_finish_in_range(c)) {
        GRANNY_POSE_FINISH_ORIG(c);
        return;
    }
    const uint32_t E = c->r[4], ret_exp = c->ret_expect;
    if (mode != 3) {
        pose_finish_compute(c);
        granny_ret(c, E, 0, ret_exp);
        return;
    }
    /* check: the original, then the native run over the same starting memory */
    const uint32_t p1 = rd32(E + 4u), first = rd32(E + 8u), n = rd32(E + 12u);
    const uint32_t S0 = rd32(p1 + 4u) + first * 0x50u, bytes = n * 0x50u;
    uint8_t *before = malloc(bytes), *orig = malloc(bytes);
    uint32_t stk0[6], stko[6], stkn[6];
    for (uint32_t i = 0; i < bytes; ++i)
        before[i] = rd8(S0 + i);
    for (int i = 0; i < 6; ++i)
        stk0[i] = rd32(E - 20u + (uint32_t)i * 4u); /* E-20 .. E+0 skipped below */
    const uint32_t a8 = rd32(E + 8u), ac = rd32(E + 12u);
    X86 nn = *c;
    GRANNY_POSE_FINISH_ORIG(c);
    for (uint32_t i = 0; i < bytes; ++i)
        orig[i] = rd8(S0 + i), wr8(S0 + i, before[i]);
    for (int i = 0; i < 6; ++i)
        stko[i] = rd32(E - 20u + (uint32_t)i * 4u), wr32(E - 20u + (uint32_t)i * 4u, stk0[i]);
    const uint32_t o8 = rd32(E + 8u), oc = rd32(E + 12u);
    wr32(E + 8u, a8);
    wr32(E + 12u, ac);
    pose_finish_compute(&nn);
    int bad = 0;
    for (uint32_t i = 0; i < bytes; ++i)
        if (rd8(S0 + i) != orig[i]) {
            if (!bad && 0)
                ;
            bad = 1;
            wr8(S0 + i, orig[i]);
        }
    for (int i = 0; i < 6; ++i)
        stkn[i] = rd32(E - 20u + (uint32_t)i * 4u), wr32(E - 20u + (uint32_t)i * 4u, stko[i]);
    const uint32_t n8 = rd32(E + 8u), nc = rd32(E + 12u);
    wr32(E + 8u, o8);
    wr32(E + 12u, oc);
    static unsigned calls, diffs;
    ++calls;
    const int slots_bad = bad;
    bad |= memcmp(stko + 1, stkn + 1, 4 * 4) != 0 || o8 != n8 || oc != nc || nn.r[0] != c->r[0] ||
           nn.r[1] != c->r[1] || nn.r[2] != c->r[2] || nn.r[3] != c->r[3] || nn.r[5] != c->r[5] ||
           nn.r[6] != c->r[6] || nn.r[7] != c->r[7] || (nn.fpu_sw & 0x4700u) != (c->fpu_sw & 0x4700u);
    if (bad && diffs++ < 20)
        fprintf(stderr,
                "[granny] pose_finish differs (call %u, bones %u+%u): slots %s, args %08x %08x / %08x %08x, "
                "EAX %08x/%08x ECX %08x/%08x EDX %08x/%08x sw %04x/%04x\n",
                calls, first, n, slots_bad ? "DIFFER" : "same", o8, oc, n8, nc, nn.r[0], c->r[0], nn.r[1], c->r[1],
                nn.r[2], c->r[2], nn.fpu_sw & 0x4700u, c->fpu_sw & 0x4700u);
    if (calls % 100000u == 0)
        fprintf(stderr, "[granny] pose_finish check: %u calls, %u differ\n", calls, diffs);
    free(before);
    free(orig);
}
