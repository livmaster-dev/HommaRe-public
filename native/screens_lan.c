/* native/screens_lan.c - the LAN games' screens drawn natively
 * (native/native_screens.c): the server lists ("lan_lobby": NUI::CLobbyScreen
 * and the duel one, CMPDMLobbyScreen), the name and password box
 * ("lan_name": CSimpleEditBox with a lobby's callback) and the duel's "create
 * a game" dialog ("form": CMPDMCreateScreen). The network game's map screen
 * (CMPCreateScreen) is screens_custom.c's third variant.
 *
 * The lobby is the game's: it hears of the games on the network, keeps the
 * chat and who is in; the view shows them and runs the lobby's reactions.
 * The base game's addresses: research notes lan/re-lan-screens.md. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "native_screens.h"
#include "x86.h"

#ifdef RECOMP_OVERRIDE_HEADER
#include RECOMP_OVERRIDE_HEADER
#endif
#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif

/* The base game's; an expansion's addresses.h names its own. */
#ifndef RECOMP_ADDRESS_HEADER
#define NS_VT_EDIT_BOX_LAN 0x00fc83e8   /* CSimpleEditBox's IScreen */
#define NS_SUB_EDIT_BOX_LAN 0x1b8
#define NS_LAN_VT_LOBBY 0x00fc2180      /* CLobbyScreen's IScreen */
#define NS_LAN_SUB_LOBBY 0x200
#define NS_LAN_VT_DUEL_LOBBY 0x00fc237c /* CMPDMLobbyScreen's IScreen */
#define NS_LAN_SUB_DUEL_LOBBY 0x1f8
#define NS_LAN_LOBBY_REACTION 0x0093b320 /* reactions, ECX screen+0x174, RET 0xc */
#define NS_LAN_DUEL_LOBBY_REACTION 0x0093cda0
#define NS_LAN_SELECTED 0x0099a1a0      /* fastcall ECX games view: SGameInfo* or 0 */
#define NS_LAN_PLAYERS_NOW 0x0099b7f0   /* fastcall ECX SGameInfo: players in */
#define NS_LAN_RTTI_IWINDOW 0x010e9ab0
#define NS_LAN_RTTI_LIST_ITEM 0x010ed854
#define NS_LAN_RTTI_OBJECT 0x010e67a0
#define NS_LAN_RTTI_GAME_DATA 0x010f0b6c
#define NS_LAN_RTTI_PLAYER_DATA 0x010f17d0
#define NS_LAN_BOX_CALLBACK_STD 0x00fc2134  /* the box's callback: the standard lobby's */
#define NS_LAN_BOX_CALLBACK_DUEL 0x00fc2334 /* the duel lobby's */
#define NS_LAN_BOX_OK 0x009a7b80        /* fastcall ECX box */
#define NS_LAN_BOX_CANCEL 0x009a7ad0
#define NS_LAN_VT_DUEL_CREATE 0x00fc7cec /* CMPDMCreateScreen's IScreen */
#define NS_LAN_SUB_DUEL_CREATE 0x1b0
#define NS_LAN_DUEL_CREATE_OK 0x0099c900 /* game messages: thiscall (screen, 0), RET 4 */
#define NS_LAN_DUEL_CREATE_CANCEL 0x0099c760
#define NS_LAN_SPEED_NAME 0x00b97ca0    /* fastcall ECX speed 0..3: const wstr* */
#define NS_LAN_DYNAMIC_CAST 0x009fe372  /* __RTDynamicCast, cdecl (p, 0, from, to, 0) */
#define NS_LAN_TEXT 0x00a09de0          /* fastcall ECX a text ref: const wstr* */
#define NS_LAN_MAP_NAME 0x009d9400      /* fastcall ECX info+0xcc: const wstr* */
#define NS_LAN_MAP_SIZE 0x00729b30      /* fastcall ECX out wstr, EDX size */
/* ubi.com mode (research notes lan/re-ubi-screens.md §5): the same lobbies. */
#define NS_LAN_SERVICES 0x005e8050      /* fastcall ECX id: the matchmaker container */
#define NS_LAN_MATCHMAKER_ID 0x8c348b00
#define NS_LAN_CONTAINER_MODE 0x20      /* container vt+0x20: 0 LAN, 1 ubi.com */
#define NS_LAN_L_PROFILE 0x1a8          /* MenuProfile IButton (ubi.com only) */
#define NS_LAN_L_LEAVING 0x1b0          /* CLobbyLeaveListener while changing channel */
#endif

#if defined(NS_LAN_VT_LOBBY)

#ifndef NS_VT_EDIT_BOX_LAN /* an expansion: the edit box its menu already names */
#define NS_VT_EDIT_BOX_LAN NS_VT_EDIT_BOX
#define NS_SUB_EDIT_BOX_LAN NS_SUB_EDIT_BOX
#endif

/* Fields and slots an expansion moves (its addresses.h defines what differs). */
#ifndef NS_LAN_L_REACTIONS
#define NS_LAN_L_REACTIONS 0x174
#endif
#ifndef NS_LAN_L_CHAT
#define NS_LAN_L_CHAT 0x19c       /* CMPChatLogModel */
#endif
#ifndef NS_LAN_L_PLAYERS
#define NS_LAN_L_PLAYERS 0x1a4    /* CPlayersController: [+0x38] the list */
#endif
#ifndef NS_LAN_L_EDIT
#define NS_LAN_L_EDIT 0x1ac       /* the chat's IEditLine */
#endif
#ifndef NS_LAN_L_JOINING
#define NS_LAN_L_JOINING 0x1b4    /* CWaitBox* while joining */
#endif
#ifndef NS_LAN_L_JOIN
#define NS_LAN_L_JOIN 0x1b8       /* MenuJoin IButton */
#endif
#ifndef NS_LAN_L_GAMES
#define NS_LAN_L_GAMES 0x1d4      /* CGamesView2 */
#endif
#ifndef NS_LAN_PLAYERS_LIST
#define NS_LAN_PLAYERS_LIST 0x38
#endif
#ifndef NS_LAN_GV_LIST
#define NS_LAN_GV_LIST 0x08       /* the games view: its IListControl */
#endif
#ifndef NS_LAN_GV_RUNNING
#define NS_LAN_GV_RUNNING 0x24
#endif
#ifndef NS_LAN_GV_OBSERVABLE
#define NS_LAN_GV_OBSERVABLE 0x25
#endif
#ifndef NS_LAN_LIST_ROWS
#define NS_LAN_LIST_ROWS 0x20     /* an IListControl's rows (std::list sentinel) */
#endif
#ifndef NS_LAN_LIST_SELECT
#define NS_LAN_LIST_SELECT 0x28   /* IListControl SelectByData(data) */
#endif
#ifndef NS_LAN_ITEM_DATA
#define NS_LAN_ITEM_DATA 0x04     /* IListControlItem GetData */
#endif
#ifndef NS_LAN_GAME_INFO
#define NS_LAN_GAME_INFO 0x0c     /* CGameData: its SGameInfo */
#endif
/* SGameInfo */
#ifndef NS_LAN_GI_OPEN
#define NS_LAN_GI_OPEN 0x18
#endif
#ifndef NS_LAN_GI_NAME
#define NS_LAN_GI_NAME 0x1c
#endif
#ifndef NS_LAN_GI_LOCKED
#define NS_LAN_GI_LOCKED 0x34
#endif
#ifndef NS_LAN_GI_MAX
#define NS_LAN_GI_MAX 0x6c
#endif
#ifndef NS_LAN_GI_SIZE
#define NS_LAN_GI_SIZE 0x70
#endif
#ifndef NS_LAN_GI_SAVED
#define NS_LAN_GI_SAVED 0x90
#endif
#ifndef NS_LAN_GI_ARENA
#define NS_LAN_GI_ARENA 0xf8
#endif
#ifndef NS_LAN_GI_SPEED
#define NS_LAN_GI_SPEED 0x10c
#endif
/* What a game may lack (Hammers of Fate): the running / full games filters
 * (their reactions and the games view's bytes), the combat speed of a duel. */
#ifndef NS_LAN_HAS_FILTERS
#define NS_LAN_HAS_FILTERS 1
#endif
#ifndef NS_LAN_HAS_SPEED
#define NS_LAN_HAS_SPEED 1
#endif
#ifndef NS_LAN_GI_MAP
#define NS_LAN_GI_MAP 0xcc
#endif
/* chat model */
#ifndef NS_LAN_CHAT_LINES
#define NS_LAN_CHAT_LINES 0x0c
#endif
#ifndef NS_LAN_CHAT_STRIDE
#define NS_LAN_CHAT_STRIDE 0x18
#endif
#ifndef NS_LAN_CHAT_TEXT
#define NS_LAN_CHAT_TEXT 0x0c
#endif
#ifndef NS_LAN_PLAYER_NAME
#define NS_LAN_PLAYER_NAME 0x0c   /* SPlayerData: its name, a narrow str */
#endif
/* the edit box */
#ifndef NS_LAN_BOX_CB
#define NS_LAN_BOX_CB 0x17c
#endif
#ifndef NS_LAN_BOX_EDIT
#define NS_LAN_BOX_EDIT 0x180
#endif
#ifndef NS_LAN_BOX_ID
#define NS_LAN_BOX_ID 0x188       /* 1 the name, 0 a password */
#endif
/* the duel's create dialog */
#ifndef NS_LAN_DC_NAME
#define NS_LAN_DC_NAME 0x17c
#endif
#ifndef NS_LAN_DC_PASSWORD
#define NS_LAN_DC_PASSWORD 0x180
#endif
#ifndef NS_LAN_DC_LOCKED
#define NS_LAN_DC_LOCKED 0x184
#endif
#ifndef NS_LAN_DC_TRIPLE
#define NS_LAN_DC_TRIPLE 0x18c
#endif
#ifndef NS_LAN_DC_SPEED
#define NS_LAN_DC_SPEED 0x194
#endif
/* NS_LAN_DC_REALTIME: Hammers of Fate's "realtime" check box (no speed). */
#ifndef NS_LAN_EDIT_TEXT
#define NS_LAN_EDIT_TEXT 0x3c     /* an IEditLine's text (wstr) */
#endif

/* ----------------------------------------------------------- helpers -- */

static uint32_t cast(Scratch *s, uint32_t p, uint32_t from, uint32_t to) {
    if (!ns_ptr(p))
        return 0;
    const uint32_t args[5] = {p, 0, from, to, 0};
    const uint32_t r = ns_call(s, NS_LAN_DYNAMIC_CAST, 0, 0, args, 5);
    return ns_ptr(r) ? r : 0;
}

/* An IListControl's rows' data, in its list's order (at most cap). */
static int list_data(Scratch *s, uint32_t ilc, uint32_t to, uint32_t *out, int cap) {
    if (!ns_ptr(ilc))
        return 0;
    const uint32_t sentinel = rd32(ilc + NS_LAN_LIST_ROWS);
    int n = 0, guard = 512;
    for (uint32_t node = ns_ptr(sentinel) ? rd32(sentinel) : 0; ns_ptr(node) && node != sentinel && n < cap && guard--;
         node = rd32(node)) {
        const uint32_t item = cast(s, rd32(node + 8), NS_LAN_RTTI_IWINDOW, NS_LAN_RTTI_LIST_ITEM);
        if (!item)
            continue;
        const uint32_t data = ns_vcall(s, item, NS_LAN_ITEM_DATA, NULL, 0);
        const uint32_t typed = cast(s, data, NS_LAN_RTTI_OBJECT, to);
        if (typed)
            out[n++] = typed;
    }
    return n;
}

static void read_narrow(uint32_t str, char *out, size_t cap) {
    const uint32_t b = rd32(str), e = rd32(str + 4);
    size_t n = 0;
    if (ns_ptr(b) && e > b && e - b < cap)
        for (; n < e - b; ++n)
            out[n] = (char)rd8(b + (uint32_t)n);
    out[n] = 0;
}

static void set_edit(Scratch *s, uint32_t edit, const char *utf8) {
    if (!ns_ptr(edit))
        return;
    const uint32_t wide = ns_wide(s, utf8);
    ns_vcall(s, edit, 0x08, &wide, 1); /* SetText */
}

static void message(Scratch *s, uint32_t screen, uint32_t fn) {
    const uint32_t zero = 0;
    ns_call(s, fn, screen, 0, &zero, 1);
}

static int lang_ru(Scratch *s) {
    char diff[64];
    ns_difficulty_name(s, 0, diff, sizeof diff);
    return ns_set_lang(diff);
}

/* The game's chat log, the latest lines ("sender: text"). */
static void j_chat(Json *j, uint32_t model, int enabled) {
    j_lit(j, ",\"chat\":{\"lines\":[");
    uint32_t b = ns_ptr(model) ? rd32(model + NS_LAN_CHAT_LINES) : 0;
    const uint32_t e = ns_ptr(model) ? rd32(model + NS_LAN_CHAT_LINES + 4) : 0;
    const int n = ns_ptr(b) && e > b && e - b <= NS_LAN_CHAT_STRIDE * 4096u ? (int)((e - b) / NS_LAN_CHAT_STRIDE) : 0;
    const int from = n > 60 ? n - 60 : 0;
    for (int i = from; i < n; ++i) {
        const uint32_t entry = b + NS_LAN_CHAT_STRIDE * (uint32_t)i;
        char who[64], text[400], line[480];
        read_narrow(entry, who, sizeof who);
        ns_wstr_utf8(entry + NS_LAN_CHAT_TEXT, text, sizeof text);
        if (who[0])
            snprintf(line, sizeof line, "%s: %s", who, text);
        else
            snprintf(line, sizeof line, "%s", text);
        if (i > from)
            j_lit(j, ",");
        j_str(j, line);
    }
    j_printf(j, "],\"enabled\":%s}", enabled ? "true" : "false");
}

/* The chat and an edit line, for the players' screen (screens_custom.c). */
void ns_lan_chat(Json *j, uint32_t model, int enabled) {
    j_chat(j, model, enabled);
}
void ns_lan_set_edit(Scratch *s, uint32_t edit, const char *utf8) {
    set_edit(s, edit, utf8);
}

/* ------------------------------------------------------------ lobbies -- */

static int g_duel; /* the lobby being read: 0 standard, 1 duel */

static uint32_t lobby_reaction(void) {
    return g_duel ? NS_LAN_DUEL_LOBBY_REACTION : NS_LAN_LOBBY_REACTION;
}

/* The lobby is ubi.com's (ubi-gs): its Menu panel instead of MenuLan, no
 * name, duel or standard button (the channel decides), profile and channel. */
static int lobby_ubi(Scratch *s) {
#ifdef NS_LAN_SERVICES
    const uint32_t container = ns_call(s, NS_LAN_SERVICES, NS_LAN_MATCHMAKER_ID, 0, NULL, 0);
    return ns_ptr(container) && ns_vcall(s, container, NS_LAN_CONTAINER_MODE, NULL, 0) == 1;
#else
    (void)s;
    return 0;
#endif
}

/* The players list's selected row, as its index in `players`, or -1. */
static int selected_player(Scratch *s, uint32_t list, const uint32_t *players, int n) {
    const uint32_t item = ns_ptr(list) ? ns_vcall(s, list, 0x24, NULL, 0) : 0; /* GetSelected */
    const uint32_t data = ns_ptr(item) ? cast(s, ns_vcall(s, item, NS_LAN_ITEM_DATA, NULL, 0), NS_LAN_RTTI_OBJECT,
                                              NS_LAN_RTTI_PLAYER_DATA)
                                       : 0;
    for (int i = 0; data && i < n; ++i)
        if (players[i] == data)
            return i;
    return -1;
}

static void lobby_command(Scratch *s, uint32_t screen, const char *cmd) {
    char verb[16], name[32], text[256];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    const uint32_t reactions = screen + NS_LAN_L_REACTIONS;
    const uint32_t gv = rd32(screen + NS_LAN_L_GAMES);
    if (strcmp(verb, "select") == 0 && ns_ptr(gv)) {
        static uint32_t games[64];
        const int n = list_data(s, rd32(gv + NS_LAN_GV_LIST), NS_LAN_RTTI_GAME_DATA, games, 64);
        const int i = ns_cmd_int(cmd, "index", -1);
        if (i >= 0 && i < n) {
            const uint32_t data = games[i];
            ns_vcall(s, rd32(gv + NS_LAN_GV_LIST), NS_LAN_LIST_SELECT, &data, 1);
        }
    } else if (strcmp(verb, "select_player") == 0) {
        static uint32_t players[64];
        const uint32_t ctrl = rd32(screen + NS_LAN_L_PLAYERS);
        const uint32_t list = ns_ptr(ctrl) ? rd32(ctrl + NS_LAN_PLAYERS_LIST) : 0;
        const int n = list_data(s, list, NS_LAN_RTTI_PLAYER_DATA, players, 64);
        const int i = ns_cmd_int(cmd, "index", -1);
        if (i >= 0 && i < n) {
            const uint32_t data = players[i];
            ns_vcall(s, list, NS_LAN_LIST_SELECT, &data, 1);
        }
    } else if (strcmp(verb, "press") == 0) {
        ns_cmd_str(cmd, "name", name, sizeof name);
        static const char *const lan[] = {"join", "create", "changename", "duelmode", "standardmode", "disconnect"};
        static const char *const ubi[] = {"join", "create", "profile", "channel", "disconnect"};
        const int is_ubi = lobby_ubi(s);
        const char *const *ok = is_ubi ? ubi : lan;
        const size_t count = is_ubi ? sizeof ubi / sizeof ubi[0] : sizeof lan / sizeof lan[0];
        for (size_t k = 0; k < count; ++k)
            if (strcmp(name, ok[k]) == 0)
                ns_react(s, reactions, lobby_reaction(), name);
        if (strcmp(name, "back") == 0)
            ns_react(s, reactions, lobby_reaction(), "disconnect");
    } else if (NS_LAN_HAS_FILTERS && strcmp(verb, "filter") == 0 && ns_ptr(gv)) {
        const int id = ns_cmd_int(cmd, "id", -1);
        if (id == 0)
            ns_react(s, reactions, lobby_reaction(), rd8(gv + NS_LAN_GV_RUNNING) ? "hide_running" : "show_running");
        else if (id == 1 && g_duel)
            ns_react(s, reactions, lobby_reaction(),
                     rd8(gv + NS_LAN_GV_OBSERVABLE) ? "hide_observable" : "show_observable");
    } else if (strcmp(verb, "chat") == 0) {
        ns_cmd_str(cmd, "text", text, sizeof text);
        if (text[0]) {
            set_edit(s, rd32(screen + NS_LAN_L_EDIT), text);
            ns_react(s, reactions, lobby_reaction(), "chat_edit_line_return");
        }
    }
}

/* A size's name, as the load screen's facts give it. */
static void size_name(Scratch *s, uint32_t size, char *out, size_t cap) {
    const uint32_t w = ns_alloc(s, 12);
    wr32(w, 0);
    ns_call(s, NS_LAN_MAP_SIZE, w, size, NULL, 0);
    ns_wstr_utf8(w, out, cap);
    ns_free(s, rd32(w));
}

static void lobby_document(Json *j, Scratch *s, uint32_t screen) {
    lang_ru(s);
    const uint32_t gv = rd32(screen + NS_LAN_L_GAMES);
    static uint32_t games[64];
    const int n = ns_ptr(gv) ? list_data(s, rd32(gv + NS_LAN_GV_LIST), NS_LAN_RTTI_GAME_DATA, games, 64) : 0;
    const uint32_t chosen = ns_ptr(gv) ? ns_call(s, NS_LAN_SELECTED, gv, 0, NULL, 0) : 0;
    j_lit(j, "{\"screen\":\"lan_lobby\",");
    j_kv_str(j, "lang", ns_lang_code());
    j_lit(j, ",");
    j_kv_str(j, "title", g_duel ? ns_tr("Дуэль: список серверов", "Duel: Server List")
                                : ns_tr("Стандартный режим: список серверов", "Standard Mode: Server List"));
    j_lit(j, ",\"columns\":[");
    if (g_duel) {
        j_str(j, ns_tr("Имя", "Name"));
        j_lit(j, ",");
        j_str(j, ns_tr("Арена", "Arena"));
        if (NS_LAN_HAS_SPEED) {
            j_lit(j, ",");
            j_str(j, ns_tr("Скорость", "Speed"));
        }
    } else {
        j_str(j, ns_tr("Имя", "Name"));
        j_lit(j, ",");
        j_str(j, ns_tr("Игроки", "Players"));
        j_lit(j, ",");
        j_str(j, ns_tr("Карта", "Map"));
        j_lit(j, ",");
        j_str(j, ns_tr("Размер", "Size"));
    }
    j_lit(j, "],\"games\":[");
    int selected = -1;
    for (int i = 0; i < n; ++i) {
        const uint32_t info = games[i] + NS_LAN_GAME_INFO;
        char text[256];
        if (info == chosen)
            selected = i;
        if (i)
            j_lit(j, ",");
        j_lit(j, "{\"cells\":[");
        ns_wstr_utf8(info + NS_LAN_GI_NAME, text, sizeof text);
        j_str(j, text);
        if (g_duel) {
            ns_text_of(s, NS_LAN_TEXT, info + NS_LAN_GI_ARENA, text, sizeof text);
            j_lit(j, ",");
            j_str(j, text);
#if NS_LAN_HAS_SPEED
            const uint32_t speed = rd32(info + NS_LAN_GI_SPEED);
            text[0] = 0;
            if (speed < 4)
                ns_text_of(s, NS_LAN_SPEED_NAME, speed, text, sizeof text);
            j_lit(j, ",");
            j_str(j, text);
#endif
        } else {
#ifdef NS_LAN_GI_PLAYERS /* no "players in" function: the field the game's list reads */
            const uint32_t now = rd32(info + NS_LAN_GI_PLAYERS);
#else
            const uint32_t now = ns_call(s, NS_LAN_PLAYERS_NOW, info, 0, NULL, 0);
#endif
            snprintf(text, sizeof text, "%u/%u", now, rd32(info + NS_LAN_GI_MAX));
            j_lit(j, ",");
            j_str(j, text);
            ns_text_of(s, NS_LAN_MAP_NAME, info + NS_LAN_GI_MAP, text, sizeof text);
            j_lit(j, ",");
            j_str(j, text);
            size_name(s, rd32(info + NS_LAN_GI_SIZE), text, sizeof text);
            j_lit(j, ",");
            j_str(j, text);
        }
        j_printf(j, "],\"started\":%s,\"locked\":%s}", rd8(info + NS_LAN_GI_OPEN) ? "false" : "true",
                 rd8(info + NS_LAN_GI_LOCKED) ? "true" : "false");
    }
    j_printf(j, "],\"selected\":%d,\"filters\":[", selected);
#if NS_LAN_HAS_FILTERS
    j_lit(j, "{\"id\":0,");
    j_kv_str(j, "label", ns_tr("Запущенные игры", "Running games"));
    j_printf(j, ",\"on\":%s}", ns_ptr(gv) && rd8(gv + NS_LAN_GV_RUNNING) ? "true" : "false");
    if (g_duel) {
        j_lit(j, ",{\"id\":1,");
        j_kv_str(j, "label", ns_tr("Заполненные игры", "Full games"));
        j_printf(j, ",\"on\":%s}", ns_ptr(gv) && rd8(gv + NS_LAN_GV_OBSERVABLE) ? "true" : "false");
    }
#endif
    j_lit(j, "],");
    j_kv_str(j, "players_title", ns_tr("Игроки", "Players"));
    j_lit(j, ",\"players\":[");
    const int ubi = lobby_ubi(s);
    int player = -1;
    {
        static uint32_t players[64];
        const uint32_t ctrl = rd32(screen + NS_LAN_L_PLAYERS);
        const uint32_t list = ns_ptr(ctrl) ? rd32(ctrl + NS_LAN_PLAYERS_LIST) : 0;
        const int m = list_data(s, list, NS_LAN_RTTI_PLAYER_DATA, players, 64);
        for (int i = 0; i < m; ++i) {
            char who[64];
            read_narrow(players[i] + NS_LAN_PLAYER_NAME, who, sizeof who);
            if (i)
                j_lit(j, ",");
            j_str(j, who);
        }
        /* ubi.com: a player is picked for their profile. */
        if (ubi)
            player = selected_player(s, list, players, m);
    }
    j_lit(j, "]");
    if (ubi) {
        j_printf(j, ",\"player_selected\":%d,", player);
        j_kv_str(j, "empty", ns_tr("На этом канале пока нет игр", "No games on this channel yet"));
    }
    j_chat(j, rd32(screen + NS_LAN_L_CHAT), 1);
    const int joining = ns_ptr(rd32(screen + NS_LAN_L_JOINING));
#ifdef NS_LAN_L_LEAVING
    const int leaving = ubi && ns_ptr(rd32(screen + NS_LAN_L_LEAVING));
#else
    const int leaving = 0;
#endif
    if (joining || leaving) {
        j_lit(j, ",");
        j_kv_str(j, "busy", joining ? ns_tr("Выполняется вход в игру...", "Joining the game...")
                                    : ns_tr("Смена канала...", "Changing the channel..."));
    }
    const int idle = !joining && !leaving;
    const uint32_t join = ns_iwindow(rd32(screen + NS_LAN_L_JOIN));
    j_lit(j, ",\"buttons\":[");
    j_button(j, 1, "join", ns_tr("Войти", "Join"), idle && join && ns_enabled(s, join));
    j_button(j, 0, "create", ns_tr("Создать", "Create"), idle);
    if (ubi) {
#ifdef NS_LAN_L_PROFILE
        const uint32_t profile = ns_iwindow(rd32(screen + NS_LAN_L_PROFILE));
        j_button(j, 0, "profile", ns_tr("Профиль", "Profile"), idle && player >= 0 && profile && ns_enabled(s, profile));
#endif
        j_button(j, 0, "channel", ns_tr("Канал", "Channel"), idle);
    } else {
        j_button(j, 0, "changename", ns_tr("Имя игрока", "Player Name"), idle);
        if (g_duel)
            j_button(j, 0, "standardmode", ns_tr("Обычный режим", "Standard Mode"), idle);
        else
            j_button(j, 0, "duelmode", ns_tr("Дуэль", "Duel"), idle);
    }
    j_button(j, 0, "back", ns_tr("Выйти", "Leave"), 1);
    j_lit(j, "]}");
}

static void std_lobby_command(Scratch *s, uint32_t screen, const char *cmd) {
    g_duel = 0;
    lobby_command(s, screen, cmd);
}
static void std_lobby_document(Json *j, Scratch *s, uint32_t screen) {
    g_duel = 0;
    lobby_document(j, s, screen);
}
static void duel_lobby_command(Scratch *s, uint32_t screen, const char *cmd) {
    g_duel = 1;
    lobby_command(s, screen, cmd);
}
static void duel_lobby_document(Json *j, Scratch *s, uint32_t screen) {
    g_duel = 1;
    lobby_document(j, s, screen);
}

const NsScreen ns_lan_lobby = {.id = "lan_lobby", .vtable = NS_LAN_VT_LOBBY, .sub = NS_LAN_SUB_LOBBY,
                               .command = std_lobby_command, .document = std_lobby_document};
const NsScreen ns_lan_duel_lobby = {.id = "lan_duel_lobby", .vtable = NS_LAN_VT_DUEL_LOBBY,
                                    .sub = NS_LAN_SUB_DUEL_LOBBY, .command = duel_lobby_command,
                                    .document = duel_lobby_document};

/* ------------------------------------------------- the name / password -- */

static int box_claims(uint32_t box) {
    const uint32_t cb = rd32(box + NS_LAN_BOX_CB);
    return ns_ptr(cb) && (rd32(cb) == NS_LAN_BOX_CALLBACK_STD || rd32(cb) == NS_LAN_BOX_CALLBACK_DUEL);
}

static void box_command(Scratch *s, uint32_t box, const char *cmd) {
    char verb[16], text[64];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    if (strcmp(verb, "accept") == 0) {
        ns_cmd_str(cmd, "text", text, sizeof text);
        if (!text[0])
            return;
        set_edit(s, rd32(box + NS_LAN_BOX_EDIT), text);
        ns_call(s, NS_LAN_BOX_OK, box, 0, NULL, 0);
    } else if (strcmp(verb, "cancel") == 0) {
        ns_call(s, NS_LAN_BOX_CANCEL, box, 0, NULL, 0);
    }
}

static void box_document(Json *j, Scratch *s, uint32_t box) {
    lang_ru(s);
    char text[64] = "";
    const uint32_t edit = rd32(box + NS_LAN_BOX_EDIT);
    if (ns_ptr(edit))
        ns_wstr_utf8(edit + NS_LAN_EDIT_TEXT, text, sizeof text);
    const int name = rd32(box + NS_LAN_BOX_ID) == 1;
    j_lit(j, "{\"screen\":\"lan_name\",");
    j_kv_str(j, "lang", ns_lang_code());
    j_lit(j, ",");
    j_kv_str(j, "title", name ? ns_tr("Ввод имени", "Your Name") : ns_tr("Ввод пароля", "Password"));
    j_lit(j, ",");
    j_kv_str(j, "text", text);
    j_lit(j, ",");
    j_kv_str(j, "hint", name ? ns_tr("Так вас увидят другие игроки", "How other players see you") : "");
    j_lit(j, ",");
    j_kv_str(j, "ok", "OK");
    j_lit(j, ",");
    j_kv_str(j, "cancel", ns_tr("Отмена", "Cancel"));
    j_printf(j, ",\"max\":16,\"secret\":%s}", name ? "false" : "true");
}

const NsScreen ns_lan_name = {.id = "lan_name", .vtable = NS_VT_EDIT_BOX_LAN, .sub = NS_SUB_EDIT_BOX_LAN,
                              .claims = box_claims, .command = box_command, .document = box_document};

/* ------------------------------------------------ the duel's new game -- */

/* Not mapped in every game (an expansion may name the lobbies alone). */
#if defined(NS_LAN_VT_DUEL_CREATE)


enum { F_NAME, F_LOCKED, F_PASSWORD, F_TRIPLE, F_SPEED, F_REALTIME };

/* A check box: IButton GetState, 0 is on. */
static int button_on(Scratch *s, uint32_t button) {
    return ns_ptr(button) && ns_vcall(s, button, 0x14, NULL, 0) == 0;
}

static void dcreate_command(Scratch *s, uint32_t screen, const char *cmd) {
    char verb[16], name[32], text[64];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    const int id = ns_cmd_int(cmd, "id", -1), value = ns_cmd_int(cmd, "value", -1);
    if (strcmp(verb, "text") == 0) {
        ns_cmd_str(cmd, "text", text, sizeof text);
        if (id == F_NAME || id == F_PASSWORD)
            set_edit(s, rd32(screen + (id == F_NAME ? NS_LAN_DC_NAME : NS_LAN_DC_PASSWORD)), text);
    } else if (strcmp(verb, "set") == 0 && value >= 0) {
        uint32_t box = 0;
        if (id == F_LOCKED)
            box = NS_LAN_DC_LOCKED;
        else if (id == F_TRIPLE)
            box = NS_LAN_DC_TRIPLE;
#ifdef NS_LAN_DC_REALTIME
        else if (id == F_REALTIME)
            box = NS_LAN_DC_REALTIME;
#endif
        if (box) {
            const uint32_t state = value ? 0 : 1;
            const uint32_t b = rd32(screen + box);
            if (ns_ptr(b))
                ns_vcall(s, b, 0x0c, &state, 1); /* SetState: runs its commands */
        } else if (NS_LAN_HAS_SPEED && id == F_SPEED && value < 4) {
            const uint32_t seg = (uint32_t)value, bar = rd32(screen + NS_LAN_DC_SPEED);
            if (ns_ptr(bar))
                ns_vcall(s, bar, NS_SLIDER_SET_SEGMENT, &seg, 1);
        }
    } else if (strcmp(verb, "press") == 0) {
        ns_cmd_str(cmd, "name", name, sizeof name);
        if (strcmp(name, "ok") == 0)
            message(s, screen, NS_LAN_DUEL_CREATE_OK);
        else if (strcmp(name, "cancel") == 0 || strcmp(name, "back") == 0)
            message(s, screen, NS_LAN_DUEL_CREATE_CANCEL);
    }
}

static void j_text_field(Json *j, int first, int id, const char *kind, const char *label, int enabled, uint32_t edit) {
    char text[64] = "";
    if (ns_ptr(edit))
        ns_wstr_utf8(edit + NS_LAN_EDIT_TEXT, text, sizeof text);
    j_printf(j, "%s{\"id\":%d,", first ? "" : ",", id);
    j_kv_str(j, "kind", kind);
    j_lit(j, ",");
    j_kv_str(j, "label", label);
    j_lit(j, ",");
    j_kv_str(j, "text", text);
    j_printf(j, ",\"enabled\":%s}", enabled ? "true" : "false");
}

static void j_toggle_field(Json *j, int id, const char *label, int on) {
    j_printf(j, ",{\"id\":%d,\"kind\":\"toggle\",", id);
    j_kv_str(j, "label", label);
    j_printf(j, ",\"on\":%s,\"enabled\":true}", on ? "true" : "false");
}

static void dcreate_document(Json *j, Scratch *s, uint32_t screen) {
    lang_ru(s);
    const int locked = button_on(s, rd32(screen + NS_LAN_DC_LOCKED));
    j_lit(j, "{\"screen\":\"form\",");
    j_kv_str(j, "lang", ns_lang_code());
    j_lit(j, ",");
    j_kv_str(j, "title", ns_tr("Создание игры", "New Game"));
    j_lit(j, ",\"fields\":[");
    j_text_field(j, 1, F_NAME, "text", ns_tr("Название игры", "Game name"), 1, rd32(screen + NS_LAN_DC_NAME));
    j_toggle_field(j, F_LOCKED, ns_tr("Закрытая игра", "Private game"), locked);
    j_text_field(j, 0, F_PASSWORD, "secret", ns_tr("Пароль", "Password"), locked, rd32(screen + NS_LAN_DC_PASSWORD));
    j_toggle_field(j, F_TRIPLE, ns_tr("Режим 3x3", "3x3 mode"), button_on(s, rd32(screen + NS_LAN_DC_TRIPLE)));
#ifdef NS_LAN_DC_REALTIME
    j_toggle_field(j, F_REALTIME, ns_tr("Динамичные бои", "Dynamic battles"),
                   button_on(s, rd32(screen + NS_LAN_DC_REALTIME)));
#endif
#if NS_LAN_HAS_SPEED
    const uint32_t bar = rd32(screen + NS_LAN_DC_SPEED);
    if (ns_ptr(bar)) {
        int seg = (int)ns_vcall(s, bar, NS_SLIDER_GET_SEGMENT, NULL, 0);
        seg = seg < 0 ? 0 : seg > 3 ? 3 : seg;
        j_printf(j, ",{\"id\":%d,\"kind\":\"choice\",", F_SPEED);
        j_kv_str(j, "label", ns_tr("Динамичные бои", "Combat speed"));
        j_printf(j, ",\"index\":%d,\"enabled\":true,\"values\":[", seg);
        for (uint32_t i = 0; i < 4; ++i) {
            char v[64];
            ns_text_of(s, NS_LAN_SPEED_NAME, i, v, sizeof v);
            if (i)
                j_lit(j, ",");
            j_str(j, v);
        }
        j_lit(j, "]}");
    }
#endif
    j_lit(j, "],\"buttons\":[");
    j_button(j, 1, "cancel", ns_tr("Отмена", "Cancel"), 1);
    j_button(j, 0, "ok", ns_tr("Создать", "Create"), 1);
    j_lit(j, "]}");
}

const NsScreen ns_lan_duel_create = {.id = "lan_duel_create", .vtable = NS_LAN_VT_DUEL_CREATE,
                                     .sub = NS_LAN_SUB_DUEL_CREATE, .command = dcreate_command,
                                     .document = dcreate_document};

#else

const NsScreen ns_lan_duel_create = {.id = "lan_duel_create"};

#endif

#else

/* No addresses for this game: never matched (a zero vtable). */
const NsScreen ns_lan_lobby = {.id = "lan_lobby"};
const NsScreen ns_lan_duel_lobby = {.id = "lan_duel_lobby"};
const NsScreen ns_lan_name = {.id = "lan_name"};
const NsScreen ns_lan_duel_create = {.id = "lan_duel_create"};

#endif
