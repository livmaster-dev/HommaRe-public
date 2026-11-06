/* libm.h - recomp-kit's stand-in for musl's src/internal/libm.h, carrying
 * only what the files vendored here use (README.md): the word-access macros,
 * the evaluation barriers and the overflow helpers. It is written for this
 * subset; musl's own header also covers long double, float and the
 * table-driven log and pow, none of which are here. Every definition below
 * is musl's, from musl 1.2.6 (MIT, see LICENSE). */
#ifndef RECOMP_MUSL_LIBM_H
#define RECOMP_MUSL_LIBM_H

#include <float.h>
#include <math.h>
#include <stdint.h>

#define hidden

/* The functions here are deterministic only where double_t is double and
 * every operation rounds once (FLT_EVAL_METHOD 0, no FMA contraction, which
 * CMakeLists.txt turns off for every target). */
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0
#error "musl-libm: needs FLT_EVAL_METHOD 0 (SSE2, arm64 or wasm arithmetic)"
#endif

#define WANT_ROUNDING 1
#define WANT_SNAN 0
#define issignaling_inline(x) 0
#define TOINT_INTRINSICS 0

#ifdef __GNUC__
#define predict_true(x) __builtin_expect(!!(x), 1)
#define predict_false(x) __builtin_expect(x, 0)
#else
#define predict_true(x) (x)
#define predict_false(x) (x)
#endif

static inline double eval_as_double(double x) {
    double y = x;
    return y;
}
static inline double fp_barrier(double x) {
    volatile double y = x;
    return y;
}
static inline void fp_force_eval(double x) {
    volatile double y;
    y = x;
    (void)y;
}
static inline void fp_force_evalf(float x) {
    volatile float y;
    y = x;
    (void)y;
}
#define FORCE_EVAL(x)                                                                              \
    do {                                                                                           \
        if (sizeof(x) == sizeof(float))                                                            \
            fp_force_evalf(x);                                                                     \
        else                                                                                       \
            fp_force_eval(x);                                                                      \
    } while (0)

#define asuint64(f) ((union {                                                                      \
                         double _f;                                                                \
                         uint64_t _i;                                                              \
                     }){f})                                                                        \
                        ._i
#define asdouble(i) ((union {                                                                      \
                         uint64_t _i;                                                              \
                         double _f;                                                                \
                     }){i})                                                                        \
                        ._f

#define EXTRACT_WORDS(hi, lo, d)                                                                   \
    do {                                                                                           \
        uint64_t __u = asuint64(d);                                                                \
        (hi) = __u >> 32;                                                                          \
        (lo) = (uint32_t)__u;                                                                      \
    } while (0)
#define GET_HIGH_WORD(hi, d)                                                                       \
    do {                                                                                           \
        (hi) = asuint64(d) >> 32;                                                                  \
    } while (0)
#define GET_LOW_WORD(lo, d)                                                                        \
    do {                                                                                           \
        (lo) = (uint32_t)asuint64(d);                                                              \
    } while (0)
#define INSERT_WORDS(d, hi, lo)                                                                    \
    do {                                                                                           \
        (d) = asdouble(((uint64_t)(hi) << 32) | (uint32_t)(lo));                                   \
    } while (0)
#define SET_HIGH_WORD(d, hi) INSERT_WORDS(d, hi, (uint32_t)asuint64(d))
#define SET_LOW_WORD(d, lo) INSERT_WORDS(d, asuint64(d) >> 32, lo)

/* The kernels the functions share (renamed by recomp_libm_names.h). */
int __rem_pio2_large(double *, double *, int, int, int);
int __rem_pio2(double, double *);
double __sin(double, double, int);
double __cos(double, double);
double __tan(double, double, int);

/* musl's src/math/__math_xflow.c, __math_oflow.c and __math_uflow.c. */
static inline double __math_xflow(uint32_t sign, double y) {
    return eval_as_double(fp_barrier(sign ? -y : y) * y);
}
static inline double __math_oflow(uint32_t sign) {
    return __math_xflow(sign, 0x1p769);
}
static inline double __math_uflow(uint32_t sign) {
    return __math_xflow(sign, 0x1p-767);
}

#endif
