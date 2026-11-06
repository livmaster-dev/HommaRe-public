/* recomp_libm.h - the transcendental functions the guest's arithmetic uses
 * (x87 FSIN, FCOS, FPTAN, FPATAN, FYL2X, FYL2XP1, F2XM1 and the CRT's math
 * imports), from one implementation on every host: musl's (README.md).
 * The host C libraries differ in the last bit of these - glibc, Apple's libm,
 * the UCRT, bionic and emscripten's musl each have their own - and a game
 * whose peers compute the same state from the same commands must not. The
 * correctly rounded operations (sqrt, floor, ceil, trunc, fmod, ldexp, frexp,
 * scalbn) are the same everywhere and stay the host's. */
#ifndef RECOMP_LIBM_H
#define RECOMP_LIBM_H

#ifdef __cplusplus
extern "C" {
#endif

double recomp_libm_sin(double x);
double recomp_libm_cos(double x);
double recomp_libm_tan(double x);
double recomp_libm_asin(double x);
double recomp_libm_acos(double x);
double recomp_libm_atan(double x);
double recomp_libm_atan2(double y, double x);
double recomp_libm_exp(double x);
double recomp_libm_log(double x);
double recomp_libm_log10(double x);
double recomp_libm_log2(double x);
double recomp_libm_log1p(double x);
double recomp_libm_expm1(double x);
double recomp_libm_pow(double x, double y);

#ifdef __cplusplus
}
#endif

#endif
