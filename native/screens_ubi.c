/* native/screens_ubi.c - the ubi.com (ubi-gs) screens drawn natively
 * (native/native_screens.c): the login ("ubi_login": CMPRegisterScreen, which
 * also initialises ubi.com and checks the CD key first), the CD key box
 * ("ubi_cdkey": CSimpleEditBox with the login screen's callback) and the
 * channel list ("ubi_channels": CMPChangeChannelScreen). The lobby after the
 * channel is the LAN one in ubi.com mode (screens_lan.c).
 *
 * The screens are the game's: they talk to the server, keep the fields and the
 * list; the views show them and press the screens' own buttons. The base
 * game's addresses: research notes lan/re-ubi-screens.md. */
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

/* The base game's only: the expansions' inner offsets are not mapped. */
#ifndef RECOMP_ADDRESS_HEADER
#define NS_UBI_VT_LOGIN 0x00fc2cb0     /* CMPRegisterScreen's IScreen */
#define NS_UBI_SUB_LOGIN 0x1e0
#define NS_UBI_LOGIN_OK 0x00947010     /* game messages: thiscall (screen, 0), RET 4 */
#define NS_UBI_LOGIN_CANCEL 0x009473e0
#define NS_UBI_VT_EDIT_BOX 0x00fc83e8  /* CSimpleEditBox's IScreen */
#define NS_UBI_SUB_EDIT_BOX 0x1b8
#define NS_UBI_BOX_CALLBACK 0x00fc2c48 /* the box's callback: the login screen's (the CD key) */
#define NS_UBI_BOX_OK 0x009a7b80       /* fastcall ECX box */
#define NS_UBI_BOX_CANCEL 0x009a7ad0
#define NS_UBI_VT_CHANNELS 0x00fb70f0  /* CMPChangeChannelScreen's IScreen */
#define NS_UBI_SUB_CHANNELS 0x1e0
#define NS_UBI_CHANNELS_JOIN 0x0088e7a0 /* game messages: thiscall (screen, 0), RET 4 */
#define NS_UBI_CHANNELS_CANCEL 0x0088e740
#define NS_UBI_VT_WAIT_BOX 0x00fadd04  /* CWaitBox */
#define NS_UBI_RTTI_IWINDOW 0x010e9ab0
#define NS_UBI_RTTI_LIST_ITEM 0x010ed854
#define NS_UBI_RTTI_OBJECT 0x010e67a0
#define NS_UBI_RTTI_CHANNEL 0x010ed774 /* CChannelData */
#define NS_UBI_DYNAMIC_CAST 0x009fe372 /* __RTDynamicCast, cdecl (p, 0, from, to, 0) */
#endif

#if defined(NS_UBI_VT_LOGIN)

/* Fields an expansion moves (its addresses.h defines what differs). */
/* CMPRegisterScreen */
#ifndef NS_UBI_UL_LOGIN
#define NS_UBI_UL_LOGIN 0x1a8    /* IEditLine */
#endif
#ifndef NS_UBI_UL_PASSWORD
#define NS_UBI_UL_PASSWORD 0x1ac /* IEditLine */
#endif
#ifndef NS_UBI_UL_LOGGING
#define NS_UBI_UL_LOGGING 0x1b0  /* CUbiLoginListener while logging in */
#endif
#ifndef NS_UBI_UL_WAIT
#define NS_UBI_UL_WAIT 0x1b4     /* CWaitBox (its +0xf8) while busy */
#endif
#ifndef NS_UBI_UL_INIT
#define NS_UBI_UL_INIT 0x1b8     /* CUbiInitListener while initialising */
#endif
#ifndef NS_UBI_UL_FORM
#define NS_UBI_UL_FORM 0x1bc     /* the mp_register window */
#endif
/* CSimpleEditBox */
#ifndef NS_UBI_UB_CALLBACK
#define NS_UBI_UB_CALLBACK 0x17c
#endif
#ifndef NS_UBI_UB_EDIT
#define NS_UBI_UB_EDIT 0x180
#endif
/* CMPChangeChannelScreen */
#ifndef NS_UBI_UC_LIST
#define NS_UBI_UC_LIST 0x194     /* IListControl */
#endif
#ifndef NS_UBI_UC_JOIN
#define NS_UBI_UC_JOIN 0x1c0     /* ButtonJoin IButton */
#endif
#ifndef NS_UBI_UC_WAIT
#define NS_UBI_UC_WAIT 0x1c4     /* CWaitBox while joining */
#endif
/* CChannelData */
#ifndef NS_UBI_CH_NAME
#define NS_UBI_CH_NAME 0x0c      /* wstr */
#endif
#ifndef NS_UBI_CH_MODE
#define NS_UBI_CH_MODE 0x18      /* wstr, the game's own words */
#endif
#ifndef NS_UBI_CH_PLAYERS
#define NS_UBI_CH_PLAYERS 0x24
#endif
/* a stored CWaitBox pointer is the box's +0xf8; its notify (Cancel) at +0x108 */
#ifndef NS_UBI_WAIT_BOX_FROM
#define NS_UBI_WAIT_BOX_FROM 0xf8
#endif
#ifndef NS_UBI_WAIT_BOX_NOTIFY
#define NS_UBI_WAIT_BOX_NOTIFY 0x108
#endif
#define UL_LOGIN NS_UBI_UL_LOGIN
#define UL_PASSWORD NS_UBI_UL_PASSWORD
#define UL_LOGGING NS_UBI_UL_LOGGING
#define UL_WAIT NS_UBI_UL_WAIT
#define UL_INIT NS_UBI_UL_INIT
#define UL_FORM NS_UBI_UL_FORM
#define UB_CALLBACK NS_UBI_UB_CALLBACK
#define UB_EDIT NS_UBI_UB_EDIT
#define UC_LIST NS_UBI_UC_LIST
#define UC_JOIN NS_UBI_UC_JOIN
#define UC_WAIT NS_UBI_UC_WAIT
#define CH_NAME NS_UBI_CH_NAME
#define CH_MODE NS_UBI_CH_MODE
#define CH_PLAYERS NS_UBI_CH_PLAYERS
#define WAIT_BOX_FROM NS_UBI_WAIT_BOX_FROM
#define WAIT_BOX_NOTIFY NS_UBI_WAIT_BOX_NOTIFY
/* an IEditLine's text (wstr), an IListControl's rows, GetSelected, SelectByData, GetData */
#ifndef NS_UBI_EDIT_TEXT
#define NS_UBI_EDIT_TEXT 0x3c
#endif
#ifndef NS_UBI_LIST_ROWS
#define NS_UBI_LIST_ROWS 0x20
#endif
#ifndef NS_UBI_LIST_SELECTED
#define NS_UBI_LIST_SELECTED 0x24
#endif
#ifndef NS_UBI_LIST_SELECT
#define NS_UBI_LIST_SELECT 0x28
#endif
#ifndef NS_UBI_ITEM_DATA
#define NS_UBI_ITEM_DATA 0x04
#endif
#define EDIT_TEXT NS_UBI_EDIT_TEXT
#define LIST_ROWS NS_UBI_LIST_ROWS
#define LIST_SELECTED NS_UBI_LIST_SELECTED
#define LIST_SELECT NS_UBI_LIST_SELECT
#define ITEM_DATA NS_UBI_ITEM_DATA
#ifndef NS_UBI_VT_EDIT_BOX /* an expansion: the edit box its menu already names */
#define NS_UBI_VT_EDIT_BOX NS_VT_EDIT_BOX
#define NS_UBI_SUB_EDIT_BOX NS_SUB_EDIT_BOX
#endif

enum { F_LOGIN, F_PASSWORD };

/* ----------------------------------------------------------- helpers -- */

static uint32_t cast(Scratch *s, uint32_t p, uint32_t from, uint32_t to) {
    if (!ns_ptr(p))
        return 0;
    const uint32_t args[5] = {p, 0, from, to, 0};
    const uint32_t r = ns_call(s, NS_UBI_DYNAMIC_CAST, 0, 0, args, 5);
    return ns_ptr(r) ? r : 0;
}

/* An IListControl's rows' data, in its list's order (at most cap). */
static int list_data(Scratch *s, uint32_t ilc, uint32_t to, uint32_t *out, int cap) {
    if (!ns_ptr(ilc))
        return 0;
    const uint32_t sentinel = rd32(ilc + LIST_ROWS);
    int n = 0, guard = 512;
    for (uint32_t node = ns_ptr(sentinel) ? rd32(sentinel) : 0; ns_ptr(node) && node != sentinel && n < cap && guard--;
         node = rd32(node)) {
        const uint32_t item = cast(s, rd32(node + 8), NS_UBI_RTTI_IWINDOW, NS_UBI_RTTI_LIST_ITEM);
        if (!item)
            continue;
        const uint32_t data = ns_vcall(s, item, ITEM_DATA, NULL, 0);
        const uint32_t typed = cast(s, data, NS_UBI_RTTI_OBJECT, to);
        if (typed)
            out[n++] = typed;
    }
    return n;
}

static void message(Scratch *s, uint32_t screen, uint32_t fn) {
    const uint32_t zero = 0;
    ns_call(s, fn, screen, 0, &zero, 1);
}

static void edit_text(uint32_t edit, char *out, size_t cap) {
    out[0] = 0;
    if (ns_ptr(edit))
        ns_wstr_utf8(edit + EDIT_TEXT, out, cap);
}

static int lang_ru(Scratch *s) {
    char diff[64];
    ns_difficulty_name(s, 0, diff, sizeof diff);
    return ns_set_lang(diff);
}

/* A wait box a screen keeps (the box's +0xf8), checked by the box's vtable. */
static uint32_t wait_box(uint32_t kept) {
    if (!ns_ptr(kept))
        return 0;
    const uint32_t box = kept - WAIT_BOX_FROM;
    return rd32(box) == NS_UBI_VT_WAIT_BOX ? box : 0;
}

/* The box's Cancel, as its button runs it (007cbcd0): its notify's slot 0.
 * False when the box has none (it shows no Cancel then either). */
static int wait_box_has_cancel(uint32_t box) {
    return box && ns_ptr(rd32(box + WAIT_BOX_NOTIFY));
}
static void wait_box_cancel(Scratch *s, uint32_t box) {
    if (!wait_box_has_cancel(box))
        return;
    const uint32_t notify = rd32(box + WAIT_BOX_NOTIFY);
    ns_call(s, rd32(rd32(notify)), notify, 0, NULL, 0);
}

/* What the login's edit lines take (ETET_UBI): the game's dialog lets no
 * other character in, and the GS login sends the narrow string it makes. */
static const char UBI_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-";

/* `text` with only UBI_CHARS left, in place. */
static void ubi_filter(char *text) {
    char *out = text;
    for (const char *in = text; *in; ++in)
        if (strchr(UBI_CHARS, *in))
            *out++ = *in;
    *out = 0;
}

static void j_text_field(Json *j, int first, int id, const char *kind, const char *label, const char *text) {
    j_printf(j, "%s{\"id\":%d,", first ? "" : ",", id);
    j_kv_str(j, "kind", kind);
    j_lit(j, ",");
    j_kv_str(j, "label", label);
    j_lit(j, ",");
    j_kv_str(j, "text", text);
    j_lit(j, ",");
    j_kv_str(j, "allowed", UBI_CHARS);
    j_lit(j, ",\"enabled\":true}");
}

/* ------------------------------------------------------------ the login -- */

static void login_command(Scratch *s, uint32_t screen, const char *cmd) {
    char verb[16], name[32], text[128];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    if (strcmp(verb, "text") == 0) {
        const int id = ns_cmd_int(cmd, "id", -1);
        ns_cmd_str(cmd, "text", text, sizeof text);
        ubi_filter(text); /* as the game's own edit lines would */
        if (id == F_LOGIN || id == F_PASSWORD)
            ns_lan_set_edit(s, rd32(screen + (id == F_LOGIN ? UL_LOGIN : UL_PASSWORD)), text);
    } else if (strcmp(verb, "press") == 0) {
        ns_cmd_str(cmd, "name", name, sizeof name);
        if (strcmp(name, "ok") == 0) {
            /* The game's OK takes the fields as they are: its button is only
             * enabled with both filled in, so the same check here. */
            char login[128], password[128];
            edit_text(rd32(screen + UL_LOGIN), login, sizeof login);
            edit_text(rd32(screen + UL_PASSWORD), password, sizeof password);
            if (login[0] && password[0] && !rd32(screen + UL_WAIT))
                message(s, screen, NS_UBI_LOGIN_OK);
        } else if (strcmp(name, "cancel") == 0 || strcmp(name, "back") == 0) {
            message(s, screen, NS_UBI_LOGIN_CANCEL);
        } else if (strcmp(name, "busy_cancel") == 0) {
            wait_box_cancel(s, wait_box(rd32(screen + UL_WAIT)));
        }
    }
}

static void login_document(Json *j, Scratch *s, uint32_t screen) {
    lang_ru(s);
    char login[128], password[128];
    edit_text(rd32(screen + UL_LOGIN), login, sizeof login);
    edit_text(rd32(screen + UL_PASSWORD), password, sizeof password);
    const uint32_t form = ns_iwindow(rd32(screen + UL_FORM));
    const uint32_t box = wait_box(rd32(screen + UL_WAIT));
    const int busy = box || !form || !ns_visible(s, form);
    j_lit(j, "{\"screen\":\"form\",");
    j_kv_str(j, "lang", ns_lang_code());
    j_lit(j, ",");
    j_kv_str(j, "title", ns_label(s, "/UI/MPRegiter/account-name.txt", "Сетевой профиль", "Online Profile"));
    j_lit(j, ",\"fields\":[");
    j_text_field(j, 1, F_LOGIN, "text", ns_tr("Имя", "Name"), login);
    j_text_field(j, 0, F_PASSWORD, "secret", ns_label(s, "/UI/MPRegiter/password.txt", "Пароль", "Password"),
                 password);
    j_lit(j, "],");
    j_kv_str(j, "note", ns_tr("Подойдёт любой CD-ключ. Новое имя с паролем создаёт учётную запись на этом сервере. "
                              "Пароль уходит туда только в виде хеша, но всё равно придумайте новый, "
                              "не от Ubisoft и не от других сервисов.",
                              "Any CD key works. A new name and password make an account on this server. "
                              "The password reaches it only as a hash, but still make up a new one, "
                              "not your Ubisoft login or one you use elsewhere."));
    if (busy) {
        const char *what = rd32(screen + UL_INIT)      ? ns_tr("Подключение к серверу...", "Connecting to the server...")
                           : rd32(screen + UL_LOGGING) ? ns_tr("Входим на сервер...", "Logging in...")
                                                       : ns_tr("Проверка сетевого ключа...", "Checking the CD key...");
        j_lit(j, ",");
        j_kv_str(j, "busy", what);
        if (wait_box_has_cancel(box)) {
            j_lit(j, ",");
            j_kv_str(j, "busy_cancel", ns_tr("Отмена", "Cancel"));
        }
    }
    j_lit(j, ",\"buttons\":[");
    j_button(j, 1, "cancel", ns_tr("Отмена", "Cancel"), !box);
    j_button(j, 0, "ok", "OK", !busy && login[0] && password[0]);
    j_lit(j, "]}");
}

const NsScreen ns_ubi_login = {.id = "ubi_login", .vtable = NS_UBI_VT_LOGIN, .sub = NS_UBI_SUB_LOGIN,
                               .command = login_command, .document = login_document};

/* ------------------------------------------------------------- the CD key -- */

static int cdkey_claims(uint32_t box) {
    const uint32_t cb = rd32(box + UB_CALLBACK);
    return ns_ptr(cb) && rd32(cb) == NS_UBI_BOX_CALLBACK;
}

static void cdkey_command(Scratch *s, uint32_t box, const char *cmd) {
    char verb[16], text[64];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    if (strcmp(verb, "accept") == 0) {
        ns_cmd_str(cmd, "text", text, sizeof text);
        if (!text[0])
            return;
        ns_lan_set_edit(s, rd32(box + UB_EDIT), text);
        ns_call(s, NS_UBI_BOX_OK, box, 0, NULL, 0);
    } else if (strcmp(verb, "cancel") == 0) {
        ns_call(s, NS_UBI_BOX_CANCEL, box, 0, NULL, 0);
    }
}

static void cdkey_document(Json *j, Scratch *s, uint32_t box) {
    lang_ru(s);
    char text[64];
    edit_text(rd32(box + UB_EDIT), text, sizeof text);
    j_lit(j, "{\"screen\":\"lan_name\",");
    j_kv_str(j, "lang", ns_lang_code());
    j_lit(j, ",");
    j_kv_str(j, "title", ns_label(s, "/UI/MPRegister/EnterCDKeyHeader.txt", "Пожалуйста, введите сетевой ключ",
                                  "Please enter the CD key"));
    j_lit(j, ",");
    j_kv_str(j, "text", text);
    j_lit(j, ",");
    j_kv_str(j, "hint", ns_tr("Подойдёт любой: сервер сообщества не проверяет ключ. Не вводите настоящий.",
                              "Any key works: a community server doesn't check it. Don't type your real one."));
    j_lit(j, ",");
    j_kv_str(j, "ok", "OK");
    j_lit(j, ",");
    j_kv_str(j, "cancel", ns_tr("Отмена", "Cancel"));
    j_lit(j, ",\"max\":40,\"secret\":false}");
}

const NsScreen ns_ubi_cdkey = {.id = "ubi_cdkey", .vtable = NS_UBI_VT_EDIT_BOX, .sub = NS_UBI_SUB_EDIT_BOX,
                               .claims = cdkey_claims, .command = cdkey_command, .document = cdkey_document};

/* ----------------------------------------------------------- the channels -- */

static void channels_command(Scratch *s, uint32_t screen, const char *cmd) {
    char verb[16], name[32];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    if (strcmp(verb, "select") == 0) {
        static uint32_t rows[64];
        const uint32_t list = rd32(screen + UC_LIST);
        const int n = list_data(s, list, NS_UBI_RTTI_CHANNEL, rows, 64);
        const int i = ns_cmd_int(cmd, "index", -1);
        if (i >= 0 && i < n) {
            const uint32_t data = rows[i];
            ns_vcall(s, list, LIST_SELECT, &data, 1);
        }
    } else if (strcmp(verb, "press") == 0) {
        ns_cmd_str(cmd, "name", name, sizeof name);
        const uint32_t box = wait_box(rd32(screen + UC_WAIT));
        if (strcmp(name, "join") == 0 && !box)
            message(s, screen, NS_UBI_CHANNELS_JOIN);
        else if ((strcmp(name, "cancel") == 0 || strcmp(name, "back") == 0) && !box)
            message(s, screen, NS_UBI_CHANNELS_CANCEL);
        else if (strcmp(name, "busy_cancel") == 0)
            wait_box_cancel(s, box);
    }
}

static void channels_document(Json *j, Scratch *s, uint32_t screen) {
    lang_ru(s);
    static uint32_t rows[64];
    const uint32_t list = rd32(screen + UC_LIST);
    const int n = list_data(s, list, NS_UBI_RTTI_CHANNEL, rows, 64);
    /* GetSelected answers the item; its data is the row's CChannelData (as 0088e7a0 reads it). */
    const uint32_t item = ns_ptr(list) ? ns_vcall(s, list, LIST_SELECTED, NULL, 0) : 0;
    const uint32_t chosen =
        ns_ptr(item) ? cast(s, ns_vcall(s, item, ITEM_DATA, NULL, 0), NS_UBI_RTTI_OBJECT, NS_UBI_RTTI_CHANNEL) : 0;
    const uint32_t box = wait_box(rd32(screen + UC_WAIT));
    j_lit(j, "{\"screen\":\"table\",");
    j_kv_str(j, "lang", ns_lang_code());
    j_lit(j, ",");
    j_kv_str(j, "title", ns_label(s, "/UI/MPChangeChannel/available-channels.txt", "Доступные каналы",
                                  "Available Channels"));
    j_lit(j, ",\"columns\":[");
    j_str(j, ns_label(s, "/UI/MPChangeChannel/channels-table-name.txt", "Имя", "Name"));
    j_lit(j, ",");
    j_str(j, ns_label(s, "/UI/MPChangeChannel/channel-mode-header.txt", "Режим", "Mode"));
    j_lit(j, ",");
    j_str(j, ns_label(s, "/UI/MPChangeChannel/nplayers.txt", "Игроки", "Players"));
    j_lit(j, "],\"rows\":[");
    int selected = -1;
    for (int i = 0; i < n; ++i) {
        char text[128];
        if (chosen && rows[i] == chosen)
            selected = i;
        if (i)
            j_lit(j, ",");
        j_lit(j, "[");
        ns_wstr_utf8(rows[i] + CH_NAME, text, sizeof text);
        j_str(j, text);
        j_lit(j, ",");
        ns_wstr_utf8(rows[i] + CH_MODE, text, sizeof text);
        j_str(j, text);
        snprintf(text, sizeof text, "%d", (int32_t)rd32(rows[i] + CH_PLAYERS));
        j_lit(j, ",");
        j_str(j, text);
        j_lit(j, "]");
    }
    j_printf(j, "],\"selected\":%d", selected);
    if (box) {
        j_lit(j, ",");
        j_kv_str(j, "busy", ns_tr("Подключение к каналу...", "Joining the channel..."));
        if (wait_box_has_cancel(box)) {
            j_lit(j, ",");
            j_kv_str(j, "busy_cancel", ns_tr("Отмена", "Cancel"));
        }
    }
    const uint32_t join = ns_iwindow(rd32(screen + UC_JOIN));
    j_lit(j, ",\"buttons\":[");
    j_button(j, 1, "cancel", ns_tr("Отмена", "Cancel"), !box);
    j_button(j, 0, "join", ns_tr("Войти", "Join"), !box && selected >= 0 && join && ns_enabled(s, join));
    j_lit(j, "]}");
}

const NsScreen ns_ubi_channels = {.id = "ubi_channels", .vtable = NS_UBI_VT_CHANNELS, .sub = NS_UBI_SUB_CHANNELS,
                                  .command = channels_command, .document = channels_document};

#else

/* No addresses for this game: never matched (a zero vtable). */
const NsScreen ns_ubi_login = {.id = "ubi_login"};
const NsScreen ns_ubi_cdkey = {.id = "ubi_cdkey"};
const NsScreen ns_ubi_channels = {.id = "ubi_channels"};

#endif
