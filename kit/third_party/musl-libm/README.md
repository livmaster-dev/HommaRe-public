# musl-libm

The double-precision transcendental functions the guest's arithmetic uses,
from one implementation on every host, so that network peers on different
systems compute the same game. The host C libraries (glibc, Apple's libm, the
UCRT, bionic, emscripten's musl) all return results within an ulp or so, but
not the same ulp; a game whose peers each compute the state from the commands
they exchange cannot afford even that.

The runtime calls these as `recomp_libm_*` (`runtime/x86.h`): the translator's
FSIN, FCOS, FPTAN, FSINCOS, FPATAN, FYL2X, FYL2XP1 and F2XM1, and the CRT
imports `_CIacos`, `_CIatan`, `_CIatan2`, `_CIpow` and `_libm_sse2_*_precise`
(`runtime/ucrt.cpp`). The correctly rounded operations (sqrt, floor, ceil,
trunc, fmod, ldexp, frexp, scalbn) give the same bits in every C library and
stay the host's.

## Source

musl 1.2.6's `src/math`, as emscripten 6.0.5 ships it
(`system/lib/libc/musl/src/math`), unmodified except as noted:

- `sin.c`, `cos.c`, `tan.c`, `__sin.c`, `__cos.c`, `__tan.c`, `__rem_pio2.c`,
  `__rem_pio2_large.c`, `asin.c`, `acos.c`, `atan.c`, `atan2.c`, `log10.c`,
  `log1p.c`, `expm1.c`: musl's, from FreeBSD's msun (Sun's fdlibm).
- `pow_small.c`, `log_small.c`, `log2_small.c`: the fdlibm-derived `pow`,
  `log` and `log2` emscripten keeps beside musl's table-driven ones. These were
  chosen over musl's newer `pow.c`, `log.c` and `log2.c` because those take a
  different path where the compiler defines `__FP_FAST_FMA` (arm64 does,
  x86_64 does not), which would give the two architectures different answers.
- `exp.c`, `exp_data.c`, `exp_data.h`: musl's (Arm's optimized-routines),
  which has no FMA path. `exp_data.h` includes `libm.h` instead of musl's
  internal `<features.h>`, for `hidden`.

Ours: `libm.h` stands in for musl's internal `src/internal/libm.h` with only
what these files use (its definitions are musl's); `recomp_libm_names.h` is
force-included before each file (`runtime/CMakeLists.txt`) and renames every
function to `recomp_libm_*`, so the host's own libm is never replaced or
interposed; `recomp_libm.h` declares them.

Deterministic as built here: every operation is a plain IEEE double operation
rounded once (`FLT_EVAL_METHOD` 0, which `libm.h` checks, and no FMA
contraction, which `CMakeLists.txt` turns off for every target), in the default
rounding mode the runtime never changes. `runtime/tests/fp_determinism_tests.cpp`
hashes a few thousand results and fails on a host where the hash differs.

## License

MIT; see LICENSE (musl's COPYRIGHT, and its note on the math code's origins).
The Sun Microsystems, FreeBSD and Arm notices are kept in the source files.
