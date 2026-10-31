/* native/screens_load.c - Load game drawn natively (native/native_screens.c):
 * NUI::CLoadScreen<SHeaderInfo> ("load_game"), opened from Single player,
 * from Custom game and from the game itself; and Save in a game,
 * NUI::CSaveScreen<SHeaderInfo> ("save_game"): the same list, a name field
 * and Save (re-load-options.md "Save").
 *
 * The saves are the screen's own headers, newest first; the game reads 20
 * when the screen opens and 5 more each frame it is on top, so the list is
 * read again every frame. A save's picture is copied out of its header into
 * a file the view shows (raw BGRA after its width and height).
 * The base game's addresses: research notes phone-menus/native/re-load-options.md. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native_screens.h"
#include "x86.h"

#ifdef RECOMP_OVERRIDE_HEADER
#include RECOMP_OVERRIDE_HEADER
#endif
#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif

/* The base game's; an expansion has none yet (native/native_screens.c). */
#ifndef RECOMP_ADDRESS_HEADER
#define NS_VT_LOAD 0x00fa2b7c       /* CLoadScreen<SHeaderInfo>'s IScreen, at +0x2b4 */
#define NS_SUB_LOAD 0x2b4
#define NS_LOAD_CLICK 0x00748590    /* thiscall (screen, force), RET 4: the row chosen */
#define NS_LOAD_LOAD 0x0072d920     /* thiscall (screen, 0), RET 4: load (asks in game) */
#define NS_LOAD_CONFIRMED 0x0072dab0 /* thiscall (screen+0x174, 0 delete / 2 load), RET 4 */
#define NS_LOAD_CLOSE 0x00743b20    /* thiscall (any, 0), RET 4: close the screen */
#define NS_LOAD_TYPE 0x00729630     /* fastcall ECX 0, DL campaign: the game's type, const wstr* */
#define NS_LOAD_SIZE 0x00729b30     /* fastcall ECX out wstr*, EDX size */
#define NS_DIFF_OF_SAVE 0x00b97c10  /* fastcall ECX a save's difficulty: its table index */
#define NS_DIFF_ENTRY 0x00b97bd0    /* fastcall ECX index: the table entry */
#define NS_TEXT_OF_REF 0x00a09de0   /* fastcall ECX a text ref: const wstr* */
#define NS_VT_SAVE 0x00fa26cc       /* CSaveScreen<SHeaderInfo>'s IScreen, at +0x2c4 */
#define NS_SUB_SAVE 0x2c4
/* A LAN game's and a hot seat's load screens: CLoadScreen<SLanSaveHeaderInfo>
 * and CLoadScreen<SHotSeatHeaderInfo>, also at +0x2b4 (research notes
 * lan-load-base-map.md). */
#define NS_VT_LOAD_LAN 0x00fa3cd8
#define NS_LOAD_LAN_CLICK 0x00748750
#define NS_LOAD_LAN_LOAD 0x00728120  /* closes the list; the game waits for the players */
#define NS_LOAD_LAN_CONFIRMED 0x00743320
#define NS_L_LAN_ROW 0x22c
#define NS_VT_LOAD_HS 0x00fa2a74
#define NS_LOAD_HS_CLICK 0x00748670
#define NS_LOAD_HS_LOAD 0x00728850
#define NS_LOAD_HS_CONFIRMED 0x007289e0
#define NS_L_HS_ROW 0x94
/* Their Save screens, CSaveScreen<SLanSaveHeaderInfo> and <SHotSeatHeaderInfo>,
 * also at +0x2c4; the save may be made only when 00729bf0 lets it (research
 * notes save-screens-*-map.md). */
#define NS_VT_SAVE_LAN 0x00fa3da0
#define NS_SAVE_LAN_SAVE 0x00744130
#define NS_VT_SAVE_HS 0x00fa2dac
#define NS_SAVE_HS_SAVE 0x0072e080
#define NS_SAVE_CAN 0x00729bf0      /* fastcall ECX [screen+0x198]: AL may save now */
#define NS_S_GAME 0x198
#define NS_SAVE_SAVE 0x007279e0     /* thiscall (screen, 0), RET 4: save (asks before overwriting) */
#endif
#ifndef NS_S_EDIT
#define NS_S_EDIT 0x298             /* the save's name, IEditLine* (its text a wstr at +0x3c) */
#endif

#if defined(NS_VT_LOAD)

/* Fields and slots an expansion moves: its addresses.h defines the ones that
 * differ (research notes phone-menus/native/expansion-*-addresses.md). */
#ifndef NS_L_CONTAINER
#define NS_L_CONTAINER 0x17c
#endif
#ifndef NS_L_REACTIONS
#define NS_L_REACTIONS 0x174
#endif
#ifndef NS_L_SELECTED
#define NS_L_SELECTED 0x188
#endif
#ifndef NS_L_HEADERS
#define NS_L_HEADERS 0x18c
#endif
#ifndef NS_L_READING
#define NS_L_READING 0x1bc
#endif
#ifndef NS_L_CAN_DELETE
#define NS_L_CAN_DELETE 0x270
#endif

enum {
    L_CONTAINER = NS_L_CONTAINER,     /* IScrollableContainer*: +0x10 SetSelected */
    L_REACTIONS = NS_L_REACTIONS,
    L_SELECTED = NS_L_SELECTED,      /* the chosen row's window */
    L_HEADERS = NS_L_HEADERS,       /* vector<CHeader*>, newest first */
    L_READING = NS_L_READING,       /* non-zero while files are still being read */
    L_CAN_DELETE = NS_L_CAN_DELETE,    /* byte */
    /* a save's header */
    H_DATE = 0x1c,           /* DOS date and time */
    H_IMAGE = 0x30,          /* u32* pixels, +4 present, +8 width, +0xc height */
    H_PLAYERS = 0x40,        /* -1 none */
    H_DIFFICULTY = 0x44,     /* -1 none */
    H_SIZE = 0x4c,
    H_MAP = 0x50,            /* wstr */
    H_DESC = 0x5c,           /* wstr */
    H_NAME = 0x68,           /* wstr */
    H_CAMPAIGN = 0x74,       /* non-zero for a campaign's save (a single game's only) */
    H_ROW = 0xa8,            /* its row's window (a single game's save) */
};

/* The load screens of the other saves, a LAN game's (CLoadScreen<
 * SLanSaveHeaderInfo>, opened by Load on the LAN game's screen) and a hot
 * seat's (CLoadScreen<SHotSeatHeaderInfo>): the same screen with their own
 * functions and longer headers, the row's window at their end. */
/* The Save screens (CSaveScreen<...>) share the load screen's fields, row
 * click and delete of their kind; only the save function is their own. */
#ifndef NS_SAVE_SAVE
#define NS_SAVE_SAVE 0
#endif
#ifndef NS_SAVE_LAN_SAVE
#define NS_SAVE_LAN_SAVE 0
#endif
#ifndef NS_SAVE_HS_SAVE
#define NS_SAVE_HS_SAVE 0
#endif
typedef struct {
    uint32_t click, load, confirmed;
    uint32_t type; /* the game's type function's ECX: 0 a single game, 2 LAN, 4 hot seat */
    uint32_t row;  /* H_ROW */
    uint32_t save; /* the Save screen's save: thiscall (screen, 0), RET 4 */
} LoadKind;
enum { KIND_SINGLE, KIND_LAN, KIND_HOTSEAT };
static const LoadKind KINDS[] = {
    {NS_LOAD_CLICK, NS_LOAD_LOAD, NS_LOAD_CONFIRMED, 0, H_ROW, NS_SAVE_SAVE},
#ifdef NS_VT_LOAD_LAN
    {NS_LOAD_LAN_CLICK, NS_LOAD_LAN_LOAD, NS_LOAD_LAN_CONFIRMED, 2, NS_L_LAN_ROW, NS_SAVE_LAN_SAVE},
#else
    {0, 0, 0, 2, 0, 0},
#endif
#ifdef NS_VT_LOAD_HS
    {NS_LOAD_HS_CLICK, NS_LOAD_HS_LOAD, NS_LOAD_HS_CONFIRMED, 4, NS_L_HS_ROW, NS_SAVE_HS_SAVE},
#else
    {0, 0, 0, 4, 0, 0},
#endif
};
static int g_kind; /* KIND_*: the kind of save the screen being read lists */

/* Whether saves may be deleted here (a game without the flag: always). */
static int can_delete(uint32_t screen) {
    return !L_CAN_DELETE || rd8(screen + L_CAN_DELETE);
}

static int headers(uint32_t screen, uint32_t *b) {
    *b = rd32(screen + L_HEADERS);
    const uint32_t e = rd32(screen + L_HEADERS + 4);
    return ns_ptr(*b) && e >= *b && e - *b <= 4u * 4096u ? (int)((e - *b) / 4u) : 0;
}

static uint32_t chosen_header(uint32_t screen) {
    uint32_t b;
    const int n = headers(screen, &b);
    const uint32_t row = rd32(screen + L_SELECTED);
    for (int i = 0; i < n && ns_ptr(row); ++i)
        if (rd32(rd32(b + 4u * (uint32_t)i) + KINDS[g_kind].row) == row)
            return rd32(b + 4u * (uint32_t)i);
    return 0;
}

static int g_save; /* the screen being read: 0 Load, 1 Save */

/* The name typed for the save, as typing leaves it (SetText). */
static void set_name(Scratch *s, uint32_t screen, const char *utf8) {
    const uint32_t edit = rd32(screen + NS_S_EDIT);
    if (!ns_ptr(edit))
        return;
    const uint32_t wide = ns_wide(s, utf8);
    ns_vcall(s, edit, 0x08, &wide, 1);
}

static void load_command(Scratch *s, uint32_t screen, const char *cmd) {
    char verb[16];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    const uint32_t zero = 0;
    if (g_save && (strcmp(verb, "name") == 0 || strcmp(verb, "save") == 0)) {
#ifdef NS_VT_SAVE
        char text[256];
        ns_cmd_str(cmd, "text", text, sizeof text);
        set_name(s, screen, text);
        /* The game checks the name and asks before overwriting (its box,
         * shown natively). */
        if (verb[0] == 's' && text[0] && KINDS[g_kind].save)
            ns_call(s, KINDS[g_kind].save, screen, 0, &zero, 1);
#endif
        return;
    }
    if (strcmp(verb, "select") == 0) {
        uint32_t b;
        const int n = headers(screen, &b), i = ns_cmd_int(cmd, "index", -1);
        const uint32_t cont = rd32(screen + L_CONTAINER);
        if (i < 0 || i >= n || !ns_ptr(cont))
            return;
        const uint32_t row = rd32(rd32(b + 4u * (uint32_t)i) + KINDS[g_kind].row), one = 1;
        ns_vcall(s, cont, 0x10, &row, 1); /* SetSelected */
        ns_call(s, KINDS[g_kind].click, screen, 0, &one, 1);
    } else if (strcmp(verb, "load") == 0 && chosen_header(screen)) {
        ns_leave();
        ns_call(s, KINDS[g_kind].load, screen, 0, &zero, 1);
    } else if (strcmp(verb, "delete") == 0 && chosen_header(screen) && can_delete(screen) &&
               rd32(screen + L_READING) == 0) {
        /* The view asked first; the game's own question is not shown. */
        ns_call(s, KINDS[g_kind].confirmed, screen + L_REACTIONS, 0, &zero, 1);
    } else if (strcmp(verb, "back") == 0) {
        ns_call(s, NS_LOAD_CLOSE, screen, 0, &zero, 1);
    }
}

/* A save's picture, written for the view once per header (raw BGRA after a
 * 32-bit width and height). The path, or "" when it has none. */
static const char *picture(uint32_t hdr) {
    static uint32_t written;
    static char path[512];
    if (hdr == written)
        return path;
    written = hdr;
    path[0] = 0;
    const uint32_t pixels = rd32(hdr + H_IMAGE), w = rd32(hdr + H_IMAGE + 8), h = rd32(hdr + H_IMAGE + 0xc);
    if (!ns_ptr(pixels) || !rd32(hdr + H_IMAGE + 4) || !w || !h || w > 2048 || h > 2048)
        return path;
    const char *tmp = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/save-picture.bgra", tmp && *tmp ? tmp : "/tmp");
    FILE *f = fopen(path, "wb");
    if (!f) {
        path[0] = 0;
        return path;
    }
    fwrite(&w, 4, 1, f);
    fwrite(&h, 4, 1, f);
    for (uint32_t i = 0; i < w * h; ++i) {
        const uint32_t px = rd32(pixels + 4u * i);
        fwrite(&px, 4, 1, f);
    }
    fclose(f);
    /* A new name each time, so the view does not show a cached old one. */
    static unsigned serial;
    char moved[512];
    snprintf(moved, sizeof moved, "%s/save-picture-%u.bgra", tmp && *tmp ? tmp : "/tmp", ++serial);
    if (rename(path, moved) == 0)
        snprintf(path, sizeof path, "%s", moved);
    return path;
}

static void load_document(Json *j, Scratch *s, uint32_t screen) {
    char diff[64], text[1024];
    ns_difficulty_name(s, 0, diff, sizeof diff);
    ns_ru = (unsigned char)diff[0] >= 0xd0;
    uint32_t b;
    const int n = headers(screen, &b);
    const uint32_t chosen = chosen_header(screen);
    int selected = -1;
    if (g_save)
        j_lit(j, "{\"screen\":\"save_game\",");
    else
        j_lit(j, "{\"screen\":\"load_game\",");
    j_kv_str(j, "lang", ns_ru ? "ru" : "en");
    j_lit(j, ",");
    j_kv_str(j, "title", g_save ? ns_label(s, "/UI/SaveLoad/SaveSpecific/header.txt", "Сохранить игру", "Save Game")
                                : ns_label(s, "/UI/SaveLoad/LoadSpecific/header.txt", "Загрузить игру", "Load Game"));
    int name_ok = 0;
#ifdef NS_VT_SAVE
    if (g_save) {
        /* The name in the field (a tapped save's, or the one typed). The
         * game checks it when saving, and says so in its box. */
        const uint32_t edit = rd32(screen + NS_S_EDIT);
        text[0] = 0;
        if (ns_ptr(edit))
            ns_wstr_utf8(edit + 0x3c, text, sizeof text);
        name_ok = text[0] != 0;
#ifdef NS_SAVE_CAN
        /* As the game's own button: in a network game only the player the
         * game lets save, and not while someone is busy. */
        if (name_ok)
            name_ok = ns_call(s, NS_SAVE_CAN, rd32(screen + NS_S_GAME), 0, NULL, 0) & 0xff;
#endif
        j_lit(j, ",");
        j_kv_str(j, "name", text);
        j_lit(j, ",");
        j_kv_str(j, "name_hint", ns_tr("Название сохранения", "Save name"));
    }
#endif
    j_lit(j, ",\"saves\":[");
    for (int i = 0; i < n; ++i) {
        const uint32_t hdr = rd32(b + 4u * (uint32_t)i);
        const uint32_t d = rd32(hdr + H_DATE);
        if (hdr == chosen)
            selected = i;
        if (i)
            j_lit(j, ",");
        j_lit(j, "{");
        ns_wstr_utf8(hdr + H_NAME, text, sizeof text);
        j_kv_str(j, "name", text);
        j_lit(j, ",");
        ns_wstr_utf8(hdr + H_MAP, text, 256);
        j_kv_str(j, "map", text);
        snprintf(text, sizeof text, "%02u.%02u.%04u %02u:%02u", (d >> 16) & 31, (d >> 21) & 15,
                 (d >> 25) + 1980, (d >> 11) & 31, (d >> 5) & 63);
        j_lit(j, ",");
        j_kv_str(j, "date", text);
        j_lit(j, "}");
    }
    j_printf(j, "],\"selected\":%d,\"reading\":%s,\"info\":", selected,
             rd32(screen + L_READING) ? "true" : "false");
    if (ns_ptr(chosen)) {
        j_lit(j, "{");
        ns_wstr_utf8(chosen + H_MAP, text, sizeof text);
        j_kv_str(j, "name", text);
        j_lit(j, ",\"facts\":[");
        int first = 1;
        const int players = (int)rd32(chosen + H_PLAYERS);
        if (players >= 0) {
            char v[16];
            snprintf(v, sizeof v, "%d", players);
            j_fact(j, first, ns_label(s, "/UI/SaveLoad/SaveInfo/Definition/playerscountname.txt", "Игроков:", "Players:"), v);
            first = 0;
        }
        {
            /* The game's type: fastcall ECX the kind of save, DL whether it
             * is a campaign's. */
            const uint32_t campaign = g_kind == KIND_SINGLE && rd32(chosen + H_CAMPAIGN) != 0;
            ns_wstr_utf8(ns_call(s, NS_LOAD_TYPE, KINDS[g_kind].type, campaign, NULL, 0), text, sizeof text);
            if (text[0]) {
                j_fact(j, first, ns_label(s, "/UI/SaveLoad/SaveInfo/Definition/gametypename.txt", "Тип:", "Type:"), text);
                first = 0;
            }
        }
        {
            const uint32_t w = ns_alloc(s, 12);
            wr32(w, 0);
            ns_call(s, NS_LOAD_SIZE, w, rd32(chosen + H_SIZE), NULL, 0);
            ns_wstr_utf8(w, text, sizeof text);
            ns_free(s, rd32(w));
            if (text[0]) {
                j_fact(j, first, ns_tr("Размер:", "Size:"), text);
                first = 0;
            }
        }
        const int difficulty = (int)rd32(chosen + H_DIFFICULTY);
        if (difficulty >= 0) {
            const uint32_t index = ns_call(s, NS_DIFF_OF_SAVE, (uint32_t)difficulty, 0, NULL, 0);
            const uint32_t entry = ns_call(s, NS_DIFF_ENTRY, index, 0, NULL, 0);
            if (ns_ptr(entry)) {
                ns_text_of(s, NS_TEXT_OF_REF, entry + 4, text, sizeof text);
                j_fact(j, first, ns_label(s, "/UI/SaveLoad/SaveInfo/Definition/difficultyname.txt", "Сложность:", "Difficulty:"), text);
                first = 0;
            }
        }
        j_lit(j, "],");
        ns_wstr_utf8(chosen + H_DESC, text, sizeof text);
        j_kv_str(j, "description", text);
        j_lit(j, ",");
        j_kv_str(j, "preview", picture(chosen));
        j_lit(j, "}");
    } else {
        j_lit(j, "null");
    }
    j_lit(j, ",\"buttons\":[");
    if (g_save)
        j_button(j, 1, "save", ns_label(s, "/UI/SaveLoad/SaveSpecific/save.txt", "Сохранить", "Save"), name_ok);
    else
        j_button(j, 1, "load", ns_label(s, "/UI/SaveLoad/LoadSpecific/load.txt", "Загрузить", "Load"), ns_ptr(chosen));
    j_button(j, 0, "delete", ns_label(s, "/UI/SaveLoad/Delete.txt", "Удалить", "Delete"),
             ns_ptr(chosen) && can_delete(screen) && rd32(screen + L_READING) == 0);
    j_lit(j, "]}");
}

#define LOAD_KIND(name, kind)                                                       \
    static void name##_cmd(Scratch *s, uint32_t screen, const char *cmd) {          \
        g_save = 0;                                                                 \
        g_kind = kind;                                                              \
        load_command(s, screen, cmd);                                               \
    }                                                                               \
    static void name##_doc(Json *j, Scratch *s, uint32_t screen) {                  \
        g_save = 0;                                                                 \
        g_kind = kind;                                                              \
        load_document(j, s, screen);                                                \
    }
LOAD_KIND(load, KIND_SINGLE)

const NsScreen ns_load_game = {.id = "load_game", .vtable = NS_VT_LOAD, .sub = NS_SUB_LOAD,
                               .command = load_cmd, .document = load_doc};
#ifdef NS_VT_LOAD_LAN
LOAD_KIND(lan_load, KIND_LAN)
const NsScreen ns_lan_load = {.id = "lan_load", .vtable = NS_VT_LOAD_LAN, .sub = NS_SUB_LOAD,
                              .command = lan_load_cmd, .document = lan_load_doc};
#else
const NsScreen ns_lan_load = {.id = "lan_load"};
#endif
#ifdef NS_VT_LOAD_HS
LOAD_KIND(hs_load, KIND_HOTSEAT)
const NsScreen ns_hotseat_load = {.id = "hotseat_load", .vtable = NS_VT_LOAD_HS, .sub = NS_SUB_LOAD,
                                  .command = hs_load_cmd, .document = hs_load_doc};
#else
const NsScreen ns_hotseat_load = {.id = "hotseat_load"};
#endif

#if defined(NS_VT_SAVE)
static void save_cmd(Scratch *s, uint32_t screen, const char *cmd) {
    g_save = 1;
    g_kind = KIND_SINGLE;
    load_command(s, screen, cmd);
}
static void save_doc(Json *j, Scratch *s, uint32_t screen) {
    g_save = 1;
    g_kind = KIND_SINGLE;
    load_document(j, s, screen);
}
const NsScreen ns_save_game = {.id = "save_game", .vtable = NS_VT_SAVE, .sub = NS_SUB_SAVE,
                               .command = save_cmd, .document = save_doc};
#else
const NsScreen ns_save_game = {.id = "save_game"};
#endif
#define SAVE_KIND(name, kind)                                                       \
    static void name##_cmd(Scratch *s, uint32_t screen, const char *cmd) {          \
        g_save = 1;                                                                 \
        g_kind = kind;                                                              \
        load_command(s, screen, cmd);                                               \
    }                                                                               \
    static void name##_doc(Json *j, Scratch *s, uint32_t screen) {                  \
        g_save = 1;                                                                 \
        g_kind = kind;                                                              \
        load_document(j, s, screen);                                                \
    }
#if defined(NS_VT_SAVE) && defined(NS_VT_SAVE_LAN) && defined(NS_VT_LOAD_LAN)
SAVE_KIND(lan_save, KIND_LAN)
const NsScreen ns_lan_save = {.id = "lan_save", .vtable = NS_VT_SAVE_LAN, .sub = NS_SUB_SAVE,
                              .command = lan_save_cmd, .document = lan_save_doc};
#else
const NsScreen ns_lan_save = {.id = "lan_save"};
#endif
#if defined(NS_VT_SAVE) && defined(NS_VT_SAVE_HS) && defined(NS_VT_LOAD_HS)
SAVE_KIND(hs_save, KIND_HOTSEAT)
const NsScreen ns_hotseat_save = {.id = "hotseat_save", .vtable = NS_VT_SAVE_HS, .sub = NS_SUB_SAVE,
                                  .command = hs_save_cmd, .document = hs_save_doc};
#else
const NsScreen ns_hotseat_save = {.id = "hotseat_save"};
#endif

#else

/* No addresses for this game: never matched (a zero vtable). */
const NsScreen ns_load_game = {.id = "load_game"};
const NsScreen ns_save_game = {.id = "save_game"};
const NsScreen ns_lan_load = {.id = "lan_load"};
const NsScreen ns_hotseat_load = {.id = "hotseat_load"};
const NsScreen ns_lan_save = {.id = "lan_save"};
const NsScreen ns_hotseat_save = {.id = "hotseat_save"};

#endif
