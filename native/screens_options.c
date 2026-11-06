/* native/screens_options.c - Options drawn natively
 * (native/native_screens.c): NUI::CGameOptionsScreen ("options").
 *
 * The game's categories are tabs (its menu of categories is skipped: the
 * first one opens at once); a category's rows are its options as the game
 * built them - headers, check boxes, combo boxes and sliders - with the
 * values in their widgets. A change goes into the widget and the game's
 * "option_changed" runs (links, enables, live previews such as the volume);
 * Apply stores and applies the category as the game's Accept does, then the
 * tab stays open. Another tab drops the unapplied changes first, as the
 * game's own back from a page does. Back closes without applying.
 * Opened from the battle's menu (mode 2) the game's own back from a page
 * closes the screen, so a tab there puts the previews back by itself; and
 * Apply and Defaults close it, as the game's do.
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
#define NS_VT_OPTIONS 0x00fb75d8    /* CGameOptionsScreen's IScreen, at +0x1f8 */
#define NS_SUB_OPTIONS 0x1f8
#define NS_OPT_SHOW 0x00895540      /* thiscall (screen, category), RET 4 */
#define NS_OPT_CHANGED 0x008946d0   /* fastcall ECX screen: "option_changed" */
#define NS_OPT_ACCEPT 0x008951c0    /* fastcall ECX screen: "accept" */
#define NS_OPT_PAGE_BACK 0x00895490 /* fastcall ECX screen: "options_back" (previews reverted) */
#define NS_OPT_CLOSE 0x008957b0     /* fastcall ECX screen: "back" */
#define NS_OPT_DEFAULT 0x008958b0   /* thiscall (screen+0x17c, the box's id), RET 4 */
#define NS_OPT_DEFAULT_OK 0x172a6c00
#define NS_OPT_REVERT 0x00894340    /* (category, 4), RET 8: its rows' previews put back */
#define NS_VT_OPT_HEADER 0x00fb764c /* an option row's kind, by its vtable */
#define NS_VT_OPT_CHECK 0x00fb767c
#define NS_VT_OPT_COMBO 0x00fb76ac
#define NS_VT_OPT_SLIDER 0x00fb76dc
#endif

#if defined(NS_VT_OPTIONS)

/* Fields and slots an expansion moves: its addresses.h defines the ones that
 * differ (research notes phone-menus/native/expansion-*-addresses.md). */
#ifndef NS_O_BOXES
#define NS_O_BOXES 0x17c
#endif
#ifndef NS_O_SHOWN
#define NS_O_SHOWN 0x1a0
#endif
#ifndef NS_O_CATEGORIES
#define NS_O_CATEGORIES 0x1a4
#endif
#ifndef NS_O_MODE
#define NS_O_MODE 0x1bc
#endif

enum {
    O_BOXES = NS_O_BOXES,
    O_SHOWN = NS_O_SHOWN,         /* the category showing, -1 the menu of them */
    O_CATEGORIES = NS_O_CATEGORIES,    /* vector<SCategory*> */
    O_MODE = NS_O_MODE,          /* 0 main menu, 1 game menu, 2 straight to a page, 3 the map's button */
    CAT_LABEL = 0x1c,        /* wstr: its button's label */
    CAT_ROWS = 0x4c,         /* vector<SOption*> in page order */
    OPT_PAGE_ON = 0x28,
    OPT_CODE_OFF = 0x29,
    OPT_LINK_OFF = 0x30,
};

static int categories(uint32_t screen, uint32_t *b) {
    *b = rd32(screen + O_CATEGORIES);
    const uint32_t e = rd32(screen + O_CATEGORIES + 4);
    return ns_ptr(*b) && e >= *b && e - *b <= 4u * 16u ? (int)((e - *b) / 4u) : 0;
}

static int rows_of(uint32_t cat, uint32_t *b) {
    *b = ns_ptr(cat) ? rd32(cat + CAT_ROWS) : 0;
    const uint32_t e = ns_ptr(cat) ? rd32(cat + CAT_ROWS + 4) : 0;
    return ns_ptr(*b) && e >= *b && e - *b <= 4u * 128u ? (int)((e - *b) / 4u) : 0;
}

static uint32_t shown_category(uint32_t screen) {
    uint32_t b;
    const int n = categories(screen, &b);
    const int i = (int)rd32(screen + O_SHOWN);
    return i >= 0 && i < n ? rd32(b + 4u * (uint32_t)i) : 0;
}

/* The game's menu of categories is not drawn: the first category opens. */
static void options_step(Scratch *s, uint32_t screen) {
    uint32_t b;
    if ((int)rd32(screen + O_SHOWN) < 0 && categories(screen, &b) > 0) {
        const uint32_t zero = 0;
        ns_call(s, NS_OPT_SHOW, screen, 0, &zero, 1);
    }
}

/* A row's values for a combo box (items: wstr name at item+0xc) or a slider
 * (0x10 each: wstr name at +0). */
static int items(uint32_t opt, uint32_t vt, uint32_t *b, uint32_t *stride, uint32_t *name_at) {
    const uint32_t at = vt == NS_VT_OPT_COMBO ? 0x80u : 0xa4u;
    *b = rd32(opt + at);
    const uint32_t e = rd32(opt + at + 4);
    *stride = vt == NS_VT_OPT_COMBO ? 4u : 0x10u;
    *name_at = vt == NS_VT_OPT_COMBO ? 0xcu : 0u;
    return ns_ptr(*b) && e >= *b && e - *b <= *stride * 128u ? (int)((e - *b) / *stride) : 0;
}

static void options_command(Scratch *s, uint32_t screen, const char *cmd) {
    char verb[16];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    const uint32_t cat = shown_category(screen);
    if (strcmp(verb, "tab") == 0) {
        uint32_t b;
        const int i = ns_cmd_int(cmd, "index", -1);
        if (i < 0 || i >= categories(screen, &b) || i == (int)rd32(screen + O_SHOWN))
            return;
        if (rd32(screen + O_MODE) == 2) {
            /* Its back from a page would close the screen. */
            if (cat) {
                const uint32_t args[2] = {cat, 4};
                ns_call(s, NS_OPT_REVERT, 0, 0, args, 2);
            }
            ns_call(s, NS_OPT_CHANGED, screen, 0, NULL, 0);
        } else {
            ns_call(s, NS_OPT_PAGE_BACK, screen, 0, NULL, 0); /* the previews put back */
        }
        const uint32_t k = (uint32_t)i;
        ns_call(s, NS_OPT_SHOW, screen, 0, &k, 1);
    } else if (strcmp(verb, "set") == 0 && cat) {
        uint32_t b;
        const int row = ns_cmd_int(cmd, "row", -1), value = ns_cmd_int(cmd, "value", -1);
        if (row < 0 || row >= rows_of(cat, &b) || value < 0)
            return;
        const uint32_t opt = rd32(b + 4u * (uint32_t)row), vt = rd32(opt), v = (uint32_t)value;
        if (vt == NS_VT_OPT_CHECK)
            ns_vcall(s, rd32(opt + 0x80), 0x0c, &v, 1); /* IButton SetState */
        else if (vt == NS_VT_OPT_COMBO)
            ns_vcall(s, rd32(opt + 0x7c), 0x18, &v, 1); /* IComboBox SetSelected */
        else if (vt == NS_VT_OPT_SLIDER)
            ns_vcall(s, rd32(opt + 0x80), NS_SLIDER_SET_SEGMENT, &v, 1);
        else
            return;
        ns_call(s, NS_OPT_CHANGED, screen, 0, NULL, 0);
    } else if (strcmp(verb, "accept") == 0 && cat) {
        const uint32_t k = rd32(screen + O_SHOWN);
        ns_call(s, NS_OPT_ACCEPT, screen, 0, NULL, 0);
        if (rd32(screen + O_MODE) != 2) /* it closes in mode 2 */
            ns_call(s, NS_OPT_SHOW, screen, 0, &k, 1); /* Accept goes back to the menu: stay */
    } else if (strcmp(verb, "default") == 0 && cat) {
        /* The view asked first; the game's own question is not shown. */
        const uint32_t k = rd32(screen + O_SHOWN), ok = NS_OPT_DEFAULT_OK;
        ns_call(s, NS_OPT_DEFAULT, screen + O_BOXES, 0, &ok, 1);
        if (rd32(screen + O_MODE) != 2)
            ns_call(s, NS_OPT_SHOW, screen, 0, &k, 1);
    } else if (strcmp(verb, "back") == 0) {
        ns_call(s, NS_OPT_CLOSE, screen, 0, NULL, 0);
    }
}

static void options_document(Json *j, Scratch *s, uint32_t screen) {
    char diff[64], text[256];
    ns_difficulty_name(s, 0, diff, sizeof diff);
    ns_set_lang(diff);
    uint32_t cb;
    const int ncat = categories(screen, &cb);
    j_lit(j, "{\"screen\":\"options\",");
    j_kv_str(j, "lang", ns_lang_code());
    j_lit(j, ",");
    j_kv_str(j, "title", ns_label(s, "/UI/MainMenu2/Options.txt", "Настройки", "Options"));
    j_lit(j, ",\"tabs\":[");
    for (int i = 0; i < ncat; ++i) {
        ns_wstr_utf8(rd32(cb + 4u * (uint32_t)i) + CAT_LABEL, text, sizeof text);
        if (i)
            j_lit(j, ",");
        j_str(j, text);
    }
    j_printf(j, "],\"tab\":%d,\"rows\":[", (int)rd32(screen + O_SHOWN));
    const uint32_t cat = shown_category(screen);
    uint32_t rb;
    const int nrows = rows_of(cat, &rb);
    int first = 1;
    for (int r = 0; r < nrows; ++r) {
        const uint32_t opt = rd32(rb + 4u * (uint32_t)r), vt = ns_ptr(opt) ? rd32(opt) : 0;
        const int enabled = rd8(opt + OPT_PAGE_ON) && !rd8(opt + OPT_CODE_OFF) && !rd8(opt + OPT_LINK_OFF);
        const char *kind = vt == NS_VT_OPT_HEADER  ? "header"
                           : vt == NS_VT_OPT_CHECK  ? "toggle"
                           : vt == NS_VT_OPT_COMBO  ? "choice"
                           : vt == NS_VT_OPT_SLIDER ? "choice"
                                                    : NULL;
        if (!kind)
            continue;
        const uint32_t label = vt == NS_VT_OPT_HEADER  ? 0x80u
                               : vt == NS_VT_OPT_CHECK ? 0x94u
                               : vt == NS_VT_OPT_COMBO ? 0xb0u
                                                       : 0x98u;
        ns_wstr_utf8(opt + label, text, sizeof text);
        j_printf(j, "%s{\"row\":%d,", first ? "" : ",", r);
        first = 0;
        j_kv_str(j, "kind", kind);
        j_lit(j, ",");
        j_kv_str(j, "label", text);
        j_printf(j, ",\"enabled\":%s", enabled ? "true" : "false");
        if (vt == NS_VT_OPT_CHECK) {
            const uint32_t btn = rd32(opt + 0x80);
            j_printf(j, ",\"on\":%s", ns_ptr(btn) && ns_vcall(s, btn, 0x14, NULL, 0) ? "true" : "false");
        } else if (vt == NS_VT_OPT_COMBO || vt == NS_VT_OPT_SLIDER) {
            uint32_t ib, stride, name_at;
            const int n = items(opt, vt, &ib, &stride, &name_at);
            const uint32_t widget = rd32(opt + (vt == NS_VT_OPT_COMBO ? 0x7c : 0x80));
            const int at = !ns_ptr(widget)             ? -1
                           : vt == NS_VT_OPT_COMBO      ? (int)rd32(widget + 0x58)
                                                        : (int)ns_vcall(s, widget, NS_SLIDER_GET_SEGMENT, NULL, 0);
            j_printf(j, ",\"index\":%d,\"values\":[", at);
            for (int i = 0; i < n; ++i) {
                const uint32_t item = vt == NS_VT_OPT_COMBO ? rd32(ib + 4u * (uint32_t)i)
                                                            : ib + stride * (uint32_t)i;
                ns_wstr_utf8(item + name_at, text, sizeof text);
                if (i)
                    j_lit(j, ",");
                j_str(j, text);
            }
            j_lit(j, "]");
        }
        j_lit(j, "}");
    }
    j_lit(j, "],\"buttons\":[");
    const int closes = rd32(screen + O_MODE) == 2;
    j_button(j, 1, "accept", closes ? ns_tr("Применить и выйти", "Apply and Close")
                                    : ns_label(s, "/UI/GameOptions/Accept.txt", "Применить", "Apply"),
             cat != 0);
    j_button(j, 0, "default", ns_label(s, "/UI/GameOptions/Default.txt", "По умолчанию", "Defaults"), cat != 0);
    j_lit(j, "]}");
}

const NsScreen ns_options = {.id = "options", .vtable = NS_VT_OPTIONS, .sub = NS_SUB_OPTIONS,
                             .command = options_command,
                             .step = options_step, .document = options_document};

#else

/* No addresses for this game: never matched (a zero vtable). */
const NsScreen ns_options = {.id = "options"};

#endif
