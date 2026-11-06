/* native/screens_hof.c - the Hall of Fame drawn natively
 * (native/native_screens.c): NUI::CHallOfFameScreen ("hall_of_fame").
 * Two books of scores (the campaigns', the scenarios'), each a list of
 * entries the filter chooses from, each entry its table of rows.
 * The base game's addresses: research notes phone-menus/native/re-campaigns-hof.md. */
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
#define NS_VT_HOF 0x00fc35c8        /* the Hall of Fame's IScreen, at +0x1cc */
#define NS_SUB_HOF 0x1cc
#define NS_HOF_REACTION 0x00952370  /* its reactions, ECX screen+0x174, RET 0xc */
#define NS_HOF_PICK 0x00952a80      /* thiscall (screen, entry, row to light -1), RET 8 */
#define NS_HOF_CLEAR 0x00953490     /* thiscall (screen+0x17c, the box's OK id), RET 4 */
#define NS_HOF_CLEAR_OK 0x17363b00
#endif

#if defined(NS_VT_HOF)

/* Fields and slots an expansion moves: its addresses.h defines the ones that
 * differ (research notes phone-menus/native/expansion-*-addresses.md). */
#ifndef NS_HF_REACTIONS
#define NS_HF_REACTIONS 0x174
#endif
#ifndef NS_HF_BOXES
#define NS_HF_BOXES 0x17c
#endif
#ifndef NS_HF_DATA
#define NS_HF_DATA 0x198
#endif
#ifndef NS_HF_BOOK
#define NS_HF_BOOK 0x19c
#endif
#ifndef NS_HF_ENTRY
#define NS_HF_ENTRY 0x1a0
#endif

enum {
    HF_REACTIONS = NS_HF_REACTIONS,
    HF_BOXES = NS_HF_BOXES,
    HF_DATA = NS_HF_DATA,         /* -> +0x14: the books, 0x10 each: +4 entries' begin, +8 end */
    HF_BOOK = NS_HF_BOOK,         /* 0 the campaigns', 1 the scenarios' */
    HF_ENTRY = NS_HF_ENTRY,
    ENTRY_SIZE = 0x74,       /* +4 wstr name, +0x10 wstr second name, +0x1c rows' vector */
    ROW_SIZE = 0x54,         /* +0 rank, +4 wstr name, +0x10 days, +0x14 score, +0x48 wstr badge */
    ROW_BADGE_PATH = 0x2c,   /* str: the badge's texture (a creature's icon), its path in the paks */
};

static int entries(uint32_t screen, uint32_t *begin) {
    const uint32_t books = rd32(rd32(screen + HF_DATA) + 0x14);
    const uint32_t book = books + 0x10u * (rd32(screen + HF_BOOK) & 1u);
    *begin = ns_ptr(books) ? rd32(book + 4) : 0;
    const uint32_t end = ns_ptr(books) ? rd32(book + 8) : 0;
    return ns_ptr(*begin) && end >= *begin && end - *begin <= ENTRY_SIZE * 256u
               ? (int)((end - *begin) / ENTRY_SIZE)
               : 0;
}

static void hof_command(Scratch *s, uint32_t screen, const char *cmd) {
    char verb[16], name[24];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    if (strcmp(verb, "book") == 0) {
        const int i = ns_cmd_int(cmd, "index", 0);
        ns_react(s, screen + HF_REACTIONS, NS_HOF_REACTION, i ? "bookmark02" : "bookmark01");
    } else if (strcmp(verb, "entry") == 0) {
        uint32_t b;
        const int i = ns_cmd_int(cmd, "index", -1);
        if (i >= 0 && i < entries(screen, &b)) {
            const uint32_t args[2] = {(uint32_t)i, 0xffffffffu};
            ns_call(s, NS_HOF_PICK, screen, 0, args, 2);
        }
    } else if (strcmp(verb, "clear") == 0) {
        /* The view asked first; the game's own question is not shown. */
        const uint32_t ok = NS_HOF_CLEAR_OK;
        ns_call(s, NS_HOF_CLEAR, screen + HF_BOXES, 0, &ok, 1);
    } else if (strcmp(verb, "press") == 0) {
        ns_cmd_str(cmd, "name", name, sizeof name);
        if (strcmp(name, "close") == 0)
            ns_react(s, screen + HF_REACTIONS, NS_HOF_REACTION, "close");
    }
}

static void hof_document(Json *j, Scratch *s, uint32_t screen) {
    char diff[64], a[128], b2[128], text[300];
    ns_difficulty_name(s, 0, diff, sizeof diff);
    ns_set_lang(diff);
    uint32_t b;
    const int n = entries(screen, &b);
    int entry = (int)rd32(screen + HF_ENTRY);
    entry = entry < 0 || entry >= n ? -1 : entry;
    j_lit(j, "{\"screen\":\"hall_of_fame\",");
    j_kv_str(j, "lang", ns_lang_code());
    j_lit(j, ",");
    j_kv_str(j, "title", ns_label(s, "/UI/MainMenu2/HallOfFame/Header.txt", "Зал славы", "Hall of Fame"));
    j_lit(j, ",\"books\":[");
    j_str(j, ns_label(s, "/UI/MainMenu2/HallOfFame/CategoryCampaign.txt", "Кампании", "Campaigns"));
    j_lit(j, ",");
    j_str(j, ns_label(s, "/UI/MainMenu2/HallOfFame/CategorySingle.txt", "Сценарии", "Scenarios"));
    j_printf(j, "],\"book\":%u,\"entries\":[", rd32(screen + HF_BOOK) & 1u);
    for (int i = 0; i < n; ++i) {
        const uint32_t e = b + ENTRY_SIZE * (uint32_t)i;
        ns_wstr_utf8(e + 0x04, a, sizeof a);
        ns_wstr_utf8(e + 0x10, b2, sizeof b2);
        snprintf(text, sizeof text, b2[0] ? "%s – %s" : "%s", a, b2);
        if (i)
            j_lit(j, ",");
        j_str(j, text);
    }
    j_printf(j, "],\"entry\":%d,\"rows\":[", entry);
    if (entry >= 0) {
        const uint32_t e = b + ENTRY_SIZE * (uint32_t)entry;
        const uint32_t rb = rd32(e + 0x1c), re = rd32(e + 0x20);
        const int rows = ns_ptr(rb) && re >= rb && re - rb <= ROW_SIZE * 64u ? (int)((re - rb) / ROW_SIZE) : 0;
        for (int i = 0; i < rows; ++i) {
            const uint32_t r = rb + ROW_SIZE * (uint32_t)i;
            ns_wstr_utf8(r + 0x04, a, sizeof a);
            ns_wstr_utf8(r + 0x48, b2, sizeof b2);
            j_printf(j, "%s{\"rank\":%d,", i ? "," : "", (int)rd32(r));
            j_kv_str(j, "name", a);
            j_lit(j, ",");
            j_kv_str(j, "badge", b2);
            {
                char path[256];
                const uint32_t pb = rd32(r + ROW_BADGE_PATH), pe = rd32(r + ROW_BADGE_PATH + 4);
                uint32_t n = 0;
                if (ns_ptr(pb) && pe > pb && pe - pb < sizeof path)
                    for (; n < pe - pb; ++n)
                        path[n] = (char)rd8(pb + n);
                path[n] = 0;
                j_lit(j, ",");
                j_kv_str(j, "badge_icon", path);
            }
            j_printf(j, ",\"days\":%d,\"score\":%d}", (int)rd32(r + 0x10), (int)rd32(r + 0x14));
        }
    }
    j_lit(j, "],\"columns\":[");
    j_str(j, ns_label(s, "/UI/MainMenu2/HallOfFame/RankHeader.txt", "Звание", "Rank"));
    j_lit(j, ",");
    j_str(j, ns_label(s, "/UI/MainMenu2/HallOfFame/NameHeader.txt", "Имя", "Name"));
    j_lit(j, ",");
    j_str(j, ns_label(s, "/UI/MainMenu2/HallOfFame/DaysHeader.txt", "Дни", "Days"));
    j_lit(j, ",");
    j_str(j, ns_label(s, "/UI/MainMenu2/HallOfFame/PointsHeader.txt", "Очки", "Score"));
    j_lit(j, ",");
    j_str(j, ns_label(s, "/UI/MainMenu2/HallOfFame/IconHeader.txt", "Значок", "Badge"));
    j_lit(j, "],\"buttons\":[");
    j_button(j, 1, "clear", ns_label(s, "/UI/MainMenu2/HallOfFame/Clear.txt", "Очистить", "Clear"), entry >= 0);
    j_button(j, 0, "close", ns_label(s, "/UI/MainMenu2/HallOfFame/Close.txt", "Закрыть", "Close"), 1);
    j_lit(j, "]}");
}

const NsScreen ns_hall_of_fame = {.id = "hall_of_fame", .vtable = NS_VT_HOF, .sub = NS_SUB_HOF,
                                  .command = hof_command, .document = hof_document};

#else

/* No addresses for this game: never matched (a zero vtable). */
const NsScreen ns_hall_of_fame = {.id = "hall_of_fame"};

#endif
