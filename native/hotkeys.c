/* native/hotkeys.c - the commands the touch panel offers on the screen showing
 * now (runtime/native_seam.h recomp_hotkeys_state).
 *
 * The screen: a screen that takes keys turns its key section on by name
 * (the base game's 006bb7e0 with DL=1; Hammers of Fate's 006e5360, Tribes of
 * the East's 005b59b0), which keeps the names of the active sections in one
 * global std::vector<std::string-like> {char *begin, *end, *cap} of 12-byte
 * elements, the names without a NUL. The names are the bindings file's
 * bindsection ones (adventure_screen, combat_screen, town_screen...), so the
 * host thread reads them as touch.c reads the screen stack: plain words, no
 * call into the game.
 *
 * The keys: the player's profile's bindings file (hotkeys_cfg.c), found
 * under the player's documents as the game keeps them (Profiles/global.cfg
 * names the profile), or the game's default profile's. It is read again when
 * its time or size changes, checked at most once a second.
 *
 * The labels are in the game's language, which the native screens tell from
 * the game's own words (native/screens_words.c). Without them, the first time
 * a screen offers commands the game's thread reads the same words once
 * (homm5_hotkeys_frame).
 *
 * RECOMP_TRACE_HOTKEYS logs the active sections and each new document. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hotkeys_cfg.h"
#include "native_screens.h"
#include "native_seam.h"
#include "platform/os.h"
#include "x86.h"

#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif
#ifndef HOMM5_BIND_SECTIONS
#define HOMM5_BIND_SECTIONS 0x0114b258 /* the active sections' vector: begin, end */
#define HOMM5_INPUT_CFG "input.cfg"
#define HOMM5_GLOBAL_CFG "global.cfg"
#endif

/* Where the game keeps its profiles: under the documents folder, in a folder
 * whose name differs per game and language ("Heroes of Might and Magic V/
 * Hammers of Fate", "... — Tribes of the East"), so it is looked for. */
#define GUEST_MY_GAMES "C:\\Heroes5\\Documents\\My Games"
#define GUEST_DEFAULT_PROFILE "C:\\Heroes5\\profiles\\default_profile\\"

/* native/screens_words.c: the game's language, NS_* = HK_*; ns_set_lang tells
 * it from the difficulties' names, as native_screens.c reads them. */
int ns_set_lang(const char *easy, const char *hard);
void ns_difficulty_name(Scratch *s, int index, char *out, size_t cap);

static int g_trace = -1;

/* Set from the host thread once a document has commands; the game's thread
 * then reads the language (a guest call: on its own thread only). */
static volatile int g_lang_wanted;

static uint32_t peek32(uint32_t addr) {
    uint32_t v;
    if (!g_mem || addr < 0x1000u || addr > (uint32_t)GUEST_SIZE - 4u)
        return 0;
    memcpy(&v, g_mem + addr, sizeof v);
    return v;
}

/* The game's own buttons: native/touch.c finds the map's or a battle's root
 * window, native/phone_layout.c reads a window's name and whether it shows. */
uint32_t homm5_top_root(void);
void homm5_window_name(uint32_t win, char *out, size_t cap);
int homm5_window_shown(uint32_t win);

/* native/phone_hud.c: where the interface stands, as fractions of the picture. */
int homm5_hud_busy(float (*out)[4], int cap);
int homm5_hud_window_frac(uint32_t win, float q[4]);

/* That, from the game's thread, for the host's panel to keep clear of: two
 * buffers, the one `g_busy_at` names complete. */
enum { BUSY_MAX = 24 };
static float g_busy[2][BUSY_MAX][4];
static int g_busy_n[2];
/* The size of the game's own round button (its menu's, on the map's and a
 * battle's dial), as fractions of the picture's width and height; 0 unseen.
 * The panel's buttons take it on a large screen. */
static float g_button[2][2];
static volatile int g_busy_at;

/* The catalog's commands whose button the screen on top shows now (bit i:
 * hk_command_buttons(i)), from the game's thread; the host thread reads it. */
static volatile uint32_t g_onscreen;

/* Whether the space-separated `list` holds `word`. */
static int listed(const char *list, const char *word) {
    const size_t len = strlen(word);
    for (const char *p = list; p && *p;) {
        while (*p == ' ')
            ++p;
        const char *e = p;
        while (*e && *e != ' ')
            ++e;
        if (len && (size_t)(e - p) == len && !memcmp(p, word, len))
            return 1;
        p = e;
    }
    return 0;
}

/* Walks the top screen's windows (children at +0xa4, a vector of pointers)
 * for the catalog's buttons and keeps the commands whose button shows; the
 * dial's menu button's size into `button` (w, h), where it shows. */
static uint32_t scan_onscreen(float button[2]) {
    button[0] = button[1] = 0.0f;
    const uint32_t root = homm5_top_root();
    if (!root)
        return 0;
    uint32_t stack[256];
    int top = 0, seen = 0;
    uint32_t found = 0;
    const int count = hk_command_count() < 32 ? hk_command_count() : 32;
    stack[top++] = root;
    while (top && seen++ < 4096) {
        const uint32_t win = stack[--top];
        char name[64];
        homm5_window_name(win, name, sizeof name);
        for (int i = 0; name[0] && i < count; ++i)
            if (listed(hk_command_buttons(i), name) && homm5_window_shown(win))
                found |= 1u << i;
        float q[4];
        if (!button[0] && !strcmp(name, "OnJog-Menu") && homm5_window_shown(win) &&
            homm5_hud_window_frac(win, q)) {
            button[0] = q[2] - q[0];
            button[1] = q[3] - q[1];
        }
        const uint32_t b = peek32(win + 0xa4u), e = peek32(win + 0xa8u);
        if (!b || e < b || e - b > 4u * 256u)
            continue;
        for (uint32_t at = b; at < e && top < (int)(sizeof stack / sizeof stack[0]); at += 4)
            if (peek32(at))
                stack[top++] = peek32(at);
    }
    return found;
}

void homm5_hotkeys_frame(X86 *c) {
    static unsigned frame, last_ask;
    static int done, asked;
    if (++frame % 15 == 0) {
        const int w = !g_busy_at;
        g_onscreen = scan_onscreen(g_button[w]);
        g_busy_n[w] = homm5_hud_busy(g_busy[w], BUSY_MAX);
        __atomic_store_n(&g_busy_at, w, __ATOMIC_RELEASE);
    }
    /* The words may not be loaded yet: asked again each second, ten times. */
    if (done || !g_lang_wanted || (asked && frame - last_ask < 60))
        return;
    last_ask = frame;
    Scratch s = ns_scratch(c);
    char easy[64], hard[64];
    ns_difficulty_name(&s, 0, easy, sizeof easy);
    ns_difficulty_name(&s, 2, hard, sizeof hard);
    if (easy[0])
        ns_set_lang(easy, hard);
    done = easy[0] || ++asked >= 10;
    if (g_trace)
        fprintf(stderr, "[hotkeys] language from \"%s\": %d\n", easy, ns_lang);
}

enum { MAX_SECTIONS = 8, MAX_BINDS = 1024 };

/* native/camera.c: the map's camera, a battle's, has drawn a frame lately. */
int homm5_map_camera_live(void);
int homm5_zoom_camera_live(void);
/* native/touch.c: the top screen, 1 the map, 2 a battle, 0 another. */
int homm5_top_screen_kind(void);

/* The active sections' names, in the vector's order; how many. The map's and
 * a battle's are on while their screen still loads behind its loading picture
 * ("press any key"), where no command does anything: they count once their
 * camera draws. The map's stays on under a town's too, where its keys mean
 * the town's (M the market): it counts only while the map is the top screen. */
static int active_sections(char names[MAX_SECTIONS][40]) {
    const uint32_t begin = peek32(HOMM5_BIND_SECTIONS), end = peek32(HOMM5_BIND_SECTIONS + 4u);
    if (!begin || end < begin || (end - begin) % 12u)
        return 0;
    int n = 0;
    for (uint32_t e = begin; e < end && n < MAX_SECTIONS; e += 12u) {
        const uint32_t s = peek32(e), se = peek32(e + 4u);
        if (!s || se < s || se - s >= 40u || se > (uint32_t)GUEST_SIZE)
            continue;
        memcpy(names[n], g_mem + s, se - s);
        names[n][se - s] = 0;
        if (!strcmp(names[n], "adventure_screen") &&
            (homm5_top_screen_kind() != 1 || !homm5_map_camera_live()))
            continue;
        if (!strncmp(names[n], "combat_screen", 13) &&
            (homm5_top_screen_kind() != 2 || !homm5_zoom_camera_live()))
            continue;
        /* Plain words only: the vector may be moving under the host's read. */
        int plain = names[n][0] != 0;
        for (const char *ch = names[n]; *ch && plain; ++ch)
            plain = (*ch >= 'a' && *ch <= 'z') || (*ch >= '0' && *ch <= '9') || *ch == '_';
        if (!plain)
            continue;
        ++n;
    }
    return n;
}

/* ---- The bindings file ---- */

typedef struct {
    const char *parent;
    int depth;
    char *out;
    size_t cap;
    int found;
} FindProfiles;

/* Looks for <folder>/Profiles/<global.cfg> in the folders under `parent`,
 * `depth` more levels down. */
static int find_profiles_in(const char *name, void *user) {
    FindProfiles *f = (FindProfiles *)user;
    char path[1024], probe[1100];
    OsStat st;
    snprintf(path, sizeof path, "%s/%s", f->parent, name);
    if (os_stat(path, &st) != 0 || !st.is_dir)
        return 0;
    snprintf(probe, sizeof probe, "%s/Profiles/%s", path, HOMM5_GLOBAL_CFG);
    if (os_stat(probe, &st) == 0 && st.is_regular) {
        snprintf(f->out, f->cap, "%s/Profiles", path);
        f->found = 1;
        return 1;
    }
    if (f->depth > 0) {
        FindProfiles sub = {path, f->depth - 1, f->out, f->cap, 0};
        os_listdir(path, find_profiles_in, &sub);
        f->found = sub.found;
    }
    return f->found;
}

static int profiles_dir(char *out, size_t cap) {
    char root[1024];
    if (!recomp_readable_path(GUEST_MY_GAMES, root, sizeof root))
        return 0;
    FindProfiles f = {root, 1, out, cap, 0};
    os_listdir(root, find_profiles_in, &f);
    return f.found;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *text = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long size = ftell(f);
        if (size >= 0 && size < (1 << 20) && fseek(f, 0, SEEK_SET) == 0) {
            text = (char *)malloc((size_t)size + 1);
            if (text && fread(text, 1, (size_t)size, f) == (size_t)size) {
                text[size] = 0;
                *len = (size_t)size;
            } else {
                free(text);
                text = NULL;
            }
        }
    }
    fclose(f);
    return text;
}

/* The profile global.cfg names ("setvar profile_name = Игрок", in the game's
 * ANSI code page), raw. */
static int profile_name(const char *global, char *out, size_t cap) {
    size_t len = 0;
    char *text = read_file(global, &len);
    int ok = 0;
    const char *p = text ? strstr(text, "profile_name") : NULL;
    if (p) {
        p += 12;
        while (*p == ' ' || *p == '\t' || *p == '=')
            ++p;
        size_t n = 0;
        while (p[n] && p[n] != '\r' && p[n] != '\n')
            ++n;
        while (n && (p[n - 1] == ' ' || p[n - 1] == '\t'))
            --n;
        if (n && n < cap) {
            memcpy(out, p, n);
            out[n] = 0;
            ok = 1;
        }
    }
    free(text);
    return ok;
}

/* ANSI text as UTF-8: the Cyrillic page (1251) a Russian release names its
 * profiles in, or Latin-1. The host's folder is the UTF-8 spelling of the
 * game's name (runtime/codepage.h), and which page the game uses is not known
 * here, so both are tried. */
static void ansi_to_utf8(const char *in, int cyrillic, char *out, size_t cap) {
    static const uint16_t HIGH[64] = {
        0x0402, 0x0403, 0x201a, 0x0453, 0x201e, 0x2026, 0x2020, 0x2021, 0x20ac, 0x2030, 0x0409,
        0x2039, 0x040a, 0x040c, 0x040b, 0x040f, 0x0452, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
        0x2013, 0x2014, 0xfffd, 0x2122, 0x0459, 0x203a, 0x045a, 0x045c, 0x045b, 0x045f, 0x00a0,
        0x040e, 0x045e, 0x0408, 0x00a4, 0x0490, 0x00a6, 0x00a7, 0x0401, 0x00a9, 0x0404, 0x00ab,
        0x00ac, 0x00ad, 0x00ae, 0x0407, 0x00b0, 0x00b1, 0x0406, 0x0456, 0x0491, 0x00b5, 0x00b6,
        0x00b7, 0x0451, 0x2116, 0x0454, 0x00bb, 0x0458, 0x0405, 0x0455, 0x0457,
    };
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; ++p) {
        uint32_t u = *p;
        if (cyrillic && u >= 0xc0)
            u = 0x0410 + (u - 0xc0);
        else if (cyrillic && u >= 0x80)
            u = HIGH[u - 0x80];
        if (u < 0x80) {
            out[o++] = (char)u;
        } else if (u < 0x800) {
            out[o++] = (char)(0xc0 | (u >> 6));
            out[o++] = (char)(0x80 | (u & 0x3f));
        } else {
            out[o++] = (char)(0xe0 | (u >> 12));
            out[o++] = (char)(0x80 | ((u >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (u & 0x3f));
        }
    }
    out[o] = 0;
}

/* The bindings file the game reads now: the chosen profile's, or the default
 * profile's while there is none (the first start). */
static int bindings_path(char *out, size_t cap) {
    char dir[1024], global[1100], name[128], utf8[512];
    OsStat st;
    if (profiles_dir(dir, sizeof dir)) {
        snprintf(global, sizeof global, "%s/%s", dir, HOMM5_GLOBAL_CFG);
        if (profile_name(global, name, sizeof name)) {
            for (int cyrillic = 1; cyrillic >= 0; --cyrillic) {
                ansi_to_utf8(name, cyrillic, utf8, sizeof utf8);
                snprintf(out, cap, "%s/%s/%s", dir, utf8, HOMM5_INPUT_CFG);
                if (os_stat(out, &st) == 0 && st.is_regular)
                    return 1;
            }
        }
    }
    return recomp_readable_path(GUEST_DEFAULT_PROFILE HOMM5_INPUT_CFG, out, cap);
}

static HkBind g_binds[MAX_BINDS];
static int g_nbinds;

/* Reads the bindings again when the file the game uses, or that file, has
 * changed; checked at most once a second. Non-zero when they changed. */
static int refresh_bindings(void) {
    static uint64_t checked_ns;
    static char loaded[1100];
    static int64_t loaded_mtime = -1;
    static uint64_t loaded_size;
    const uint64_t now = os_monotonic_ns();
    if (checked_ns && now - checked_ns < 1000000000ull)
        return 0;
    checked_ns = now;
    char path[1100];
    OsStat st;
    if (!bindings_path(path, sizeof path) || os_stat(path, &st) != 0)
        return 0;
    if (!strcmp(path, loaded) && st.mtime == loaded_mtime && st.size == loaded_size)
        return 0;
    size_t len = 0;
    char *text = read_file(path, &len);
    if (!text)
        return 0;
    g_nbinds = hk_parse(text, len, g_binds, MAX_BINDS);
    free(text);
    snprintf(loaded, sizeof loaded, "%s", path);
    loaded_mtime = st.mtime;
    loaded_size = st.size;
    if (g_trace)
        fprintf(stderr, "[hotkeys] %d binds from %s\n", g_nbinds, path);
    return 1;
}

/* ---- The seam ---- */

/* The interface's rectangles as the document's "busy": [[x0, y0, x1, y1], ...]
 * in hundredths of the picture, so a panel a pixel over does not count as a
 * change; and the game's round button's size as "button": [w, h], where one
 * shows. */
static void busy_json(char *out, size_t cap) {
    const int at = __atomic_load_n(&g_busy_at, __ATOMIC_ACQUIRE);
    size_t len = (size_t)snprintf(out, cap, ",\"busy\":[");
    for (int i = 0; i < g_busy_n[at] && len + 40 < cap; ++i) {
        const float *r = g_busy[at][i];
        len += (size_t)snprintf(out + len, cap - len, "%s[%.2f,%.2f,%.2f,%.2f]", i ? "," : "",
                                r[0], r[1], r[2], r[3]);
    }
    len += (size_t)snprintf(out + len, cap - len, "]");
    const float *b = g_button[at];
    if (len < cap && b[0] > 0.005f && b[0] < 0.5f && b[1] > 0.005f && b[1] < 0.5f)
        snprintf(out + len, cap - len, ",\"button\":[%.3f,%.3f]", b[0], b[1]);
}

int recomp_hotkeys_state(char *buf, int cap, unsigned *serial) {
    static char doc[8192];
    static int doc_len; /* 0: no command applies */
    static unsigned doc_serial = 1;
    static char shown_sections[MAX_SECTIONS * 41];
    static int shown_lang = -1;
    static uint32_t shown_onscreen;
    static char shown_busy[1100];
    char busy[1100];
    busy_json(busy, sizeof busy);
    if (g_trace < 0)
        g_trace = getenv("RECOMP_TRACE_HOTKEYS") != NULL;
    char names[MAX_SECTIONS][40], joined[MAX_SECTIONS * 41];
    const int n = active_sections(names);
    joined[0] = 0;
    for (int i = 0; i < n; ++i) {
        if (i)
            strcat(joined, " ");
        strcat(joined, names[i]);
    }
    const int changed = refresh_bindings();
    const uint32_t onscreen = g_onscreen;
    if (changed || ns_lang != shown_lang || onscreen != shown_onscreen ||
        strcmp(busy, shown_busy) != 0 || strcmp(joined, shown_sections) != 0) {
        if (g_trace && onscreen != shown_onscreen)
            fprintf(stderr, "[hotkeys] the game shows its own buttons for: %08x\n", onscreen);
        if (g_trace && strcmp(joined, shown_sections) != 0)
            fprintf(stderr, "[hotkeys] active sections: %s\n", joined[0] ? joined : "(none)");
        const char *ptrs[MAX_SECTIONS];
        for (int i = 0; i < n; ++i)
            ptrs[i] = names[i];
        const int len =
            hk_document(g_binds, g_nbinds, ptrs, n, ns_lang, onscreen, doc, (int)sizeof doc);
        doc_len = len < (int)sizeof doc ? len : 0;
        /* The interface's place, before the document's closing brace. */
        const size_t extra = strlen(busy);
        if (doc_len && (size_t)doc_len + extra < sizeof doc) {
            memcpy(doc + doc_len - 1, busy, extra);
            doc[doc_len - 1 + extra] = '}';
            doc_len += (int)extra;
            doc[doc_len] = 0;
        }
        strcpy(shown_busy, busy);
        strcpy(shown_sections, joined);
        shown_lang = ns_lang;
        shown_onscreen = onscreen;
        ++doc_serial;
        if (g_trace)
            fprintf(stderr, "[hotkeys] document: %s\n", doc_len ? doc : "(none)");
    }
    if (!doc_len)
        return -1;
    g_lang_wanted = 1;
    if (*serial == doc_serial)
        return 0;
    if (doc_len >= cap)
        return doc_len;
    memcpy(buf, doc, (size_t)doc_len + 1);
    *serial = doc_serial;
    return doc_len;
}
