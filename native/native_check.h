/* native_check.h - what the native replacements share: the mode switch
 * (RECOMP_<NAME>=0 the original, 1 native, a alternate every 10 s, c check)
 * and check mode's comparison of the guest state the original and the native
 * version left. Header-only: each source gets its own copies. */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x86.h"
#include "platform/os.h"

enum { NC_ORIGINAL = 0, NC_NATIVE = 1, NC_ALTERNATE = 2, NC_CHECK = 3 };

/* The mode for this call: *mode caches the variable (start it at -1); the
 * alternate mode answers 0 or 1 by the 10-second phase and logs each switch. */
static inline int nc_mode(int *mode, int *shown, const char *var, const char *tag, int dflt) {
    if (*mode < 0) {
        const char *v = getenv(var);
        *mode = !v || !*v ? dflt : v[0] == '0' ? 0 : v[0] == 'a' ? 2 : v[0] == 'c' ? 3 : 1;
        fprintf(stderr, "[%s] native mode %d\n", tag, *mode);
    }
    if (*mode != NC_ALTERNATE)
        return *mode;
    const int on = (int)((os_monotonic_ns() / 10000000000ull) & 1);
    if (on != *shown) {
        *shown = on;
        fprintf(stderr, "[%s] %s\n", tag, on ? "native" : "original");
    }
    return on;
}

/* The registers, flags, x87, MMX and XMM state of a (the original's) and b
 * (the native version's); 0 when they agree. The first difference goes into
 * why. ret_expect is left out: the caller's, untouched by either. */
static inline int nc_state_differs(const X86 *a, const X86 *b, char *why, size_t n) {
    for (int i = 0; i < 8; ++i)
        if (a->r[i] != b->r[i])
            return snprintf(why, n, "r%d %08x native %08x", i, a->r[i], b->r[i]), 1;
    if (a->eip != b->eip)
        return snprintf(why, n, "eip %08x native %08x", a->eip, b->eip), 1;
    const uint32_t fa[7] = {a->eflags_cf, a->eflags_zf, a->eflags_sf, a->eflags_of,
                            a->eflags_pf, a->eflags_af, a->eflags_df};
    const uint32_t fb[7] = {b->eflags_cf, b->eflags_zf, b->eflags_sf, b->eflags_of,
                            b->eflags_pf, b->eflags_af, b->eflags_df};
    static const char *const fname[7] = {"cf", "zf", "sf", "of", "pf", "af", "df"};
    for (int i = 0; i < 7; ++i)
        if (fa[i] != fb[i])
            return snprintf(why, n, "%s %u native %u", fname[i], fa[i], fb[i]), 1;
    if (a->eflags_misc != b->eflags_misc)
        return snprintf(why, n, "eflags_misc %08x native %08x", a->eflags_misc, b->eflags_misc), 1;
    if (a->fpu_top != b->fpu_top || a->fpu_cw != b->fpu_cw || a->fpu_sw != b->fpu_sw ||
        a->fpu_tag != b->fpu_tag)
        return snprintf(why, n, "x87 top %u cw %04x sw %04x tag %04x native %u %04x %04x %04x",
                        a->fpu_top, a->fpu_cw, a->fpu_sw, a->fpu_tag, b->fpu_top, b->fpu_cw,
                        b->fpu_sw, b->fpu_tag),
               1;
    for (int i = 0; i < 8; ++i) {
        if (memcmp(&a->st[i], &b->st[i], 8) != 0 || a->st_exact[i] != b->st_exact[i] ||
            (a->st_exact[i] && a->st_bits[i] != b->st_bits[i]))
            return snprintf(why, n, "st slot %d %a native %a", i, a->st[i], b->st[i]), 1;
    }
    for (int i = 0; i < 8; ++i)
        if (a->mm[i] != b->mm[i])
            return snprintf(why, n, "mm%d %016llx native %016llx", i, (unsigned long long)a->mm[i],
                            (unsigned long long)b->mm[i]),
                   1;
    for (int i = 0; i < 8; ++i)
        if (memcmp(a->xmm[i], b->xmm[i], 16) != 0)
            return snprintf(why, n, "xmm%d %08x %08x %08x %08x native %08x %08x %08x %08x", i,
                            a->xmm[i][0], a->xmm[i][1], a->xmm[i][2], a->xmm[i][3], b->xmm[i][0],
                            b->xmm[i][1], b->xmm[i][2], b->xmm[i][3]),
                   1;
    return 0;
}

/* Check mode's bookkeeping: counts calls and differences, prints the first
 * 20 differences and a summary every `every` calls. */
typedef struct {
    const char *tag;
    unsigned long long calls, diffs, skipped;
} NcCheck;
static inline void nc_report(NcCheck *k, int bad, const char *what, const char *why) {
    ++k->calls;
    if (bad && k->diffs++ < 20)
        fprintf(stderr, "[%s] %s differs (call %llu): %s\n", k->tag, what, k->calls, why);
    if (k->calls % 1000000ull == 0)
        fprintf(stderr, "[%s] check: %llu calls, %llu differ, %llu not checked\n", k->tag, k->calls,
                k->diffs, k->skipped);
}
