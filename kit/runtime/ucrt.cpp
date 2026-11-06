// ucrt.cpp - the Universal C Runtime (api-ms-win-crt-*-l1-1-0.dll) a Visual
// C++ 2015+ program imports instead of linking its C library in.
//
// MSVC 7.1 games (NFS Carbon) ship msvcr71.dll, which the kit translates like
// the game. A 14.x build (Heroes V's 2025 rebuild) imports the UCRT through
// the api-ms-win-crt-* forwarders, which are part of Windows and never ship
// with the game, so this runtime answers them itself, on guest memory:
//
//   - startup and exit (_initterm, the onexit tables, the WinMain command line)
//   - the heap, on the kit's guest allocator
//   - strings, ctype and conversions in the "C" locale, where a narrow byte is
//     the code point of the same value, as the UCRT's C locale treats it
//   - math, in both of MSVC's private conventions: _CIxxx (x87 stack) and
//     _libm_sse2_xxx_precise (XMM0/XMM1)
//   - time, rand/srand, qsort
//
// stdio (FILE, the printf and scanf engines) lives in ucrt_stdio.cpp.
#include "codepage.h"
#include "imports.h"
#include "kernel32_internal.h"
#include "memory.h"
#include "ucrt_internal.h"
#include "win32.h"
#include "../platform/os.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <vector>

namespace {

// ------------------------------------------------------------- helpers --

// A cdecl double argument: two dwords starting at argument slot i.
double arg_f64(X86 *c, int i) {
    return rdf64(c->r[R_ESP] + 4 + 4 * (uint32_t)i);
}
// A cdecl function returning double leaves it in ST(0).
void ret_f64(X86 *c, double v) {
    fpush(c, v);
}

[[noreturn]] void exit_now(X86 *c, uint32_t code) {
    ucrt_exit_process(c, code);
}

// A UTF-8 locale for the wide classifiers: the UCRT classifies wchar_t by
// Unicode whatever the C locale, and the host's "C" locale knows only ASCII.
// C.UTF-8 is glibc's built-in one; macOS has only the named locales.
locale_t unicode_locale() {
    static locale_t loc = [] {
        for (const char *name : {"C.UTF-8", "en_US.UTF-8"})
            if (locale_t l = newlocale(LC_CTYPE_MASK, name, (locale_t)0))
                return l;
        return ucrt_c_locale();
    }();
    return loc;
}

// Guest wchar_t is 16 bits.
uint16_t rdw(uint32_t a) {
    return rd16(a);
}

// ------------------------------------------------------- startup, exit --

// atexit/_crt_atexit registrations, run newest first by exit and _cexit.
std::vector<uint32_t> g_atexit;
bool g_atexit_ran = false;

void run_atexit(X86 *c) {
    if (g_atexit_ran)
        return;
    g_atexit_ran = true;
    for (size_t i = g_atexit.size(); i-- > 0;)
        guest_call(c, g_atexit[i]);
}

// _initterm(first, last): call every non-null function pointer in [first, last).
void u_initterm(X86 *c) {
    uint32_t first = arg(c, 0), last = arg(c, 1);
    for (uint32_t p = first; p < last; p += 4) {
        uint32_t fn = rd32(p);
        if (fn)
            guest_call(c, fn);
    }
}
// _initterm_e: the same, stopping at the first that returns non-zero.
void u_initterm_e(X86 *c) {
    uint32_t first = arg(c, 0), last = arg(c, 1);
    for (uint32_t p = first; p < last; p += 4) {
        uint32_t fn = rd32(p);
        if (!fn)
            continue;
        uint32_t r = guest_call(c, fn);
        if (r) {
            set_eax(c, r);
            return;
        }
    }
    set_eax(c, 0);
}

// _get_narrow_winmain_command_line: the command line after the program name,
// as WinMain's lpCmdLine.
void u_get_narrow_winmain_command_line(X86 *c) {
    get_command_line(c);
    uint32_t p = c->r[R_EAX];
    if (!p) {
        set_eax(c, 0);
        return;
    }
    bool quoted = false;
    for (uint8_t ch; (ch = rd8(p)) != 0; ++p) {
        if (ch == '"')
            quoted = !quoted;
        else if ((ch == ' ' || ch == '\t') && !quoted)
            break;
    }
    while (rd8(p) == ' ' || rd8(p) == '\t')
        ++p;
    set_eax(c, p);
}

// _onexit_table_t { first, last, end }: a module's own atexit list.
void u_initialize_onexit_table(X86 *c) {
    uint32_t t = arg(c, 0);
    if (t) {
        wr32(t, 0);
        wr32(t + 4, 0);
        wr32(t + 8, 0);
    }
    set_eax(c, 0);
}
void u_register_onexit_function(X86 *c) {
    uint32_t t = arg(c, 0), fn = arg(c, 1);
    uint32_t first = rd32(t), last = rd32(t + 4), end = rd32(t + 8);
    if (last == end) {
        uint32_t count = first ? (end - first) / 4 : 0, grown = count ? count * 2 : 32;
        uint32_t block = first ? heap_realloc(first, grown * 4) : heap_alloc(grown * 4, true);
        if (!block) {
            set_eax(c, (uint32_t)-1);
            return;
        }
        last = block + (last - first);
        first = block;
        end = block + grown * 4;
    }
    wr32(last, fn);
    wr32(t, first);
    wr32(t + 4, last + 4);
    wr32(t + 8, end);
    set_eax(c, 0);
}
// _execute_onexit_table is not imported by Heroes V; its exe runs its own table.
void u_crt_atexit(X86 *c) {
    g_atexit.push_back(arg(c, 0));
    set_eax(c, 0);
}
void u_exit(X86 *c) {
    uint32_t code = arg(c, 0);
    run_atexit(c);
    exit_now(c, code);
}
void u__exit(X86 *c) {
    exit_now(c, arg(c, 0));
}
void u_abort(X86 *c) {
    LOGW("ucrt: abort()");
    exit_now(c, 3);
}
void u_terminate(X86 *c) {
    LOGW("ucrt: terminate()");
    exit_now(c, 3);
}
void u_invalid_parameter_noreturn(X86 *c) {
    LOGW("ucrt: _invalid_parameter_noinfo_noreturn (return address %08x)", rd32(c->r[R_ESP]));
    exit_now(c, 0xc0000417u); // STATUS_INVALID_CRUNTIME_PARAMETER
}
void u_cexit(X86 *c) {
    run_atexit(c);
}
void u_zero(X86 *c) {
    set_eax(c, 0);
}
void u_none(X86 *) {}

uint32_t g_fmode = 0x4000; // _O_TEXT
uint32_t g_new_mode = 0;
uint32_t g_commode_addr = 0;

void u_set_fmode(X86 *c) {
    g_fmode = arg(c, 0);
    set_eax(c, 0);
}
void u_p_commode(X86 *c) {
    if (!g_commode_addr)
        g_commode_addr = heap_alloc(4, true);
    set_eax(c, g_commode_addr);
}
void u_set_new_mode(X86 *c) {
    uint32_t old = g_new_mode;
    g_new_mode = arg(c, 0);
    set_eax(c, old);
}
// _configthreadlocale(type): 0 queries; the answer is "per-thread locale off".
void u_configthreadlocale(X86 *c) {
    set_eax(c, 2); // _DISABLE_PER_THREAD_LOCALE
}
// _seh_filter_exe: EXCEPTION_CONTINUE_SEARCH, as for an exception it does not own.
void u_seh_filter_exe(X86 *c) {
    set_eax(c, 0);
}

// ------------------------------------------------------- x87 control --
// _control87 speaks the CRT's abstract control word, not the x87 one.
uint32_t abstract_from_cw(uint16_t cw) {
    uint32_t a = 0;
    if (cw & 0x01) a |= 0x10;    // _EM_INVALID
    if (cw & 0x02) a |= 0x80000; // _EM_DENORMAL
    if (cw & 0x04) a |= 0x08;    // _EM_ZERODIVIDE
    if (cw & 0x08) a |= 0x04;    // _EM_OVERFLOW
    if (cw & 0x10) a |= 0x02;    // _EM_UNDERFLOW
    if (cw & 0x20) a |= 0x01;    // _EM_INEXACT
    a |= (uint32_t)((cw >> 10) & 3) << 8;           // _MCW_RC: same order
    static const uint32_t pc[4] = {0x20000, 0x20000, 0x10000, 0}; // 24, -, 53, 64
    a |= pc[(cw >> 8) & 3];
    return a;
}
uint16_t cw_from_abstract(uint32_t a, uint16_t cw) {
    cw &= (uint16_t)~0x0f3fu;
    if (a & 0x10) cw |= 0x01;
    if (a & 0x80000) cw |= 0x02;
    if (a & 0x08) cw |= 0x04;
    if (a & 0x04) cw |= 0x08;
    if (a & 0x02) cw |= 0x10;
    if (a & 0x01) cw |= 0x20;
    cw |= (uint16_t)(((a >> 8) & 3) << 10);
    uint32_t pc = a & 0x30000;
    cw |= (uint16_t)((pc == 0 ? 3 : pc == 0x10000 ? 2 : 0) << 8);
    return cw;
}
// The same abstract word as MXCSR's exception masks (bits 7-12) and rounding
// field (bits 13-14); precision control has no SSE counterpart, and denormal
// control (_MCW_DN) is not modelled.
uint32_t mxcsr_from_abstract(uint32_t a, uint32_t mxcsr) {
    mxcsr &= ~0x7f80u;
    if (a & 0x10) mxcsr |= 0x0080;    // _EM_INVALID: IM
    if (a & 0x80000) mxcsr |= 0x0100; // _EM_DENORMAL: DM
    if (a & 0x08) mxcsr |= 0x0200;    // _EM_ZERODIVIDE: ZM
    if (a & 0x04) mxcsr |= 0x0400;    // _EM_OVERFLOW: OM
    if (a & 0x02) mxcsr |= 0x0800;    // _EM_UNDERFLOW: UM
    if (a & 0x01) mxcsr |= 0x1000;    // _EM_INEXACT: PM
    mxcsr |= ((a >> 8) & 3u) << 13;   // _MCW_RC: same order as RC
    return mxcsr;
}
// On x86 the UCRT's _control87 and _controlfp_s set the SSE2 control word as
// well as the x87 one (Heroes V's _control87(0xa031f, ~0) rounds its SSE
// arithmetic toward zero), so both change here. The new MXCSR takes effect
// when the shim returns to translated code (imports_dispatch).
uint32_t control87(X86 *c, uint32_t value, uint32_t mask) {
    mask &= 0x000b031fu | 0x80000u | 0x30000u;
    uint32_t cur = abstract_from_cw(c->fpu_cw);
    if (mask) {
        cur = (cur & ~mask) | (value & mask);
        c->fpu_cw = cw_from_abstract(cur, c->fpu_cw);
        c->mxcsr = mxcsr_from_abstract(cur, c->mxcsr);
    }
    return abstract_from_cw(c->fpu_cw);
}
void u_control87(X86 *c) {
    set_eax(c, control87(c, arg(c, 0), arg(c, 1)));
}
// _controlfp_s(unsigned *current, value, mask): errno_t.
void u_controlfp_s(X86 *c) {
    uint32_t out = arg(c, 0);
    uint32_t mask = arg(c, 2) & ~0x80000u; // _controlfp never changes denormal control
    uint32_t v = control87(c, arg(c, 1), mask);
    if (out)
        wr32(out, v);
    set_eax(c, 0);
}

// ---------------------------------------------------------------- heap --
void u_malloc(X86 *c) {
    uint32_t n = arg(c, 0);
    set_eax(c, heap_alloc(n ? n : 1));
}
void u_calloc(X86 *c) {
    uint64_t n = (uint64_t)arg(c, 0) * arg(c, 1);
    set_eax(c, n > 0xffffffffull ? 0 : heap_alloc(n ? (uint32_t)n : 1, true));
}
void u_realloc(X86 *c) {
    uint32_t p = arg(c, 0), n = arg(c, 1);
    if (!p) {
        set_eax(c, heap_alloc(n ? n : 1));
        return;
    }
    if (!n) {
        heap_free(p);
        set_eax(c, 0);
        return;
    }
    set_eax(c, heap_realloc(p, n));
}
// _msize(p): the block's usable size; -1 (with errno EINVAL) for a null pointer.
void u_msize(X86 *c) {
    const uint32_t p = arg(c, 0);
    set_eax(c, p ? heap_size(p) : 0xffffffffu);
}
void u_free(X86 *c) {
    uint32_t p = arg(c, 0);
    if (p && !heap_free(p))
        log_once("ucrt-free-foreign", "ucrt: free(%08x) of a block the heap does not own", p);
}

// --------------------------------------------------------------- ctype --
// The "C" locale: ASCII only; anything above 0x7f is no class at all.
int c_class(int ch, int (*fn)(int)) {
    ch &= 0xff;
    return ch < 0x80 && fn(ch) ? 1 : 0;
}
void u_isdigit(X86 *c) { set_eax(c, c_class((int)arg(c, 0), ::isdigit) ? 4 : 0); }
void u_isspace(X86 *c) { set_eax(c, c_class((int)arg(c, 0), ::isspace) ? 8 : 0); }
void u_isalpha(X86 *c) { set_eax(c, c_class((int)arg(c, 0), ::isalpha) ? 0x103 : 0); }
void u_isalnum(X86 *c) { set_eax(c, c_class((int)arg(c, 0), ::isalnum) ? 0x107 : 0); }
void u_iscntrl(X86 *c) { set_eax(c, c_class((int)arg(c, 0), ::iscntrl) ? 0x20 : 0); }
void u_tolower(X86 *c) {
    int ch = (int)arg(c, 0);
    set_eax(c, (uint32_t)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch));
}
void u_toupper(X86 *c) {
    int ch = (int)arg(c, 0);
    set_eax(c, (uint32_t)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch));
}
int w_class(uint32_t ch, int (*fn)(wint_t, locale_t)) {
    ch &= 0xffff;
    return fn((wint_t)ch, unicode_locale()) ? 1 : 0;
}
void u_iswalpha(X86 *c) { set_eax(c, w_class(arg(c, 0), ::iswalpha_l) ? 0x103 : 0); }
void u_iswalnum(X86 *c) { set_eax(c, w_class(arg(c, 0), ::iswalnum_l) ? 0x107 : 0); }
void u_iswdigit(X86 *c) { set_eax(c, (arg(c, 0) & 0xffff) - '0' < 10 ? 4 : 0); }
void u_iswpunct(X86 *c) { set_eax(c, w_class(arg(c, 0), ::iswpunct_l) ? 0x10 : 0); }
void u_iswprint(X86 *c) { set_eax(c, w_class(arg(c, 0), ::iswprint_l) ? 0x157 : 0); }
void u_iswgraph(X86 *c) { set_eax(c, w_class(arg(c, 0), ::iswgraph_l) ? 0x117 : 0); }

// ------------------------------------------------------------- strings --
uint8_t lower8(uint8_t ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }
uint8_t upper8(uint8_t ch) { return ch >= 'a' && ch <= 'z' ? ch - 32 : ch; }

void u_strncmp(X86 *c) {
    uint32_t a = arg(c, 0), b = arg(c, 1), n = arg(c, 2);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t x = rd8(a + i), y = rd8(b + i);
        if (x != y) {
            set_eax(c, x < y ? (uint32_t)-1 : 1);
            return;
        }
        if (!x)
            break;
    }
    set_eax(c, 0);
}
void u_strncpy(X86 *c) {
    uint32_t d = arg(c, 0), s = arg(c, 1), n = arg(c, 2);
    uint32_t i = 0;
    for (; i < n; ++i) {
        uint8_t ch = rd8(s + i);
        wr8(d + i, ch);
        if (!ch)
            break;
    }
    for (; i < n; ++i)
        wr8(d + i, 0);
    set_eax(c, d);
}
void u_wcsncmp(X86 *c) {
    uint32_t a = arg(c, 0), b = arg(c, 1), n = arg(c, 2);
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t x = rdw(a + 2 * i), y = rdw(b + 2 * i);
        if (x != y) {
            set_eax(c, x < y ? (uint32_t)-1 : 1);
            return;
        }
        if (!x)
            break;
    }
    set_eax(c, 0);
}
uint16_t lowerw(uint16_t ch) {
    return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; // the C locale folds ASCII only
}
void u_wcsicmp(X86 *c) {
    uint32_t a = arg(c, 0), b = arg(c, 1);
    for (uint32_t i = 0;; ++i) {
        uint16_t x = lowerw(rdw(a + 2 * i)), y = lowerw(rdw(b + 2 * i));
        if (x != y) {
            set_eax(c, (uint32_t)((int)x - (int)y));
            return;
        }
        if (!x)
            break;
    }
    set_eax(c, 0);
}
bool in_set(uint32_t set, uint8_t ch) {
    for (uint8_t s; (s = rd8(set)) != 0; ++set)
        if (s == ch)
            return true;
    return false;
}
void u_strspn(X86 *c) {
    uint32_t s = arg(c, 0), set = arg(c, 1), n = 0;
    for (uint8_t ch; (ch = rd8(s + n)) != 0 && in_set(set, ch);)
        ++n;
    set_eax(c, n);
}
void u_strcspn(X86 *c) {
    uint32_t s = arg(c, 0), set = arg(c, 1), n = 0;
    for (uint8_t ch; (ch = rd8(s + n)) != 0 && !in_set(set, ch);)
        ++n;
    set_eax(c, n);
}
uint32_t g_strtok_next = 0;
void u_strtok(X86 *c) {
    uint32_t s = arg(c, 0), set = arg(c, 1);
    if (!s)
        s = g_strtok_next;
    if (!s) {
        set_eax(c, 0);
        return;
    }
    while (rd8(s) && in_set(set, rd8(s)))
        ++s;
    if (!rd8(s)) {
        g_strtok_next = 0;
        set_eax(c, 0);
        return;
    }
    uint32_t token = s;
    while (rd8(s) && !in_set(set, rd8(s)))
        ++s;
    if (rd8(s)) {
        wr8(s, 0);
        g_strtok_next = s + 1;
    } else {
        g_strtok_next = 0;
    }
    set_eax(c, token);
}
void u_strlwr(X86 *c) {
    uint32_t s = arg(c, 0);
    for (uint32_t p = s; rd8(p); ++p)
        wr8(p, lower8(rd8(p)));
    set_eax(c, s);
}
void u_strupr(X86 *c) {
    uint32_t s = arg(c, 0);
    for (uint32_t p = s; rd8(p); ++p)
        wr8(p, upper8(rd8(p)));
    set_eax(c, s);
}

// --------------------------------------------------------- conversions --
// strtol/strtod on a bounded host copy, the end pointer mapped back.
void u_strtol(X86 *c) {
    uint32_t s = arg(c, 0), endp = arg(c, 1);
    std::string text = gm_str(s, 4096);
    char *end = nullptr;
    long v = strtol(text.c_str(), &end, (int)arg(c, 2));
    if (v > INT32_MAX) v = INT32_MAX;
    if (v < INT32_MIN) v = INT32_MIN;
    if (endp)
        wr32(endp, s + (uint32_t)(end - text.c_str()));
    set_eax(c, (uint32_t)(int32_t)v);
}
void u_atoi(X86 *c) {
    std::string text = gm_str(arg(c, 0), 4096);
    long v = strtol(text.c_str(), nullptr, 10);
    if (v > INT32_MAX) v = INT32_MAX;
    if (v < INT32_MIN) v = INT32_MIN;
    set_eax(c, (uint32_t)(int32_t)v);
}
void u_strtod(X86 *c) {
    uint32_t s = arg(c, 0), endp = arg(c, 1);
    std::string text = gm_str(s, 4096);
    char *end = nullptr;
    double v = strtod_l(text.c_str(), &end, ucrt_c_locale());
    if (endp)
        wr32(endp, s + (uint32_t)(end - text.c_str()));
    ret_f64(c, v);
}
void u_atof(X86 *c) {
    std::string text = gm_str(arg(c, 0), 4096);
    ret_f64(c, strtod_l(text.c_str(), nullptr, ucrt_c_locale()));
}
// A guest wide string's leading ASCII run, for the wide numeric parsers.
std::string wide_ascii(uint32_t s) {
    std::string out;
    for (uint32_t i = 0; i < 4096; ++i) {
        uint16_t ch = rdw(s + 2 * i);
        if (!ch || ch > 0x7f)
            break;
        out.push_back((char)ch);
    }
    return out;
}
void u_wtoi(X86 *c) {
    long v = strtol(wide_ascii(arg(c, 0)).c_str(), nullptr, 10);
    if (v > INT32_MAX) v = INT32_MAX;
    if (v < INT32_MIN) v = INT32_MIN;
    set_eax(c, (uint32_t)(int32_t)v);
}
void u_wtof(X86 *c) {
    ret_f64(c, strtod_l(wide_ascii(arg(c, 0)).c_str(), nullptr, ucrt_c_locale()));
}
std::string itoa_text(int32_t value, int radix) {
    if (radix < 2 || radix > 36)
        return std::string();
    bool neg = radix == 10 && value < 0;
    uint32_t v = neg ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
    std::string out;
    do {
        uint32_t d = v % (uint32_t)radix;
        out.push_back((char)(d < 10 ? '0' + d : 'a' + d - 10));
        v /= (uint32_t)radix;
    } while (v);
    if (neg)
        out.push_back('-');
    std::reverse(out.begin(), out.end());
    return out;
}
void u_itoa(X86 *c) {
    uint32_t buf = arg(c, 1);
    std::string t = itoa_text((int32_t)arg(c, 0), (int)arg(c, 2));
    gm_put_str(buf, t.c_str(), (uint32_t)t.size() + 1);
    set_eax(c, buf);
}
void u_itow(X86 *c) {
    uint32_t buf = arg(c, 1);
    std::string t = itoa_text((int32_t)arg(c, 0), (int)arg(c, 2));
    for (size_t i = 0; i <= t.size(); ++i)
        wr16(buf + 2 * (uint32_t)i, i < t.size() ? (uint8_t)t[i] : 0);
    set_eax(c, buf);
}
// mbstowcs(dst, src, n) in the C locale: each byte is the code point.
void u_mbstowcs(X86 *c) {
    uint32_t d = arg(c, 0), s = arg(c, 1), n = arg(c, 2), i = 0;
    for (;; ++i) {
        uint8_t ch = rd8(s + i);
        if (d) {
            if (i >= n)
                break;
            wr16(d + 2 * i, ch);
        }
        if (!ch)
            break;
    }
    set_eax(c, i);
}
// wcstombs(dst, src, n): above 0xff has no C-locale byte -> (size_t)-1.
void u_wcstombs(X86 *c) {
    uint32_t d = arg(c, 0), s = arg(c, 1), n = arg(c, 2), i = 0;
    for (;; ++i) {
        uint16_t ch = rdw(s + 2 * i);
        if (ch > 0xff) {
            set_eax(c, (uint32_t)-1);
            return;
        }
        if (d) {
            if (i >= n)
                break;
            wr8(d + i, (uint8_t)ch);
        }
        if (!ch)
            break;
    }
    set_eax(c, i);
}

// ---------------------------------------------------------------- math --
// cdecl: doubles on the stack, the result in ST(0).
void u_floor(X86 *c) { ret_f64(c, floor(arg_f64(c, 0))); }
void u_ceil(X86 *c) { ret_f64(c, ceil(arg_f64(c, 0))); }
void u_ldexp(X86 *c) { ret_f64(c, ldexp(arg_f64(c, 0), (int)arg(c, 2))); }
void u_frexp(X86 *c) {
    int e = 0;
    double m = frexp(arg_f64(c, 0), &e);
    if (arg(c, 2))
        wr32(arg(c, 2), (uint32_t)e);
    ret_f64(c, m);
}

// The transcendental functions are the bundled libm's (recomp_libm_*, x86.h),
// not the host's: glibc, Apple's libm, the UCRT, bionic and emscripten's musl
// each round them their own way in the last bit, and network peers must
// compute the same. A NaN result is the one Microsoft's CRT gives on x86: a
// NaN argument back (quieted), else the "indefinite" -nan(ind); the host's
// arithmetic would make its own default NaN, whose sign differs between x86
// (set) and arm64 or wasm (clear).
double guest_nan(double r, double x, double y) {
    if (r == r)
        return r;
    uint64_t b = 0xfff8000000000000ull;
    if (x != x)
        memcpy(&b, &x, 8);
    else if (y != y)
        memcpy(&b, &y, 8);
    b |= 1ull << 51;
    memcpy(&r, &b, 8);
    return r;
}

// _CIxxx: the operand(s) on the x87 stack, popped, the result pushed. For
// two operands the compiler loads the first argument first, so it is ST(1).
void ci1(X86 *c, double (*fn)(double)) {
    double x = ST(c, 0);
    fdrop(c);
    fpush(c, guest_nan(fn(x), x, 0.0));
}
void ci2(X86 *c, double (*fn)(double, double)) {
    double y = ST(c, 0), x = ST(c, 1);
    fdrop(c);
    fdrop(c);
    fpush(c, guest_nan(fn(x, y), x, y));
}
void u_CIacos(X86 *c) { ci1(c, recomp_libm_acos); }
void u_CIatan(X86 *c) { ci1(c, recomp_libm_atan); }
void u_CIatan2(X86 *c) { ci2(c, recomp_libm_atan2); }
void u_CIpow(X86 *c) { ci2(c, recomp_libm_pow); }
// _ftol: ST(0), popped, truncated toward zero into EDX:EAX; out of range (and
// NaN) gives the integer indefinite 0x8000000000000000, as FISTP does.
void u_ftol(X86 *c) {
    const double x = ST(c, 0);
    fdrop(c);
    const int64_t v = x == x && x < 9223372036854775808.0 && x >= -9223372036854775808.0 ? (int64_t)x : INT64_MIN;
    c->r[0] = (uint32_t)(uint64_t)v;
    c->r[2] = (uint32_t)((uint64_t)v >> 32);
}
void u_CIfmod(X86 *c) { ci2(c, ::fmod); }

// _libm_sse2_xxx_precise: the argument in XMM0 (the second in XMM1), the
// result in XMM0. The bundled libm, as above.
void sse1(X86 *c, double (*fn)(double)) {
    const double x = xmm_f64(c, 0);
    xmm_set_f64(c, 0, guest_nan(fn(x), x, 0.0));
}
void sse2(X86 *c, double (*fn)(double, double)) {
    const double x = xmm_f64(c, 0), y = xmm_f64(c, 1);
    xmm_set_f64(c, 0, guest_nan(fn(x, y), x, y));
}
void u_sse_sin(X86 *c) { sse1(c, recomp_libm_sin); }
void u_sse_cos(X86 *c) { sse1(c, recomp_libm_cos); }
void u_sse_tan(X86 *c) { sse1(c, recomp_libm_tan); }
void u_sse_asin(X86 *c) { sse1(c, recomp_libm_asin); }
void u_sse_acos(X86 *c) { sse1(c, recomp_libm_acos); }
void u_sse_atan(X86 *c) { sse1(c, recomp_libm_atan); }
void u_sse_exp(X86 *c) { sse1(c, recomp_libm_exp); }
void u_sse_log(X86 *c) { sse1(c, recomp_libm_log); }
void u_sse_log10(X86 *c) { sse1(c, recomp_libm_log10); }
void u_sse_sqrt(X86 *c) { xmm_set_f64(c, 0, recomp_sse_sqrt(xmm_f64(c, 0))); }
void u_sse_pow(X86 *c) { sse2(c, recomp_libm_pow); }

// ---------------------------------------------------------------- time --
// The guest's struct tm: nine ints, in the C order.
uint32_t put_tm(uint32_t slot, const struct tm &t) {
    const int v[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon,
                      t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    for (int i = 0; i < 9; ++i)
        wr32(slot + 4 * i, (uint32_t)v[i]);
    return slot;
}
struct tm get_tm(uint32_t a) {
    struct tm t = {};
    int *v[9] = {&t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon,
                 &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
    for (int i = 0; i < 9; ++i)
        *v[i] = (int)rd32(a + 4 * i);
    return t;
}
uint32_t g_tm_slot = 0, g_asctime_slot = 0;
uint32_t tm_slot() {
    if (!g_tm_slot)
        g_tm_slot = heap_alloc(36, true);
    return g_tm_slot;
}
void u_time32(X86 *c) {
    uint32_t now = (uint32_t)time(nullptr);
    if (arg(c, 0))
        wr32(arg(c, 0), now);
    set_eax(c, now);
}
void u_time64(X86 *c) {
    int64_t now = (int64_t)time(nullptr);
    if (arg(c, 0))
        wr64(arg(c, 0), (uint64_t)now);
    set_eax64(c, (uint64_t)now);
}
void u_localtime32(X86 *c) {
    if (!arg(c, 0)) {
        set_eax(c, 0);
        return;
    }
    const int64_t t = (int32_t)rd32(arg(c, 0));
    struct tm out;
    set_eax(c, os_localtime(t, &out) == 0 ? put_tm(tm_slot(), out) : 0);
}
void u_gmtime64(X86 *c) {
    if (!arg(c, 0)) {
        set_eax(c, 0);
        return;
    }
    const int64_t t = (int64_t)rd64(arg(c, 0));
    struct tm out;
    set_eax(c, os_gmtime(t, &out) == 0 ? put_tm(tm_slot(), out) : 0);
}
void u_difftime32(X86 *c) {
    ret_f64(c, (double)(int32_t)arg(c, 0) - (double)(int32_t)arg(c, 1));
}
// clock(): milliseconds since the process started (CLOCKS_PER_SEC = 1000).
void u_clock(X86 *c) {
    static const uint64_t start = os_monotonic_ns();
    set_eax(c, (uint32_t)((os_monotonic_ns() - start) / 1000000u));
}
void u_wasctime(X86 *c) {
    struct tm t = get_tm(arg(c, 0));
    char text[64];
    strftime(text, sizeof text, "%a %b %d %H:%M:%S %Y\n", &t);
    if (!g_asctime_slot)
        g_asctime_slot = heap_alloc(64, true);
    for (int i = 0; i < 26; ++i)
        wr16(g_asctime_slot + 2 * i, (uint8_t)text[i]);
    set_eax(c, g_asctime_slot);
}

// ------------------------------------------------------ rand, qsort ... --
uint32_t g_rand_seed = 1;
void u_srand(X86 *c) {
    g_rand_seed = arg(c, 0);
}
void u_rand(X86 *c) {
    g_rand_seed = g_rand_seed * 214013u + 2531011u;
    set_eax(c, (g_rand_seed >> 16) & 0x7fffu);
}
// qsort(base, count, width, compare): a stable merge sort over a guest copy,
// so the comparator only ever sees guest addresses of whole elements and an
// inconsistent one cannot take the sort outside the array.
void u_qsort(X86 *c) {
    uint32_t base = arg(c, 0), count = arg(c, 1), width = arg(c, 2), cmp = arg(c, 3);
    if (count < 2 || !width)
        return;
    uint64_t bytes = (uint64_t)count * width;
    uint32_t copy = bytes <= 0xffffffffull ? heap_alloc((uint32_t)bytes) : 0;
    if (!copy) {
        LOGW("ucrt: qsort of %u x %u bytes: no memory for the copy", count, width);
        return;
    }
    memcpy(gm_ptr(copy), gm_ptr(base), (size_t)bytes);
    std::vector<uint32_t> order(count);
    for (uint32_t i = 0; i < count; ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return (int32_t)guest_call(c, cmp, copy + a * width, copy + b * width) < 0;
    });
    for (uint32_t i = 0; i < count; ++i)
        memcpy(gm_ptr(base + i * width), gm_ptr(copy + order[i] * width), width);
    heap_free(copy);
}
void u_remove(X86 *c) {
    std::string path = win32_host_path_op(win32cp::ansi_to_utf8(gm_str(arg(c, 0))), WIN32_FILE_DELETE);
    set_eax(c, !path.empty() && os_unlink(path.c_str()) == 0 ? 0 : (uint32_t)-1);
}

#define RT "api-ms-win-crt-runtime-l1-1-0.dll"
#define HEAP "api-ms-win-crt-heap-l1-1-0.dll"
#define STR "api-ms-win-crt-string-l1-1-0.dll"
#define CONV "api-ms-win-crt-convert-l1-1-0.dll"
#define MATH "api-ms-win-crt-math-l1-1-0.dll"
#define TIME "api-ms-win-crt-time-l1-1-0.dll"
#define UTIL "api-ms-win-crt-utility-l1-1-0.dll"
#define FS "api-ms-win-crt-filesystem-l1-1-0.dll"
#define LOC "api-ms-win-crt-locale-l1-1-0.dll"
const ImportShim shims[] = {
    {RT, "_initterm", ARGC_CDECL, u_initterm},
    {RT, "_initterm_e", ARGC_CDECL, u_initterm_e},
    {RT, "_get_narrow_winmain_command_line", ARGC_CDECL, u_get_narrow_winmain_command_line},
    {RT, "_configure_narrow_argv", ARGC_CDECL, u_zero},
    {RT, "_initialize_narrow_environment", ARGC_CDECL, u_zero},
    {RT, "_initialize_onexit_table", ARGC_CDECL, u_initialize_onexit_table},
    {RT, "_register_onexit_function", ARGC_CDECL, u_register_onexit_function},
    {RT, "_crt_atexit", ARGC_CDECL, u_crt_atexit},
    {RT, "_register_thread_local_exe_atexit_callback", ARGC_CDECL, u_none},
    {RT, "_set_app_type", ARGC_CDECL, u_none},
    {RT, "_seh_filter_exe", ARGC_CDECL, u_seh_filter_exe},
    {RT, "_controlfp_s", ARGC_CDECL, u_controlfp_s},
    {RT, "_control87", ARGC_CDECL, u_control87},
    {RT, "exit", ARGC_CDECL, u_exit},
    {RT, "_exit", ARGC_CDECL, u__exit},
    {RT, "abort", ARGC_CDECL, u_abort},
    {RT, "terminate", ARGC_CDECL, u_terminate},
    {RT, "_invalid_parameter_noinfo_noreturn", ARGC_CDECL, u_invalid_parameter_noreturn},
    {RT, "_cexit", ARGC_CDECL, u_cexit},
    {RT, "_c_exit", ARGC_CDECL, u_none},
    {"api-ms-win-crt-stdio-l1-1-0.dll", "_set_fmode", ARGC_CDECL, u_set_fmode},
    {"api-ms-win-crt-stdio-l1-1-0.dll", "__p__commode", ARGC_CDECL, u_p_commode},
    {HEAP, "malloc", ARGC_CDECL, u_malloc},
    {HEAP, "calloc", ARGC_CDECL, u_calloc},
    {HEAP, "realloc", ARGC_CDECL, u_realloc},
    {HEAP, "free", ARGC_CDECL, u_free},
    {HEAP, "_msize", ARGC_CDECL, u_msize},
    {HEAP, "_set_new_mode", ARGC_CDECL, u_set_new_mode},
    {LOC, "_configthreadlocale", ARGC_CDECL, u_configthreadlocale},
    {MATH, "__setusermatherr", ARGC_CDECL, u_none},
    {STR, "isdigit", ARGC_CDECL, u_isdigit},
    {STR, "isspace", ARGC_CDECL, u_isspace},
    {STR, "isalpha", ARGC_CDECL, u_isalpha},
    {STR, "isalnum", ARGC_CDECL, u_isalnum},
    {STR, "iscntrl", ARGC_CDECL, u_iscntrl},
    {STR, "tolower", ARGC_CDECL, u_tolower},
    {STR, "toupper", ARGC_CDECL, u_toupper},
    {STR, "iswalpha", ARGC_CDECL, u_iswalpha},
    {STR, "iswalnum", ARGC_CDECL, u_iswalnum},
    {STR, "iswdigit", ARGC_CDECL, u_iswdigit},
    {STR, "iswpunct", ARGC_CDECL, u_iswpunct},
    {STR, "iswprint", ARGC_CDECL, u_iswprint},
    {STR, "iswgraph", ARGC_CDECL, u_iswgraph},
    {STR, "strncmp", ARGC_CDECL, u_strncmp},
    {STR, "strncpy", ARGC_CDECL, u_strncpy},
    {STR, "wcsncmp", ARGC_CDECL, u_wcsncmp},
    {STR, "_wcsicmp", ARGC_CDECL, u_wcsicmp},
    {STR, "strspn", ARGC_CDECL, u_strspn},
    {STR, "strcspn", ARGC_CDECL, u_strcspn},
    {STR, "strtok", ARGC_CDECL, u_strtok},
    {STR, "_strlwr", ARGC_CDECL, u_strlwr},
    {STR, "_strupr", ARGC_CDECL, u_strupr},
    {CONV, "strtol", ARGC_CDECL, u_strtol},
    {CONV, "atoi", ARGC_CDECL, u_atoi},
    {CONV, "strtod", ARGC_CDECL, u_strtod},
    {CONV, "atof", ARGC_CDECL, u_atof},
    {CONV, "_wtoi", ARGC_CDECL, u_wtoi},
    {CONV, "_wtol", ARGC_CDECL, u_wtoi},
    {CONV, "_wtof", ARGC_CDECL, u_wtof},
    {CONV, "_itoa", ARGC_CDECL, u_itoa},
    {CONV, "_itow", ARGC_CDECL, u_itow},
    {CONV, "mbstowcs", ARGC_CDECL, u_mbstowcs},
    {CONV, "wcstombs", ARGC_CDECL, u_wcstombs},
    {MATH, "floor", ARGC_CDECL, u_floor},
    {MATH, "ceil", ARGC_CDECL, u_ceil},
    {MATH, "ldexp", ARGC_CDECL, u_ldexp},
    {MATH, "frexp", ARGC_CDECL, u_frexp},
    {MATH, "_CIacos", ARGC_CDECL, u_CIacos},
    {MATH, "_CIatan2", ARGC_CDECL, u_CIatan2},
    {MATH, "_CIpow", ARGC_CDECL, u_CIpow},
    {MATH, "_ftol", ARGC_CDECL, u_ftol},
    {MATH, "_CIfmod", ARGC_CDECL, u_CIfmod},
    {MATH, "_libm_sse2_sin_precise", ARGC_CDECL, u_sse_sin},
    {MATH, "_libm_sse2_cos_precise", ARGC_CDECL, u_sse_cos},
    {MATH, "_libm_sse2_tan_precise", ARGC_CDECL, u_sse_tan},
    {MATH, "_libm_sse2_asin_precise", ARGC_CDECL, u_sse_asin},
    {MATH, "_libm_sse2_acos_precise", ARGC_CDECL, u_sse_acos},
    {MATH, "_libm_sse2_atan_precise", ARGC_CDECL, u_sse_atan},
    {MATH, "_libm_sse2_exp_precise", ARGC_CDECL, u_sse_exp},
    {MATH, "_libm_sse2_log_precise", ARGC_CDECL, u_sse_log},
    {MATH, "_libm_sse2_log10_precise", ARGC_CDECL, u_sse_log10},
    {MATH, "_libm_sse2_sqrt_precise", ARGC_CDECL, u_sse_sqrt},
    {MATH, "_libm_sse2_pow_precise", ARGC_CDECL, u_sse_pow},
    {TIME, "_time32", ARGC_CDECL, u_time32},
    {TIME, "_time64", ARGC_CDECL, u_time64},
    {TIME, "_localtime32", ARGC_CDECL, u_localtime32},
    {TIME, "_gmtime64", ARGC_CDECL, u_gmtime64},
    {TIME, "_difftime32", ARGC_CDECL, u_difftime32},
    {TIME, "clock", ARGC_CDECL, u_clock},
    {TIME, "_wasctime", ARGC_CDECL, u_wasctime},
    {UTIL, "rand", ARGC_CDECL, u_rand},
    {UTIL, "srand", ARGC_CDECL, u_srand},
    {UTIL, "qsort", ARGC_CDECL, u_qsort},
    {FS, "remove", ARGC_CDECL, u_remove},
    {FS, "_lock_file", ARGC_CDECL, u_none},
    {FS, "_unlock_file", ARGC_CDECL, u_none},
};
} // namespace

[[noreturn]] void ucrt_exit_process(X86 *c, uint32_t code) {
    c->r[R_ESP] -= 8;
    wr32(c->r[R_ESP], 0);
    wr32(c->r[R_ESP] + 4, code);
    imports_dispatch(c, imports_alloc_trampoline("KERNEL32.dll", "ExitProcess", nullptr, 1));
    abort(); // ExitProcess longjmps out of the run; reaching here means it could not
}

locale_t ucrt_c_locale() {
    static locale_t loc = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    return loc;
}

void ucrt_register() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
    ucrt_stdio_register();
    vcruntime_register();
    msvcp140_register();
}
