/* lighting.c - native replacements for the game's MMX vertex normal skinning
 * (the file's name is older than that reading; the lighting proper is
 * 00586880 and its neighbours). Each
 * leaves the guest exactly as the translated original does: the result, the
 * registers (the MMX ones included), the stack slots it wrote and the flags
 * of its last flag-setting instruction. Integer MMX arithmetic, so the NEON
 * versions are exact by construction; check mode compares anyway.
 *
 * RECOMP_HOMM5_NATIVE_LIGHT=1 (default) runs these, 0 the originals, check
 * runs both on every call and logs the first differences (the original's
 * results stay), ab alternates every 10 s. */
#include "neon_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x86.h"
#include "platform/os.h"
#include "funcs.h"

#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif
#ifndef HOMM5_LIGHT_VERTEX_ORIG /* the base game */
#define HOMM5_LIGHT_VERTEX_ORIG fn_0066f2e0
#define HOMM5_LIGHT_RSQRT 0x01139088u
#endif

static int light_mode(void) {
    static int mode = -1; /* 0 original, 1 native, 2 alternate, 3 check */
    if (mode < 0) {
        const char *v = getenv("RECOMP_HOMM5_NATIVE_LIGHT");
        mode = !v || !*v ? 1 : v[0] == '0' ? 0 : v[0] == 'a' ? 2 : v[0] == 'c' ? 3 : 1;
        fprintf(stderr, "[light] native mode %d\n", mode);
    }
    if (mode != 2)
        return mode;
    static int shown = -1;
    const int on = (int)((os_monotonic_ns() / 10000000000ull) & 1);
    if (on != shown) {
        shown = on;
        fprintf(stderr, "[light] %s\n", on ? "native" : "original");
    }
    return on;
}

static inline uint64_t u64_of(int16x4_t v) {
    return vget_lane_u64(vreinterpret_u64_s16(v), 0);
}
static inline int16x4_t s16_of(uint64_t v) {
    return vreinterpret_s16_u64(vcreate_u64(v));
}
/* PMULHW: the high halves of the signed 32-bit products. */
static inline int16x4_t pmulhw(int16x4_t a, int16x4_t b) {
    return vshrn_n_s32(vmull_s16(a, b), 16);
}

/* 00557470 (ToE; the base game's 0066f2e0; fastcall: ECX the packed normal
 * out, EDX the packed normal in, then on the stack the decode offsets and a
 * bone's 3x4 i16 matrix from CMMXAnimationMatrices; RET 8): skins one vertex
 * normal through one bone with MMX - the normal's three bytes as words less
 * the decode offset (0x8000), through the matrix's three rotated rows (PMULHW,
 * saturating sums), normalised by the reciprocal-square-root table at
 * HOMM5_LIGHT_RSQRT indexed by the length squared, scaled, plus the encode
 * offset (0x8080), packed back to bytes; the fourth byte kept. 00557220 and
 * 00557330 are its two- and three-bone siblings. 2.7% of the zoomed-out ToE
 * map's main thread translated. */
static void light_vertex_compute(X86 *c) {
    const uint32_t E = c->r[4];
    const uint32_t dst = c->r[1], src = c->r[2];
    const uint32_t vec = rd32(E + 4u), mat = rd32(E + 8u);
    /* SUB ESP,8; PUSH EBX, ESI, EDI; the two register arguments spilled */
    wr32(E - 12u, c->r[3]);
    wr32(E - 16u, c->r[6]);
    wr32(E - 20u, c->r[7]);
    wr32(E - 8u, src);
    wr32(E - 4u, dst);
    const uint32_t colour = rd32(src);
    const uint32_t rgb = colour & 0xffffffu;
    /* PUNPCKLBW of zero with the colour: each byte into a word's high half. */
    const uint64_t mm7 = rgb;
    const int16x4_t mm0 = vsub_s16(
        vreinterpret_s16_u16(vshl_n_u16(vget_low_u16(vmovl_u8(vcreate_u8(mm7))), 8)), s16_of(rd64(vec)));
    int16x4_t mm1 = pmulhw(mm0, s16_of(rd64(mat)));
    const uint64_t m0 = u64_of(mm0);
    int16x4_t mm2 = pmulhw(vadd_s16(s16_of(m0 << 16), s16_of(m0 >> 32)), s16_of(rd64(mat + 8u)));
    mm1 = vqadd_s16(mm1, mm2);
    const int16x4_t mm4 = s16_of(m0 >> 16);
    int16x4_t mm3 = pmulhw(vadd_s16(s16_of(m0 << 32), mm4), s16_of(rd64(mat + 16u)));
    mm1 = vqadd_s16(mm1, mm3);
    mm1 = vshl_n_s16(mm1, 3);
    /* PMADDWD MM1,MM1 then the two dwords summed into the low one */
    const int32x4_t sq = vmull_s16(mm1, mm1);
    const int32x2_t madd = vpadd_s32(vget_low_s32(sq), vget_high_s32(sq));
    const uint64_t madd64 = vget_lane_u64(vreinterpret_u64_s32(madd), 0);
    const uint64_t hi = madd64 >> 32;
    const uint64_t mm2v = (uint64_t)(uint32_t)((uint32_t)madd64 + (uint32_t)hi) | (hi << 32);
    mm3 = s16_of(hi);
    const uint32_t ebx = (uint32_t)mm2v >> 18;
    const uint32_t eax = rd16(ebx * 2u + HOMM5_LIGHT_RSQRT);
    const int16x4_t t = vdup_n_s16((int16_t)eax);
    mm1 = pmulhw(mm1, t);
    mm1 = vshl_n_s16(mm1, 5);
    mm1 = vadd_s16(mm1, s16_of(rd64(vec + 8u)));
    mm1 = vreinterpret_s16_u16(vshr_n_u16(vreinterpret_u16_s16(mm1), 8));
    const uint8x8_t packed = vqmovun_s16(vcombine_s16(mm1, mm1));
    const uint64_t mm1v = vget_lane_u64(vreinterpret_u64_u8(packed), 0);
    const uint32_t ecx = (colour & 0xff000000u) | (uint32_t)mm1v;
    wr32(dst, ecx);
    c->mm[0] = m0;
    c->mm[1] = mm1v;
    c->mm[2] = (uint64_t)eax * 0x0001000100010001ull;
    c->mm[3] = u64_of(mm3);
    c->mm[4] = u64_of(mm4);
    c->mm[7] = mm7;
    c->r[0] = eax;
    c->r[1] = ecx;
    /* ADD ESP,8 (before the RET's pop of 4 + 8): from E-8 to E */
    const uint32_t a = E - 8u, b = 8u, r = E;
    c->eflags_cf = r < a;
    c->eflags_of = ((~(a ^ b) & (a ^ r)) >> 31) & 1u;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_zf = r == 0;
    c->eflags_sf = (r >> 31) & 1u;
    c->eflags_pf = parity8(r);
}

void homm5_light_vertex(X86 *c) {
    const int mode = light_mode();
    if (mode == 0) {
        HOMM5_LIGHT_VERTEX_ORIG(c);
        return;
    }
    const uint32_t E = c->r[4], ret_exp = c->ret_expect;
    if (mode != 3) {
        light_vertex_compute(c);
        c->eip = rd32(E);
        c->r[4] = E + 12u;
        if (c->eip != ret_exp)
            recomp_return(c);
        return;
    }
    /* check: the original, then this over the same memory, compared */
    const uint32_t dst = c->r[1];
    uint32_t stk0[5], stko[5], stkn[5];
    for (int i = 0; i < 5; ++i)
        stk0[i] = rd32(E - 20u + (uint32_t)i * 4u);
    const uint32_t d0 = rd32(dst);
    X86 n = *c;
    HOMM5_LIGHT_VERTEX_ORIG(c);
    const uint32_t dout = rd32(dst);
    for (int i = 0; i < 5; ++i)
        stko[i] = rd32(E - 20u + (uint32_t)i * 4u), wr32(E - 20u + (uint32_t)i * 4u, stk0[i]);
    wr32(dst, d0);
    light_vertex_compute(&n);
    const uint32_t dn = rd32(dst);
    wr32(dst, dout);
    for (int i = 0; i < 5; ++i)
        stkn[i] = rd32(E - 20u + (uint32_t)i * 4u), wr32(E - 20u + (uint32_t)i * 4u, stko[i]);
    n.eip = rd32(E);
    n.r[4] = E + 12u;
    static unsigned calls, diffs;
    ++calls;
    int mmbad = 0;
    for (int i = 0; i < 8; ++i)
        mmbad |= n.mm[i] != c->mm[i];
    const int bad = dn != dout || memcmp(stko, stkn, sizeof stko) != 0 || mmbad || n.r[0] != c->r[0] ||
                    n.r[1] != c->r[1] || n.r[2] != c->r[2] || n.r[3] != c->r[3] || n.r[4] != c->r[4] ||
                    n.r[6] != c->r[6] || n.r[7] != c->r[7] || n.eflags_cf != c->eflags_cf ||
                    n.eflags_zf != c->eflags_zf || n.eflags_sf != c->eflags_sf || n.eflags_of != c->eflags_of ||
                    n.eflags_pf != c->eflags_pf || n.eflags_af != c->eflags_af;
    if (bad && diffs++ < 20) {
        fprintf(stderr, "[light] vertex differs (call %u): out %08x/%08x EAX %08x/%08x ECX %08x/%08x\n", calls, dn,
                dout, n.r[0], c->r[0], n.r[1], c->r[1]);
        for (int i = 0; i < 8; ++i)
            if (n.mm[i] != c->mm[i])
                fprintf(stderr, "[light]   mm%d %016llx original %016llx\n", i, (unsigned long long)n.mm[i],
                        (unsigned long long)c->mm[i]);
    }
    if (calls % 1000000u == 0)
        fprintf(stderr, "[light] vertex check: %u calls, %u differ\n", calls, diffs);
}
