/* native/screens_escmenu.c - the game's system menu drawn natively
 * (native/native_screens.c): NUI::CEscMenuScreen ("esc_menu"), the gear
 * button's menu on the adventure map (mode 0) and in battle (mode 1).
 *
 * Its items are its buttons, as the game set them when it opened (shown,
 * enabled, caption); an item runs the button's own reaction. The ones that
 * ask first (restart, main menu, surrender) open the game's question, which
 * the native popup shows (native/screens_question.c); quit opens the game's
 * exit question the same way. The map or the battle stays drawn behind.
 * The base game's addresses: research notes phone-menus/native/re-ingame-menus.md A. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "native_screens.h"
#include "x86.h"

#ifdef RECOMP_OVERRIDE_HEADER
#include RECOMP_OVERRIDE_HEADER
#endif
#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif

/* The base game's; an expansion has none yet. */
#ifndef RECOMP_ADDRESS_HEADER
#define NS_VT_ESC_MENU 0x00fb2f98   /* CEscMenuScreen's IScreen, at +0x1dc */
#define NS_SUB_ESC_MENU 0x1dc
#define NS_ESC_REACTION 0x008304d0  /* thiscall (screen+0x174; sender, name, flags), RET 0xc */
#endif

#if defined(NS_VT_ESC_MENU)

/* Fields and slots an expansion moves: its addresses.h defines the ones that
 * differ (research notes phone-menus/native/expansion-*-addresses.md). */
#ifndef NS_E_REACTIONS
#define NS_E_REACTIONS 0x174
#endif
#ifndef NS_E_MAIN
#define NS_E_MAIN 0x184
#endif
#ifndef NS_E_MODE
#define NS_E_MODE 0x1c0
#endif

enum {
    E_REACTIONS = NS_E_REACTIONS,
    E_MAIN = NS_E_MAIN,    /* the "Main" window: every button is in it */
    E_MODE = NS_E_MODE,    /* 0 adventure, 1 battle, 3 a battle's replay */
};

typedef struct {
    const char *window, *reaction, *ru, *en;
} Item;

/* In the panel's order (top down). Restart and Surrender share a place; the
 * game shows one of them. */
static const Item ITEMS[] = {
    {"SaveBtn", "save", "Сохранить игру", "Save Game"},
    {"LoadBtn", "load", "Загрузить игру", "Load Game"},
    {"OptionsBtn", "options", "Настройки", "Options"},
    {"RestartMissionBtn", "restart_mission", "Играть заново", "Restart"},
    {"Surrender", "surrender", "Сдаться", "Surrender"},
    {"MainMenuBtn", "main_menu", "Главное меню", "Main Menu"},
    {"QuitToWindowsBtn", "exit_to_windows", "Выйти из игры", "Quit Game"},
    {"ReturnToGameBtn", "return_to_game", "Вернуться в игру", "Return to Game"},
};
enum { ITEM_COUNT = sizeof ITEMS / sizeof ITEMS[0] };

/* The menu's windows and captions, read once per menu (guest calls); a menu
 * not read for a while is a new one, even at the same address. */
static uint32_t g_menu;
static int64_t g_menu_ms;
static uint32_t g_windows[ITEM_COUNT];
static char g_captions[ITEM_COUNT][64];

static void read_menu(Scratch *s, uint32_t screen) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000, last = g_menu_ms;
    g_menu_ms = now;
    if (g_menu == screen && now - last < 500)
        return;
    g_menu = screen;
    const uint32_t main = rd32(screen + E_MAIN);
    for (int i = 0; i < ITEM_COUNT; ++i) {
        g_windows[i] = ns_find(s, main, ITEMS[i].window);
        g_captions[i][0] = 0;
        if (g_windows[i])
            ns_caption(s, g_windows[i], g_captions[i], sizeof g_captions[i]);
        if (ns_trace())
            fprintf(stderr, "[screens] esc_menu: %s %08x \"%s\"\n", ITEMS[i].window, g_windows[i],
                    g_captions[i]);
    }
}

static int esc_claims(uint32_t screen) {
    const uint32_t mode = rd32(screen + E_MODE);
    return mode == 0 || mode == 1;
}

static void esc_command(Scratch *s, uint32_t screen, const char *cmd) {
    char verb[16], name[32];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    ns_cmd_str(cmd, "name", name, sizeof name);
    read_menu(s, screen);
    if (strcmp(verb, "back") == 0) {
        ns_react(s, screen + E_REACTIONS, NS_ESC_REACTION, "return_to_game");
        return;
    }
    if (strcmp(verb, "press") != 0)
        return;
    for (int i = 0; i < ITEM_COUNT; ++i)
        if (strcmp(name, ITEMS[i].reaction) == 0) {
            /* What the game shows and allows, as its own button would. */
            if (g_windows[i] && ns_visible(s, g_windows[i]) && ns_enabled(s, g_windows[i]))
                ns_react(s, screen + E_REACTIONS, NS_ESC_REACTION, ITEMS[i].reaction);
            return;
        }
}

static void esc_document(Json *j, Scratch *s, uint32_t screen) {
    read_menu(s, screen);
    const int battle = rd32(screen + E_MODE) == 1;
    j_lit(j, "{\"screen\":\"esc_menu\",");
    j_kv_str(j, "lang", ns_lang_code());
    j_lit(j, ",");
    j_kv_str(j, "title", battle ? ns_tr("Бой", "Battle") : ns_tr("Меню", "Menu"));
    j_lit(j, ",\"buttons\":[");
    int first = 1;
    for (int i = 0; i < ITEM_COUNT; ++i) {
        const uint32_t w = g_windows[i];
        if (!w || !ns_visible(s, w))
            continue;
        j_button(j, first, ITEMS[i].reaction, g_captions[i][0] ? g_captions[i] : ns_tr(ITEMS[i].ru, ITEMS[i].en),
                 ns_enabled(s, w));
        first = 0;
    }
    j_lit(j, "]}");
}

const NsScreen ns_esc_menu = {.id = "esc_menu", .vtable = NS_VT_ESC_MENU, .sub = NS_SUB_ESC_MENU,
                              .claims = esc_claims, .command = esc_command, .document = esc_document};

#else

/* No addresses for this game: never matched (a zero vtable). */
const NsScreen ns_esc_menu = {.id = "esc_menu"};

#endif
