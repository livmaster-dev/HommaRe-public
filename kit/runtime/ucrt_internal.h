// ucrt_internal.h - the Visual C++ 2015+ runtime shims (ucrt*.cpp,
// vcruntime.cpp, msvcp140.cpp) and what they share.
#pragma once
#include "guest.h"
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif
#ifdef _WIN32
#include <wctype.h>
// The Windows C runtime spells POSIX's locale objects with an underscore and
// builds a locale for one category at a time rather than from a mask.
typedef _locale_t locale_t;
#define LC_CTYPE_MASK LC_CTYPE
#define LC_ALL_MASK LC_ALL
inline locale_t newlocale(int category, const char *name, locale_t) {
    return _create_locale(category, name);
}
#define strtod_l _strtod_l
#define iswalpha_l _iswalpha_l
#define iswalnum_l _iswalnum_l
#define iswpunct_l _iswpunct_l
#define iswprint_l _iswprint_l
#define iswgraph_l _iswgraph_l
#endif

// Registers every UCRT, VCRUNTIME140 and MSVCP140 table. Called by imports_init().
void ucrt_register();
void ucrt_stdio_register();
void vcruntime_register();
void msvcp140_register();

// Opens a guest path with a Windows fopen mode and returns the guest FILE *
// (0 on failure), for MSVCP140's std::_Fiopen as much as fopen itself.
uint32_t ucrt_fopen(const std::string &guest_path, const std::string &mode);

// Ends the process with `code` through KERNEL32's own ExitProcess shim, from
// any CRT shim (exit, abort, terminate, a pure virtual call...). The code and
// a dead return address are pushed so ExitProcess reads them as its call.
[[noreturn]] void ucrt_exit_process(X86 *c, uint32_t code);

// The host's "C" locale as an object, for strtod_l and friends: Apple's
// libc takes a null locale_t as "C", glibc's dereferences it.
locale_t ucrt_c_locale();
