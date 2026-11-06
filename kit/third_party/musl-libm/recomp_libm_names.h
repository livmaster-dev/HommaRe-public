/* recomp_libm_names.h - force-included (-include) before every file here, so
 * the vendored musl sources define recomp_libm_sin and friends instead of the
 * C library's names: the host's own libm stays what it is for everything
 * else, and nothing interposes on anything. The host's <math.h> comes first,
 * under its own names; the sources' later #include <math.h> is then a no-op. */
#ifndef RECOMP_LIBM_NAMES_H
#define RECOMP_LIBM_NAMES_H

#include <math.h>

#define sin recomp_libm_sin
#define cos recomp_libm_cos
#define tan recomp_libm_tan
#define asin recomp_libm_asin
#define acos recomp_libm_acos
#define atan recomp_libm_atan
#define atan2 recomp_libm_atan2
#define exp recomp_libm_exp
#define log recomp_libm_log
#define log10 recomp_libm_log10
#define log2 recomp_libm_log2
#define log1p recomp_libm_log1p
#define expm1 recomp_libm_expm1
#define pow recomp_libm_pow
#define __sin recomp_libm_ksin
#define __cos recomp_libm_kcos
#define __tan recomp_libm_ktan
#define __rem_pio2 recomp_libm_rem_pio2
#define __rem_pio2_large recomp_libm_rem_pio2_large
#define __exp_data recomp_libm_exp_data

#include "recomp_libm.h" /* declares the renamed functions, for the calls between them */

#endif
