/* native/native_screens.c - front-end screens the host draws natively over
 * the game (iOS: SwiftUI, the game repository's native-screens/), on while
 * RECOMP_NATIVE_SCREENS=1: the phone's menus. This file is what they share;
 * each screen is a file of its own (native/screens_*.c) and a row of the
 * table below.
 *
 * The game keeps such a screen alive and works it as ever: it builds it, its
 * lists, its messages and its rules. Only its root window is not drawn
 * (homm5_native_screens_draw, from native/phone_hud.c's CWindow::Draw hook),
 * and each frame the root would have drawn this file reads the screen into a
 * JSON document (runtime/native_seam.h recomp_screen_state) and applies the
 * native view's commands, through the screen's own paths: its game messages
 * ("create", "back", "sortbyname"...), its list's selection, its rows' arrows.
 * Nothing here decides what the game allows; what the document says enabled
 * is what the game has enabled.
 *
 * Covered (a message box or another screen above it) or gone, the screen has
 * no document: the host hides its view and the game draws what is on top.
 * The base game's addresses (research notes phone-menus/native/re-*.md). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include "native_screens.h"
#include "native_seam.h"
#include "x86.h"

#ifdef RECOMP_OVERRIDE_HEADER
#include RECOMP_OVERRIDE_HEADER
#endif
#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif

int mods_settings_file_get(const char *key, int64_t *out); /* kit/mods/settings_file.h */

#ifndef NS_PAK_DIRS
#define NS_PAK_DIRS "data,DataLS"
#endif

/* On by the launcher's Menus choice ("host.ui/native_menus": 1 native, 2 the
 * game's own; its default by device is sdl/main.cpp's), read once, at the
 * game's start; RECOMP_NATIVE_SCREENS=1/0 overrides it. */
static int64_t now_ms(void);

/* RECOMP_NATIVE_SCREENS_OFF_AFTER=<s>: native screens go off for good that
 * many seconds after the first look, as if never on (a measurement: one run,
 * the same scene with and without them). */
static int switched_off(void) {
    static int64_t at = -1;
    if (at < 0) {
        const char *v = getenv("RECOMP_NATIVE_SCREENS_OFF_AFTER");
        at = v && *v ? now_ms() + (int64_t)atoi(v) * 1000 : 0;
    }
    if (!at || now_ms() < at)
        return 0;
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "[screens] native screens switched off (RECOMP_NATIVE_SCREENS_OFF_AFTER)\n");
    }
    return 1;
}

static int screens_on(void) {
    static int on = -1;
    if (on > 0 && switched_off())
        return 0;
    if (on < 0) {
        const char *v = getenv("RECOMP_NATIVE_SCREENS");
        int64_t chosen = 0;
        if (v && *v)
            on = *v == '1';
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        else /* only iOS has the native views (platform_ui_ios.mm) */
            on = mods_settings_file_get("host.ui/native_menus", &chosen) && chosen == 1;
#else
        else
            on = 0;
        (void)chosen;
#endif
        if (on) {
            fprintf(stderr, "[screens] native screens on\n");
            /* The paks this game mounts, in order, for the views' pictures
             * (native-screens/GameArt.swift): the base game and HoF share one
             * install, but each reads its own set. */
            setenv("RECOMP_NATIVE_PAK_DIRS", NS_PAK_DIRS, 1);
        }
    }
    return on;
}

int ns_trace(void) {
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_TRACE_NATIVE_SCREENS") != NULL;
    return on;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int ns_screens_on(void) {
    return screens_on();
}

/* ---------------------------------------------- between the two threads -- */

/* The document the game thread published last and the commands the host
 * thread queued, under one spin lock (both sides hold it for a copy only). */
static volatile int g_lock;
static char *g_doc;          /* NUL-terminated, malloc'd */
static int g_doc_len;
static unsigned g_doc_serial; /* moves on with each new document */
static int64_t g_doc_ms;      /* when the game thread last stood by it */
static int g_doc_shown;       /* 0: no native screen shows */

static int g_stats_docs; /* documents that changed (stats) */

enum { QUEUE = 32 };
static char *g_queue[QUEUE];
static int g_queue_head, g_queue_count;

static void lock(void) {
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
        ;
}
static void unlock(void) {
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

/* A document not stood by for this long is gone: a safety net only. A screen
 * that leaves is noticed by the next window drawn (homm5_native_screens_draw);
 * a game that stops drawing a while (the app's keyboard up, a long frame)
 * keeps its document. */
enum { STALE_MS = 5000 };

int recomp_screen_state(char *buf, int cap, unsigned *serial) {
    if (!screens_on())
        return -1;
    lock();
    int n;
    if (!g_doc_shown || !g_doc || now_ms() - g_doc_ms > STALE_MS) {
        n = -1;
    } else if (*serial == g_doc_serial) {
        n = 0;
    } else {
        n = g_doc_len;
        if (n < cap) {
            memcpy(buf, g_doc, (size_t)n + 1);
            *serial = g_doc_serial;
        }
    }
    unlock();
    return n;
}

void recomp_screen_command(const char *command) {
    if (!screens_on() || !command)
        return;
    char *copy = strdup(command);
    if (!copy)
        return;
    lock();
    if (g_queue_count < QUEUE) {
        g_queue[(g_queue_head + g_queue_count++) % QUEUE] = copy;
        copy = NULL;
    }
    unlock();
    free(copy); /* a full queue drops it */
}

/* The next queued command (the caller frees it), or NULL. */
static char *next_command(void) {
    char *c = NULL;
    lock();
    if (g_queue_count) {
        c = g_queue[g_queue_head];
        g_queue_head = (g_queue_head + 1) % QUEUE;
        --g_queue_count;
    }
    unlock();
    return c;
}

/* Publishes `doc` if it differs from the last one; either way the screen is
 * shown and its document current. */
static void publish(const char *doc, int len) {
    lock();
    if (!g_doc || g_doc_len != len || memcmp(g_doc, doc, (size_t)len) != 0) {
        char *copy = (char *)malloc((size_t)len + 1);
        if (copy) {
            memcpy(copy, doc, (size_t)len + 1);
            free(g_doc);
            g_doc = copy;
            g_doc_len = len;
            ++g_doc_serial;
            ++g_stats_docs;
        }
    }
    g_doc_shown = 1;
    g_doc_ms = now_ms();
    unlock();
}

static void withdraw(void) {
    lock();
    g_doc_shown = 0;
    unlock();
}

/* A press that starts loading (ns_leave): the screen it was made on, and
 * when. The game keeps drawing that screen while it loads, so its document
 * is held back until another screen or a question comes (or LEAVE_MS, if
 * the game stayed without a word). */
enum { LEAVE_MS = 30000 };
static int g_leave_pending;
static uint32_t g_left_screen;
static int64_t g_left_ms;

void ns_leave(void) {
    g_leave_pending = 1;
    withdraw();
}

/* Whether `screen`'s document is held back now. */
static int held_back(uint32_t screen) {
    if (!g_left_screen)
        return 0;
    if (screen != g_left_screen || ns_question_box() || now_ms() - g_left_ms > LEAVE_MS) {
        g_left_screen = 0;
        return 0;
    }
    return 1;
}

/* --------------------------------------------------------- the document -- */

void j_raw(Json *j, const char *s, int n) {
    if (j->len + n + 1 > j->cap) {
        int cap = j->cap ? j->cap * 2 : 4096;
        while (cap < j->len + n + 1)
            cap *= 2;
        char *s2 = (char *)realloc(j->s, (size_t)cap);
        if (!s2)
            return;
        j->s = s2;
        j->cap = cap;
    }
    memcpy(j->s + j->len, s, (size_t)n);
    j->len += n;
    j->s[j->len] = 0;
}

void j_printf(Json *j, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0)
        j_raw(j, tmp, n < (int)sizeof tmp ? n : (int)sizeof tmp - 1);
}

/* A JSON string of UTF-8 `s`. */
void j_str(Json *j, const char *s) {
    j_lit(j, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            const char e[2] = {'\\', (char)*p};
            j_raw(j, e, 2);
        } else if (*p == '\n') {
            j_lit(j, "\\n");
        } else if (*p < 0x20) {
            j_printf(j, "\\u%04x", *p);
        } else {
            j_raw(j, (const char *)p, 1);
        }
    }
    j_lit(j, "\"");
}

/* Guest UTF-16 text, n units at `at`, into UTF-8 `out` with the game's markup
 * left out: its tags (<h3>, <color_disabled>, <value=...>) go, <br> becomes a
 * new line, and runs of spaces and new lines close up. */
void ns_text_utf8(uint32_t at, uint32_t n, char *out, size_t cap) {
    size_t o = 0;
    int in_tag = 0;
    char tag[8];
    int tag_len = 0;
    for (uint32_t i = 0; i < n && o + 4 < cap; ++i) {
        uint32_t u = rd16(at + 2u * i);
        if (u >= 0xd800 && u < 0xdc00 && i + 1 < n) {
            const uint32_t lo = rd16(at + 2u * (i + 1));
            if (lo >= 0xdc00 && lo < 0xe000) {
                u = 0x10000u + ((u - 0xd800u) << 10) + (lo - 0xdc00u);
                ++i;
            }
        }
        if (in_tag) {
            if (u == '>') {
                in_tag = 0;
                tag[tag_len < 7 ? tag_len : 7] = 0;
                if ((strcmp(tag, "br") == 0 || strcmp(tag, "br/") == 0) && o && out[o - 1] != '\n')
                    out[o++] = '\n';
            } else if (tag_len < 7) {
                tag[tag_len++] = (char)(u < 0x80 ? u : '?');
            }
            continue;
        }
        if (u == '<') {
            in_tag = 1;
            tag_len = 0;
            continue;
        }
        if (u == '\r')
            continue;
        if ((u == ' ' || u == '\t') && (!o || out[o - 1] == ' ' || out[o - 1] == '\n'))
            continue;
        if (u == '\n' && (!o || out[o - 1] == '\n'))
            continue;
        if (u < 0x80) {
            out[o++] = (char)(u == '\t' ? ' ' : u);
        } else if (u < 0x800) {
            out[o++] = (char)(0xc0 | (u >> 6));
            out[o++] = (char)(0x80 | (u & 0x3f));
        } else if (u < 0x10000) {
            out[o++] = (char)(0xe0 | (u >> 12));
            out[o++] = (char)(0x80 | ((u >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (u & 0x3f));
        } else {
            out[o++] = (char)(0xf0 | (u >> 18));
            out[o++] = (char)(0x80 | ((u >> 12) & 0x3f));
            out[o++] = (char)(0x80 | ((u >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (u & 0x3f));
        }
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '\n'))
        --o;
    out[o] = 0;
}

/* ------------------------------------------------------------ commands -- */

/* A command's string field `key` into out ("" when it has none). The
 * commands are the native views' own, flat: {"do":"press","name":"create"}. */
void ns_cmd_str(const char *cmd, const char *key, char *out, size_t cap) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(cmd, pat);
    size_t n = 0;
    if (p)
        for (p += strlen(pat); *p && *p != '"' && n + 1 < cap; ++p)
            out[n++] = *p;
    out[n] = 0;
}

int ns_cmd_int(const char *cmd, const char *key, int fallback) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(cmd, pat);
    return p ? atoi(p + strlen(pat)) : fallback;
}

/* ------------------------------------------------------- guest calls -- */

/* The base game's; an expansion has native screens once its addresses.h
 * names them (not mapped yet): until then the hook below has no table. */
#ifndef RECOMP_ADDRESS_HEADER
#define NS_SCREEN_STACK 0x0114b2e4 /* std::list<IScreen*>: the sentinel; top = sentinel->prev */
#define NS_DIFFICULTY 0x00b97bd0   /* fastcall ECX index: the difficulty's table entry */
#define NS_TEXT 0x00a09de0         /* fastcall ECX a text ref: const wstr* */
#define NS_FREE 0x005e40c0         /* free, cdecl */
#endif


uint32_t homm5_guest_call_regs(X86 *c, uint32_t fn, uint32_t ecx, uint32_t edx,
                               const uint32_t *args, int nargs, uint32_t stack_top);

int ns_ptr(uint32_t a) {
    return a >= 0x10000u && a < (uint32_t)GUEST_SIZE - 0x100u;
}

Scratch ns_scratch(X86 *c) {
    Scratch s = {c, (c->r[R_ESP] - 0x1000u) & ~15u, 0};
    return s;
}

uint32_t ns_call(Scratch *s, uint32_t fn, uint32_t ecx, uint32_t edx, const uint32_t *args,
                     int nargs) {
    return homm5_guest_call_regs(s->c, fn, ecx, edx, args, nargs, s->base);
}

uint32_t ns_vcall(Scratch *s, uint32_t self, uint32_t slot, const uint32_t *args, int nargs) {
    return ns_call(s, rd32(rd32(self) + slot), self, 0, args, nargs);
}

uint32_t ns_alloc(Scratch *s, uint32_t n) {
    const uint32_t at = s->base + s->used;
    s->used += (n + 15u) & ~15u;
    return at;
}

/* The game's narrow string {begin, end, cap} of `text`, in the scratch: only
 * for calls that read a name. */
uint32_t ns_str(Scratch *s, const char *text) {
    const uint32_t n = (uint32_t)strlen(text);
    const uint32_t str = ns_alloc(s, 12 + n + 1);
    for (uint32_t i = 0; i <= n; ++i)
        wr8(str + 12 + i, (uint8_t)text[i]);
    wr32(str, str + 12);
    wr32(str + 4, str + 12 + n);
    wr32(str + 8, str + 12 + n + 1);
    return str;
}

/* A wide string {begin, end, cap} at `w` into UTF-8, its markup left out. */
void ns_wstr_utf8(uint32_t w, char *out, size_t cap) {
    out[0] = 0;
    if (!ns_ptr(w))
        return;
    const uint32_t b = rd32(w), e = rd32(w + 4);
    if (ns_ptr(b) && e >= b && e - b < 0x10000u)
        ns_text_utf8(b, (e - b) / 2u, out, cap);
}

/* A formatter that returns a const wstr* (fastcall ECX). */
void ns_text_of(Scratch *s, uint32_t fn, uint32_t ecx, char *out, size_t cap) {
    ns_wstr_utf8(ns_call(s, fn, ecx, 0, NULL, 0), out, cap);
}

/* A text file of the game by its path, as its windows show it (the
 * installed language, DataLS's changes): 00a09de0 on a ref holding the path.
 * Read once; "" when there is none. */
const char *ns_game_text(Scratch *s, const char *path) {
    enum { TEXTS = 96 };
    static struct {
        char path[96];
        char text[192];
    } cache[TEXTS];
    static int count, next;
    for (int i = 0; i < count; ++i)
        if (strcmp(cache[i].path, path) == 0)
            return cache[i].text;
    const int at = count < TEXTS ? count++ : next++ % TEXTS;
    snprintf(cache[at].path, sizeof cache[at].path, "%s", path);
    cache[at].text[0] = 0;
#ifdef NS_TEXT
    ns_text_of(s, NS_TEXT, ns_str(s, path), cache[at].text, sizeof cache[at].text);
    for (size_t n = strlen(cache[at].text); n && (cache[at].text[n - 1] == ' ' || cache[at].text[n - 1] == '\n');)
        cache[at].text[--n] = 0;
#else
    (void)s;
#endif
    return cache[at].text;
}

const char *ns_label(Scratch *s, const char *path, const char *ru, const char *en) {
    const char *t = ns_game_text(s, path);
    return t[0] ? t : ns_tr(ru, en);
}

/* `text`'s first letter as a capital (Latin or Cyrillic), in place. */
char *ns_capital(char *text) {
    unsigned char *u = (unsigned char *)text;
    if (u[0] >= 'a' && u[0] <= 'z')
        u[0] = (unsigned char)(u[0] - 32);
    else if (u[0] == 0xd0 && u[1] >= 0xb0 && u[1] <= 0xbf) /* а..п */
        u[1] = (unsigned char)(u[1] - 0x20);
    else if (u[0] == 0xd1 && u[1] >= 0x80 && u[1] <= 0x8f) { /* р..я */
        u[0] = 0xd0;
        u[1] = (unsigned char)(u[1] + 0x20);
    } else if (u[0] == 0xd1 && u[1] == 0x91) { /* ё */
        u[0] = 0xd0;
        u[1] = 0x81;
    }
    return text;
}

void ns_difficulty_name(Scratch *s, int index, char *out, size_t cap) {
    out[0] = 0;
#ifdef NS_DIFFICULTY
    const uint32_t entry = ns_call(s, NS_DIFFICULTY, (uint32_t)index, 0, NULL, 0);
    if (ns_ptr(entry))
        ns_text_of(s, NS_TEXT, entry + 4, out, cap);
#else
    (void)s;
    (void)index;
    (void)cap;
#endif
}

/* A screen's button reaction, as its button would run it ("create", "back"). */
void ns_react(Scratch *s, uint32_t reactions, uint32_t handler, const char *name) {
    ns_react_as(s, reactions, handler, "", name);
}

void ns_react_as(Scratch *s, uint32_t reactions, uint32_t handler, const char *sender,
                 const char *name) {
    const uint32_t args[3] = {ns_str(s, sender), ns_str(s, name), 0};
    ns_call(s, handler, reactions, 0, args, 3);
}

void ns_free(Scratch *s, uint32_t p) {
#ifdef NS_FREE
    if (ns_ptr(p))
        ns_call(s, NS_FREE, 0, 0, &p, 1);
#else
    (void)s;
    (void)p;
#endif
}

uint32_t ns_wide(Scratch *s, const char *utf8) {
    const uint32_t text = ns_alloc(s, 2u * 64u);
    uint32_t n = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p && n < 63;) {
        uint32_t u = *p++;
        if (u >= 0xc0 && u < 0xe0 && (*p & 0xc0) == 0x80) {
            u = ((u & 0x1f) << 6) | (*p++ & 0x3f);
        } else if (u >= 0xe0 && u < 0xf0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) {
            u = ((u & 0x0f) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f);
            p += 2;
        } else if (u >= 0x80) {
            continue; /* nothing a name holds */
        }
        wr16(text + 2u * n++, (uint16_t)u);
    }
    wr16(text + 2u * n, 0);
    return text;
}

uint32_t ns_iwindow(uint32_t p) {
    if (!ns_ptr(p) || !ns_ptr(rd32(p + 4)))
        return 0;
    const uint32_t w = p + 4 + rd32(rd32(p + 4) + 8);
    return ns_ptr(w) ? w : 0;
}

uint32_t ns_find(Scratch *s, uint32_t iwindow, const char *name) {
    if (!ns_ptr(iwindow))
        return 0;
    const uint32_t args[2] = {ns_str(s, name), 1};
    const uint32_t w = ns_vcall(s, iwindow, NS_IW_FIND, args, 2);
    return ns_ptr(w) ? w : 0;
}

int ns_enabled(Scratch *s, uint32_t iwindow) {
    return ns_ptr(iwindow) && (ns_vcall(s, iwindow, 0x40, NULL, 0) & 0xff);
}

int ns_visible(Scratch *s, uint32_t iwindow) {
    return ns_ptr(iwindow) && (ns_vcall(s, iwindow, 0x48, NULL, 0) & 0xff);
}

void ns_caption(Scratch *s, uint32_t iwindow, char *out, size_t cap) {
    out[0] = 0;
    if (!ns_ptr(iwindow))
        return;
    const uint32_t w = ns_alloc(s, 12);
    wr32(w, 0);
    wr32(w + 4, 0);
    ns_vcall(s, iwindow, NS_IW_CAPTION, &w, 1);
    ns_wstr_utf8(w, out, cap);
    ns_free(s, rd32(w));
}

/* ------------------------------------------------------------ language -- */

/* The views' own words, in the game's language: Russian when the game's
 * texts are (its difficulty names tell), else English. */
int ns_ru;

const char *ns_tr(const char *ru, const char *en) {
    return ns_ru ? ru : en;
}

void j_kv_str(Json *j, const char *key, const char *value) {
    j_str(j, key);
    j_lit(j, ":");
    j_str(j, value);
}

void j_fact(Json *j, int first, const char *label, const char *value) {
    if (!first)
        j_lit(j, ",");
    j_lit(j, "[");
    j_str(j, label);
    j_lit(j, ",");
    j_str(j, value);
    j_lit(j, "]");
}

void j_bool(Json *j, const char *key, int value) {
    j_str(j, key);
    j_printf(j, ":%s", value ? "true" : "false");
}

void j_button(Json *j, int first, const char *name, const char *label, int enabled) {
    if (!first)
        j_lit(j, ",");
    j_lit(j, "{");
    j_kv_str(j, "name", name);
    j_lit(j, ",");
    j_kv_str(j, "label", label);
    j_printf(j, ",\"enabled\":%s}", enabled ? "true" : "false");
}

/* ------------------------------------------------------------- the hook -- */

extern const NsScreen ns_custom_create, ns_custom_wait, ns_hotseat_create, ns_hotseat_wait,
    ns_main_menu, ns_profile_name, ns_load_game, ns_hall_of_fame, ns_options, ns_duel, ns_duel3,
    ns_esc_menu, ns_lan_name, ns_lan_lobby, ns_lan_duel_lobby, ns_mp_create, ns_lan_duel_create, ns_mp_wait, ns_save_game,
    ns_ubi_login, ns_ubi_cdkey, ns_ubi_channels, ns_lan_load, ns_hotseat_load, ns_mp_load_wait;
/* Every game links them all; a screen without addresses in a game has a zero
 * vtable and is never matched. */
static const NsScreen *const SCREENS[] = {&ns_custom_create, &ns_custom_wait, &ns_hotseat_create,
                                          &ns_hotseat_wait, &ns_main_menu, &ns_profile_name,
                                          &ns_load_game, &ns_hall_of_fame, &ns_options, &ns_duel,
                                          &ns_duel3, &ns_esc_menu, &ns_lan_name, &ns_lan_lobby,
                                          &ns_lan_duel_lobby, &ns_mp_create, &ns_lan_duel_create, &ns_mp_wait, &ns_save_game,
                                          &ns_ubi_login, &ns_ubi_cdkey, &ns_ubi_channels, &ns_lan_load,
                                          &ns_hotseat_load, &ns_mp_load_wait};
enum { SCREEN_COUNT = sizeof SCREENS / sizeof SCREENS[0] };

/* The table's row for IScreen* p, and the full screen object into *screen,
 * when that screen is drawn natively now. */
/* RECOMP_NATIVE_SCREENS_SKIP: screen ids (comma separated) the game draws as
 * its own, to compare. */
static int skipped(const char *id) {
    static const char *skip = (const char *)1;
    if (skip == (const char *)1)
        skip = getenv("RECOMP_NATIVE_SCREENS_SKIP");
    if (!skip || !*skip)
        return 0;
    const size_t n = strlen(id);
    for (const char *p = strstr(skip, id); p; p = strstr(p + 1, id))
        if ((p == skip || p[-1] == ',') && (p[n] == 0 || p[n] == ','))
            return 1;
    return 0;
}

static const NsScreen *screen_of(uint32_t p, uint32_t *screen) {
    const uint32_t vt = ns_ptr(p) ? rd32(p) : 0;
    for (int i = 0; i < SCREEN_COUNT; ++i)
        if (vt && vt == SCREENS[i]->vtable && !skipped(SCREENS[i]->id)) {
            /* One class may serve several entries (the edit box: the first
             * profile's name, the LAN name and password): the first that
             * claims it. */
            *screen = p - SCREENS[i]->sub;
            if (SCREENS[i]->claims && !SCREENS[i]->claims(*screen))
                continue;
            return SCREENS[i];
        }
    return NULL;
}

/* The screen on top of the stack when it is one of the table's. */
static const NsScreen *top_screen(uint32_t *screen) {
#ifdef NS_SCREEN_STACK
    const uint32_t sentinel = rd32(NS_SCREEN_STACK);
    const uint32_t node = ns_ptr(sentinel) ? rd32(sentinel + 4) : 0;
    if (ns_ptr(node) && node != sentinel)
        return screen_of(rd32(node + 8), screen);
#endif
    return NULL;
}

/* Whether `root` is the root of one of the table's screens anywhere in the
 * stack. */
static int our_root(uint32_t root) {
#ifdef NS_SCREEN_STACK
    const uint32_t sentinel = rd32(NS_SCREEN_STACK);
    int guard = 64;
    for (uint32_t node = ns_ptr(sentinel) ? rd32(sentinel) : 0;
         ns_ptr(node) && node != sentinel && guard--; node = rd32(node)) {
        uint32_t screen;
        if (screen_of(rd32(node + 8), &screen) && rd32(screen + NS_SCREEN_ROOT) == root)
            return 1;
    }
#endif
    (void)root;
    return 0;
}

/* RECOMP_TRACE_NATIVE_SCREENS: each change of the screen on top. */
static void trace_top(void) {
#ifdef NS_SCREEN_STACK
    static uint32_t said;
    const uint32_t sentinel = rd32(NS_SCREEN_STACK);
    const uint32_t node = ns_ptr(sentinel) ? rd32(sentinel + 4) : 0;
    const uint32_t p = ns_ptr(node) && node != sentinel ? rd32(node + 8) : 0;
    if (p != said) {
        said = p;
        uint32_t screen = 0;
        const NsScreen *kind = screen_of(p, &screen);
        fprintf(stderr, "[screens] top: IScreen %08x vt %08x%s%s\n", p, ns_ptr(p) ? rd32(p) : 0,
                kind ? " = " : "", kind ? kind->id : "");
    }
#endif
}

enum {
    WINDOW_PART = 0x10c,   /* a window: its draw part, what 00e3dd70 gets */
    WINDOW_PARENT = 0x28,
    WINDOW_CHILDREN = 0xa4,
    ROOT_WINDOW = 0xf8,    /* a root WindowScreen: back to its window */
};

/* A window of a screen that keeps some windows drawn (NsScreen.keeps): kept
 * when it is one of those or holds one; any other is not drawn. The root
 * itself is drawn. Non-zero: not drawn. */
static int hidden_in(const NsScreen *kind, uint32_t screen, uint32_t win, int depth) {
    char name[64];
    homm5_window_name(win, name, sizeof name);
    if (kind->keeps(screen, name))
        return 0;
    if (depth > 0) {
        const uint32_t b = rd32(win + WINDOW_CHILDREN), e = rd32(win + WINDOW_CHILDREN + 4);
        if (ns_ptr(b) && e > b && e - b <= 4u * 256u)
            for (uint32_t at = b; at < e; at += 4)
                if (ns_ptr(rd32(at)) && !hidden_in(kind, screen, rd32(at), depth - 1))
                    return 0;
    }
    return 1;
}

/* For a window that is not a root: the screen on top, when the window is in
 * its root and that screen keeps some of its windows. */
static int kept_screen_hides(uint32_t win) {
    uint32_t screen = 0;
    const NsScreen *kind = top_screen(&screen);
    if (!kind || !kind->keeps)
        return 0;
    /* The root's draw part is root + 0x14, a window's is window + 0x10c: the
     * root's window is root - 0xf8. */
    const uint32_t root = rd32(screen + NS_SCREEN_ROOT) - ROOT_WINDOW;
    int guard = 32;
    for (uint32_t up = rd32(win + WINDOW_PARENT); ns_ptr(up) && guard--; up = rd32(up + WINDOW_PARENT))
        if (up == root)
            return hidden_in(kind, screen, win, 3);
    return 0;
}

/* RECOMP_NATIVE_SCREENS_DUMP=<dir>: each new document also into
 * <dir>/<id>-<n>.json, to show it again without the game (the host's
 * RECOMP_NATIVE_SCREEN_SAMPLE) or to read it whole. */
static void dump(const char *id, const char *doc, int len) {
    static const char *dir = (const char *)1;
    static char last[16384];
    static int last_len, count;
    if (dir == (const char *)1)
        dir = getenv("RECOMP_NATIVE_SCREENS_DUMP");
    if (!dir || !*dir || count >= 500 || (len == last_len && memcmp(last, doc, (size_t)len) == 0))
        return;
    if (len < (int)sizeof last) {
        memcpy(last, doc, (size_t)len);
        last_len = len;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s-%03d.json", dir, id, count++);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(doc, 1, (size_t)len, f);
        fclose(f);
    }
}

/* Whether a screen read this frame is still the one up. */
static int still_up(const NsScreen *kind, uint32_t screen) {
    if (kind == &ns_question)
        return ns_question_box() == screen;
    uint32_t again = 0;
    return top_screen(&again) == kind && again == screen;
}

/* RECOMP_NATIVE_SCREENS_SCRIPT=<file>: a test's taps for a device nobody
 * taps (a second simulator in a LAN game). Each line "<screen id> <ms>
 * <command>" is sent as the view would send it once that screen has been up
 * <ms>; lines are taken in order, "#" lines skipped. */
static void script(const NsScreen *kind) {
    static FILE *f = (FILE *)1;
    static char line[1024];
    static int have, off;
    static int64_t since;
    if (f == (FILE *)1) {
        /* "~/..." is under the app's home (a device's container). */
        const char *path = getenv("RECOMP_NATIVE_SCREENS_SCRIPT"), *home = getenv("HOME");
        char full[1024];
        if (path && path[0] == '~' && path[1] == '/' && home) {
            snprintf(full, sizeof full, "%s/%s", home, path + 2);
            path = full;
        }
        f = path && *path ? fopen(path, "r") : NULL;
        if (path && *path)
            fprintf(stderr, "[screens] script %s: %s\n", path, f ? "read" : "not found");
    }
    if (!f)
        return;
    char id[48];
    int ms = 0;
    while (!have) {
        if (!fgets(line, sizeof line, f)) {
            fclose(f);
            f = NULL;
            return;
        }
        line[strcspn(line, "\r\n")] = 0;
        have = line[0] && line[0] != '#' && sscanf(line, "%47s %d %n", id, &ms, &off) == 2 && off > 0;
        since = 0;
    }
    sscanf(line, "%47s %d", id, &ms);
    if (strcmp(id, kind->id) != 0) {
        since = 0;
        return;
    }
    if (!since)
        since = now_ms();
    if (now_ms() - since < ms)
        return;
    fprintf(stderr, "[screens] script: %s\n", line);
    recomp_screen_command(line + off);
    have = 0;
}

/* One frame of a native screen: its queued commands, its step, then its
 * document. */
static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* RECOMP_NATIVE_SCREENS_STATS=1: what the hook costs the game thread, every
 * 5 s: the windows it saw, its time in all (and the slowest call), the
 * frames a native screen was read (drive) and their time, the documents that
 * changed. */
static struct {
    int64_t since, hook_ns, hook_max, drive_ns;
    int calls, drives, docs;
} g_stats;

static int stats_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_NATIVE_SCREENS_STATS");
        on = v && *v == '1';
    }
    return on;
}

static void stats_note(int64_t spent) {
    g_stats.hook_ns += spent;
    if (spent > g_stats.hook_max)
        g_stats.hook_max = spent;
    ++g_stats.calls;
    const int64_t now = now_ns();
    if (!g_stats.since)
        g_stats.since = now;
    if (now - g_stats.since < 5000000000LL)
        return;
    const double secs = (double)(now - g_stats.since) / 1e9;
    fprintf(stderr,
            "[screens] stats %.1f s: hook %d calls (%.0f/s), %.2f ms/s (%.3f%% of a core), slowest %.1f us; "
            "drive %d (%.0f/s), %.2f ms/s; documents %d (%.1f/s)\n",
            secs, g_stats.calls, g_stats.calls / secs, g_stats.hook_ns / 1e6 / secs, g_stats.hook_ns / 1e7 / secs,
            g_stats.hook_max / 1e3, g_stats.drives, g_stats.drives / secs, g_stats.drive_ns / 1e6 / secs,
            g_stats.docs, g_stats.docs / secs);
    memset(&g_stats, 0, sizeof g_stats);
    g_stats.since = now;
}

static void drive_timed(X86 *c, const NsScreen *kind, uint32_t screen);

static void drive(X86 *c, const NsScreen *kind, uint32_t screen) {
    if (!stats_on()) {
        drive_timed(c, kind, screen);
        return;
    }
    const int64_t t0 = now_ns();
    drive_timed(c, kind, screen);
    g_stats.drive_ns += now_ns() - t0;
    ++g_stats.drives;
}

static void drive_timed(X86 *c, const NsScreen *kind, uint32_t screen) {
    script(kind);
    Scratch s = ns_scratch(c);
    for (char *cmd; (cmd = next_command()) != NULL; free(cmd)) {
        if (ns_trace())
            fprintf(stderr, "[screens] %s: %s\n", kind->id, cmd);
        s.used = 0;
        kind->command(&s, screen, cmd);
    }
    if (g_leave_pending) {
        g_leave_pending = 0;
        g_left_screen = screen;
        g_left_ms = now_ms();
    }
    if (held_back(screen))
        return;
    /* A command may have closed the screen (deferred, but read it again). */
    if (!still_up(kind, screen)) {
        if (ns_trace())
            fprintf(stderr, "[screens] %s left after its commands\n", kind->id);
        return;
    }
    s.used = 0;
    if (kind->step)
        kind->step(&s, screen);
    static Json j;
    j.len = 0;
    s.used = 0;
    kind->document(&j, &s, screen);
    if (j.s) {
        if (ns_trace()) {
            static uint32_t said_len;
            if (said_len != (uint32_t)j.len) {
                said_len = (uint32_t)j.len;
                fprintf(stderr, "[screens] %s: document %d bytes: %.300s\n", kind->id, j.len, j.s);
            }
        }
        publish(j.s, j.len);
        dump(kind->id, j.s, j.len);
    }
}

static int draw_hook(X86 *c, uint32_t part) {
    if (SCREEN_COUNT > 0 && screens_on() && ns_trace())
        trace_top();
    if (SCREEN_COUNT == 0 || !screens_on())
        return 0;
    /* A game question up (a message box, in the top screen's root): the
     * native popup, read once a frame: by the box's own draw, which is then
     * not drawn, or, when the top screen is native and its root not drawn,
     * by that root's. */
    const uint32_t box = ns_question_box();
    if (box) {
        const uint32_t win = part - WINDOW_PART;
        uint32_t screen = 0;
        const NsScreen *top = top_screen(&screen);
        const int top_root = top && rd32(screen + NS_SCREEN_ROOT) == part - NS_ROOT_DRAW;
        if (win == box || (top_root && !top->keeps)) {
            drive(c, &ns_question, box);
            return 1;
        }
        if (our_root(part - NS_ROOT_DRAW))
            return top_root ? 0 : 1;
        return top && top->keeps && kept_screen_hides(win);
    }
    if (!our_root(part - NS_ROOT_DRAW)) {
        /* Any other window drawn while ours shows: is ours still on top? */
        uint32_t top;
        if (g_doc_shown && !top_screen(&top)) {
            if (ns_trace())
                fprintf(stderr, "[screens] covered or gone\n");
            withdraw();
        }
        /* A window inside a screen that keeps some of its own. */
        return g_doc_shown && kept_screen_hides(part - WINDOW_PART);
    }
    if (ns_trace()) {
        static int64_t since;
        static int calls;
        ++calls;
        if (now_ms() - since > 2000) {
            fprintf(stderr, "[screens] our root drawn %d times since the last note\n", calls);
            since = now_ms();
            calls = 0;
        }
    }
    uint32_t screen = 0;
    const NsScreen *kind = top_screen(&screen);
    if (!kind) {
        /* A message box or another screen above ours: the game's own. */
        if (ns_trace() && g_doc_shown)
            fprintf(stderr, "[screens] covered\n");
        withdraw();
        return 1;
    }
    if (rd32(screen + NS_SCREEN_ROOT) != part - NS_ROOT_DRAW) {
        if (ns_trace()) {
            static uint32_t said;
            if (said != part) {
                said = part;
                fprintf(stderr, "[screens] root %08x drawn beneath %s (root %08x)\n",
                        part - NS_ROOT_DRAW, kind->id, rd32(screen + NS_SCREEN_ROOT));
            }
        }
        return 1; /* ours, but beneath the one on top: read once, by the top's root */
    }
    drive(c, kind, screen);
    /* Kept windows (the logo): the root draws, and its other windows are
     * not drawn as they come (kept_screen_hides). */
    return kind->keeps ? 0 : 1;
}

int homm5_native_screens_draw(X86 *c, uint32_t part) {
    if (!stats_on())
        return draw_hook(c, part);
    const int64_t t0 = now_ns();
    const int r = draw_hook(c, part);
    g_stats.docs += g_stats_docs;
    g_stats_docs = 0;
    stats_note(now_ns() - t0);
    return r;
}
