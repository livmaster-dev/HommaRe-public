// Floating point that network play depends on, the same on every host. Heroes
// V's peers each compute the game from the commands they exchange, so the
// translated arithmetic must give one answer on x86_64, arm64 and wasm32:
//
//   - the x87 and SSE conversions to integer, where a C cast of a NaN or an
//     out-of-range value is undefined (x86_64 gives the "integer indefinite"
//     0x80000000, arm64 saturates) and the helpers must give x86's answer;
//   - the control word's rounding and precision, computed explicitly;
//   - no FMA contraction (CMakeLists.txt's -ffp-contract=off);
//   - the transcendental functions (FSIN, FPATAN, FYL2X..., the CRT's sin,
//     pow, exp...), from the bundled musl libm.
//
// Each part is checked against the x86 answer where one is known, and folded
// into a hash. The bundled libm's hash must equal the one recorded below on
// every host, so a host that computes differently fails here instead of
// desyncing a game. The host C library's own results are hashed too, for
// information only: they differ between glibc, Apple's libm, the UCRT, bionic
// and emscripten's musl, which is why the guest does not use them.
//
// Standalone on purpose (x86.h and the libm, nothing else), so the same file
// builds for a cross target and runs under an emulator:
//   clang++ -std=c++20 -O2 -ffp-contract=off -I kit/runtime \
//     kit/runtime/tests/fp_determinism_tests.cpp <musl-libm objects>
#include "x86.h"
#include "../third_party/musl-libm/recomp_libm.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_checks = 0, g_failures = 0;
static bool check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool check(bool ok, const char *fmt, ...) {
    ++g_checks;
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!ok)
        ++g_failures;
    if (!ok || getenv("FP_VERBOSE"))
        printf("  [%s] %s\n", ok ? "ok" : "FAIL", msg);
    return ok;
}

// FNV-1a over the bits of every result.
struct Hash {
    uint64_t h = 0xcbf29ce484222325ull;
    void bytes(const void *p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            h ^= ((const uint8_t *)p)[i];
            h *= 0x100000001b3ull;
        }
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            const uint8_t b = (uint8_t)(v >> (8 * i));
            bytes(&b, 1);
        }
    }
    void f64(double v) {
        uint64_t b;
        memcpy(&b, &v, 8);
        u64(b);
    }
};

static uint64_t bits(double v) {
    uint64_t b;
    memcpy(&b, &v, 8);
    return b;
}
static uint32_t bitsf(float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    return b;
}

// A context with the given x87 control word.
static X86 ctx(uint16_t cw) {
    X86 c;
    memset(&c, 0, sizeof c);
    c.fpu_cw = cw;
    c.fpu_tag = 0xffff;
    return c;
}
enum : uint16_t {
    RC_NEAR = 0x037f,
    RC_DOWN = 0x077f,
    RC_UP = 0x0b7f,
    RC_CHOP = 0x0f7f,
    PC_24 = 0x007f
};

// Opaque to the optimiser, so nothing below is folded at compile time with
// the compiler's own arithmetic instead of the target's.
template <typename T> static T opaque(T v) {
    volatile T x = v;
    return x;
}

// ------------------------------------------------------- the exact part --

static void test_conversions(Hash &h) {
    const double nan = opaque((double)NAN), inf = opaque((double)INFINITY);
    X86 c = ctx(RC_NEAR);
    struct {
        double v;
        int32_t near, down, up, chop;
    } cases[] = {
        {2.5, 2, 2, 3, 2},
        {3.5, 4, 3, 4, 3},
        {-2.5, -2, -3, -2, -2},
        {-0.5, 0, -1, 0, 0},
        {2147483647.4, 2147483647, 2147483647, INT32_MIN, 2147483647},
        {-2147483648.6, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN},
        {2147483648.0, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN},
        {1e10, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN},
        {-1e300, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN},
    };
    for (auto &k : cases) {
        const double v = opaque(k.v);
        const uint16_t cws[4] = {RC_NEAR, RC_DOWN, RC_UP, RC_CHOP};
        const int32_t want[4] = {k.near, k.down, k.up, k.chop};
        for (int m = 0; m < 4; ++m) {
            c = ctx(cws[m]);
            const int32_t got = fto_i32(&c, v);
            check(got == want[m], "FIST %.1f, RC %d: %d (x86 %d)", k.v, m, got, want[m]);
            h.u64((uint32_t)got);
        }
    }
    c = ctx(RC_NEAR);
    check(fto_i32(&c, nan) == INT32_MIN && (c.fpu_sw & 1), "FIST NaN: the integer indefinite, IE");
    check(fto_i32(&c, inf) == INT32_MIN, "FIST +inf: indefinite");
    check(fto_i16(&c, opaque(32767.5)) == INT16_MIN,
          "FIST word 32767.5 rounds to 32768: indefinite");
    check(fto_i16(&c, opaque(-32768.4)) == -32768, "FIST word -32768.4");
    check(fto_i64(&c, opaque(9.3e18)) == INT64_MIN, "FIST qword 9.3e18: indefinite");
    check(fto_i64(&c, opaque(-9223372036854775808.0)) == INT64_MIN, "FIST qword -2^63 fits");
    h.u64((uint64_t)fto_i64(&c, opaque(4503599627370497.0)));

    // SSE: CVTTSS2SI / CVTTSD2SI / CVTTPS2DQ truncate; CVTPS2PI rounds to even.
    struct {
        float v;
        int32_t want;
    } ss[] = {{3e9f, INT32_MIN},
              {-3e9f, INT32_MIN},
              {2147483520.0f, 2147483520},
              {-2147483648.0f, INT32_MIN},
              {-2147483520.0f, -2147483520},
              {-1.9f, -1},
              {(float)NAN, INT32_MIN},
              {(float)INFINITY, INT32_MIN}};
    for (auto &k : ss) {
        const int32_t got = recomp_sse_cvtt_i32(opaque(k.v));
        check(got == k.want, "CVTTSS2SI %g: %d (x86 %d)", (double)k.v, got, k.want);
        h.u64((uint32_t)got);
    }
    check(recomp_sse_cvt_i32(opaque(2.5f)) == 2 && recomp_sse_cvt_i32(opaque(3.5f)) == 4 &&
              recomp_sse_cvt_i32(opaque(-2.5f)) == -2 &&
              recomp_sse_cvt_i32(opaque(5e9f)) == INT32_MIN,
          "CVTPS2PI: nearest-even, indefinite out of range");
    struct {
        double v;
        int32_t want;
    } sd[] = {{2147483647.9, 2147483647}, {-2147483648.9, INT32_MIN}, {2147483648.0, INT32_MIN},
              {-2147483649.0, INT32_MIN}, {nan, INT32_MIN},           {-0.99, 0}};
    for (auto &k : sd) {
        const int32_t got = recomp_sse_cvtt_i32d(opaque(k.v));
        check(got == k.want, "CVTTSD2SI %.2f: %d (x86 %d)", k.v, got, k.want);
        h.u64((uint32_t)got);
    }
    // FISTTP truncates whatever RC says.
    c = ctx(RC_UP);
    c.st[0] = opaque(-7.9);
    check(fistt_i32(&c) == -7, "FISTTP -7.9 under RC up: -7");
    c.st[0] = opaque(5e9);
    check(fistt_i32(&c) == INT32_MIN, "FISTTP 5e9: indefinite");
}

// FSCALE's scale reaches ldexp as an int: out of int's range a C cast would
// give x86_64 0 and arm64 infinity for the same 1 * 2^1e10.
static void test_fscale(Hash &h) {
    struct {
        double a, b, want;
    } cases[] = {
        {3.0, 2.7, 12.0},
        {3.0, -1.5, 1.5},
        {1.0, 1e10, (double)INFINITY},
        {-1.0, 1e10, -(double)INFINITY},
        {1.0, -1e10, 0.0},
        {1.0, 3e9, (double)INFINITY},
        {5.0, -(double)INFINITY, 0.0},
        {5.0, (double)INFINITY, (double)INFINITY},
        {0x1p-1074, 2000.0, 0x1p926},
        {0x1p1023, -2100.0, 0.0},
    };
    for (auto &k : cases) {
        const double got = fscale(opaque(k.a), opaque(k.b));
        check(bits(got) == bits(k.want), "FSCALE %g by 2^%g: %g (x86 %g)", k.a, k.b, got, k.want);
        h.f64(got);
    }
    X86 c = ctx(RC_NEAR);
    check(bits(fx87_exact(&c, fscale(opaque(0.0), opaque((double)INFINITY)))) ==
              0xfff8000000000000ull,
          "FSCALE 0 by 2^inf: invalid");
    check(fscale(opaque(-0.0), opaque(5.0)) == 0.0 &&
              std::signbit(fscale(opaque(-0.0), opaque(5.0))),
          "FSCALE keeps -0");
}

static void test_rounding(Hash &h) {
    // FRNDINT per RC, ties to even, exact above 2^52.
    X86 c = ctx(RC_NEAR);
    const double r[] = {0.5, 1.5, 2.5, -0.5, -1.5, 4503599627370497.0, 1e300};
    for (double v : r)
        h.f64(fround_cw(&c, opaque(v)));
    check(fround_cw(&c, opaque(2.5)) == 2.0 && fround_cw(&c, opaque(-1.5)) == -2.0 &&
              fround_cw(&c, opaque(4503599627370497.0)) == 4503599627370497.0,
          "FRNDINT nearest-even");
    // PC = 24 bits: the basic operations round to single.
    c = ctx(PC_24);
    const double one_eps = opaque(1.0) + opaque(0x1p-30);
    check(fx87(&c, one_eps) == 1.0, "PC=24: 1 + 2^-30 rounds to 1");
    c = ctx(RC_NEAR);
    check(fx87(&c, one_eps) == one_eps, "PC=64 (a double here): 1 + 2^-30 stays");
    // FST to a float under each RC.
    const double v = opaque(1.0 + 0x1p-40);
    const uint16_t cws[4] = {RC_NEAR, RC_DOWN, RC_UP, RC_CHOP};
    const uint32_t want[4] = {0x3f800000u, 0x3f800000u, 0x3f800001u, 0x3f800000u};
    for (int m = 0; m < 4; ++m) {
        c = ctx(cws[m]);
        const uint32_t got = bitsf(fto_float(&c, v));
        check(got == want[m], "FST float 1+2^-40, RC %d: %08x", m, got);
        h.u64(got);
    }
    // NaN results are the x87's indefinite, not the host's default NaN
    // (x86 0xfff8..., arm64 0x7ff8...).
    c = ctx(RC_NEAR);
    const double inv = fx87(&c, opaque(0.0) / opaque(0.0));
    check(bits(inv) == 0xfff8000000000000ull, "x87 0/0: the indefinite %016llx",
          (unsigned long long)bits(inv));
    check(bits(recomp_sse_sqrt(opaque(-1.0))) == 0xfff8000000000000ull,
          "SQRTSD -1: the indefinite");
    check(bitsf(recomp_sse_sqrtf(opaque(-4.0f))) == 0xffc00000u, "SQRTSS -4: the indefinite");
    h.f64(inv);
}

// A multiply and an add in one expression must round twice. With contraction
// (clang's default) arm64 fuses them into one FMA and gets -2^-54 here.
static void test_contraction(Hash &h) {
    const double a = opaque(1.0 + 0x1p-27), b = opaque(1.0 - 0x1p-27), c = opaque(-1.0);
    const double r = a * b + c;
    check(r == 0.0, "a*b+c rounds the product first (no FMA): %a", r);
    const float af = opaque(1.0f + 0x1p-13f), bf = opaque(1.0f - 0x1p-13f), cf = opaque(-1.0f);
    const float rf = af * bf + cf;
    check(rf == 0.0f, "float a*b+c: %a", (double)rf);
    h.f64(r);
    h.f64((double)rf);
}

// ------------------------------------------------------------ the libm --

// The inputs: fixed, from a xorshift generator, plus the edges.
struct Rng {
    uint64_t s = 0x9e3779b97f4a7c15ull;
    double unit() { // [0, 1), exactly
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return (double)(s >> 11) * 0x1p-53;
    }
    double in(double lo, double hi) {
        const double u = unit();
        return lo + (hi - lo) * u;
    }
};

struct Libm {
    double (*sin)(double), (*cos)(double), (*tan)(double), (*asin)(double), (*acos)(double),
        (*atan)(double), (*atan2)(double, double), (*exp)(double), (*log)(double), (*log10)(double),
        (*log2)(double), (*log1p)(double), (*expm1)(double), (*pow)(double, double);
};

// Every function over its inputs, and the translator's compositions (FYL2X is
// y * log2(x), FYL2XP1 y * log1p(x) / ln 2, F2XM1 expm1(x * ln 2)). Returns
// the hash; `out`, when given, receives every result for comparison.
static const double PI = 0x1.921fb54442d18p1; // M_PI, which strict C++ on Windows lacks
static const int SAMPLES = 4000;
static const int RESULTS = SAMPLES * 18 + 64;
static uint64_t run_libm(const Libm &m, double *out) {
    Hash h;
    Rng g;
    int n = 0;
    // A NaN counts as one value: which NaN the hardware makes (x86 sets the
    // sign, arm64 and wasm do not) is the caller's to settle, as fx87_exact
    // and ucrt.cpp's math imports do.
    auto put = [&](double v) {
        if (v != v)
            v = (double)NAN;
        h.f64(v);
        if (out)
            out[n] = v;
        ++n;
    };
    for (int i = 0; i < SAMPLES; ++i) {
        const double small = g.in(-10, 10), big = g.in(-1e6, 1e6), unit = g.in(-1, 1);
        put(m.sin(small));
        put(m.cos(big));
        put(m.tan(small));
        put(m.asin(unit));
        put(m.acos(unit));
        put(m.atan(g.in(-100, 100)));
        put(m.atan2(g.in(-10, 10), g.in(-10, 10)));
        put(m.exp(g.in(-700, 700)));
        const double pos = g.in(0, 1e6);
        put(m.log(pos));
        put(m.log10(pos));
        put(m.log2(g.in(0.5, 2)));
        put(m.log1p(g.in(-0.99, 10)));
        put(m.expm1(g.in(-50, 50)));
        put(m.pow(g.in(0, 100), g.in(-20, 20)));
        put(m.pow(-g.in(0, 10), (double)(int)g.in(-9, 9)));
        put(g.in(-100, 100) * m.log2(g.in(0, 1000)));            // FYL2X
        put(g.in(-10, 10) * m.log1p(g.in(-0.29, 0.29)) / M_LN2); // FYL2XP1
        put(m.expm1(g.in(-1, 1) * M_LN2));                       // F2XM1
    }
    const double edges[] = {
        0.0, -0.0, 1.0,   -1.0,   0x1p-1074,        1e-300,           1e300, PI, PI / 2, 1e22,
        0.5, 2.0,  710.0, -745.2, (double)INFINITY, -(double)INFINITY};
    for (double e : edges) {
        const double x = opaque(e);
        put(m.sin(x));
        put(m.cos(x));
        put(m.tan(x));
        put(m.exp(x));
    }
    return h.h;
}

#define LIBM_OF(prefix)                                                                            \
    Libm {                                                                                         \
        prefix##sin, prefix##cos, prefix##tan, prefix##asin, prefix##acos, prefix##atan,           \
            prefix##atan2, prefix##exp, prefix##log, prefix##log10, prefix##log2, prefix##log1p,   \
            prefix##expm1, prefix##pow                                                             \
    }

static double host_sin(double x) {
    return std::sin(x);
}
static double host_cos(double x) {
    return std::cos(x);
}
static double host_tan(double x) {
    return std::tan(x);
}
static double host_asin(double x) {
    return std::asin(x);
}
static double host_acos(double x) {
    return std::acos(x);
}
static double host_atan(double x) {
    return std::atan(x);
}
static double host_atan2(double y, double x) {
    return std::atan2(y, x);
}
static double host_exp(double x) {
    return std::exp(x);
}
static double host_log(double x) {
    return std::log(x);
}
static double host_log10(double x) {
    return std::log10(x);
}
static double host_log2(double x) {
    return std::log2(x);
}
static double host_log1p(double x) {
    return std::log1p(x);
}
static double host_expm1(double x) {
    return std::expm1(x);
}
static double host_pow(double x, double y) {
    return std::pow(x, y);
}

// What the bundled libm gives on x86_64 Linux; every other host must agree.
static const uint64_t BUNDLED_LIBM_HASH = 0x01c07fad3cbb805cull;

static void test_libm(Hash &total) {
    static double ours[RESULTS], host[RESULTS];
    const uint64_t a = run_libm(LIBM_OF(recomp_libm_), ours);
    const uint64_t b = run_libm(LIBM_OF(host_), host);
    int differ = 0;
    for (int i = 0; i < RESULTS; ++i)
        differ += bits(ours[i]) != bits(host[i]) && !(ours[i] != ours[i] && host[i] != host[i]);
    printf("bundled libm hash %016llx\n", (unsigned long long)a);
    printf("host libm hash    %016llx (%d of %d results differ from the bundled libm; information "
           "only)\n",
           (unsigned long long)b, differ, RESULTS);
    check(a == BUNDLED_LIBM_HASH, "the bundled libm computes what it computes on x86_64 (%016llx)",
          (unsigned long long)BUNDLED_LIBM_HASH);
    // A few values every correct libm agrees on.
    check(recomp_libm_sin(0.0) == 0.0 && recomp_libm_cos(0.0) == 1.0 &&
              recomp_libm_exp(0.0) == 1.0 && recomp_libm_log(1.0) == 0.0 &&
              recomp_libm_pow(2.0, 10.0) == 1024.0 && recomp_libm_log2(8.0) == 3.0 &&
              recomp_libm_atan2(0.0, -1.0) == PI,
          "the bundled libm's exact cases");
    total.u64(a);
}

// The hash of the exact part as x86_64 Linux computes it; every host must agree.
static const uint64_t EXACT_HASH = 0x576cfde6d47af9b1ull;

int main() {
    Hash h;
    test_conversions(h);
    test_fscale(h);
    test_rounding(h);
    test_contraction(h);
    const uint64_t exact = h.h;
    test_libm(h);
    printf("exact-semantics hash %016llx\n", (unsigned long long)exact);
    check(exact == EXACT_HASH, "the exact part hashes as on x86_64 (%016llx)",
          (unsigned long long)EXACT_HASH);
    printf("fp determinism hash  %016llx\n", (unsigned long long)h.h);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
