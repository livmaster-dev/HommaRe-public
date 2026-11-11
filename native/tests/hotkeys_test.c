/* native/tests/hotkeys_test.c - unit tests for native/hotkeys_cfg.c, the touch
 * panel's bindings, catalog and document, on synthetic bindings files.
 *
 * Build and run (from the worktree root):
 *   cc -o /tmp/hotkeys_test native/tests/hotkeys_test.c native/hotkeys_cfg.c \
 *      && /tmp/hotkeys_test
 */
#include <stdio.h>
#include <string.h>

#include "../hotkeys_cfg.h"

static int g_failures;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

/* The base game's file, cut down: the common section, comments (one in
 * CP1251), mouse binds, a modifier combination, an action bound twice. */
static const char BASE[] = "//====\r\n"
                           "// Common section (always active)\r\n"
                           "bind +cursor_x\t\t'MOUSE_AXIS_X'\r\n"
                           "bind -camera_pitch\t'CTRL' + 'UP'\r\n"
                           "bind shift 'SHIFT'\r\n"
                           "bind ctrl 'CTRL'\r\n"
                           "bindconfigure\t\tMOUSE_AXIS_X 1.2\r\n"
                           "// \xcf\xee\xe2\xe5\xf0\xf5\xed\xee\xf1\xf2\xfc\r\n"
                           "bindsection adventure_screen\r\n"
                           "bind quicksave 'F5'\r\n"
                           "bind input_select_town_01 'CTRL' + '1'\r\n"
                           "bind flip_floor 'U'\r\n"
                           "bind flip_floor 'T'\r\n"
                           "bind show_heroes_mana 'M' // the mana bars\r\n"
                           "bind sleep_wake_hero 'Z'\r\n"
                           "bind adventure_teleport 'CTRL' + 'SHIFT' + 'T'\r\n"
                           "bind +camera_rotate_mouse 'MOUSE_BUTTON1' + 'MOUSE_AXIS_X'\r\n"
                           "bindsection combat_screen\r\n"
                           "bind input_Combat_Wait 'W'\r\n"
                           "bind input_switch_atb_log 'TAB'\r\n"
                           "bindsection combat_screen_tactic\r\n"
                           "bind input_autoplace_army 'A'\r\n"
                           "bind input_start_combat 'ENTER'\r\n"
                           "bindsection town_screen\r\n"
                           "bind quicksave 'F5'\r\n";

/* Hammers of Fate's map: no mana, no sleep, M moves the hero. */
static const char HOF[] = "bindsection adventure_screen\n"
                          "bind MB_Move_Hero 'M'\n"
                          "bind quicksave 'F5'\n";

static int parse(const char *text, HkBind *binds) {
    return hk_parse(text, strlen(text), binds, 64);
}

static void test_key_names(void) {
    CHECK(hk_key_code("M") == 16);
    CHECK(hk_key_code("A") == 4);
    CHECK(hk_key_code("1") == 30);
    CHECK(hk_key_code("0") == 39);
    CHECK(hk_key_code("F5") == 62);
    CHECK(hk_key_code("TAB") == 43);
    CHECK(hk_key_code("HOME") == 74);
    CHECK(hk_key_code("NUM_PLUS") == 87);
    CHECK(hk_key_code("CTRL") == HK_KEY_LCTRL);
    CHECK(hk_key_code("SHIFT") == HK_KEY_LSHIFT);
    CHECK(hk_key_code("RALT") == HK_KEY_RALT);
    CHECK(hk_key_code("MOUSE_BUTTON0") == 0);
    CHECK(hk_key_code("NOSUCHKEY") == 0);
}

static void test_parse_sections_keys_and_mice(void) {
    HkBind b[64];
    const int n = parse(BASE, b);
    /* Mouse binds are dropped; everything else stays, in order. */
    CHECK(n == 15);
    const char *common[] = {"adventure_screen"};
    const HkBind *pitch = hk_find(b, n, "-camera_pitch", common, 0);
    CHECK(pitch && !strcmp(pitch->section, "") && pitch->key == 82 &&
          pitch->mods[0] == HK_KEY_LCTRL && pitch->mods[1] == 0 && !strcmp(pitch->text, "Ctrl+UP"));
    const HkBind *town = hk_find(b, n, "input_select_town_01", common, 1);
    CHECK(town && town->key == 30 && town->mods[0] == HK_KEY_LCTRL &&
          !strcmp(town->text, "Ctrl+1"));
    const HkBind *tele = hk_find(b, n, "adventure_teleport", common, 1);
    CHECK(tele && tele->key == 23 && tele->mods[0] == HK_KEY_LCTRL &&
          tele->mods[1] == HK_KEY_LSHIFT && tele->mods[2] == 0);
    /* A modifier alone is the key. */
    const HkBind *shift = hk_find(b, n, "shift", common, 1);
    CHECK(shift && shift->key == HK_KEY_LSHIFT && shift->mods[0] == 0 &&
          !strcmp(shift->text, "Shift"));
    /* A comment after the keys is no key. */
    const HkBind *mana = hk_find(b, n, "show_heroes_mana", common, 1);
    CHECK(mana && mana->key == 16 && !strcmp(mana->text, "M"));
    CHECK(!hk_find(b, n, "+camera_rotate_mouse", common, 1));
}

static void test_find_takes_the_first_bind_of_the_active_sections(void) {
    HkBind b[64];
    const int n = parse(BASE, b);
    const char *map[] = {"adventure_screen"};
    const HkBind *flip = hk_find(b, n, "flip_floor", map, 1);
    CHECK(flip && flip->key == hk_key_code("U")); /* U, not T, which is also "the hero's town" */
    const char *battle[] = {"combat_screen"};
    CHECK(!hk_find(b, n, "flip_floor", battle, 1));
    CHECK(hk_find(b, n, "ctrl", battle, 1) != NULL); /* the common section is always active */
}

static void test_document_offers_the_screens_commands(void) {
    HkBind b[64];
    const int n = parse(BASE, b);
    char doc[4096];
    const char *map[] = {"adventure_screen"};
    int len = hk_document(b, n, map, 1, HK_RU, 0, doc, sizeof doc);
    CHECK(len > 0 && len < (int)sizeof doc && (int)strlen(doc) == len);
    CHECK(strstr(doc, "\"screen\":\"adventure_screen\""));
    CHECK(strstr(doc, "\"lang\":\"ru\""));
    CHECK(strstr(doc, "{\"id\":\"mana\",\"label\":\"Мана "
                      "героев\",\"short\":\"Мана\",\"keys\":\"M\",\"kind\":\"tap\","
                      "\"quick\":2,\"symbol\":\"drop.fill\",\"key\":16,\"mods\":[]}"));
    CHECK(strstr(doc, "\"id\":\"underground\""));
    CHECK(strstr(doc, "\"id\":\"sleep\""));
    /* Not bound in this file: no button. */
    CHECK(!strstr(doc, "\"id\":\"dig\""));
    CHECK(!strstr(doc, "\"id\":\"kingdom\""));
    /* Another screen's. */
    CHECK(!strstr(doc, "\"id\":\"wait\""));
    /* The order is the catalog's: mana before quick save before sleep. */
    CHECK(strstr(doc, "\"mana\"") < strstr(doc, "\"quicksave\"") &&
          strstr(doc, "\"quicksave\"") < strstr(doc, "\"sleep\""));
    /* The languages after Polish: each its own words and code. */
    len = hk_document(b, n, map, 1, HK_ZH, 0, doc, sizeof doc);
    CHECK(len > 0 && strstr(doc, "\"lang\":\"zh-Hans\"") &&
          strstr(doc, "\"label\":\"英雄魔法值\",\"short\":\"魔法\""));
    len = hk_document(b, n, map, 1, HK_IT, 0, doc, sizeof doc);
    CHECK(len > 0 && strstr(doc, "\"lang\":\"it\"") &&
          strstr(doc, "\"label\":\"Mana degli eroi\""));

    const char *battle[] = {"combat_screen"};
    len = hk_document(b, n, battle, 1, HK_EN, 0, doc, sizeof doc);
    CHECK(len > 0);
    CHECK(strstr(doc, "\"id\":\"wait\",\"label\":\"Wait\",\"short\":\"Wait\",\"keys\":\"W\""));
    CHECK(strstr(doc, "\"id\":\"initiative\""));
    /* No key of its own: the host turns the next tap into a right click. */
    CHECK(strstr(doc, "{\"id\":\"creature-info\",\"label\":\"Creature info\",\"short\":\"Info\","
                      "\"keys\":\"\","
                      "\"kind\":\"next_tap_right\",\"quick\":1,\"symbol\":\"info.circle\"}"));
    /* Ctrl from the common section, held. */
    CHECK(strstr(doc, "\"id\":\"ctrl-melee\"") && strstr(doc, "\"kind\":\"latch\"") &&
          strstr(doc, "\"key\":224,\"mods\":[]"));

    const char *tactic[] = {"combat_screen_tactic"};
    len = hk_document(b, n, tactic, 1, HK_EN, 0, doc, sizeof doc);
    CHECK(len > 0 && strstr(doc, "\"autoplace\"") && strstr(doc, "\"start-combat\"") &&
          strstr(doc, "\"creature-info\"") && !strstr(doc, "\"wait\""));
}

/* The skill wheel: no key, a native view of the game's, on the map, not in a
 * battle; a hero's screen has a button of its own and no panel at all. */
static void test_document_offers_the_skill_wheel(void) {
    HkBind b[64];
    const int n = parse(BASE, b);
    char doc[4096];
    const char *map[] = {"adventure_screen"};
    CHECK(hk_document(b, n, map, 1, HK_RU, 0, doc, sizeof doc) > 0);
    CHECK(strstr(doc, "{\"id\":\"skill-wheel\",\"label\":\"Колесо навыков\",\"short\":\"Навыки\","
                      "\"keys\":\"\",\"kind\":\"screen\",\"quick\":2,\"symbol\":\"chart.pie\"}"));
    const char *hero[] = {"hero_screen"};
    CHECK(hk_document(b, n, hero, 1, HK_EN, 0, doc, sizeof doc) == 0);
    const char *battle[] = {"combat_screen"};
    CHECK(hk_document(b, n, battle, 1, HK_EN, 0, doc, sizeof doc) > 0 &&
          !strstr(doc, "\"skill-wheel\""));
}

static void test_document_drops_what_the_expansion_does_not_bind(void) {
    HkBind b[64];
    const int n = parse(HOF, b);
    char doc[4096];
    const char *map[] = {"adventure_screen"};
    const int len = hk_document(b, n, map, 1, HK_EN, 0, doc, sizeof doc);
    CHECK(len > 0);
    CHECK(!strstr(doc, "\"mana\"")); /* M is the hero's move here */
    CHECK(!strstr(doc, "\"sleep\""));
    CHECK(strstr(doc, "\"quicksave\""));
}

static void test_document_none_and_too_small(void) {
    HkBind b[64];
    const int n = parse(BASE, b);
    char doc[4096];
    const char *menu[] = {"esc_menu"};
    CHECK(hk_document(b, n, menu, 1, HK_EN, 0, doc, sizeof doc) == 0);
    CHECK(hk_document(b, n, menu, 0, HK_EN, 0, doc, sizeof doc) == 0);
    const char *map[] = {"adventure_screen"};
    const int full = hk_document(b, n, map, 1, HK_EN, 0, doc, sizeof doc);
    char small[32];
    const int need = hk_document(b, n, map, 1, HK_EN, 0, small, sizeof small);
    CHECK(need == full + 1 && need > (int)sizeof small);
}

static void test_two_sections_join(void) {
    HkBind b[64];
    const int n = parse(BASE, b);
    char doc[4096];
    const char *both[] = {"town_screen", "adventure_screen"};
    CHECK(hk_document(b, n, both, 2, HK_EN, 0, doc, sizeof doc) > 0);
    CHECK(strstr(doc, "\"screen\":\"town_screen adventure_screen\""));
    /* One quick save, though both sections offer it. */
    const char *first = strstr(doc, "\"quicksave\"");
    CHECK(first && !strstr(first + 1, "\"quicksave\""));
}

/* A command whose own button the game shows is left out: the battle's wait
 * where the jog dial has one, kept where it has none (Hammers of Fate). */
static void test_document_leaves_out_what_the_game_shows(void) {
    HkBind b[64];
    const int n = parse(BASE, b);
    char doc[4096];
    const char *battle[] = {"combat_screen"};
    int wait = -1;
    for (int i = 0; i < hk_command_count(); ++i)
        if (hk_command_buttons(i) && strstr(hk_command_buttons(i), "OnJog-Wait"))
            wait = i;
    CHECK(wait >= 0);
    CHECK(hk_document(b, n, battle, 1, HK_EN, 0, doc, sizeof doc) > 0 && strstr(doc, "\"wait\""));
    CHECK(hk_document(b, n, battle, 1, HK_EN, 1u << wait, doc, sizeof doc) > 0 &&
          !strstr(doc, "\"wait\"") && strstr(doc, "\"initiative\""));
    /* Commands with no button of the game's never name one. */
    for (int i = 0; i < hk_command_count(); ++i)
        CHECK(!hk_command_buttons(i) || hk_command_buttons(i)[0]);
}

int main(void) {
    test_document_leaves_out_what_the_game_shows();
    test_key_names();
    test_parse_sections_keys_and_mice();
    test_find_takes_the_first_bind_of_the_active_sections();
    test_document_offers_the_screens_commands();
    test_document_offers_the_skill_wheel();
    test_document_drops_what_the_expansion_does_not_bind();
    test_document_none_and_too_small();
    test_two_sections_join();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("hotkeys_test: all passed\n");
    return 0;
}
