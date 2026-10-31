/* native/screens_menu.c - the main menu drawn natively
 * (native/native_screens.c): NUI::CMainMenuScreen ("main_menu"), one screen
 * whose panels are its submenus - the main buttons, Single player with the
 * profile and the difficulty, Multiplayer, Hot seat's modes, the campaigns
 * and a campaign's missions - and the first profile's name
 * (CSimpleEditBox, "profile_name").
 *
 * The panel showing is the screen's current submenu index. The screen is
 * drawn natively on the panels above only; on any other (the replays) the game
 * draws its own. Its buttons are the submenus' reactions, run through the
 * screen's handler, which passes them to the submenu showing; the main panel's
 * are game messages whose handlers only switch the submenu, called directly.
 * Captions come from the game's data, in its language.
 * The base game's addresses: research notes phone-menus/native/re-main-menu.md and
 * re-campaigns-hof.md. */
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
#define NS_VT_MENU 0x00fb96e8       /* NUI::CMainMenuScreen's IScreen, at +0x214 */
#define NS_SUB_MENU 0x214
#define NS_MENU_REACTION 0x008ad270 /* the screen's reactions, ECX screen+0x174, RET 0xc */
#define NS_MENU_SWITCH 0x008ad570   /* the submenu switch, ECX screen+0x17c, push index, RET 4 */
#define NS_MENU_CREDITS 0x0095e1f0  /* "credits" */
#define NS_MENU_EXIT 0x00832060     /* "main_menu_exit": the question screen */
#define NS_PROFILE_VALID 0x00956090 /* AL: a valid profile is current */
#define NS_PROFILE_NAME 0x009560e0  /* push out wstr*, RET 4: the current profile's name */
#define NS_PROFILE_SHOWN 0x00956070 /* fastcall ECX list: AL the profile list shows */
#define NS_PROFILE_SHOW 0x00955fb0  /* thiscall (list, bool), RET 4 */
#define NS_PROFILE_DELETE 0x00955180 /* fastcall ECX list: delete the selected one */
#define NS_SETUP_REACTION 0x009612d0 /* the setup panel's reactions ("difficulty_changed") */
#define NS_DIFFICULTY_NOW 0x0116cb30 /* int: the difficulty chosen */
#define NS_CAMPAIGN_OF 0x0095a820   /* fastcall ECX campaign submenu: the campaign chosen */
#define NS_VT_EDIT_BOX 0x00fc83e8   /* CSimpleEditBox's IScreen, at +0x1b8 */
#define NS_SUB_EDIT_BOX 0x1b8
#define NS_VT_FIRST_PROFILE 0x00fc3888 /* its callback when it asks the first profile's name */
#define NS_EDIT_BOX_OK 0x009a7b80   /* fastcall ECX box: accept */
#define NS_TEXT_OF_REF 0x00a09de0   /* fastcall ECX a text ref: const wstr* */
#endif

#if defined(NS_VT_MENU)

/* Fields and slots an expansion moves: its addresses.h defines the ones that
 * differ (research notes phone-menus/native/expansion-*-addresses.md). */
#ifndef NS_M_REACTIONS
#define NS_M_REACTIONS 0x174
#endif
#ifndef NS_M_BASE
#define NS_M_BASE 0x17c
#endif
#ifndef NS_M_MAIN
#define NS_M_MAIN 0x1bc
#endif
#ifndef NS_M_SINGLE
#define NS_M_SINGLE 0x1c0
#endif
#ifndef NS_M_MULTI
#define NS_M_MULTI 0x1c4
#endif
#ifndef NS_M_INDEX
#define NS_M_INDEX 0x1c8
#endif
#ifndef NS_M_MISSIONS
#define NS_M_MISSIONS 0x1cc
#endif
#ifndef NS_M_HOT_SEAT
#define NS_M_HOT_SEAT 0x1dc
#endif
#ifndef NS_M_USER_CAMPAIGNS
#define NS_M_USER_CAMPAIGNS 0x1f0
#endif
#ifndef NS_M_CAMPAIGNS
#define NS_M_CAMPAIGNS 0x1f8
#endif
#ifndef NS_BOX_CALLBACK
#define NS_BOX_CALLBACK 0x17c
#endif
#ifndef NS_BOX_EDIT
#define NS_BOX_EDIT 0x180
#endif

enum {
    M_REACTIONS = NS_M_REACTIONS,
    M_BASE = NS_M_BASE,          /* IMainMenuBase: the switch */
    M_MAIN = NS_M_MAIN,          /* the submenus: */
    M_SINGLE = NS_M_SINGLE,
    M_MULTI = NS_M_MULTI,
    M_INDEX = NS_M_INDEX,         /* the one showing */
    M_MISSIONS = NS_M_MISSIONS,
    M_HOT_SEAT = NS_M_HOT_SEAT,
    M_USER_CAMPAIGNS = NS_M_USER_CAMPAIGNS, /* 0: the game has none */ /* the user campaigns' submenu */
    M_CAMPAIGNS = NS_M_CAMPAIGNS,     /* the campaign submenu last shown (official or user) */
    SUB_PANEL = 0x0c,        /* a submenu: its panel (IWindow*) */
    SP_PANEL = 0x14,
    SP_LIST = 0x1c,          /* CSelectProfile */
    SP_SETUP = 0x34,         /* CMenuSingleSetupPanel */
    C_PANEL = 0x10,
    C_SET = 0x08,            /* NWorld::CCampaignsSet */
    C_SETUP = 0x14,
    MS_SETUP = 0x10,
    SETUP_COMBO = 0x1c,      /* IComboBox: +0x18 SetSelected */
    PL_CREATE = 0x1c,        /* the profile list: its buttons, IButton* */
    PL_EDIT = 0x28,          /* IEditLine*: the new profile's name */
    PL_ROWS = 0x34,          /* vector of 16 bytes: +4 the name (wstr) */
    PL_SELECTED = 0x40,
    EDIT_TEXT = 0x3c,        /* an IEditLine's text (wstr) */
    BOX_CALLBACK = NS_BOX_CALLBACK,
    BOX_EDIT = NS_BOX_EDIT,
};

/* The submenus' indices (the switch's argument). */
enum {
    P_MAIN = 0, P_SINGLE = 1, P_MULTI = 2, P_CAMPAIGNS = 4, P_MISSIONS = 5, P_HOT_SEAT = 9,
    P_USER_CAMPAIGNS = 0xb,
};

/* Slots and names an expansion changes (its addresses.h defines them first). */
#ifndef NS_SET_COUNT
#define NS_SET_COUNT 0x04          /* NWorld::CCampaignsSet: count */
#endif
#ifndef NS_SET_GET
#define NS_SET_GET 0x0c            /*   (i): the campaign */
#endif
#ifndef NS_SET_UNLOCKED
#define NS_SET_UNLOCKED 0x10       /*   (i): whether it may be played */
#endif
#ifndef NS_CAMPAIGN_COUNT
#define NS_CAMPAIGN_COUNT 0x10     /* a campaign: its missions */
#endif
#ifndef NS_CAMPAIGN_MISSION
#define NS_CAMPAIGN_MISSION 0x14   /*   (i): the mission's record */
#endif
#ifndef NS_CAMPAIGN_DONE
#define NS_CAMPAIGN_DONE 0x18      /*   (i) */
#endif
#ifndef NS_CAMPAIGN_AVAILABLE
#define NS_CAMPAIGN_AVAILABLE 0x1c /*   (i) */
#endif
/* A mission's loading picture by its campaign's key ("C1") and its index
 * from 0, into out; an expansion names its own (HoF PWLPic, ToE one per campaign). */
#ifndef NS_MISSION_PICTURE
#define NS_MISSION_PICTURE(out, cap, key, i) snprintf(out, cap, "/Maps/Scenario/%sM%d/PWL.(Texture).dds", key, (i) + 1)
#endif
#ifndef NS_GAME_VERSION
#define NS_GAME_VERSION "1.60"
#endif

/* The panel showing: the switch's last index, its "back to the campaigns"
 * (0xc) read as the campaign list it went back to. */
static int panel(uint32_t screen) {
    const int index = (int)rd32(screen + M_INDEX);
    if (index != 0xc)
        return index;
    return M_USER_CAMPAIGNS && rd32(screen + M_CAMPAIGNS) == rd32(screen + M_USER_CAMPAIGNS) ? P_USER_CAMPAIGNS
                                                                                          : P_CAMPAIGNS;
}

static int menu_claims(uint32_t screen) {
    switch (panel(screen)) {
    case P_MAIN: case P_SINGLE: case P_MULTI: case P_CAMPAIGNS: case P_MISSIONS:
    case P_HOT_SEAT: case P_USER_CAMPAIGNS:
        return 1;
    default:
        return 0;
    }
}

static void react(Scratch *s, uint32_t screen, const char *name) {
    ns_react(s, screen + M_REACTIONS, NS_MENU_REACTION, name);
}

static void switch_to(Scratch *s, uint32_t screen, uint32_t index) {
    ns_call(s, NS_MENU_SWITCH, screen + M_BASE, 0, &index, 1);
}

/* ------------------------------------------------------------ buttons -- */

/* A panel's buttons: the window's name in the data, what pressing it sends
 * (a reaction, or for the main panel the game message's own work), and the
 * caption to show when the data has none. */
typedef struct {
    const char *window, *reaction, *ru, *en;
} Button;

static const Button MAIN_BUTTONS[] = {
    {"SingleGameBtn", "single", "Одиночная игра", "Single Player"},
    {"MultiPlayerBtn", "multi", "Сетевая игра", "Multiplayer"},
    {"OptionsBtn", "options", "Настройки", "Options"},
    {"CreditsBtn", "credits", "Авторы", "Credits"},
    {"QuitBtn", "main_menu_exit", "Выход", "Exit"},
};
static const Button SINGLE_BUTTONS[] = {
    {"CampaignBtn", "campaign", "Кампания", "Campaign"},
    {"CustomGameBtn", "custom_game", "Сценарии", "Scenarios"},
    {"LoadGameBtn", "load_game", "Загрузить игру", "Load Game"},
    {"UserCampaignBtn", "user_campaign", "Модификация", "Custom Campaign"},
    {"HallOfFameBtn", "hall_of_fame", "Зал славы", "Hall of Fame"},
};
static const Button MULTI_BUTTONS[] = {
    {"HotSeatBtn", "hot_seat", "Один компьютер", "Hot Seat"},
    {"LANBtn", "lan", "Локальная сеть", "LAN"},
    {"UbiComBtn", "ubi_com", "ubi.com", "ubi.com"},
};
static const Button HOT_SEAT_BUTTONS[] = {
    {"standart_game", "standart_hot_seat", "Обычный режим", "Standard"},
    {"duel_game", "duel_mode_hotseat", "Дуэль", "Duel"},
    {"triple_duel_game", "triple_duel_mode_hotseat", "Дуэль 3х3", "Duel 3x3"},
};

/* The captions read once per window (a guest call each). */
static struct {
    uint32_t window;
    char text[64];
} g_captions[32];
static int g_caption_count;

static const char *caption(Scratch *s, uint32_t window, const Button *b) {
    for (int i = 0; i < g_caption_count; ++i)
        if (g_captions[i].window == window)
            return g_captions[i].text[0] ? g_captions[i].text : ns_tr(b->ru, b->en);
    if (g_caption_count == 32)
        g_caption_count = 0;
    g_captions[g_caption_count].window = window;
    ns_caption(s, window, g_captions[g_caption_count].text, sizeof g_captions[0].text);
    const char *t = g_captions[g_caption_count++].text;
    return t[0] ? t : ns_tr(b->ru, b->en);
}

static uint32_t panel_of(uint32_t screen, int index) {
    switch (index) {
    case P_MAIN: return rd32(rd32(screen + M_MAIN) + SUB_PANEL);
    case P_SINGLE: return rd32(rd32(screen + M_SINGLE) + SP_PANEL);
    case P_MULTI: return rd32(rd32(screen + M_MULTI) + SUB_PANEL);
    case P_HOT_SEAT: return rd32(rd32(screen + M_HOT_SEAT) + SUB_PANEL);
    default: return 0;
    }
}

static const Button *buttons_of(int index, int *n) {
    switch (index) {
    case P_MAIN: *n = sizeof MAIN_BUTTONS / sizeof MAIN_BUTTONS[0]; return MAIN_BUTTONS;
    case P_SINGLE: *n = sizeof SINGLE_BUTTONS / sizeof SINGLE_BUTTONS[0]; return SINGLE_BUTTONS;
    case P_MULTI: *n = sizeof MULTI_BUTTONS / sizeof MULTI_BUTTONS[0]; return MULTI_BUTTONS;
    case P_HOT_SEAT: *n = sizeof HOT_SEAT_BUTTONS / sizeof HOT_SEAT_BUTTONS[0]; return HOT_SEAT_BUTTONS;
    default: *n = 0; return NULL;
    }
}

/* ------------------------------------------------------- the profiles -- */

static uint32_t profile_list(uint32_t screen) {
    return rd32(rd32(screen + M_SINGLE) + SP_LIST);
}

static void profile_name(Scratch *s, char *out, size_t cap) {
    const uint32_t w = ns_alloc(s, 12);
    wr32(w, 0);
    wr32(w + 4, 0);
    ns_call(s, NS_PROFILE_NAME, 0, 0, &w, 1);
    ns_wstr_utf8(w, out, cap);
    ns_free(s, rd32(w));
}

static int profile_count(uint32_t list) {
    const uint32_t b = rd32(list + PL_ROWS), e = rd32(list + PL_ROWS + 4);
    return ns_ptr(b) && e >= b && e - b <= 16u * 64u ? (int)((e - b) / 16u) : 0;
}

/* The setup panel (profile and difficulty) of the panel showing. */
static uint32_t setup_of(uint32_t screen, int index) {
    switch (index) {
    case P_SINGLE: return rd32(rd32(screen + M_SINGLE) + SP_SETUP);
    case P_CAMPAIGNS: case P_USER_CAMPAIGNS: return rd32(rd32(screen + M_CAMPAIGNS) + C_SETUP);
    case P_MISSIONS: return rd32(rd32(screen + M_MISSIONS) + MS_SETUP);
    default: return 0;
    }
}

/* ------------------------------------------------------ the campaigns -- */

static uint32_t campaign_set(uint32_t screen) {
    return rd32(rd32(screen + M_CAMPAIGNS) + C_SET);
}

/* ------------------------------------------------------------ commands -- */

static void menu_command(Scratch *s, uint32_t screen, const char *cmd) {
    char verb[24], name[40];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    const int index = panel(screen);
    const int i = ns_cmd_int(cmd, "index", -1);
    if (strcmp(verb, "press") == 0) {
        ns_cmd_str(cmd, "name", name, sizeof name);
        if (strcmp(name, "single") == 0)
            switch_to(s, screen, P_SINGLE);
        else if (strcmp(name, "multi") == 0)
            switch_to(s, screen, P_MULTI);
        else if (strcmp(name, "options") == 0) {
#ifdef NS_MENU_OPTIONS
            /* The game's own handler: HoF's switch has no Options panel. */
            const uint32_t unused = 0;
            ns_call(s, NS_MENU_OPTIONS, screen, 0, &unused, 1);
#else
            switch_to(s, screen, 3);
#endif
        }
        else if (strcmp(name, "credits") == 0)
            ns_call(s, NS_MENU_CREDITS, 0, 0, NULL, 0);
        else if (strcmp(name, "main_menu_exit") == 0)
            ns_call(s, NS_MENU_EXIT, 0, 0, NULL, 0);
        else if (strcmp(name, "back") == 0)
            react(s, screen, index == P_CAMPAIGNS || index == P_USER_CAMPAIGNS ? "select_campaign_back"
                             : index == P_MISSIONS                            ? "select_mission_back"
                                                                               : "back");
        else if (name[0])
            react(s, screen, name); /* the panel's reaction; the view sent only enabled ones */
    } else if (strcmp(verb, "difficulty") == 0 && i >= 0 && i < 4) {
        const uint32_t setup = setup_of(screen, index);
        const uint32_t combo = ns_ptr(setup) ? rd32(setup + SETUP_COMBO) : 0;
        if (!ns_ptr(combo))
            return;
        const uint32_t sel = (uint32_t)i;
        ns_vcall(s, combo, 0x18, &sel, 1); /* SetSelected: it does not tell the panel */
        if (index == P_SINGLE) {
            const uint32_t args[3] = {ns_str(s, ""), ns_str(s, "difficulty_changed"), 0};
            ns_call(s, NS_SETUP_REACTION, setup, 0, args, 3);
        } else {
            react(s, screen, "difficulty_changed");
        }
    } else if (strcmp(verb, "profiles") == 0) {
        const uint32_t list = profile_list(screen), open = (uint32_t)ns_cmd_int(cmd, "open", 0);
        if (ns_ptr(list))
            ns_call(s, NS_PROFILE_SHOW, list, 0, &open, 1);
    } else if (strcmp(verb, "profile_select") == 0) {
        const uint32_t list = profile_list(screen);
        if (ns_ptr(list) && i >= 0 && i < profile_count(list)) {
            wr32(list + PL_SELECTED, (uint32_t)i);
            react(s, screen, "profile_select");
        }
    } else if (strcmp(verb, "profile_delete") == 0) {
        /* The view asked first; the game's own question is not shown. */
        const uint32_t list = profile_list(screen);
        if (ns_ptr(list) && i >= 0 && i < profile_count(list) && profile_count(list) > 1) {
            wr32(list + PL_SELECTED, (uint32_t)i);
            ns_call(s, NS_PROFILE_DELETE, list, 0, NULL, 0);
        }
    } else if (strcmp(verb, "profile_create") == 0) {
        /* As typing the name in the list's field and pressing Create, then
         * choosing it: the game's checks decide (its Create stays off for a
         * name it refuses). */
        const uint32_t list = profile_list(screen);
        const uint32_t edit = ns_ptr(list) ? rd32(list + PL_EDIT) : 0;
        char text[64];
        ns_cmd_str(cmd, "text", text, sizeof text);
        if (!ns_ptr(edit) || !text[0])
            return;
        const uint32_t wide = ns_wide(s, text);
        ns_vcall(s, edit, 0x08, &wide, 1); /* SetText */
        react(s, screen, "profile_name_changed");
        if (!ns_enabled(s, ns_iwindow(rd32(list + PL_CREATE))))
            return;
        react(s, screen, "profile_create");
        react(s, screen, "profile_select_item");
        react(s, screen, "profile_select");
    } else if (strcmp(verb, "campaign") == 0 && i >= 0 && i < 16) {
        char sender[24];
        snprintf(sender, sizeof sender, "CampaignBtn%d", i);
        ns_react_as(s, screen + M_REACTIONS, NS_MENU_REACTION, sender, "campaign_selected");
    } else if (strcmp(verb, "mission") == 0 && i >= 0 && i < 16) {
        char sender[24];
        snprintf(sender, sizeof sender, "MissionBtn%d", i);
        ns_leave();
        ns_react_as(s, screen + M_REACTIONS, NS_MENU_REACTION, sender, "mission_selected");
    }
}

/* ------------------------------------------------------------ document -- */

/* The panel's buttons; the number of the game's own shown. */
static int j_buttons(Json *j, Scratch *s, uint32_t screen, int index) {
    int n;
    const Button *bs = buttons_of(index, &n);
    const uint32_t panel = panel_of(screen, index);
    const int profile = index == P_SINGLE && (ns_call(s, NS_PROFILE_VALID, 0, 0, NULL, 0) & 0xff);
    j_lit(j, ",\"buttons\":[");
    int first = 1, shown = 0;
    for (int k = 0; k < n; ++k) {
        const uint32_t w = ns_find(s, panel, bs[k].window);
        if (!w || !ns_visible(s, w))
            continue; /* hidden in the data (Multiplayer's Load) */
        int enabled = ns_enabled(s, w);
        /* Single player's buttons want a profile; the game's handler does
         * not check, its buttons' state does. */
        if (index == P_SINGLE && strcmp(bs[k].reaction, "user_campaign") != 0)
            enabled = enabled && profile;
        j_button(j, first, bs[k].reaction, caption(s, w, &bs[k]), enabled);
        first = 0;
        ++shown;
    }
    if (index != P_MAIN)
        j_button(j, first, "back", ns_label(s, "/UI/MainMenu2/SinglePlayer/Back.txt", "Назад", "Back"), 1);
    j_lit(j, "]");
    return shown;
}

static void j_setup(Json *j, Scratch *s, uint32_t screen, int index) {
    char name[64], diff[4][64];
    profile_name(s, name, sizeof name);
    for (int i = 0; i < 4; ++i)
        ns_difficulty_name(s, i, diff[i], sizeof diff[i]);
    int now = (int)rd32(NS_DIFFICULTY_NOW);
    now = now < 0 || now > 3 ? 0 : now;
    j_lit(j, ",\"profile\":{");
    j_kv_str(j, "name", name);
    j_lit(j, ",");
    j_kv_str(j, "change", ns_tr("Сменить", "Change"));
    j_lit(j, "},\"difficulty\":{");
    j_kv_str(j, "label", ns_tr("Сложность", "Difficulty"));
    j_printf(j, ",\"index\":%d,\"values\":[", now);
    for (int i = 0; i < 4; ++i) {
        if (i)
            j_lit(j, ",");
        j_str(j, diff[i]);
    }
    j_lit(j, "]}");
    (void)screen;
    (void)index;
}

static void j_profiles(Json *j, Scratch *s, uint32_t screen) {
    const uint32_t list = profile_list(screen);
    if (!ns_ptr(list) || !(ns_call(s, NS_PROFILE_SHOWN, list, 0, NULL, 0) & 0xff))
        return;
    char current[64], name[64];
    profile_name(s, current, sizeof current);
    const int n = profile_count(list);
    int at = -1;
    j_lit(j, ",\"profiles\":{\"names\":[");
    for (int i = 0; i < n; ++i) {
        ns_wstr_utf8(rd32(list + PL_ROWS) + 16u * (uint32_t)i + 4u, name, sizeof name);
        if (strcmp(name, current) == 0)
            at = i;
        if (i)
            j_lit(j, ",");
        j_str(j, name);
    }
    j_printf(j, "],\"current\":%d,", at);
    j_kv_str(j, "title", ns_tr("Профиль", "Profile"));
    j_lit(j, ",");
    j_kv_str(j, "new", ns_tr("Новый профиль", "New profile"));
    j_lit(j, "}");
}

/* A campaign's internal name ("C1", SCampaign+0x44, a str), into `out`. */
static void campaign_key(uint32_t desc, char *out, size_t cap) {
    out[0] = 0;
    if (!ns_ptr(desc))
        return;
    const uint32_t b = rd32(desc + 0x44), e = rd32(desc + 0x44 + 4);
    size_t n = 0;
    if (ns_ptr(b) && e > b && e - b < cap)
        for (; n < e - b; ++n)
            out[n] = (char)rd8(b + (uint32_t)n);
    out[n] = 0;
    if (ns_trace()) {
        static uint32_t said;
        if (said != desc) {
            said = desc;
            fprintf(stderr, "[screens] campaign %08x key \"%s\" (str %08x..%08x)\n", desc, out, b, e);
        }
    }
}

/* A mission's loading picture (the game shows it while the map loads), by
 * its path in the paks; missions without one have no such file. */
static void j_mission_picture(Json *j, const char *key, int mission) {
    char path[96] = "";
    if (key[0])
        NS_MISSION_PICTURE(path, sizeof path, key, mission);
    j_lit(j, ",");
    j_kv_str(j, "picture", path);
}

static void j_campaigns(Json *j, Scratch *s, uint32_t screen) {
    const uint32_t set = campaign_set(screen);
    const int n = ns_ptr(set) ? (int)ns_vcall(s, set, NS_SET_COUNT, NULL, 0) : 0;
    char text[512];
    j_lit(j, ",\"campaigns\":[");
    for (int i = 0; i < n && i < 16; ++i) {
        const uint32_t k = (uint32_t)i;
        const uint32_t c = ns_vcall(s, set, NS_SET_GET, &k, 1);
        const uint32_t desc = ns_ptr(c) ? ns_vcall(s, c, 0x04, NULL, 0) : 0;
        const int unlocked = ns_vcall(s, set, NS_SET_UNLOCKED, &k, 1) & 0xff;
        const int missions = ns_ptr(c) ? (int)ns_vcall(s, c, NS_CAMPAIGN_COUNT, NULL, 0) : 0;
        if (i)
            j_lit(j, ",");
        j_lit(j, "{");
        ns_text_of(s, NS_TEXT_OF_REF, desc + 0x50, text, sizeof text);
        j_kv_str(j, "name", text);
        j_lit(j, ",");
        ns_text_of(s, NS_TEXT_OF_REF, desc + 0x68, text, sizeof text);
        j_kv_str(j, "description", text);
        char key[32];
        campaign_key(desc, key, sizeof key);
        j_mission_picture(j, key, 0);
        j_printf(j, ",\"enabled\":%s,\"done\":%d,\"missions\":%d}",
                 unlocked && missions > 0 ? "true" : "false", ns_ptr(c) ? (int)rd32(c + 0x18) : 0,
                 missions);
    }
    j_lit(j, "]");
}

static void j_missions(Json *j, Scratch *s, uint32_t screen) {
    const uint32_t c = ns_call(s, NS_CAMPAIGN_OF, rd32(screen + M_CAMPAIGNS), 0, NULL, 0);
    char text[1024];
    if (!ns_ptr(c))
        return;
    const uint32_t desc = ns_vcall(s, c, 0x04, NULL, 0);
    char key[32];
    campaign_key(desc, key, sizeof key);
    if (ns_ptr(desc)) {
        ns_text_of(s, NS_TEXT_OF_REF, desc + 0x5c, text, sizeof text);
        j_lit(j, ",");
        j_kv_str(j, "campaign", text);
    }
    const int n = (int)ns_vcall(s, c, NS_CAMPAIGN_COUNT, NULL, 0);
    j_lit(j, ",\"missions\":[");
    for (int i = 0; i < n && i < 16; ++i) {
        const uint32_t k = (uint32_t)i;
        const uint32_t rec = ns_vcall(s, c, NS_CAMPAIGN_MISSION, &k, 1);
        const int available = ns_vcall(s, c, NS_CAMPAIGN_AVAILABLE, &k, 1) & 0xff;
        const int done = ns_vcall(s, c, NS_CAMPAIGN_DONE, &k, 1) & 0xff;
        if (i)
            j_lit(j, ",");
        j_lit(j, "{");
        ns_text_of(s, NS_TEXT_OF_REF, rec + 0x04, text, sizeof text);
        j_kv_str(j, "name", text);
        j_lit(j, ",");
        ns_text_of(s, NS_TEXT_OF_REF, rec + 0x10, text, sizeof text);
        j_kv_str(j, "description", text);
        j_mission_picture(j, key, i);
        j_printf(j, ",\"enabled\":%s,\"done\":%s}", available ? "true" : "false",
                 done ? "true" : "false");
    }
    j_lit(j, "]");
}

static const char *panel_name(int index) {
    switch (index) {
    case P_MAIN: return "main";
    case P_SINGLE: return "single";
    case P_MULTI: return "multi";
    case P_HOT_SEAT: return "hot_seat";
    case P_CAMPAIGNS: return "campaigns";
    case P_USER_CAMPAIGNS: return "user_campaigns";
    case P_MISSIONS: return "missions";
    default: return "";
    }
}

static const char *panel_title(Scratch *s, int index) {
    switch (index) {
    case P_MAIN: return ns_label(s, "/UI/EscMenu/MainMenu.txt", "Главное меню", "Main Menu");
    case P_SINGLE: return ns_label(s, "/UI/MainMenu2/SingleGame.txt", "Одиночная игра", "Single Player");
    case P_MULTI: return ns_label(s, "/UI/MainMenu2/MultiPlayer.txt", "Сетевая игра", "Multiplayer");
    case P_HOT_SEAT: return ns_label(s, "/UI/MainMenu2/MultiPlayer/HotSeat.txt", "Один компьютер", "Hot Seat");
    case P_CAMPAIGNS: return ns_label(s, "/UI/MainMenu2/SinglePlayer/Campaign.txt", "Кампания", "Campaign");
    case P_USER_CAMPAIGNS: return ns_label(s, "/UI/MainMenu2/SinglePlayer/UserCampaign.txt", "Модификация", "Custom Campaign");
    case P_MISSIONS: return ns_tr("Миссии", "Missions");
    default: return "";
    }
}

static void menu_document(Json *j, Scratch *s, uint32_t screen) {
    const int index = panel(screen);
    char diff[64];
    ns_difficulty_name(s, 0, diff, sizeof diff);
    ns_set_lang(diff);
    static uint32_t captions_for;
    if (captions_for != screen) {
        captions_for = screen;
        g_caption_count = 0;
    }
    j_lit(j, "{\"screen\":\"main_menu\",");
    j_kv_str(j, "lang", ns_lang_code());
    j_lit(j, ",");
    j_kv_str(j, "panel", panel_name(index));
    j_lit(j, ",");
    j_kv_str(j, "title", panel_title(s, index));
    j_lit(j, ",");
    char version[48];
    snprintf(version, sizeof version, "%s" NS_GAME_VERSION, ns_tr("Версия: ", "Version: "));
    j_kv_str(j, "version", version);
    int shown = -1;
    if (index == P_MAIN || index == P_SINGLE || index == P_MULTI || index == P_HOT_SEAT)
        shown = j_buttons(j, s, screen, index);
    else
        j_lit(j, ",\"buttons\":[]");
    if (index == P_SINGLE || index == P_CAMPAIGNS || index == P_USER_CAMPAIGNS || index == P_MISSIONS)
        j_setup(j, s, screen, index);
    if (index == P_SINGLE)
        j_profiles(j, s, screen);
    if (index == P_CAMPAIGNS || index == P_USER_CAMPAIGNS)
        j_campaigns(j, s, screen);
    if (index == P_MISSIONS)
        j_missions(j, s, screen);
    j_lit(j, "}");
    /* Leaving for another screen (Load, a duel...), the game hides the
     * panel's buttons a few frames before that screen comes: the panel as
     * it was is shown meanwhile, not one with Back alone. */
    static char kept[16384];
    static int kept_len, kept_index = -1;
    static uint32_t kept_screen;
    if (shown > 0 && j->len < (int)sizeof kept) {
        memcpy(kept, j->s, (size_t)j->len);
        kept_len = j->len;
        kept_index = index;
        kept_screen = screen;
    } else if (shown == 0 && kept_index == index && kept_screen == screen && kept_len > 0) {
        j->len = 0;
        j_raw(j, kept, kept_len);
    }
}

/* ------------------------------------------------ the first profile's name -- */

static int box_claims(uint32_t box) {
    const uint32_t callback = rd32(box + BOX_CALLBACK);
    return ns_ptr(callback) && rd32(callback) == NS_VT_FIRST_PROFILE;
}

static void box_command(Scratch *s, uint32_t box, const char *cmd) {
    char verb[16], text[64];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    ns_cmd_str(cmd, "text", text, sizeof text);
    const uint32_t edit = rd32(box + BOX_EDIT);
    if (strcmp(verb, "accept") != 0 || !ns_ptr(edit) || !text[0])
        return;
    const uint32_t wide = ns_wide(s, text);
    ns_vcall(s, edit, 0x08, &wide, 1); /* SetText */
    ns_call(s, NS_EDIT_BOX_OK, box, 0, NULL, 0);
}

static void box_document(Json *j, Scratch *s, uint32_t box) {
    char diff[64], text[64] = "";
    ns_difficulty_name(s, 0, diff, sizeof diff);
    ns_set_lang(diff);
    const uint32_t edit = rd32(box + BOX_EDIT);
    if (ns_ptr(edit))
        ns_wstr_utf8(edit + EDIT_TEXT, text, sizeof text);
    j_lit(j, "{\"screen\":\"profile_name\",");
    j_kv_str(j, "lang", ns_lang_code());
    j_lit(j, ",");
    j_kv_str(j, "title", ns_tr("Ввод имени профиля", "Profile name"));
    j_lit(j, ",");
    j_kv_str(j, "text", text);
    j_lit(j, ",");
    j_kv_str(j, "hint", ns_tr("От 2 до 10 букв, цифр или пробелов", "2 to 10 letters, digits or spaces"));
    j_lit(j, ",");
    j_kv_str(j, "ok", "OK");
    j_lit(j, "}");
}

/* The game's logo stays where the native panels leave room for it: on the
 * main panels, not on the campaigns' and missions' (their own title is there). */
static int menu_keeps(uint32_t screen, const char *name) {
    const int index = panel(screen);
    return strcmp(name, "Logo") == 0 &&
           (index == P_MAIN || index == P_SINGLE || index == P_MULTI || index == P_HOT_SEAT);
}

const NsScreen ns_main_menu = {.id = "main_menu", .vtable = NS_VT_MENU, .sub = NS_SUB_MENU,
                               .claims = menu_claims, .command = menu_command,
                               .document = menu_document, .keeps = menu_keeps};
const NsScreen ns_profile_name = {.id = "profile_name", .vtable = NS_VT_EDIT_BOX,
                                  .sub = NS_SUB_EDIT_BOX, .claims = box_claims,
                                  .command = box_command, .document = box_document};

#else

/* No addresses for this game: never matched (a zero vtable). */
const NsScreen ns_main_menu = {.id = "main_menu"};
const NsScreen ns_profile_name = {.id = "profile_name"};

#endif
