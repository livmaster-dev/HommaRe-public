/* native/screens_custom.c - a game set up on this device, drawn natively
 * (native/native_screens.c): the map and its settings, then the players
 * before the game starts. Two variants of the same pair of screens, which
 * differ in where things are kept and how their buttons are pressed:
 * - Single player > Custom game: NUI::CCustomGameCreateScreen
 *   ("custom_create") and NUI::CCustomGameWaitScreen ("custom_players"),
 *   their buttons UISRunReaction reactions;
 * - Multiplayer > Hot seat: NUI::CHSCreate ("hotseat_create") and
 *   NUI::CHSWaitScreen ("hotseat_players"), their buttons game messages whose
 *   handlers are called directly, the players named in the rows.
 * The base game's addresses: research notes phone-menus/native/re-custom-game.md and
 * re-hot-seat.md. */
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
#define NS_VT_CREATE 0x00fc85b8      /* NUI::CCustomGameCreateScreen's IScreen, at +0x1c4 */
#define NS_SUB_CREATE 0x1c4
#define NS_VT_WAIT 0x00fcb840        /* NUI::CCustomGameWaitScreen's IScreen, at +0x2c4 */
#define NS_SUB_WAIT 0x2c4
#define NS_NEW 0x005e40b0            /* operator new, cdecl */
#define NS_FREE 0x005e40c0           /* free, cdecl */
#define NS_CREATE_REACTION 0x009a90a0 /* the map screen's button reactions, ECX screen+0x174, RET 0xc */
#define NS_CREATE_REFRESH 0x009a9b90  /* "mapselected": the map's info, Create, the settings' texts */
#define NS_MAPS_SORT 0x009da020       /* CMapsFormer: sort by column 0..3 (again: the other way), RET 4 */
#define NS_MAPS_SELECTED 0x009dbf60   /* fastcall ECX former: the chosen entry or 0 */
#define NS_MAP_NAME 0x009d9400        /* fastcall ECX info+0x4c: const wstr* */
#define NS_MAP_DESC 0x009d9260        /* fastcall ECX info+0x4c: const wstr* */
#define NS_MAP_GOAL 0x009d9310        /* fastcall ECX info+0x6c: const wstr* */
#define NS_MAP_PLAYERS 0x009dc320     /* fastcall ECX out wstr* (built empty), EDX info */
#define NS_MAP_SIZE 0x00729b30        /* fastcall ECX out wstr* (built by it), EDX size */
#define NS_MP_MAPS 0x0117260c         /* byte: the custom game lists multiplayer maps too */
#define NS_SLIDER_VT 0x01030c74       /* CWindowSlider's ISlider, its position a float at +0x20 */
#define NS_WAIT_REACTION 0x009eeff0   /* the players screen's button reactions, ECX screen+0x174 */
/* Hot seat: its message handlers are thiscall (screen, the message: 0), RET 4. */
#define NS_VT_HS_CREATE 0x00fc8738   /* NUI::CHSCreate's IScreen, at +0x194 */
#define NS_SUB_HS_CREATE 0x194
#define NS_VT_HS_WAIT 0x00fcb9a0     /* NUI::CHSWaitScreen's IScreen, at +0x1bc */
#define NS_SUB_HS_WAIT 0x1bc
#define NS_HS_CREATE 0x009aad10      /* "create" */
#define NS_HS_LOAD 0x009ab070        /* "load" */
#define NS_HS_BACK 0x009ab100        /* "back" */
#define NS_HS_MAPSELECTED 0x009ab120 /* "mapselected": the map's info */
#define NS_HS_START 0x009f1d40       /* the players' "start" */
#define NS_HS_WAIT_BACK 0x009f1d20   /* the players' "back" */
#define NS_VT_MPC 0x00fc797c         /* NUI::CMPCreateScreen's IScreen (a LAN game's map) */
#define NS_SUB_MPC 0x1a0
#define NS_MPC_MAPSELECTED 0x00997fe0 /* game messages: thiscall (screen, 0), RET 4 */
#define NS_MPC_CREATE 0x00997b30
#define NS_MPC_LOAD 0x00997f50
#define NS_MPC_BACK 0x00997b00
#define NS_HS_NAME_ACCEPT 0x009f1dc0 /* "edit_accept": the names checked */
#define NS_VT_MPW 0x00fbaae0         /* NUI::CMPWaitScreen's IScreen (a LAN game's players) */
#define NS_SUB_MPW 0x1cc
#define NS_VT_MPL 0x00fbaba0         /* NUI::CMPUbiComLoadScreen's IScreen (a loaded LAN game's players) */
#define NS_SUB_MPL 0x1e8
#define NS_MPL_MODEL 0x200           /* the host load logic's model */
#define NS_MPW_START 0x00962750      /* game messages: thiscall (screen, 0), RET 4 */
#define NS_MPW_BACK 0x00962720
#define NS_MPW_SEND 0x00962890       /* "sendmessage": the chat's edit line sent */
#define NS_MPW_SLOT_OF 0x00c98d20    /* fastcall ECX model, EDX player id: the slot */
#define NS_MPW_SERVICES 0x005e8050   /* fastcall ECX id: vt+0x24 the IMatchMaker */
#define NS_MPW_MATCHMAKER_ID 0x8c348b00
#define NS_MPW_MM_GAME 0x44          /* IMatchMaker: thiscall (SGameInfo* out) AL = in a game */
#define NS_MPW_INFO_CTOR 0x0071bfa0  /* SGameInfo, fastcall ECX */
#define NS_MPW_INFO_DTOR 0x0071d020
#define NS_MPW_PLAYERS_NOW 0x0099b7f0 /* fastcall ECX SGameInfo: players in */
#endif

#if defined(NS_VT_CREATE)


/* Fields and slots an expansion moves: its addresses.h defines the ones that
 * differ (research notes phone-menus/native/expansion-*-addresses.md). */
#ifndef NS_SCREEN_REACTIONS
#define NS_SCREEN_REACTIONS 0x174
#endif
#ifndef NS_C_MAPS
#define NS_C_MAPS 0x180
#endif
#ifndef NS_C_TIME
#define NS_C_TIME 0x194
#endif
#ifndef NS_C_DIFFICULTY
#define NS_C_DIFFICULTY 0x19c
#endif
#ifndef NS_HC_MAPS
#define NS_HC_MAPS 0x174
#endif
#ifndef NS_HC_OPTIONS
#define NS_HC_OPTIONS 0x17c
#endif
#ifndef NS_W_INFO
#define NS_W_INFO 0x190
#endif
#ifndef NS_W_TIME
#define NS_W_TIME 0x208
#endif
#ifndef NS_W_ROWS
#define NS_W_ROWS 0x218
#endif
#ifndef NS_W_CONTROLLER
#define NS_W_CONTROLLER 0x224
#endif
#ifndef NS_W_MODEL
#define NS_W_MODEL 0x228
#endif
#ifndef NS_W_DIFFICULTY
#define NS_W_DIFFICULTY 0x2a8
#endif
#ifndef NS_HW_LOGIC
#define NS_HW_LOGIC 0x198
#endif
#ifndef NS_HL_MODEL
#define NS_HL_MODEL 0x108
#endif
#ifndef NS_ROW_NAME_EDIT
#define NS_ROW_NAME_EDIT 0x148
#endif
#ifndef NS_M_RECORD_SIZE
#define NS_M_RECORD_SIZE 0x8c
#endif
#ifndef NS_M_CAN_BONUS
#define NS_M_CAN_BONUS 0x44
#endif
#ifndef NS_M_CAN_TYPE
#define NS_M_CAN_TYPE 0x6c
#endif
#ifndef NS_R_ARTIFACT_OK
#define NS_R_ARTIFACT_OK 0x5e
#endif

/* Fields and slots an expansion moves: its addresses.h defines the ones that
 * differ (research notes phone-menus/native/expansion-*-addresses.md). */
#ifndef NS_E_ROW
#define NS_E_ROW 0x84
#endif
#ifndef NS_E_KIND
#define NS_E_KIND 0x88
#endif
#ifndef NS_HL_TIME
#define NS_HL_TIME 0x8c
#endif
#ifndef NS_HL_DIFFICULTY
#define NS_HL_DIFFICULTY 0x90
#endif

enum {
    SCREEN_REACTIONS = NS_SCREEN_REACTIONS,
    /* CCustomGameCreateScreen */
    C_MAPS = NS_C_MAPS,          /* CMapsFormer* */
    C_TIME = NS_C_TIME,          /* ISlider*: position 1..45 minutes, 46 unlimited */
    C_DIFFICULTY = NS_C_DIFFICULTY,    /* ISlider*: segment 0..3 */
    /* CHSCreate */
    HC_MAPS = NS_HC_MAPS,         /* CMapsFormer* (multiplayer maps) */
    HC_OPTIONS = NS_HC_OPTIONS,      /* CHSCreateOptions*: */
    HO_TIME = 0x0c,          /*   ISlider*: segment 1..59 minutes, 60 unlimited */
    HO_DIFFICULTY = 0x14,    /*   ISlider*: segment 0..3 */
    /* CMapsFormer */
    F_DIRS = 0x24,           /* -> a byte per column: 1 the list's own way (A to Z) */
    F_COLUMN = 0x30,
    F_CONTAINER = 0x34,      /* IScrollableContainer*: +0x10 SetSelected, +0x1c EnsureVisible */
    F_FLAGS = 0x38,
    F_FILTER = 0x3c,
    F_SORTED = 0x40,         /* vector<Entry*> in the order shown */
    /* a map's entry */
    E_INFO = 0x0c,
    E_ROW = NS_E_ROW,
    E_KIND = NS_E_KIND,
    /* its info (also the players screen's copy) */
    I_SIZE = 0x0c,
    I_UNDERGROUND = 0x48,
    I_DESC = 0x4c,
    I_GOAL = 0x6c,
    /* CCustomGameWaitScreen */
    W_INFO = NS_W_INFO,          /* the map's info, copied */
    W_TIME = NS_W_TIME,          /* minutes, 0 unlimited */
    W_ROWS = NS_W_ROWS,          /* vector<IMPWaitItem*>, a row per slot */
    W_CONTROLLER = NS_W_CONTROLLER,
    W_MODEL = NS_W_MODEL,         /* NWorld::CPlayersStartInfo */
    W_DIFFICULTY = NS_W_DIFFICULTY,
    /* CHSWaitScreen: its CHotSeatLogic */
    HW_LOGIC = NS_HW_LOGIC,
    HL_ROWS = 0x08,          /* vector<IMPWaitItem*>, a row per slot */
    HL_INFO = 0x14,          /* the map's info, copied */
    HL_TIME = NS_HL_TIME,          /* minutes, -1 unlimited */
    HL_DIFFICULTY = NS_HL_DIFFICULTY,
    HL_MODEL = NS_HL_MODEL,
    HL_CAN_START = 0x0c,     /* vtable slot */
    ROW_NAME_EDIT = NS_ROW_NAME_EDIT,   /* a hot seat row: its IEditLine*, the text a wstr at +0x3c */
    /* a row (IMPWaitItem): the arrows' request, popped by the controller's Update */
    ROW_FIELD = 0xbc,
    ROW_DIR = 0xc0,
    /* the controller */
    CT_ROTATORS = 0x1c,      /* vector<CStatesRotator*>: who plays, per slot */
    CT_LOCKED = 0x28,
    CT_CAN_START = 0x20,     /* vtable slot */
    ROT_STATES = 0x0c,       /* vector<int>: 3 a person, 2 nobody, 1 the computer */
    ROT_INDEX = 0x18,
    /* the model: vtable slots, thiscall (slot) */
    M_RECORDS = 0x10,
    M_RECORD_SIZE = NS_M_RECORD_SIZE,
    M_CAN_COLOUR = 0x2c,
    M_CAN_RACE = 0x34,
    M_CAN_HERO = 0x3c,
    M_CAN_BONUS = NS_M_CAN_BONUS, /* 0: no such slot (always) */
    M_CAN_TYPE = NS_M_CAN_TYPE,
    /* a player's record */
    R_COLOUR = 0x08,         /* 1..8 */
    R_RACE = 0x18,           /* 0..5, 7 random */
    R_TYPE = 0x00,           /* 2 a person, 3 nobody, else the computer */
    R_HUMAN_OK = 0x20,
    R_COMPUTER_OK = 0x21,
    R_HERO_PATH = 0x40,      /* str: the hero's resource, empty for a random one */
    R_BONUS = 0x4c,          /* 0 random, 1 artifact, 2 gold, 3 resources */
    R_RACES = 0x50,          /* vector<int>: the factions allowed */
    R_ARTIFACT_OK = NS_R_ARTIFACT_OK, /* 0: no such rule */
};

/* Whether the computer may play this slot (ToE dropped the rule: always). */
static int computer_ok(uint32_t rec) {
#ifdef NS_NO_COMPUTER_RULE
    (void)rec;
    return 1;
#else
    return rd8(rec + R_COMPUTER_OK);
#endif
}

/* Whether a player may start with a random artifact (a game without the rule,
 * R_ARTIFACT_OK 0: always). */
static int artifact_ok(uint32_t rec) {
    return !R_ARTIFACT_OK || rd8(rec + R_ARTIFACT_OK);
}

enum { FIELD_TYPE = 1, FIELD_COLOUR = 2, FIELD_RACE = 3, FIELD_HERO = 4, FIELD_BONUS = 5 };

/* The variant being read: 0 Custom game, 1 Hot seat, 2 a LAN game's map
 * (CMPCreateScreen: hot seat's messages, custom game's sliders, the network
 * game's own settings). The table's entry points at the end set it before the
 * shared code runs. */
static int g_hs;

/* The LAN game's map screen: its map list, its options (CMPCreateOptions)
 * and their widgets (research notes lan/re-lan-screens.md §3). */
/* A game without the LAN map screen's handlers: none (hs_handler skips 0). */
#ifndef NS_MPC_MAPSELECTED
#define NS_MPC_MAPSELECTED 0
#define NS_MPC_CREATE 0
#define NS_MPC_LOAD 0
#define NS_MPC_BACK 0
#endif
#ifndef NS_MPC_MAPS
#define NS_MPC_MAPS 0x17c
#endif
#ifndef NS_MPC_OPTIONS
#define NS_MPC_OPTIONS 0x184
#endif
#ifndef NS_MPC_CREATING
#define NS_MPC_CREATING 0x188 /* the create request's listener while it runs */
#endif
#ifndef NS_MPO_NAME
#define NS_MPO_NAME 0x10
#endif
#ifndef NS_MPO_PASSWORD
#define NS_MPO_PASSWORD 0x14
#endif
#ifndef NS_MPO_LOCKED
#define NS_MPO_LOCKED 0x18
#endif
#ifndef NS_MPO_GHOST
#define NS_MPO_GHOST 0x1c
#endif
#ifndef NS_MPO_QUICK
#define NS_MPO_QUICK 0x20
#endif
#ifndef NS_MPO_AUTOSAVE
#define NS_MPO_AUTOSAVE 0x24
#endif
#ifndef NS_MPO_TIME
#define NS_MPO_TIME 0x28
#endif
#ifndef NS_MPO_DIFFICULTY
#define NS_MPO_DIFFICULTY 0x2c
#endif
#ifndef NS_MPO_ANIMATION
#define NS_MPO_ANIMATION 0x38
#endif
#ifndef NS_MPO_COMBAT
#define NS_MPO_COMBAT 0x40
#endif
/* An expansion's options (research notes lan/mp-create-wait-expansions.md):
 * no «Закрытая игра» (NS_MPO_LOCKED 0: a non-empty password makes the game
 * private, so the password is always open), «Одновременные ходы», and its own
 * words for two boxes. 0 is "the game has no such widget". */
#ifndef NS_MPO_SIMULTANEOUS
#define NS_MPO_SIMULTANEOUS 0
#endif
#ifndef NS_MPO_QUICK_LABEL
#define NS_MPO_QUICK_LABEL "Быстрый бой", "Quick combat"
#endif
#ifndef NS_MPO_AUTOSAVE_LABEL
#define NS_MPO_AUTOSAVE_LABEL "Автосохранение", "Autosave"
#endif
#ifndef NS_MPC_SPEED_NAME
#define NS_MPC_SPEED_NAME 0x00b97ca0 /* fastcall ECX speed 0..3: const wstr* */
#endif

/* The LAN game's settings, as the view's fields (FormField ids). */
enum { MP_NAME, MP_LOCKED, MP_PASSWORD, MP_GHOST, MP_QUICK, MP_AUTOSAVE, MP_SIMULTANEOUS, MP_FIELDS };
static const uint32_t MP_WIDGET[MP_FIELDS] = {NS_MPO_NAME,  NS_MPO_LOCKED, NS_MPO_PASSWORD,    NS_MPO_GHOST,
                                              NS_MPO_QUICK, NS_MPO_AUTOSAVE, NS_MPO_SIMULTANEOUS};

/* An options widget; 0 for one the game does not have (offset 0). */
static uint32_t mp_widget(uint32_t screen, uint32_t off) {
    const uint32_t opts = rd32(screen + NS_MPC_OPTIONS);
    return off && ns_ptr(opts) ? rd32(opts + off) : 0;
}

/* A hot seat handler: thiscall (screen, the message: none), RET 4. */
static void hs_handler(Scratch *s, uint32_t screen, uint32_t fn) {
    const uint32_t message = 0;
    if (!fn)
        return;
    ns_call(s, fn, screen, 0, &message, 1);
}

/* A map's players ("4 (против всех)"): the formatter fills a wide string the
 * caller built empty on the game's heap, freed after. */
static void map_players(Scratch *s, uint32_t info, char *out, size_t cap) {
    const uint32_t sixteen = 0x10;
    const uint32_t buf = ns_call(s, NS_NEW, 0, 0, &sixteen, 1);
    out[0] = 0;
    if (!ns_ptr(buf))
        return;
    wr16(buf, 0);
    const uint32_t w = ns_alloc(s, 12);
    wr32(w, buf);
    wr32(w + 4, buf);
    wr32(w + 8, buf + 0x10);
    ns_call(s, NS_MAP_PLAYERS, w, info, NULL, 0);
    ns_wstr_utf8(w, out, cap);
    const uint32_t begin = rd32(w);
    ns_call(s, NS_FREE, 0, 0, &begin, 1);
}

/* A map's size ("большая"): the formatter builds the wide string itself. */
static void map_size(Scratch *s, uint32_t info, char *out, size_t cap) {
    const uint32_t w = ns_alloc(s, 12);
    wr32(w, 0);
    ns_call(s, NS_MAP_SIZE, w, rd32(info + I_SIZE), NULL, 0);
    ns_wstr_utf8(w, out, cap);
    const uint32_t begin = rd32(w);
    if (ns_ptr(begin))
        ns_call(s, NS_FREE, 0, 0, &begin, 1);
}


/* ------------------------------------------------------- the map screen -- */

/* A map's goal in words. The base game's formatter returns a const wstr*;
 * ToE's (NS_MAP_GOAL_OUT) builds it into an out string, as the size one does. */
static void map_goal(Scratch *s, uint32_t info, char *out, size_t cap) {
#ifdef NS_MAP_GOAL_OUT
    const uint32_t w = ns_alloc(s, 12);
    wr32(w, 0);
    ns_call(s, NS_MAP_GOAL, w, info + I_GOAL, NULL, 0);
    ns_wstr_utf8(w, out, cap);
    ns_free(s, rd32(w));
#else
    ns_text_of(s, NS_MAP_GOAL, info + I_GOAL, out, cap);
#endif
}

/* A map entry's texts, read once: the formatters are guest calls. */
typedef struct {
    uint32_t entry;
    char name[128], goal[96], players[64], size[48];
} MapText;

enum { MAP_CACHE = 512 };
static MapText g_maps[MAP_CACHE];
static int g_map_count;
static uint32_t g_maps_screen; /* the screen the cache is for */
static uint32_t g_desc_entry;  /* the entry g_desc is the description of */
static char g_desc[4096];

static const MapText *map_text(Scratch *s, uint32_t entry) {
    for (int i = 0; i < g_map_count; ++i)
        if (g_maps[i].entry == entry)
            return &g_maps[i];
    if (g_map_count == MAP_CACHE)
        g_map_count = 0;
    MapText *m = &g_maps[g_map_count++];
    const uint32_t info = entry + E_INFO;
    m->entry = entry;
    ns_text_of(s, NS_MAP_NAME, info + I_DESC, m->name, sizeof m->name);
    map_goal(s, info, m->goal, sizeof m->goal);
    map_players(s, info, m->players, sizeof m->players);
    map_size(s, info, m->size, sizeof m->size);
    return m;
}

/* The entries shown, in the list's order: those the filter lets through. */
static int shown_maps(uint32_t former, uint32_t *out, int cap) {
    const uint32_t b = rd32(former + F_SORTED), e = rd32(former + F_SORTED + 4);
    const uint32_t mask = rd32(former + F_FILTER) & rd32(former + F_FLAGS) & 0xffffu;
    int n = 0;
    if (!ns_ptr(b) || e < b || e - b > 4u * 4096u)
        return 0;
    for (uint32_t p = b; p < e && n < cap; p += 4) {
        const uint32_t entry = rd32(p);
        if (ns_ptr(entry) && (rd32(entry + E_KIND) & mask))
            out[n++] = entry;
    }
    return n;
}

/* Where the variant keeps its map list and its sliders. */
static uint32_t maps_of(uint32_t screen) {
    return rd32(screen + (g_hs == 2 ? NS_MPC_MAPS : g_hs ? HC_MAPS : C_MAPS));
}
static uint32_t time_bar(uint32_t screen) {
    return g_hs == 2 ? mp_widget(screen, NS_MPO_TIME)
           : g_hs    ? rd32(rd32(screen + HC_OPTIONS) + HO_TIME)
                     : rd32(screen + C_TIME);
}
static uint32_t difficulty_bar(uint32_t screen) {
    return g_hs == 2 ? mp_widget(screen, NS_MPO_DIFFICULTY)
           : g_hs    ? rd32(rd32(screen + HC_OPTIONS) + HO_DIFFICULTY)
                     : rd32(screen + C_DIFFICULTY);
}
/* The time limit's values: 1..45 minutes then unlimited (custom), 1..59 then
 * unlimited (hot seat). */
static int time_count(void) {
    return g_hs == 1 ? 60 : 46;
}

static float slider_pos(uint32_t slider) {
    const uint32_t inner = rd32(slider + 0x20);
    if (!ns_ptr(inner) || rd32(inner + 0xf8) != NS_SLIDER_VT)
        return 0.0f;
    return rdf32(inner + 0xf8 + 0x20);
}

static void create_command(Scratch *s, uint32_t screen, const char *cmd) {
    char verb[16], name[32];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    const uint32_t former = maps_of(screen);
    if (strcmp(verb, "select") == 0 && ns_ptr(former)) {
        static uint32_t shown[4096];
        const int n = shown_maps(former, shown, 4096);
        const int i = ns_cmd_int(cmd, "index", -1);
        const uint32_t cont = rd32(former + F_CONTAINER);
        if (i < 0 || i >= n || !ns_ptr(cont))
            return;
        const uint32_t row = rd32(shown[i] + E_ROW);
        if (!ns_ptr(row))
            return;
        ns_vcall(s, cont, 0x10, &row, 1); /* SetSelected */
        ns_vcall(s, cont, 0x1c, &row, 1); /* EnsureVisible */
        if (g_hs == 2)
            hs_handler(s, screen, NS_MPC_MAPSELECTED);
        else if (g_hs)
            hs_handler(s, screen, NS_HS_MAPSELECTED);
        else
            ns_call(s, NS_CREATE_REFRESH, screen, 0, NULL, 0);
    } else if (strcmp(verb, "sort") == 0 && ns_ptr(former)) {
        const uint32_t key = (uint32_t)ns_cmd_int(cmd, "key", 0);
        if (key < 4)
            ns_call(s, NS_MAPS_SORT, former, 0, &key, 1);
    } else if (strcmp(verb, "option") == 0) {
        ns_cmd_str(cmd, "id", name, sizeof name);
        const int i = ns_cmd_int(cmd, "index", -1);
        if (strcmp(name, "time") == 0 && i >= 0 && i < time_count()) {
            /* Index i is position (custom) or segment (hot seat) i + 1. */
            if (g_hs == 1) {
                const uint32_t seg = (uint32_t)(i + 1);
                ns_vcall(s, time_bar(screen), NS_SLIDER_SET_SEGMENT, &seg, 1);
            } else {
                const float pos = (float)(i + 1);
                uint32_t bits;
                memcpy(&bits, &pos, 4);
                ns_vcall(s, time_bar(screen), NS_SLIDER_SET_POS, &bits, 1);
            }
        } else if (strcmp(name, "difficulty") == 0 && i >= 0 && i < 4) {
            const uint32_t seg = (uint32_t)i;
            ns_vcall(s, difficulty_bar(screen), NS_SLIDER_SET_SEGMENT, &seg, 1);
        } else if (g_hs == 2 && (strcmp(name, "animation") == 0 || strcmp(name, "combat") == 0) && i >= 0 &&
                   i < (name[0] == 'a' ? 5 : 4)) {
            const uint32_t seg = (uint32_t)i;
            const uint32_t bar = mp_widget(screen, name[0] == 'a' ? NS_MPO_ANIMATION : NS_MPO_COMBAT);
            if (ns_ptr(bar))
                ns_vcall(s, bar, NS_SLIDER_SET_SEGMENT, &seg, 1);
        } else {
            return;
        }
        /* Hot seat's texts follow its sliders every frame by themselves. */
        if (!g_hs)
            ns_call(s, NS_CREATE_REFRESH, screen, 0, NULL, 0);
    } else if (strcmp(verb, "text") == 0 && g_hs == 2) {
        char text[64];
        const int id = ns_cmd_int(cmd, "id", -1);
        ns_cmd_str(cmd, "text", text, sizeof text);
        const uint32_t edit = id == MP_NAME || id == MP_PASSWORD ? mp_widget(screen, MP_WIDGET[id]) : 0;
        if (ns_ptr(edit)) {
            const uint32_t wide = ns_wide(s, text);
            ns_vcall(s, edit, 0x08, &wide, 1); /* SetText */
        }
    } else if (strcmp(verb, "set") == 0 && g_hs == 2) {
        const int id = ns_cmd_int(cmd, "id", -1), value = ns_cmd_int(cmd, "value", -1);
        if (id == MP_LOCKED || id == MP_GHOST || id == MP_QUICK || id == MP_AUTOSAVE || id == MP_SIMULTANEOUS) {
            const uint32_t b = mp_widget(screen, MP_WIDGET[id]), state = value ? 0 : 1; /* 0 is on */
            if (ns_ptr(b))
                ns_vcall(s, b, 0x0c, &state, 1); /* SetState: runs its commands */
        }
    } else if (strcmp(verb, "toggle") == 0 && !g_hs) {
        ns_cmd_str(cmd, "id", name, sizeof name);
        if (strcmp(name, "mp_maps") == 0)
            ns_react(s, screen + SCREEN_REACTIONS, NS_CREATE_REACTION, "mp_filter_changed");
    } else if (strcmp(verb, "press") == 0) {
        ns_cmd_str(cmd, "name", name, sizeof name);
        if (g_hs == 2)
            hs_handler(s, screen, strcmp(name, "create") == 0 ? NS_MPC_CREATE
                                  : strcmp(name, "load") == 0 ? NS_MPC_LOAD
                                  : strcmp(name, "back") == 0 ? NS_MPC_BACK
#ifdef NS_MPC_RMG /* the random map generator: the game's own screen over this one */
                                  : strcmp(name, "rmg") == 0 ? NS_MPC_RMG
#endif
                                                              : 0);
        else if (g_hs)
            hs_handler(s, screen, strcmp(name, "create") == 0 ? NS_HS_CREATE
                                  : strcmp(name, "load") == 0 ? NS_HS_LOAD
                                  : strcmp(name, "back") == 0 ? NS_HS_BACK
                                                              : 0);
        else if (strcmp(name, "create") == 0 || strcmp(name, "back") == 0 || strcmp(name, "load") == 0)
            ns_react(s, screen + SCREEN_REACTIONS, NS_CREATE_REACTION, name);
    }
}

/* The facts of a map's info: players, size, goal and, on the map screen,
 * whether it has an underground; the array is left open for the caller's. */
static void map_facts(Json *j, Scratch *s, uint32_t info, int underground) {
    char players[64], size[48], goal[96];
    map_players(s, info, players, sizeof players);
    map_size(s, info, size, sizeof size);
    map_goal(s, info, goal, sizeof goal);
    j_lit(j, "\"facts\":[");
    j_fact(j, 1, ns_label(s, "/UI/MPWait/MapDescription/TEXTS/players_name.txt", "Игроков:", "Players:"), players);
    j_fact(j, 0, ns_label(s, "/UI/MPWait/MapDescription/TEXTS/size_name.txt", "Размер:", "Size:"), size);
    j_fact(j, 0, ns_label(s, "/UI/MPWait/MapDescription/TEXTS/goal_name.txt", "Цель:", "Goal:"), goal);
    if (underground)
        j_fact(j, 0, ns_tr("Подземелье:", "Underground:"),
               rd8(info + I_UNDERGROUND) ? ns_label(s, "/UI/MPCreate/Maps/SingleMap/MapSize/underground_yes.txt", "есть", "yes")
                                         : ns_label(s, "/UI/MPCreate/Maps/SingleMap/MapSize/underground_no.txt", "нет", "no"));
}

/* A map's preview as the game shows it: the first of its thumbnails
 * (info+0x3c, {texture*, loaded} handles), by the texture's path in the paks
 * ("/Maps/Multiplayer/L1/mL1_pic.xdb#xpointer(/Texture)"); "" without one. */
static const char *map_preview(uint32_t info) {
    static char path[256];
    path[0] = 0;
    const uint32_t b = rd32(info + 0x3c), e = rd32(info + 0x40);
    const uint32_t tex = ns_ptr(b) && e > b && e - b <= 8u * 16u ? rd32(b) : 0;
    if (ns_ptr(tex)) {
        const uint32_t pb = rd32(tex + 0x20), pe = rd32(tex + 0x24);
        uint32_t n = 0;
        if (ns_ptr(pb) && pe > pb && pe - pb < sizeof path)
            for (; n < pe - pb; ++n)
                path[n] = (char)rd8(pb + n);
        path[n] = 0;
    }
    return path;
}

/* A LAN game's extra sliders, closing the options' list it opens: animation
 * speed (5) and combat speed (4). The caller's last option is still open. */
static void mp_options(Json *j, Scratch *s, uint32_t screen) {
    static const struct {
        const char *id, *ru, *en;
        uint32_t off;
        int count;
    } bars[2] = {{"animation", "Скорость анимации", "Animation speed", NS_MPO_ANIMATION, 5},
                 {"combat", "Динамичные бои", "Combat speed", NS_MPO_COMBAT, 4}};
    for (int k = 0; k < 2; ++k) {
        const uint32_t bar = mp_widget(screen, bars[k].off);
        if (!ns_ptr(bar))
            continue;
        int seg = (int)ns_vcall(s, bar, NS_SLIDER_GET_SEGMENT, NULL, 0);
        seg = seg < 0 ? 0 : seg >= bars[k].count ? bars[k].count - 1 : seg;
        char names[5][64];
        for (int i = 0; i < bars[k].count; ++i) {
            if (k == 0)
                snprintf(names[i], sizeof names[i], "%d", i + 1);
            else
                ns_text_of(s, NS_MPC_SPEED_NAME, (uint32_t)i, names[i], sizeof names[i]);
        }
        j_lit(j, "]},{");
        j_kv_str(j, "id", bars[k].id);
        j_lit(j, ",");
        j_kv_str(j, "label", ns_tr(bars[k].ru, bars[k].en));
        j_lit(j, ",");
        j_kv_str(j, "value", names[seg]);
        j_printf(j, ",\"index\":%d,\"count\":%d,\"enabled\":true,\"values\":[", seg, bars[k].count);
        for (int i = 0; i < bars[k].count; ++i) {
            if (i)
                j_lit(j, ",");
            j_str(j, names[i]);
        }
    }
}

/* A LAN game's own settings, as the view's fields, and whether it is being
 * created (the game's wait box, in its undrawn root, says so). */
static void mp_fields(Json *j, Scratch *s, uint32_t screen) {
    static const char *const words[MP_FIELDS][2] = {
        {"Название игры", "Game name"}, {"Закрытая игра", "Private game"},     {"Пароль", "Password"},
        {"Режим призрака", "Ghost mode"}, {NS_MPO_QUICK_LABEL},                {NS_MPO_AUTOSAVE_LABEL},
        {"Одновременные ходы", "Simultaneous turns"}};
    const uint32_t locked = mp_widget(screen, NS_MPO_LOCKED);
    /* Without the box a password is always open (and makes the game private). */
    const int is_locked = !NS_MPO_LOCKED || (ns_ptr(locked) && ns_vcall(s, locked, 0x14, NULL, 0) == 0);
    j_lit(j, ",\"fields\":[");
    for (int id = 0, first = 1; id < MP_FIELDS; ++id) {
        const uint32_t w = mp_widget(screen, MP_WIDGET[id]);
        if (!ns_ptr(w))
            continue;
        j_printf(j, "%s{\"id\":%d,", first ? "" : ",", id);
        first = 0;
        j_kv_str(j, "label", ns_tr(words[id][0], words[id][1]));
        if (id == MP_NAME || id == MP_PASSWORD) {
            char text[64];
            ns_wstr_utf8(w + 0x3c, text, sizeof text);
            j_lit(j, ",");
            j_kv_str(j, "kind", id == MP_NAME ? "text" : "secret");
            j_lit(j, ",");
            j_kv_str(j, "text", text);
            j_printf(j, ",\"enabled\":%s}", id == MP_NAME || is_locked ? "true" : "false");
        } else {
            j_printf(j, ",\"kind\":\"toggle\",\"on\":%s,\"enabled\":true}",
                     ns_vcall(s, w, 0x14, NULL, 0) == 0 ? "true" : "false");
        }
    }
    j_lit(j, "]");
    if (ns_ptr(rd32(screen + NS_MPC_CREATING))) {
        j_lit(j, ",");
        j_kv_str(j, "busy", ns_tr("Создание игры...", "Creating the game..."));
    }
}

static void create_document(Json *j, Scratch *s, uint32_t screen) {
    const uint32_t former = maps_of(screen);
    if (g_maps_screen != screen) {
        g_maps_screen = screen;
        g_map_count = 0;
        g_desc_entry = 0;
    }
    char diff[8][64];
    for (int i = 0; i < 4; ++i)
        ns_difficulty_name(s, i, diff[i], sizeof diff[i]);
    ns_ru = (unsigned char)diff[0][0] >= 0xd0;

    j_lit(j, "{");
    j_kv_str(j, "screen", g_hs == 2 ? "mp_create" : g_hs ? "hotseat_create" : "custom_create");
    j_lit(j, ",");
    j_kv_str(j, "lang", ns_ru ? "ru" : "en");
    j_lit(j, ",");
    j_kv_str(j, "title", g_hs == 2 ? ns_tr("Создание обычной игры", "New Network Game")
                         : g_hs ? ns_label(s, "/UI/HotSeat/Header.txt", "Один компьютер: создание игры", "Hot Seat: New Game")
                              : ns_label(s, "/UI/CustomGameCreate/SelectScenario.txt", "Выбрать свою игру", "Custom Game"));
    j_lit(j, ",\"columns\":[");
    j_str(j, ns_label(s, "/UI/MPCreate/Maps/Name.txt", "Имя", "Name"));
    j_lit(j, ",");
    j_str(j, ns_label(s, "/UI/MPCreate/Maps/Goal.txt", "Цель", "Goal"));
    j_lit(j, ",");
    j_str(j, ns_label(s, "/UI/MPCreate/Maps/Players.txt", "Игроков", "Players"));
    j_lit(j, ",");
    j_str(j, ns_label(s, "/UI/MPCreate/Maps/MapSize.txt", "Размер", "Size"));
    j_lit(j, "]");

    static uint32_t shown[4096];
    const int n = ns_ptr(former) ? shown_maps(former, shown, 4096) : 0;
    const uint32_t column = ns_ptr(former) ? rd32(former + F_COLUMN) & 3u : 0;
    const uint32_t dirs = ns_ptr(former) ? rd32(former + F_DIRS) : 0;
    const int descending = ns_ptr(dirs) && rd8(dirs + column) == 0;
    const uint32_t chosen = ns_ptr(former) ? ns_call(s, NS_MAPS_SELECTED, former, 0, NULL, 0) : 0;
    int selected = -1;
    j_printf(j, ",\"sort\":%u,\"descending\":%s,\"maps\":[", column, descending ? "true" : "false");
    for (int i = 0; i < n; ++i) {
        const MapText *m = map_text(s, shown[i]);
        if (shown[i] == chosen)
            selected = i;
        j_raw(j, i ? ",{" : "{", i ? 2 : 1);
        j_kv_str(j, "name", m->name);
        j_lit(j, ",");
        j_kv_str(j, "goal", m->goal);
        j_lit(j, ",");
        j_kv_str(j, "players", m->players);
        j_lit(j, ",");
        j_kv_str(j, "size", m->size);
        j_lit(j, "}");
    }
    j_printf(j, "],\"selected\":%d,\"info\":", selected);
    if (ns_ptr(chosen)) {
        const uint32_t info = chosen + E_INFO;
        if (g_desc_entry != chosen) {
            g_desc_entry = chosen;
            ns_text_of(s, NS_MAP_DESC, info + I_DESC, g_desc, sizeof g_desc);
        }
        j_lit(j, "{");
        j_kv_str(j, "name", map_text(s, chosen)->name);
        j_lit(j, ",");
        map_facts(j, s, info, 1);
        j_lit(j, "],");
        j_kv_str(j, "description_title", ns_label(s, "/UI/MPCreate/MapInfo/header.txt", "Описание карты", "Map description"));
        j_lit(j, ",");
        j_kv_str(j, "description", g_desc);
        j_lit(j, ",");
        j_kv_str(j, "preview", map_preview(info));
        j_lit(j, "}");
    } else {
        j_lit(j, "null");
    }

    /* The turn's time limit: position or segment 1..count, the last one
     * unlimited; index = that - 1. */
    const uint32_t diff_bar = difficulty_bar(screen);
    const int count = time_count();
    int pos = g_hs == 1 ? (ns_ptr(time_bar(screen)) ? (int)ns_vcall(s, time_bar(screen), NS_SLIDER_GET_SEGMENT, NULL, 0) : count)
                   : (int)(slider_pos(time_bar(screen)) + 0.5f);
    pos = pos < 1 ? 1 : pos > count ? count : pos;
    const char *unlimited = ns_tr("без ограничения", "unlimited");
    char value[32];
    if (pos >= count)
        snprintf(value, sizeof value, "%s", unlimited);
    else
        snprintf(value, sizeof value, ns_tr("%d мин.", "%d min"), pos);
    j_lit(j, ",\"options\":[{");
    j_kv_str(j, "id", "time");
    j_lit(j, ",");
    j_kv_str(j, "label", ns_tr("Время хода", "Turn time"));
    j_lit(j, ",");
    j_kv_str(j, "value", value);
    j_printf(j, ",\"index\":%d,\"count\":%d,\"enabled\":true,\"values\":[", pos - 1, count);
    for (int m = 1; m <= count; ++m) {
        if (m < count)
            snprintf(value, sizeof value, ns_tr("%d мин.", "%d min"), m);
        else
            snprintf(value, sizeof value, "%s", unlimited);
        if (m > 1)
            j_lit(j, ",");
        j_str(j, value);
    }
    int seg = ns_ptr(diff_bar) ? (int)ns_vcall(s, diff_bar, NS_SLIDER_GET_SEGMENT, NULL, 0) : 1; /* GetSegment */
    seg = seg < 0 ? 0 : seg > 3 ? 3 : seg;
    j_lit(j, "]},{");
    j_kv_str(j, "id", "difficulty");
    j_lit(j, ",");
    j_kv_str(j, "label", ns_tr("Сложность", "Difficulty"));
    j_lit(j, ",");
    j_kv_str(j, "value", diff[seg]);
    j_printf(j, ",\"index\":%d,\"count\":4,\"enabled\":true,\"values\":[", seg);
    for (int i = 0; i < 4; ++i) {
        if (i)
            j_lit(j, ",");
        j_str(j, diff[i]);
    }
    if (g_hs == 2)
        mp_options(j, s, screen);
    j_lit(j, "]}],\"toggles\":[");
    if (!g_hs) {
        j_lit(j, "{");
        j_kv_str(j, "id", "mp_maps");
        j_lit(j, ",");
        j_kv_str(j, "label", ns_label(s, "/UI/CustomGameCreate/MPFilterView.txt", "Многопользовательские карты", "Multiplayer maps"));
        j_printf(j, ",\"on\":%s}", rd8(NS_MP_MAPS) ? "true" : "false");
    }
    j_lit(j, "]");
    if (g_hs == 2)
        mp_fields(j, s, screen);
    j_lit(j, ",\"buttons\":[");
    j_button(j, 1, "create", ns_label(s, g_hs ? "/UI/HotSeat/Menu/create.txt" : "/UI/CustomGameCreate/Create.txt", "Создать", "Create"),
             ns_ptr(chosen));
    j_button(j, 0, "load", ns_label(s, g_hs ? "/UI/HotSeat/Menu/load.txt" : "/UI/CustomGameCreate/Load.txt", "Загрузить", "Load"), 1);
#ifdef NS_MPC_RMG
    if (g_hs == 2)
        j_button(j, 0, "rmg", ns_tr("Создать карту", "Generate a Map"), 1);
#endif
    j_button(j, 0, "back", ns_label(s, g_hs ? "/UI/HotSeat/Menu/back.txt" : "/UI/CustomGameCreate/Back.txt", "Назад", "Back"), 1);
    j_lit(j, "]}");
}

/* --------------------------------------------------- the players screen -- */

/* The players' texts are the game's own (its row's texts, by file), its
 * pictures named by their paths in the paks for the view to read. */
#define ITEM "/UI/MPWait/PlayersList/Item/"
#define ITEM_TEX "/Textures/Interface/MPWait/PlayersList/Item/"

static const char *const RU_COLOUR[8] = {"красный", "синий", "зелёный", "жёлтый",
                                         "оранжевый", "бирюзовый", "фиолетовый", "коричневый"};
static const char *const EN_COLOUR[8] = {"red", "blue", "green", "yellow",
                                         "orange", "teal", "purple", "tan"};
/* The colours' keys in the game's files ("player_brown.txt"). */
static const char *const COLOUR_KEY[8] = {"red", "blue", "green", "yellow",
                                          "orange", "teal", "purple", "brown"};
/* The colour's flag: the game's own key table swaps blue's and teal's files. */
static const char *const COLOUR_FLAG[8] = {"team_red", "team_teal", "team_green", "team_yellow",
                                           "team_orange", "team_blue", "team_purple", "team_tan"};

/* A game text, or the fallback when the install has none; a capital first. */
static const char *text_or(Scratch *s, const char *path, const char *fallback) {
    static char out[8][192];
    static int next;
    const char *t = ns_game_text(s, path);
    char *o = out[next++ & 7];
    snprintf(o, sizeof out[0], "%s", t[0] ? t : fallback);
    return ns_capital(o);
}

/* A faction by the game's race id: the base game's 0..5 (Haven, Sylvan,
 * Academy, Dungeon, Necropolis, Inferno), the expansions' Fortress and
 * Stronghold after them; ToE counts from 3 (NS_RACE_FIRST). -1: random. */
#ifndef NS_RACE_FIRST
#define NS_RACE_FIRST 0
#endif
#ifndef NS_RACE_RANDOM
#define NS_RACE_RANDOM 7
#endif
static int faction(int race) {
    const int f = race - NS_RACE_FIRST;
    return race != NS_RACE_RANDOM && f >= 0 && f < 8 ? f : -1;
}

static const char *race_name(Scratch *s, int race) {
    static const char *const file[] = {"heaven", "preserve", "academy", "dungeon", "necromancy", "inferno",
                                       "fortress", "stronghold"};
    static const char *const ru[] = {"Орден Порядка", "Лесной Союз", "Академия", "Лига Теней",
                                     "Некрополис", "Инферно", "Северные кланы", "Великая Орда"};
    static const char *const en[] = {"Haven", "Sylvan", "Academy", "Dungeon", "Necropolis", "Inferno",
                                     "Fortress", "Stronghold"};
    char path[96];
    const int f = faction(race);
    if (f >= 0) {
        snprintf(path, sizeof path, "/Text/Game/Towns/Types/%s.txt", file[f]);
        return text_or(s, path, ns_ru ? ru[f] : en[f]);
    }
    return text_or(s, ITEM "race_random.txt", ns_tr("Случайная", "Random"));
}

static const char *race_icon(int race) {
    static const char *const icon[] = {
        ITEM_TEX "Races/Haven.(Texture).dds", ITEM_TEX "Races/Rampart.(Texture).dds",
        ITEM_TEX "Races/Academy.(Texture).dds", ITEM_TEX "Races/Dungeon.(Texture).dds",
        ITEM_TEX "Races/Necromancy.(Texture).dds", ITEM_TEX "Races/Inferno.(Texture).dds",
        /* the expansions' own, as their Races.(WindowRelatedTextures) name them */
        "/UI/H5A1/Icons/Buildings/Towns/Fortress/Adventure_Mode_Icon/128x128/Fortress_lvl2.(Texture).xdb",
        "/UI/H5A2/Icons/Adv_map_City/Stronghold.(Texture).xdb"};
    const int f = faction(race);
    return f >= 0 ? icon[f] : ITEM_TEX "Races/Random.(Texture).dds";
}

static const char *bonus_name(Scratch *s, int bonus) {
    static const char *const ru[] = {"Случайный", "Артефакт", "Золото", "Ресурсы"};
    static const char *const en[] = {"Random", "Artifact", "Gold", "Resources"};
    const int b = bonus >= 0 && bonus < 4 ? bonus : 0;
    char path[96];
    snprintf(path, sizeof path, ITEM "bonus%d.txt", b);
    return text_or(s, path, ns_ru ? ru[b] : en[b]);
}

static const char *bonus_icon(int bonus) {
    static const char *const icon[] = {"Random", "RandomArtifact", "Gold", "Resource"};
    static char path[128];
    snprintf(path, sizeof path, ITEM_TEX "Bonuses/%s.(Texture).dds", icon[bonus >= 0 && bonus < 4 ? bonus : 0]);
    return path;
}

static const char *colour_icon(int colour) {
    static char path[128];
    snprintf(path, sizeof path, ITEM_TEX "%s.(Texture).dds", COLOUR_FLAG[colour & 7]);
    return path;
}

/* Who plays: a person, nobody, the computer; the last two in the game's words. */
static const char *who_name(Scratch *s, int closed, int computer) {
    if (closed)
        return text_or(s, ITEM "state1.txt", ns_tr("Закрыт", "Closed"));
    if (computer)
        return text_or(s, ITEM "state2.txt", ns_tr("Компьютер", "Computer"));
    return ns_tr("Человек", "Human");
}

/* Who plays, by the rotator's state: 3 a person, 2 nobody, 1 the computer. */
static const char *hs_type_name(Scratch *s, int type) {
    return who_name(s, type == 3, type != 2 && type != 3);
}

static const char *type_name(Scratch *s, int state) {
    return who_name(s, state == 2, state != 3 && state != 2);
}

static const char *who_icon(int closed, int computer) {
    return closed ? ITEM_TEX "States/Closed.(Texture).dds"
           : computer ? ITEM_TEX "States/Comp.(Texture).dds"
                      : "";
}

/* A hero as the game shows it: its name, specialisation and small portrait,
 * from its resource (SAdvMapHeroShared) found by the record's ref; the name
 * from the ref's path ("/MapObjects/Haven/Godric.(...)" gives "Godric") when
 * that cannot be read. Read once per hero. */
typedef struct {
    char key[160];
    char name[96], spec[96], face[200];
} Hero;

#ifndef RECOMP_ADDRESS_HEADER
#define NS_RESOURCE_OF 0x00a380b0    /* fastcall ECX a ref: CResource* */
#define NS_DYNAMIC_CAST 0x009fe372   /* cdecl (p, 0, from, to, 0) */
#define NS_RTTI_RESOURCE 0x010e7088
#define NS_RTTI_HERO_SHARED 0x010e8db4
#define NS_TEXT_REF 0x00a09de0       /* fastcall ECX a text ref: const wstr* */
#endif

/* Fields and slots an expansion moves: its addresses.h defines the ones that
 * differ (research notes phone-menus/native/expansion-*-addresses.md). */
#ifndef NS_HS_SPEC
#define NS_HS_SPEC 0xfc
#endif
#ifndef NS_HS_FACE_BIG
#define NS_HS_FACE_BIG 0x11c
#endif
#ifndef NS_HS_FACE_SMALL
#define NS_HS_FACE_SMALL 0x124
#endif
#ifndef NS_HS_NAME
#define NS_HS_NAME 0x15c
#endif

enum {
    R_HERO_REF = 0x2c,     /* the record's hero ref (its path the str at +0x40) */
    HS_SPEC = NS_HS_SPEC,        /* SpecializationNameFileRef */
    HS_FACE_SMALL = NS_HS_FACE_SMALL, /* FaceTextureSmall: {resource*, loaded} */
    HS_FACE_BIG = NS_HS_FACE_BIG,     /* FaceTexture: the game's fallback when there is no small one */
    HS_NAME = NS_HS_NAME,       /* Editable/NameFileRef */
    RES_PATH = 0x20,       /* a resource's own path, a str */
};

static void read_str(uint32_t str, char *out, size_t cap) {
    const uint32_t b = rd32(str), e = rd32(str + 4);
    size_t n = 0;
    if (ns_ptr(b) && e > b && e - b < cap)
        for (; n < e - b; ++n)
            out[n] = (char)rd8(b + (uint32_t)n);
    out[n] = 0;
}

static void hero_facts(Scratch *s, uint32_t shared, Hero *h);

static const Hero *hero_of(Scratch *s, uint32_t rec) {
    static Hero cache[32];
    static int count, next;
    char key[160];
    read_str(rec + R_HERO_PATH, key, sizeof key);
    if (!key[0])
        return NULL;
    for (int i = 0; i < count; ++i)
        if (strcmp(cache[i].key, key) == 0)
            return &cache[i];
    Hero *h = &cache[count < 32 ? count++ : next++ % 32];
    memset(h, 0, sizeof *h);
    snprintf(h->key, sizeof h->key, "%s", key);
    char *cut = strstr(key, ".(");
    if (cut)
        *cut = 0;
    const char *slash = strrchr(key, '/');
    snprintf(h->name, sizeof h->name, "%s", slash ? slash + 1 : key);
#ifdef NS_RESOURCE_OF
    const uint32_t res = ns_call(s, NS_RESOURCE_OF, rec + R_HERO_REF, 0, NULL, 0);
    const uint32_t args[5] = {res, 0, NS_RTTI_RESOURCE, NS_RTTI_HERO_SHARED, 0};
    const uint32_t shared = ns_ptr(res) ? ns_call(s, NS_DYNAMIC_CAST, 0, 0, args, 5) : 0;
    hero_facts(s, shared, h);
    if (ns_trace())
        fprintf(stderr, "[screens] hero %s: %08x \"%s\" \"%s\" %s\n", h->key, shared, h->name, h->spec, h->face);
#else
    (void)s;
#endif
    return h;
}

/* A hero's name, specialisation and face from its SAdvMapHeroShared. */
static void hero_facts(Scratch *s, uint32_t shared, Hero *h) {
#ifndef NS_RESOURCE_OF
    (void)s, (void)shared, (void)h;
#else
    if (ns_ptr(shared)) {
        char text[96];
        ns_text_of(s, NS_TEXT_REF, shared + HS_NAME, text, sizeof text);
        if (text[0])
            snprintf(h->name, sizeof h->name, "%s", text);
        ns_text_of(s, NS_TEXT_REF, shared + HS_SPEC, h->spec, sizeof h->spec);
        /* The small face, else the big one (a texture or its unresolved ref). */
        const uint32_t small = rd32(shared + HS_FACE_SMALL), big = rd32(shared + HS_FACE_BIG);
        if (ns_ptr(small))
            read_str(small + RES_PATH, h->face, sizeof h->face);
        if (!h->face[0] && ns_ptr(big))
            read_str(big + RES_PATH, h->face, sizeof h->face);
    }
#endif
}

static void j_icon(Json *j, const char *path) {
    j_lit(j, ",");
    j_kv_str(j, "icon", path ? path : "");
}

/* A value the native picker asked a player's setting to take: the game only
 * steps, so it is stepped toward each frame (one request per row and frame,
 * as an arrow makes it) until it is there or the tries run out. */
typedef struct {
    uint32_t screen;
    int slot, field, target, tries;
} Want;

enum { WANTS = 16 };
static Want g_wants[WANTS];

/* A LAN game's players (NUI::CMPWaitScreen): its logic keeps the rows, and
 * only the host has a model (research notes lan/re-lan-screens-2.md §8). */
#ifndef NS_MPW_HOST
#define NS_MPW_HOST 0x1b0     /* byte: this device hosts the game */
#endif
#ifndef NS_MPW_LOGIC
#define NS_MPW_LOGIC 0x1b4
#endif
#ifndef NS_MPW_CHAT
#define NS_MPW_CHAT 0x1a8     /* CMPChatLogModel */
#endif
#ifndef NS_MPW_EDIT
#define NS_MPW_EDIT 0x1ac     /* the chat's IEditLine */
#endif
#ifndef NS_MPW_STARTING
#define NS_MPW_STARTING 0x18c /* CStartGameListener while the game starts */
#endif
#ifndef NS_MPW_ML_ACCEPTING
#define NS_MPW_ML_ACCEPTING 0x108 /* the host logic's "changes taken" byte */
#endif
enum {
    MW_HOST = NS_MPW_HOST,
    MW_LOGIC = NS_MPW_LOGIC,
    MW_CHAT = NS_MPW_CHAT,
    MW_EDIT = NS_MPW_EDIT,
    MW_STARTING = NS_MPW_STARTING,
    /* the logic (host and client) */
    ML_SESSION = 0x10,
    ML_STATUS = 0x18,        /* CMPWaitStatusDemonstrator: its CWaitBox at +0x18 */
    ML_GOT_ID = 0x60,        /* client: byte, its slot heard of */
    ML_ACCEPTING = NS_MPW_ML_ACCEPTING, /* host only: byte, changes taken */
    ML_CAN_START = 0x18,     /* vtable slot */
    /* a row */
    MR_STATE = 0x28,         /* 0 a person not ready, 1 computer, 2 closed, 3 ready, 4 open */
    MR_COLOUR = 0x44,        /* 1..8 */
    MR_RACE = 0x60,
    MR_BONUS = 0x7c,
    MR_HERO = 0x80,          /* SAdvMapHeroShared*, 0 random */
    MR_LOCKS = 0xc4,         /* hash map field -> locked: buckets [+4, +8) */
    MR_NAME = 0xf4,          /* wstr */
};

/* A loaded LAN game's players (NUI::CMPUbiComLoadScreen, a CMPWaitScreen
 * opened once a LAN save is chosen): the screen is CMPWaitScreen's, its logic
 * another class (CMultiplayerHostLoadLogic, CMultiplayerClientLoadLogic) that
 * keeps the rows lower and the save's settings locked on every row; only who
 * plays changes. Its model's records are not read. */
static int g_mp_load;
#ifndef NS_MPL_ROWS
#define NS_MPL_ROWS 0x34
#define NS_MPL_HOST_ID 0x30 /* the local player's id; a client's -1 until the host gives one */
#endif
#ifndef NS_MPL_MODEL
#define NS_MPL_MODEL 0 /* a game without the screen never reads it */
#endif
#define ML_HOST_ID (g_mp_load ? NS_MPL_HOST_ID : 0x34)
#define ML_ROWS (g_mp_load ? NS_MPL_ROWS : 0x38) /* vector<IMPWaitItem*>, a row per slot */
#define ML_MODEL (g_mp_load ? NS_MPL_MODEL : 0x74) /* host only: CPlayersStartInfo */

/* Where the variant keeps its rows (a vector's begin, end) and its model. */
static uint32_t rows_of(uint32_t screen) {
    if (g_hs == 3)
        return rd32(screen + MW_LOGIC) + ML_ROWS;
    return g_hs ? rd32(screen + HW_LOGIC) + HL_ROWS : screen + W_ROWS;
}
static uint32_t model_of(uint32_t screen) {
    if (g_hs == 3)
        return rd8(screen + MW_HOST) ? rd32(rd32(screen + MW_LOGIC) + ML_MODEL) : 0;
    return g_hs ? rd32(rd32(screen + HW_LOGIC) + HL_MODEL) : rd32(screen + W_MODEL);
}

static uint32_t wait_row(uint32_t screen, int slot) {
    const uint32_t b = rd32(rows_of(screen)), e = rd32(rows_of(screen) + 4);
    if (!ns_ptr(b) || slot < 0 || (uint32_t)slot >= (e - b) / 4u)
        return 0;
    return rd32(b + 4u * (uint32_t)slot);
}

static uint32_t wait_record(uint32_t screen, int slot) {
    if (g_hs == 3 && g_mp_load)
        return 0;
    const uint32_t model = model_of(screen);
    return ns_ptr(model) ? rd32(model + M_RECORDS) + (uint32_t)slot * M_RECORD_SIZE : 0;
}

static uint32_t wait_rotator(uint32_t screen, int slot) {
    const uint32_t ct = g_hs ? 0 : rd32(screen + W_CONTROLLER);
    if (!ns_ptr(ct))
        return 0;
    const uint32_t b = rd32(ct + CT_ROTATORS), e = rd32(ct + CT_ROTATORS + 4);
    if (!ns_ptr(b) || slot < 0 || (uint32_t)slot >= (e - b) / 4u)
        return 0;
    return rd32(b + 4u * (uint32_t)slot);
}

/* Hot seat's who-plays, from the model's type: 2 a person, 3 nobody, 0 the
 * computer (1 too); and its list, in the order dir 0 steps through it. */
static int hs_type(uint32_t type) {
    return type == 2 || type == 3 ? (int)type : 0;
}
static const int HS_TYPES[3] = {2, 0, 3};

/* The current value of a setting the picker can set. */
static int setting_value(uint32_t screen, int slot, int field) {
    if (g_hs == 3) {
        /* A LAN game: the rows, on either side. */
        const uint32_t row = wait_row(screen, slot);
        if (!ns_ptr(row))
            return -1;
        return field == FIELD_TYPE ? (int)rd32(row + MR_STATE)
               : field == FIELD_RACE ? (int)rd32(row + MR_RACE)
               : field == FIELD_BONUS ? (int)rd32(row + MR_BONUS)
                                      : -1;
    }
    const uint32_t rec = wait_record(screen, slot);
    if (!ns_ptr(rec))
        return -1;
    switch (field) {
    case FIELD_TYPE: {
        if (g_hs)
            return hs_type(rd32(rec + R_TYPE));
        const uint32_t rot = wait_rotator(screen, slot);
        return ns_ptr(rot) ? (int)rd32(rot + ROT_INDEX) : -1;
    }
    case FIELD_RACE: return (int)rd32(rec + R_RACE);
    case FIELD_BONUS: return (int)rd32(rec + R_BONUS);
    default: return -1;
    }
}

static void request(uint32_t screen, int slot, int field, int dir) {
    const uint32_t row = wait_row(screen, slot);
    if (ns_ptr(row) && rd32(row + ROW_FIELD) == 0) {
        wr32(row + ROW_FIELD, (uint32_t)field);
        wr8(row + ROW_DIR, (uint8_t)dir);
    }
}

/* A hot seat player's name: the row's edit line holds it until the game
 * starts. Set as typing would leave it (SetText), then checked as Return
 * would ("edit_accept": a name the game refuses brings its message box). The
 * view checks the game's rules first: 2..10 letters, digits or spaces. */
static void set_name(Scratch *s, uint32_t screen, int slot, const char *utf8) {
    const uint32_t row = wait_row(screen, slot);
    const uint32_t edit = ns_ptr(row) ? rd32(row + ROW_NAME_EDIT) : 0;
    if (!ns_ptr(edit))
        return;
    const uint32_t text = ns_alloc(s, 2u * 64u);
    uint32_t n = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p && n < 63;) {
        uint32_t u = *p++;
        if (u >= 0xc0 && u < 0xe0 && (*p & 0xc0) == 0x80)
            u = ((u & 0x1f) << 6) | (*p++ & 0x3f);
        else if (u >= 0xe0 && u < 0xf0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) {
            u = ((u & 0x0f) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f);
            p += 2;
        } else if (u >= 0x80)
            continue; /* nothing a name may hold */
        wr16(text + 2u * n++, (uint16_t)u);
    }
    wr16(text + 2u * n, 0);
    ns_vcall(s, edit, 0x08, &text, 1); /* SetText */
    hs_handler(s, screen, NS_HS_NAME_ACCEPT);
}

static void wait_command(Scratch *s, uint32_t screen, const char *cmd) {
    char verb[16], name[32];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    const int slot = ns_cmd_int(cmd, "slot", -1), field = ns_cmd_int(cmd, "field", 0);
    if (strcmp(verb, "rotate") == 0 && field >= FIELD_TYPE && field <= FIELD_BONUS) {
        request(screen, slot, field, ns_cmd_int(cmd, "dir", 1) ? 1 : 0);
    } else if (strcmp(verb, "set") == 0) {
        /* The picker's index is the value's place in the list the document
         * gave: turn it back into the setting's own value. */
        const int index = ns_cmd_int(cmd, "index", -1);
        const uint32_t rec = wait_record(screen, slot);
        int target = -1;
        if (field == FIELD_TYPE) {
            /* Custom: the rotator's indices 1..n-1. Hot seat: a type. */
            target = g_hs ? (index >= 0 && index < 3 ? HS_TYPES[index] : -1) : index + 1;
        } else if (field == FIELD_RACE && ns_ptr(rec)) {
            const uint32_t b = rd32(rec + R_RACES), e = rd32(rec + R_RACES + 4);
            const uint32_t n = ns_ptr(b) && e >= b ? (e - b) / 4u : 0;
            target = index == 0 ? NS_RACE_RANDOM : index - 1 < (int)n ? (int)rd32(b + 4u * (uint32_t)(index - 1)) : -1;
        } else if (field == FIELD_BONUS && ns_ptr(rec)) {
            static const int with_artifact[] = {0, 1, 2, 3}, without[] = {0, 2, 3};
            const int *list = artifact_ok(rec) ? with_artifact : without;
            const int n = artifact_ok(rec) ? 4 : 3;
            target = index >= 0 && index < n ? list[index] : -1;
        }
        if (target < 0)
            return;
        for (int i = 0; i < WANTS; ++i)
            if (!g_wants[i].screen || (g_wants[i].slot == slot && g_wants[i].field == field)) {
                Want w = {screen, slot, field, target, 10};
                g_wants[i] = w;
                break;
            }
    } else if (strcmp(verb, "name") == 0 && g_hs) {
        char text[64];
        ns_cmd_str(cmd, "text", text, sizeof text);
        set_name(s, screen, slot, text);
    } else if (strcmp(verb, "press") == 0) {
        ns_cmd_str(cmd, "name", name, sizeof name);
        if (strcmp(name, "start") == 0)
            ns_leave();
        if (g_hs)
            hs_handler(s, screen, strcmp(name, "start") == 0 ? NS_HS_START
                                  : strcmp(name, "back") == 0 ? NS_HS_WAIT_BACK
                                                              : 0);
        else if (strcmp(name, "start") == 0 || strcmp(name, "back") == 0)
            ns_react(s, screen + SCREEN_REACTIONS, NS_WAIT_REACTION, name);
    }
}

/* Steps each wanted setting once more toward its value. */
static void step_wants(Scratch *s, uint32_t screen) {
    (void)s;
    for (int i = 0; i < WANTS; ++i) {
        Want *w = &g_wants[i];
        if (!w->screen)
            continue;
        if (w->screen != screen || setting_value(screen, w->slot, w->field) == w->target ||
            w->tries-- <= 0) {
            w->screen = 0;
            continue;
        }
        /* Who plays steps with dir 0 (custom: the rotator's index up; hot
         * seat: person, nobody, computer); the others with the right arrow. */
        request(screen, w->slot, w->field, w->field == FIELD_TYPE && g_hs != 3 ? 0 : 1);
    }
}

static void j_cell(Json *j, int first, int field, const char *title, const char *value, int enabled) {
    if (!first)
        j_lit(j, ",");
    j_printf(j, "{\"field\":%d,", field);
    j_kv_str(j, "title", title);
    j_lit(j, ",");
    j_kv_str(j, "value", value);
    j_printf(j, ",\"enabled\":%s", enabled ? "true" : "false");
}

static void wait_document(Json *j, Scratch *s, uint32_t screen) {
    const uint32_t model = model_of(screen);
    const uint32_t ct = g_hs ? 0 : rd32(screen + W_CONTROLLER);
    const uint32_t logic = g_hs ? rd32(screen + HW_LOGIC) : 0;
    const uint32_t b = rd32(rows_of(screen)), e = rd32(rows_of(screen) + 4);
    const int n = ns_ptr(b) && e >= b && e - b <= 4u * 8u ? (int)((e - b) / 4u) : 0;
    const int locked = ns_ptr(ct) && rd8(ct + CT_LOCKED);
    char diff[64];
    ns_difficulty_name(s, 0, diff, sizeof diff);
    ns_ru = (unsigned char)diff[0] >= 0xd0;

    j_lit(j, "{");
    j_kv_str(j, "screen", g_hs ? "hotseat_players" : "custom_players");
    j_lit(j, ",");
    j_kv_str(j, "lang", ns_ru ? "ru" : "en");
    j_lit(j, ",");
    j_kv_str(j, "title", ns_label(s, g_hs ? "/UI/HotSeatWait/header.txt" : "/UI/CustomGameWait/ScreenHeader.txt", "Настройки игры", "Game Setup"));
    j_lit(j, ",\"players\":[");
    for (int slot = 0; slot < n && ns_ptr(model); ++slot) {
        const uint32_t rec = wait_record(screen, slot);
        const uint32_t s32 = (uint32_t)slot;
        const int colour = (int)(rd32(rec + R_COLOUR) - 1u) & 7;
        char label[64], path[96];
        snprintf(label, sizeof label, "%s %s", ns_ru ? RU_COLOUR[colour] : EN_COLOUR[colour],
                 ns_tr("игрок", "player"));
        snprintf(path, sizeof path, ITEM "player_%s.txt", COLOUR_KEY[colour]);
        snprintf(label, sizeof label, "%s", text_or(s, path, label));
        j_printf(j, "%s{\"slot\":%d,", slot ? "," : "", slot);
        j_kv_str(j, "label", label);
        if (g_hs) {
            /* The name typed in the row, kept there until the game starts. */
            const uint32_t row = wait_row(screen, slot);
            const uint32_t edit = ns_ptr(row) ? rd32(row + ROW_NAME_EDIT) : 0;
            char name[64] = "";
            if (ns_ptr(edit))
                ns_wstr_utf8(edit + 0x3c, name, sizeof name);
            j_lit(j, ",");
            j_kv_str(j, "name", name);
        }
        j_printf(j, ",\"colour\":%d,\"cells\":[", colour);

        if (g_hs) {
            /* Who plays: person, computer, nobody (when it may be closed). */
            const int type = hs_type(rd32(rec + R_TYPE));
            const int can_close = ns_vcall(s, model, M_CAN_TYPE, &s32, 1) & 0xff;
            const int choices = rd8(rec + R_HUMAN_OK) + computer_ok(rec) + can_close;
            j_cell(j, 1, FIELD_TYPE, ns_tr("Игрок", "Player"), hs_type_name(s, type), choices > 1);
            j_icon(j, who_icon(type == 3, type != 2 && type != 3));
            j_lit(j, ",\"values\":[");
            int index = 0;
            for (int i = 0; i < (can_close ? 3 : 2); ++i) {
                if (HS_TYPES[i] == type)
                    index = i;
                if (i)
                    j_lit(j, ",");
                j_str(j, hs_type_name(s, HS_TYPES[i]));
            }
            j_printf(j, "],\"index\":%d}", index);
        } else {
        /* Who plays: the rotator's states 1..n-1. */
        const uint32_t rot = wait_rotator(screen, slot);
        const uint32_t sb = ns_ptr(rot) ? rd32(rot + ROT_STATES) : 0;
        const uint32_t se = ns_ptr(rot) ? rd32(rot + ROT_STATES + 4) : 0;
        const int states = ns_ptr(sb) && se >= sb && se - sb <= 4u * 8u ? (int)((se - sb) / 4u) : 0;
        const int at = ns_ptr(rot) ? (int)rd32(rot + ROT_INDEX) : 0;
        const int type_ok = slot != 0 && !locked && (ns_vcall(s, model, M_CAN_TYPE, &s32, 1) & 0xff) &&
                            computer_ok(rec);
        const int state = at >= 0 && at < states ? (int)rd32(sb + 4u * (uint32_t)at) : 3;
        j_cell(j, 1, FIELD_TYPE, ns_tr("Игрок", "Player"), type_name(s, state), type_ok && states > 2);
        j_icon(j, who_icon(state == 2, state != 3 && state != 2));
        if (states > 2 && at >= 1) {
            j_lit(j, ",\"values\":[");
            for (int i = 1; i < states; ++i) {
                if (i > 1)
                    j_lit(j, ",");
                j_str(j, type_name(s, (int)rd32(sb + 4u * (uint32_t)i)));
            }
            j_printf(j, "],\"index\":%d", at - 1);
        }
        j_lit(j, "}");
        }

        char number[8];
        snprintf(number, sizeof number, "%d", colour + 1);
        j_cell(j, 0, FIELD_COLOUR, ns_tr("Цвет", "Colour"), number,
               !locked && (ns_vcall(s, model, M_CAN_COLOUR, &s32, 1) & 0xff));
        j_icon(j, colour_icon(colour));
        j_lit(j, "}");

        /* The faction: random, then the allowed ones, in RotateRace's order. */
        const int race = (int)rd32(rec + R_RACE);
        const uint32_t rb = rd32(rec + R_RACES), re = rd32(rec + R_RACES + 4);
        const int races = ns_ptr(rb) && re >= rb && re - rb <= 4u * 8u ? (int)((re - rb) / 4u) : 0;
        j_cell(j, 0, FIELD_RACE, ns_tr("Фракция", "Faction"), race_name(s, race),
               !locked && (ns_vcall(s, model, M_CAN_RACE, &s32, 1) & 0xff));
        j_icon(j, race_icon(race));
        if (races > 0) {
            int index = 0;
            j_lit(j, ",\"values\":[");
            j_str(j, race_name(s, NS_RACE_RANDOM));
            for (int i = 0; i < races; ++i) {
                const int r = (int)rd32(rb + 4u * (uint32_t)i);
                if (r == race)
                    index = i + 1;
                j_lit(j, ",");
                j_str(j, race_name(s, r));
            }
            j_lit(j, "],\"icons\":[");
            j_str(j, race_icon(NS_RACE_RANDOM));
            for (int i = 0; i < races; ++i) {
                j_lit(j, ",");
                j_str(j, race_icon((int)rd32(rb + 4u * (uint32_t)i)));
            }
            j_printf(j, "],\"index\":%d", index);
        }
        j_lit(j, "}");

        const Hero *hero = hero_of(s, rec);
        j_cell(j, 0, FIELD_HERO, ns_tr("Герой", "Hero"), hero ? hero->name : ns_tr("Случайный", "Random"),
               !locked && !(g_hs && race == NS_RACE_RANDOM) && (ns_vcall(s, model, M_CAN_HERO, &s32, 1) & 0xff));
        j_icon(j, hero && hero->face[0] ? hero->face : "/UI/MPWait/PlayersList/Item/HeroRandom.(Texture).dds");
        if (hero && hero->spec[0]) {
            j_lit(j, ",");
            j_kv_str(j, "detail", hero->spec);
        }
        j_lit(j, "}");

        const int bonus = (int)rd32(rec + R_BONUS);
        const int artifact = artifact_ok(rec);
        j_cell(j, 0, FIELD_BONUS, ns_tr("Бонус", "Bonus"), bonus_name(s, bonus),
               !locked && (!M_CAN_BONUS || (ns_vcall(s, model, M_CAN_BONUS, &s32, 1) & 0xff)));
        j_icon(j, bonus_icon(bonus));
        j_lit(j, ",\"values\":[");
        int index = 0, k = 0;
        for (int v = 0; v < 4; ++v) {
            if (v == 1 && !artifact)
                continue;
            if (v == bonus)
                index = k;
            if (k++)
                j_lit(j, ",");
            j_str(j, bonus_name(s, v));
        }
        j_lit(j, "],\"icons\":[");
        for (int v = 0, first = 1; v < 4; ++v) {
            if (v == 1 && !artifact)
                continue;
            if (!first)
                j_lit(j, ",");
            first = 0;
            j_str(j, bonus_icon(v));
        }
        j_printf(j, "],\"index\":%d}]}", index);
    }

    const uint32_t info = g_hs ? logic + HL_INFO : screen + W_INFO;
    char name[128], dname[64], time[32];
    ns_text_of(s, NS_MAP_NAME, info + I_DESC, name, sizeof name);
    ns_difficulty_name(s, (int)rd32(g_hs ? logic + HL_DIFFICULTY : screen + W_DIFFICULTY), dname,
                       sizeof dname);
    const int minutes = (int)rd32(g_hs ? logic + HL_TIME : screen + W_TIME);
    if (minutes > 0)
        snprintf(time, sizeof time, ns_tr("%d мин.", "%d min"), minutes);
    else
        snprintf(time, sizeof time, "%s", ns_tr("без ограничения", "unlimited"));
    j_lit(j, "],\"info\":{");
    j_kv_str(j, "name", name);
    j_lit(j, ",");
    map_facts(j, s, info, 0);
    j_fact(j, 0, ns_label(s, "/UI/MPWait/MapDescription/TEXTS/difficulty_name.txt", "Сложность:", "Difficulty:"), dname);
    j_fact(j, 0, ns_label(s, "/UI/MPWait/MapDescription/TEXTS/time_limit_name.txt", "Время хода:", "Turn time:"), time);
    j_lit(j, "],");
    j_kv_str(j, "preview", map_preview(info));
    j_lit(j, "},\"buttons\":[");
    const int can_start = g_hs ? ns_ptr(logic) && (ns_vcall(s, logic, HL_CAN_START, NULL, 0) & 0xff)
                               : ns_ptr(ct) && (ns_vcall(s, ct, CT_CAN_START, NULL, 0) & 0xff);
    j_button(j, 1, "start", ns_label(s, g_hs ? "/UI/HotSeatWait/Menu/Start.txt" : "/UI/CustomGameWait/Start.txt", "Начать игру", "Start game"),
             can_start);
    j_button(j, 0, "back", ns_label(s, g_hs ? "/UI/HotSeatWait/Menu/back.txt" : "/UI/CustomGameWait/Back.txt", "Назад", "Back"), 1);
    j_lit(j, "]}");
}

/* ------------------------------------------- a LAN game's players -- */

#if defined(NS_VT_MPW)

/* SGameInfo, the game the matchmaker keeps (as the lobby lists them). */
#ifndef NS_MPW_INFO_SIZE
#define NS_MPW_INFO_SIZE 0x128
#endif
#ifndef NS_LAN_GI_NAME
#define NS_LAN_GI_NAME 0x1c
#endif
#ifndef NS_MPW_GI_TIME
#define NS_MPW_GI_TIME 0x38       /* minutes, < 1 unlimited */
#endif
#ifndef NS_MPW_GI_DIFFICULTY
#define NS_MPW_GI_DIFFICULTY 0x3c
#endif
#ifndef NS_LAN_GI_MAX
#define NS_LAN_GI_MAX 0x6c
#endif
#ifndef NS_LAN_GI_SIZE
#define NS_LAN_GI_SIZE 0x70
#endif
#ifndef NS_LAN_GI_MAP
#define NS_LAN_GI_MAP 0xcc
#endif
#ifndef NS_MPW_GI_GOAL
#define NS_MPW_GI_GOAL 0xec
#endif

/* A row's lock on a setting: 1 locked, 0 open, -1 not heard of yet. The
 * host sends them after every change (CMPWaitLockButtonsUICommand), for the
 * client's rows and its own: they are the "may it change" on either side. */
static int row_lock(uint32_t row, int field) {
    const uint32_t b = rd32(row + MR_LOCKS + 4), e = rd32(row + MR_LOCKS + 8);
    if (!ns_ptr(b) || e <= b || e - b > 4u * 256u)
        return -1;
    uint32_t node = rd32(b + 4u * ((uint32_t)field % ((e - b) / 4u)));
    for (int i = 0; i < 16 && ns_ptr(node); ++i, node = rd32(node))
        if (rd32(node + 4) == (uint32_t)field)
            return rd8(node + 8) != 0;
    return -1;
}

static int row_open(uint32_t row, int field) {
    return row_lock(row, field) != 1;
}

/* This device's slot: the host asks its model; a client's own row is the one
 * whose who-plays the host left open to it. */
static int mp_local_slot(Scratch *s, uint32_t screen, int n) {
    const uint32_t logic = rd32(screen + MW_LOGIC);
    if (rd8(screen + MW_HOST)) {
        const uint32_t model = model_of(screen);
        const int slot = ns_ptr(model) ? (int)ns_call(s, NS_MPW_SLOT_OF, model, rd32(logic + ML_HOST_ID), NULL, 0) : -1;
        if (slot >= 0 || !g_mp_load)
            return slot;
    }
    for (int slot = 0; slot < n; ++slot)
        if (row_lock(wait_row(screen, slot), FIELD_TYPE) == 0)
            return slot;
    return -1;
}

static const char *mp_state_name(Scratch *s, int state) {
    switch (state) {
    case 0: return ns_tr("Не готов", "Not ready");
    case 1: return who_name(s, 0, 1);
    case 2: return who_name(s, 1, 0);
    case 3: return ns_tr("Готов", "Ready");
    default: return ns_tr("Открыт", "Open");
    }
}

/* The empty slots' states in the order the host's right arrow steps them. */
static const int MP_FREE[3] = {4, 1, 2};

/* The game the lobby made, as the matchmaker keeps it (SGameInfo): its map,
 * name and rules for the summary. */
static void mp_info(Json *j, Scratch *s) {
    const uint32_t services = ns_call(s, NS_MPW_SERVICES, NS_MPW_MATCHMAKER_ID, 0, NULL, 0);
    const uint32_t mm = ns_ptr(services) ? ns_vcall(s, services, 0x24, NULL, 0) : 0;
    char name[128] = "", goal[96] = "", size[48] = "", players[32] = "", dname[64] = "", time[32] = "";
    if (ns_ptr(mm)) {
        const uint32_t info = ns_alloc(s, NS_MPW_INFO_SIZE);
        ns_call(s, NS_MPW_INFO_CTOR, info, 0, NULL, 0);
        if (ns_vcall(s, mm, NS_MPW_MM_GAME, &info, 1) & 0xff) {
            ns_text_of(s, NS_MAP_NAME, info + NS_LAN_GI_MAP, name, sizeof name);
#ifdef NS_MAP_GOAL_OUT /* ToE's formatter builds an out string, as map_goal says */
            {
                const uint32_t g = ns_alloc(s, 12);
                wr32(g, 0);
                ns_call(s, NS_MAP_GOAL, g, info + NS_MPW_GI_GOAL, NULL, 0);
                ns_wstr_utf8(g, goal, sizeof goal);
                ns_free(s, rd32(g));
            }
#else
            ns_text_of(s, NS_MAP_GOAL, info + NS_MPW_GI_GOAL, goal, sizeof goal);
#endif
            const uint32_t w = ns_alloc(s, 12);
            wr32(w, 0);
            ns_call(s, NS_MAP_SIZE, w, rd32(info + NS_LAN_GI_SIZE), NULL, 0);
            ns_wstr_utf8(w, size, sizeof size);
            ns_free(s, rd32(w));
#ifdef NS_MPW_GI_PLAYERS /* no "players in" function: the field the game's own list reads */
            const uint32_t now = rd32(info + NS_MPW_GI_PLAYERS);
#else
            const uint32_t now = ns_call(s, NS_MPW_PLAYERS_NOW, info, 0, NULL, 0);
#endif
            snprintf(players, sizeof players, "%u/%u", now, rd32(info + NS_LAN_GI_MAX));
            ns_difficulty_name(s, (int)rd32(info + NS_MPW_GI_DIFFICULTY), dname, sizeof dname);
            const int minutes = (int)rd32(info + NS_MPW_GI_TIME);
            if (minutes > 0)
                snprintf(time, sizeof time, ns_tr("%d мин.", "%d min"), minutes);
            else
                snprintf(time, sizeof time, "%s", ns_tr("без ограничения", "unlimited"));
            if (!name[0])
                ns_wstr_utf8(info + NS_LAN_GI_NAME, name, sizeof name);
        }
        ns_call(s, NS_MPW_INFO_DTOR, info, 0, NULL, 0);
    }
    j_lit(j, ",\"info\":{");
    j_kv_str(j, "name", name);
    j_lit(j, ",\"facts\":[");
    j_fact(j, 1, ns_label(s, "/UI/MPWait/MapDescription/TEXTS/players_name.txt", "Игроков:", "Players:"), players);
    j_fact(j, 0, ns_label(s, "/UI/MPWait/MapDescription/TEXTS/size_name.txt", "Размер:", "Size:"), size);
    j_fact(j, 0, ns_label(s, "/UI/MPWait/MapDescription/TEXTS/goal_name.txt", "Цель:", "Goal:"), goal);
    j_fact(j, 0, ns_label(s, "/UI/MPWait/MapDescription/TEXTS/difficulty_name.txt", "Сложность:", "Difficulty:"), dname);
    j_fact(j, 0, ns_label(s, "/UI/MPWait/MapDescription/TEXTS/time_limit_name.txt", "Время хода:", "Turn time:"), time);
    j_lit(j, "],\"preview\":\"\"}");
}

/* The status box the logic raises while it connects, gathers the game's
 * facts or starts it: a line over the screen instead. */
static const char *mp_busy(Scratch *s, uint32_t screen) {
    const uint32_t logic = rd32(screen + MW_LOGIC);
    const uint32_t status = ns_ptr(logic) ? rd32(logic + ML_STATUS) : 0;
    if (!ns_ptr(status) || !rd32(status + 0x18))
        return NULL;
    int starting = rd8(screen + MW_HOST) && rd32(screen + MW_STARTING);
    if (rd8(screen + MW_HOST) && !g_mp_load)
        starting |= !rd8(logic + ML_ACCEPTING);
#ifdef NS_MPL_STARTED
    if (rd8(screen + MW_HOST) && g_mp_load)
        starting |= rd8(logic + NS_MPL_STARTED) != 0; /* set once the game starts */
#endif
    if (starting)
        return ns_label(s, "/UI/Common/MULTIPLAYER_TEXTS/E_START_GAME_INIT.txt", "Запуск игры", "Starting the game");
    const int got_id = g_mp_load ? rd32(logic + ML_HOST_ID) != 0xffffffffu : rd8(logic + ML_GOT_ID) != 0;
    if (!rd8(screen + MW_HOST) && ns_ptr(rd32(logic + ML_SESSION)) && !got_id)
        return ns_label(s, "/UI/Common/MULTIPLAYER_TEXTS/E_GAMEINFO_INIT.txt", "Идет сбор информации об игре...",
                        "Getting the game's information...");
    return ns_label(s, "/UI/Common/MULTIPLAYER_TEXTS/E_COMMON_INIT.txt", "Идет установка...", "Connecting...");
}

static void mp_wait_document(Json *j, Scratch *s, uint32_t screen) {
    const int host = rd8(screen + MW_HOST);
    const uint32_t logic = rd32(screen + MW_LOGIC);
    const uint32_t model = model_of(screen);
    const uint32_t b = rd32(rows_of(screen)), e = rd32(rows_of(screen) + 4);
    const int n = ns_ptr(b) && e >= b && e - b <= 4u * 8u ? (int)((e - b) / 4u) : 0;
    const int local = mp_local_slot(s, screen, n);
    char diff[64];
    ns_difficulty_name(s, 0, diff, sizeof diff);
    ns_ru = (unsigned char)diff[0] >= 0xd0;

    j_lit(j, "{\"screen\":\"mp_players\",");
    j_kv_str(j, "lang", ns_ru ? "ru" : "en");
    j_lit(j, ",");
    j_kv_str(j, "title", ns_label(s, "/UI/MPWait/ScreenHeader.txt", "Обычные настройки", "Game Setup"));
    j_lit(j, ",\"players\":[");
    for (int slot = 0; slot < n; ++slot) {
        const uint32_t row = wait_row(screen, slot);
        const uint32_t rec = ns_ptr(model) ? wait_record(screen, slot) : 0;
        const int state = (int)rd32(row + MR_STATE);
        const int colour = (int)(rd32(row + MR_COLOUR) - 1u) & 7;
        char label[64], path[96], who[64];
        ns_wstr_utf8(row + MR_NAME, who, sizeof who);
        if (who[0] && (state == 0 || state == 3)) {
            snprintf(label, sizeof label, "%s", who);
        } else {
            snprintf(label, sizeof label, "%s %s", ns_ru ? RU_COLOUR[colour] : EN_COLOUR[colour],
                     ns_tr("игрок", "player"));
            snprintf(path, sizeof path, ITEM "player_%s.txt", COLOUR_KEY[colour]);
            snprintf(label, sizeof label, "%s", text_or(s, path, label));
        }
        if (slot == local)
            snprintf(label + strlen(label), sizeof label - strlen(label), " %s", ns_tr("(вы)", "(you)"));
        j_printf(j, "%s{\"slot\":%d,", slot ? "," : "", slot);
        j_kv_str(j, "label", label);
        j_printf(j, ",\"colour\":%d,\"cells\":[", colour);

        /* Who plays: one's own row is the ready switch; the host opens,
         * closes and gives the free ones to the computer, or asks to send a
         * person away; a client sees the others as they are. */
        const int human = state == 0 || state == 3;
        const int open = row_open(row, FIELD_TYPE);
        j_cell(j, 1, FIELD_TYPE, ns_tr("Игрок", "Player"), mp_state_name(s, state), open);
        j_icon(j, who_icon(state == 2, state == 1));
        if (slot == local) {
            j_lit(j, ",\"values\":[");
            j_str(j, mp_state_name(s, 0));
            j_lit(j, ",");
            j_str(j, mp_state_name(s, 3));
            j_printf(j, "],\"index\":%d", state == 3);
        } else if (host && human) {
            j_lit(j, ",\"values\":[");
            j_str(j, mp_state_name(s, state));
            j_lit(j, ",");
            j_str(j, ns_tr("Удалить из игры", "Remove from the game"));
            j_lit(j, "],\"index\":0");
        } else if (host) {
            j_lit(j, ",\"values\":[");
            int index = 0;
            for (int i = 0; i < 3; ++i) {
                if (MP_FREE[i] == state)
                    index = i;
                if (i)
                    j_lit(j, ",");
                j_str(j, mp_state_name(s, MP_FREE[i]));
            }
            j_printf(j, "],\"index\":%d", index);
        }
        j_lit(j, "}");

        char number[8];
        snprintf(number, sizeof number, "%d", colour + 1);
        j_cell(j, 0, FIELD_COLOUR, ns_tr("Цвет", "Colour"), number, state != 2 && state != 4 && row_open(row, FIELD_COLOUR));
        j_icon(j, colour_icon(colour));
        j_lit(j, "}");

        /* The faction: the host knows the allowed ones (a list); a client
         * steps as the game's arrow does. */
        const int race = (int)rd32(row + MR_RACE);
        j_cell(j, 0, FIELD_RACE, ns_tr("Фракция", "Faction"), race_name(s, race),
               state != 2 && state != 4 && row_open(row, FIELD_RACE));
        j_icon(j, race_icon(race));
        const uint32_t rb = ns_ptr(rec) ? rd32(rec + R_RACES) : 0, re = ns_ptr(rec) ? rd32(rec + R_RACES + 4) : 0;
        const int races = ns_ptr(rb) && re >= rb && re - rb <= 4u * 8u ? (int)((re - rb) / 4u) : 0;
        if (races > 0) {
            int index = 0;
            j_lit(j, ",\"values\":[");
            j_str(j, race_name(s, NS_RACE_RANDOM));
            for (int i = 0; i < races; ++i) {
                const int r = (int)rd32(rb + 4u * (uint32_t)i);
                if (r == race)
                    index = i + 1;
                j_lit(j, ",");
                j_str(j, race_name(s, r));
            }
            j_lit(j, "],\"icons\":[");
            j_str(j, race_icon(NS_RACE_RANDOM));
            for (int i = 0; i < races; ++i) {
                j_lit(j, ",");
                j_str(j, race_icon((int)rd32(rb + 4u * (uint32_t)i)));
            }
            j_printf(j, "],\"index\":%d", index);
        }
        j_lit(j, "}");

        static Hero heroes[16];
        static uint32_t hero_keys[16];
        const uint32_t shared = rd32(row + MR_HERO);
        const Hero *hero = NULL;
        if (ns_ptr(shared)) {
            int i = 0;
            while (i < 16 && hero_keys[i] && hero_keys[i] != shared)
                ++i;
            if (i == 16)
                i = (int)(shared >> 4) & 15;
            if (hero_keys[i] != shared) {
                memset(&heroes[i], 0, sizeof heroes[i]);
                hero_keys[i] = shared;
                hero_facts(s, shared, &heroes[i]);
            }
            hero = &heroes[i];
        }
        j_cell(j, 0, FIELD_HERO, ns_tr("Герой", "Hero"), hero && hero->name[0] ? hero->name : ns_tr("Случайный", "Random"),
               state != 2 && state != 4 && race != NS_RACE_RANDOM && row_open(row, FIELD_HERO));
        j_icon(j, hero && hero->face[0] ? hero->face : "/UI/MPWait/PlayersList/Item/HeroRandom.(Texture).dds");
        if (hero && hero->spec[0]) {
            j_lit(j, ",");
            j_kv_str(j, "detail", hero->spec);
        }
        j_lit(j, "}");

        const int bonus = (int)rd32(row + MR_BONUS);
        j_cell(j, 0, FIELD_BONUS, ns_tr("Бонус", "Bonus"), bonus_name(s, bonus),
               state != 2 && state != 4 && row_open(row, FIELD_BONUS));
        j_icon(j, bonus_icon(bonus));
        if (ns_ptr(rec)) {
            const int artifact = artifact_ok(rec);
            j_lit(j, ",\"values\":[");
            int index = 0, k = 0;
            for (int v = 0; v < 4; ++v) {
                if (v == 1 && !artifact)
                    continue;
                if (v == bonus)
                    index = k;
                if (k++)
                    j_lit(j, ",");
                j_str(j, bonus_name(s, v));
            }
            j_lit(j, "],\"icons\":[");
            for (int v = 0, first = 1; v < 4; ++v) {
                if (v == 1 && !artifact)
                    continue;
                if (!first)
                    j_lit(j, ",");
                first = 0;
                j_str(j, bonus_icon(v));
            }
            j_printf(j, "],\"index\":%d", index);
        }
        j_lit(j, "}]}");
        if (ns_trace() && slot < 8) {
            /* A row's state and locks, when they change. */
            static int said[8];
            int now = state + 1;
            for (int f = 1; f <= 5; ++f)
                now = now * 3 + row_lock(row, f) + 1;
            if (said[slot] != now) {
                said[slot] = now;
                fprintf(stderr, "[screens] mp row %d: state %d locks", slot, state);
                for (int f = 1; f <= 5; ++f)
                    fprintf(stderr, " %d", row_lock(row, f));
                fprintf(stderr, "\n");
            }
        }
    }
    j_lit(j, "]");
    mp_info(j, s);
    ns_lan_chat(j, rd32(screen + MW_CHAT), 1);
    const char *busy = mp_busy(s, screen);
    if (busy) {
        j_lit(j, ",");
        j_kv_str(j, "busy", busy);
    }
    j_lit(j, ",\"buttons\":[");
    j_button(j, 1, "back", ns_label(s, "/UI/MPWait/Cancel.txt", "Отмена", "Cancel"), 1);
    if (host)
        j_button(j, 0, "start", ns_label(s, "/UI/CustomGameWait/Start.txt", "Начать игру", "Start game"),
                 ns_ptr(logic) && (ns_vcall(s, logic, ML_CAN_START, NULL, 0) & 0xff));
    j_lit(j, "]}");
}

static void mp_wait_command(Scratch *s, uint32_t screen, const char *cmd) {
    char verb[16], name[32];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    const int slot = ns_cmd_int(cmd, "slot", -1), field = ns_cmd_int(cmd, "field", 0);
    if (strcmp(verb, "set") == 0 && field == FIELD_TYPE) {
        /* Who plays: one step for the ready switch and the "send away" box,
         * stepped toward the chosen state for a free slot. */
        const int index = ns_cmd_int(cmd, "index", -1);
        const uint32_t row = wait_row(screen, slot);
        if (!ns_ptr(row) || index < 0)
            return;
        const int b = rd32(rows_of(screen)), e = rd32(rows_of(screen) + 4);
        const int state = (int)rd32(row + MR_STATE);
        if (slot == mp_local_slot(s, screen, (int)((uint32_t)(e - b) / 4u))) {
            if ((index == 1) != (state == 3))
                request(screen, slot, FIELD_TYPE, 1);
        } else if (state == 0 || state == 3) {
            if (index == 1)
                request(screen, slot, FIELD_TYPE, 1);
        } else if (index < 3) {
            for (int i = 0; i < WANTS; ++i)
                if (!g_wants[i].screen || (g_wants[i].slot == slot && g_wants[i].field == field)) {
                    Want w = {screen, slot, field, MP_FREE[index], 4};
                    g_wants[i] = w;
                    break;
                }
        }
    } else if (strcmp(verb, "chat") == 0) {
        char text[256];
        ns_cmd_str(cmd, "text", text, sizeof text);
        if (text[0]) {
            ns_lan_set_edit(s, rd32(screen + MW_EDIT), text);
            hs_handler(s, screen, NS_MPW_SEND);
        }
    } else if (strcmp(verb, "press") == 0) {
        ns_cmd_str(cmd, "name", name, sizeof name);
        if (strcmp(name, "start") == 0)
            ns_leave();
        hs_handler(s, screen, strcmp(name, "start") == 0 ? NS_MPW_START : strcmp(name, "back") == 0 ? NS_MPW_BACK : 0);
    } else {
        wait_command(s, screen, cmd);
    }
}

#endif

/* The table's entry points: each sets the variant, then runs the shared code. */
#define VARIANT(name, hs, fn, ...) \
    static void name(__VA_ARGS__) { g_hs = hs; fn; }
VARIANT(custom_create_command, 0, create_command(s, screen, cmd), Scratch *s, uint32_t screen, const char *cmd)
VARIANT(custom_create_document, 0, create_document(j, s, screen), Json *j, Scratch *s, uint32_t screen)
VARIANT(custom_wait_command, 0, wait_command(s, screen, cmd), Scratch *s, uint32_t screen, const char *cmd)
VARIANT(custom_wait_step, 0, step_wants(s, screen), Scratch *s, uint32_t screen)
VARIANT(custom_wait_document, 0, wait_document(j, s, screen), Json *j, Scratch *s, uint32_t screen)
VARIANT(hs_create_command, 1, create_command(s, screen, cmd), Scratch *s, uint32_t screen, const char *cmd)
VARIANT(hs_create_document, 1, create_document(j, s, screen), Json *j, Scratch *s, uint32_t screen)
VARIANT(hs_wait_command, 1, wait_command(s, screen, cmd), Scratch *s, uint32_t screen, const char *cmd)
VARIANT(hs_wait_step, 1, step_wants(s, screen), Scratch *s, uint32_t screen)
VARIANT(hs_wait_document, 1, wait_document(j, s, screen), Json *j, Scratch *s, uint32_t screen)
VARIANT(mp_create_command, 2, create_command(s, screen, cmd), Scratch *s, uint32_t screen, const char *cmd)
VARIANT(mp_create_document, 2, create_document(j, s, screen), Json *j, Scratch *s, uint32_t screen)
#if defined(NS_VT_MPW)
VARIANT(mp_wait_cmd, 3, (g_mp_load = 0, mp_wait_command(s, screen, cmd)), Scratch *s, uint32_t screen, const char *cmd)
VARIANT(mp_wait_step, 3, (g_mp_load = 0, step_wants(s, screen)), Scratch *s, uint32_t screen)
VARIANT(mp_wait_doc, 3, (g_mp_load = 0, mp_wait_document(j, s, screen)), Json *j, Scratch *s, uint32_t screen)
#endif
#if defined(NS_VT_MPW) && defined(NS_VT_MPL)
VARIANT(mp_load_cmd, 3, (g_mp_load = 1, mp_wait_command(s, screen, cmd)), Scratch *s, uint32_t screen, const char *cmd)
VARIANT(mp_load_step, 3, (g_mp_load = 1, step_wants(s, screen)), Scratch *s, uint32_t screen)
VARIANT(mp_load_doc, 3, (g_mp_load = 1, mp_wait_document(j, s, screen)), Json *j, Scratch *s, uint32_t screen)
#endif

const NsScreen ns_custom_create = {.id = "custom_create", .vtable = NS_VT_CREATE, .sub = NS_SUB_CREATE,
                                   .command = custom_create_command,
                                   .document = custom_create_document};
const NsScreen ns_custom_wait = {.id = "custom_players", .vtable = NS_VT_WAIT, .sub = NS_SUB_WAIT,
                                 .command = custom_wait_command, .step = custom_wait_step,
                                 .document = custom_wait_document};
const NsScreen ns_hotseat_create = {.id = "hotseat_create", .vtable = NS_VT_HS_CREATE,
                                    .sub = NS_SUB_HS_CREATE, .command = hs_create_command,
                                    .document = hs_create_document};
const NsScreen ns_hotseat_wait = {.id = "hotseat_players", .vtable = NS_VT_HS_WAIT,
                                  .sub = NS_SUB_HS_WAIT, .command = hs_wait_command,
                                  .step = hs_wait_step, .document = hs_wait_document};
#if defined(NS_VT_MPC)
const NsScreen ns_mp_create = {.id = "mp_create", .vtable = NS_VT_MPC, .sub = NS_SUB_MPC,
                               .command = mp_create_command, .document = mp_create_document};
#else
const NsScreen ns_mp_create = {.id = "mp_create"};
#endif
#if defined(NS_VT_MPW)
const NsScreen ns_mp_wait = {.id = "mp_players", .vtable = NS_VT_MPW, .sub = NS_SUB_MPW,
                             .command = mp_wait_cmd, .step = mp_wait_step, .document = mp_wait_doc};
#else
const NsScreen ns_mp_wait = {.id = "mp_players"};
#endif
#if defined(NS_VT_MPW) && defined(NS_VT_MPL)
const NsScreen ns_mp_load_wait = {.id = "mp_load_players", .vtable = NS_VT_MPL, .sub = NS_SUB_MPL,
                                  .command = mp_load_cmd, .step = mp_load_step, .document = mp_load_doc};
#else
const NsScreen ns_mp_load_wait = {.id = "mp_load_players"};
#endif

#else

/* No addresses for this game: never matched (a zero vtable). */
const NsScreen ns_custom_create = {.id = "custom_create"};
const NsScreen ns_custom_wait = {.id = "custom_players"};
const NsScreen ns_hotseat_create = {.id = "hotseat_create"};
const NsScreen ns_hotseat_wait = {.id = "hotseat_players"};
const NsScreen ns_mp_create = {.id = "mp_create"};
const NsScreen ns_mp_wait = {.id = "mp_players"};
const NsScreen ns_mp_load_wait = {.id = "mp_load_players"};

#endif
