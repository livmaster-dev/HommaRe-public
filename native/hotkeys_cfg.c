/* hotkeys_cfg.c - the touch panel's commands: bindings, catalog, document
 * (native/hotkeys_cfg.h).
 *
 * The bindings file is the player's profile's input.cfg (input_a1.cfg,
 * input_a2.cfg in the expansions), which the game reads its keys from: a
 * player may edit it, and the expansions bind differently (Hammers of Fate's
 * M moves the hero, its K opens the skills). So the panel names an action,
 * not a key, and presses whatever the file binds that action to; an action
 * the file does not bind has no button. Research notes: hotkey-drawer-plan.md.
 */
#include "hotkeys_cfg.h"

#include <stdio.h>
#include <string.h>

/* The game's key names (the table at 0xb98118 in the base game, in
 * DirectInput order) that the bindings files use, with their HID usages. */
typedef struct {
    const char *name;
    uint8_t code;
} KeyName;

static const KeyName KEYS[] = {
    {"ESC", 41},      {"BACKSPACE", 42}, {"TAB", 43},   {"ENTER", 40},        {"SPACE", 44},
    {"LCTRL", 224},   {"RCTRL", 228},    {"CTRL", 224}, {"LSHIFT", 225},      {"RSHIFT", 229},
    {"SHIFT", 225},   {"LALT", 226},     {"RALT", 230}, {"ALT", 226},         {"F1", 58},
    {"F2", 59},       {"F3", 60},        {"F4", 61},    {"F5", 62},           {"F6", 63},
    {"F7", 64},       {"F8", 65},        {"F9", 66},    {"F10", 67},          {"F11", 68},
    {"F12", 69},      {"SYSRQ", 70},     {"PAUSE", 72}, {"INSERT", 73},       {"HOME", 74},
    {"PG_UP", 75},    {"DELETE", 76},    {"END", 77},   {"PG_DOWN", 78},      {"RIGHT", 79},
    {"LEFT", 80},     {"DOWN", 81},      {"UP", 82},    {"NUM_MULTIPLY", 85}, {"NUM_MINUS", 86},
    {"NUM_PLUS", 87}, {"NUM_ENTER", 88}, {"NUM_1", 89}, {"NUM_2", 90},        {"NUM_3", 91},
    {"NUM_4", 92},    {"NUM_5", 93},     {"NUM_6", 94}, {"NUM_7", 95},        {"NUM_8", 96},
    {"NUM_9", 97},    {"NUM_0", 98},
};

int hk_key_code(const char *name) {
    if (name[0] && !name[1]) {
        const char c = name[0];
        if (c >= 'A' && c <= 'Z')
            return 4 + (c - 'A');
        if (c >= '1' && c <= '9')
            return 30 + (c - '1');
        if (c == '0')
            return 39;
        return 0;
    }
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; ++i)
        if (!strcmp(KEYS[i].name, name))
            return KEYS[i].code;
    return 0;
}

static int is_modifier(int code) {
    return code >= HK_KEY_LCTRL && code <= HK_KEY_RALT && code != 227;
}

/* How a player reads a key name: "Ctrl", "Shift", "Alt", "Num +"; the rest as
 * the file spells it (M, F5, TAB, HOME). */
static const char *display_name(const char *name) {
    static const struct {
        const char *name, *shown;
    } SHOWN[] = {
        {"CTRL", "Ctrl"},
        {"LCTRL", "Ctrl"},
        {"RCTRL", "Ctrl"},
        {"SHIFT", "Shift"},
        {"LSHIFT", "Shift"},
        {"RSHIFT", "Shift"},
        {"ALT", "Alt"},
        {"LALT", "Alt"},
        {"RALT", "Alt"},
        {"ESC", "Esc"},
        {"TAB", "Tab"},
        {"ENTER", "Enter"},
        {"SPACE", "Space"},
        {"BACKSPACE", "Backspace"},
        {"HOME", "Home"},
        {"END", "End"},
        {"INSERT", "Ins"},
        {"DELETE", "Del"},
        {"PG_UP", "PgUp"},
        {"PG_DOWN", "PgDn"},
        {"NUM_PLUS", "Num +"},
        {"NUM_MINUS", "Num -"},
        {"NUM_ENTER", "Num Enter"},
        {"NUM_MULTIPLY", "Num *"},
    };
    for (size_t i = 0; i < sizeof SHOWN / sizeof SHOWN[0]; ++i)
        if (!strcmp(SHOWN[i].name, name))
            return SHOWN[i].shown;
    return name;
}

/* Copies a token of at most cap-1 bytes. */
static void copy_token(char *dst, size_t cap, const char *src, size_t len) {
    if (len >= cap)
        len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

/* One bind line's keys ("'CTRL' + '1'"), modifiers first; 0 when one of them
 * is no key (a mouse button or axis) or there are too many. */
static int parse_keys(const char *p, const char *end, HkBind *b) {
    int codes[4], n = 0;
    char names[4][16];
    while (p < end) {
        const char *q = memchr(p, '\'', (size_t)(end - p));
        if (!q)
            break;
        const char *close = memchr(q + 1, '\'', (size_t)(end - q - 1));
        if (!close || n == 4)
            return 0;
        copy_token(names[n], sizeof names[n], q + 1, (size_t)(close - q - 1));
        codes[n] = hk_key_code(names[n]);
        if (!codes[n])
            return 0;
        ++n;
        p = close + 1;
    }
    if (!n)
        return 0;
    /* The key is the one that is no modifier (a modifier alone when all are). */
    int key = n - 1;
    for (int i = 0; i < n; ++i)
        if (!is_modifier(codes[i]))
            key = i;
    int m = 0;
    b->text[0] = 0;
    for (int i = 0; i < n; ++i) {
        if (i == key)
            continue;
        if (m == 3)
            return 0;
        b->mods[m++] = (uint8_t)codes[i];
        strncat(b->text, display_name(names[i]), sizeof b->text - strlen(b->text) - 1);
        strncat(b->text, "+", sizeof b->text - strlen(b->text) - 1);
    }
    for (; m < 3; ++m)
        b->mods[m] = 0;
    b->key = (uint8_t)codes[key];
    strncat(b->text, display_name(names[key]), sizeof b->text - strlen(b->text) - 1);
    return 1;
}

int hk_parse(const char *text, size_t len, HkBind *out, int cap) {
    char section[40] = "";
    int n = 0;
    const char *p = text, *end = text + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = eol ? eol : end;
        const char *comment = NULL;
        for (const char *s = p; s + 1 < line_end; ++s)
            if (s[0] == '/' && s[1] == '/') {
                comment = s;
                break;
            }
        const char *le = comment ? comment : line_end;
        while (p < le && is_space(*p))
            ++p;
        const char *word = p;
        while (p < le && !is_space(*p))
            ++p;
        const size_t wlen = (size_t)(p - word);
        while (p < le && is_space(*p))
            ++p;
        const char *arg = p;
        while (p < le && !is_space(*p))
            ++p;
        const size_t alen = (size_t)(p - arg);
        if (wlen == 11 && !memcmp(word, "bindsection", 11) && alen) {
            copy_token(section, sizeof section, arg, alen);
        } else if (wlen == 4 && !memcmp(word, "bind", 4) && alen && n < cap) {
            HkBind *b = &out[n];
            memset(b, 0, sizeof *b);
            memcpy(b->section, section, sizeof b->section);
            copy_token(b->action, sizeof b->action, arg, alen);
            if (parse_keys(p, le, b))
                ++n;
        }
        p = eol ? eol + 1 : end;
    }
    return n;
}

const HkBind *hk_find(const HkBind *binds, int n, const char *action, const char *const *sections,
                      int nsections) {
    for (int s = 0; s <= nsections; ++s) {
        const char *section = s < nsections ? sections[s] : "";
        for (int i = 0; i < n; ++i)
            if (!strcmp(binds[i].section, section) && !strcmp(binds[i].action, action))
                return &binds[i];
    }
    return NULL;
}

/* ---- The catalog ---- */

typedef struct {
    const char *id;              /* the button's art: touch-ui/camera-panel/btn-<id> */
    const char *actions;         /* the bound actions it presses, the first one bound wins */
    const char *sections;        /* the screens it is offered on (bindsection names) */
    const char *kind;            /* "tap", "latch" or "next_tap_right" (no action) */
    const char *symbol;          /* the system symbol where there is no art */
    int quick;                   /* pinned by default: 1 everywhere, 2 large screens */
    const char *label[HK_LANGS]; /* en, ru, de, fr, pl, cs, hu, it, es, zh-Hans */
    const char *buttons;         /* the game's own buttons for it (window <Name>s): while
                             one is shown, the panel leaves the command out */
} Command;

/* In the panel's order: commands with no button of their own on the game's
 * screen, and some that have one only now and then or only in some of the
 * games (a battle's wait in Hammers of Fate, the dig, the placement's): those
 * name it, and are left out while it shows. None is offered in a town: its
 * screen has buttons all round, and the few commands left (quick save and
 * load, the camera, Shift) were not worth a panel over it. */
static const Command COMMANDS[] = {
    /* The adventure map */
    {"mana",
     "show_heroes_mana",
     "adventure_screen",
     "tap",
     "drop.fill",
     2,
     {"Heroes' mana", "Мана героев", "Mana der Helden", "Mana des héros", "Mana bohaterów",
      "Mana hrdinů", "Hősök manája", "Mana degli eroi", "Maná de los héroes", "英雄魔法值"},
     NULL},
    {"quicksave",
     "quicksave",
     "adventure_screen",
     "tap",
     "square.and.arrow.down",
     2,
     {"Quick save", "Быстрое сохранение", "Schnellspeichern", "Sauvegarde rapide", "Szybki zapis",
      "Rychlé uložení", "Gyorsmentés", "Salvataggio rapido", "Guardado rápido", "快速存档"},
     NULL},
    {"quickload",
     "quickload",
     "adventure_screen",
     "tap",
     "square.and.arrow.up",
     0,
     {"Quick load", "Быстрая загрузка", "Schnellladen", "Chargement rapide", "Szybki odczyt",
      "Rychlé načtení", "Gyorsbetöltés", "Caricamento rapido", "Carga rápida", "快速读档"},
     NULL},
    {"sleep",
     "sleep_wake_hero",
     "adventure_screen",
     "tap",
     "moon.zzz",
     0,
     {"Sleep / wake", "Сон / пробуждение", "Schlafen / wecken", "Dormir / réveiller",
      "Śpij / obudź", "Spát / probudit", "Alvás / ébresztés", "Dormi / sveglia",
      "Dormir / despertar", "休息 / 唤醒"},
     NULL},
    {"dig",
     "adv_dig",
     "adventure_screen",
     "tap",
     "hammer",
     0,
     {"Dig", "Копать", "Graben", "Creuser", "Kop", "Kopat", "Ásás", "Scava", "Cavar", "挖掘"},
     "OnJog-DigGrail"},
    {"underground",
     "flip_floor",
     "adventure_screen",
     "tap",
     "square.stack.3d.down.right",
     0,
     {"Surface / underground", "Поверхность / подземелье", "Oberwelt / Unterwelt",
      "Surface / souterrain", "Powierzchnia / podziemia", "Povrch / podzemí",
      "Felszín / föld alatt", "Superficie / sotterraneo", "Superficie / subterráneo",
      "地面 / 地下"},
     "OnJog-Underground"},
    {"puzzle-map",
     "input_open_puzzle_map open_puzzle_map",
     "adventure_screen",
     "tap",
     "puzzlepiece",
     0,
     {"Tear of Asha map", "Карта Слезы Асхи", "Karte der Träne Ashas", "Carte de la Larme d'Asha",
      "Mapa Łzy Ashy", "Mapa Ashiny slzy", "Asha Könnyének térképe", "Mappa della Lacrima di Asha",
      "Mapa de la Lágrima de Asha", "亚莎之泪地图"},
     NULL},
    {"camera-home",
     "set_camera_default_pos",
     "adventure_screen",
     "tap",
     "scope",
     0,
     {"Default camera", "Камера по умолчанию", "Standardkamera", "Caméra par défaut",
      "Domyślna kamera", "Výchozí kamera", "Alapértelmezett kamera", "Visuale predefinita",
      "Cámara por defecto", "默认镜头"},
     NULL},
    {"kingdom",
     "input_adv_show_kingdom_overview",
     "adventure_screen",
     "tap",
     "crown",
     0,
     {"Kingdom overview", "Обзор королевства", "Königreichsübersicht", "Aperçu du royaume",
      "Przegląd królestwa", "Přehled království", "Királyság áttekintése", "Panoramica del regno",
      "Vista general del reino", "王国概要"},
     NULL},
    {"quest-log",
     "input_adv_show_objectives",
     "adventure_screen",
     "tap",
     "scroll",
     0,
     {"Objectives", "Задания", "Ziele", "Objectifs", "Cele", "Úkoly", "Célok", "Obiettivi",
      "Objetivos", "任务目标"},
     "OnJog-Scenario"},
    /* Not a key: the skill wheel drawn natively (native_screens.c, the
     * native-screens/SkillWheel.swift view), on the map. A hero's screen and
     * the level-up box have a button of their own in the game's windows
     * (tools/skill_wheel_ui_mod.py), and a hero's screen no panel at all. */
    {"skill-wheel",
     NULL,
     "adventure_screen",
     "screen",
     "chart.pie",
     2,
     {"Skill wheel", "Колесо навыков", "Fertigkeitenrad", "Roue des compétences",
      "Koło umiejętności", "Kolo dovedností", "Ismeretek kereke", "Ruota delle capacità",
      "Rueda de habilidades", "技能轮"},
     NULL},
    /* A battle */
    {"wait",
     "input_Combat_Wait",
     "combat_screen",
     "tap",
     "hourglass",
     1,
     {"Wait", "Ждать", "Warten", "Attendre", "Czekaj", "Čekat", "Várakozás", "Attendi", "Esperar",
      "等待"},
     "OnJog-Wait"},
    {"initiative",
     "input_switch_atb_log",
     "combat_screen",
     "tap",
     "list.bullet",
     1,
     {"Initiative bar / log", "Шкала инициативы / журнал", "Initiativeleiste / Protokoll",
      "Barre d'initiative / journal", "Pasek inicjatywy / dziennik", "Lišta iniciativy / záznam",
      "Kezdeményezési sáv / napló", "Barra dell'iniziativa / registro",
      "Barra de iniciativa / registro", "主动条 / 日志"},
     "LogHider OnJog-AtbToggle"},
    /* Placement before a battle: ahead of creature info, which it shares */
    {"autoplace",
     "input_autoplace_army",
     "combat_screen_tactic",
     "tap",
     "wand.and.stars",
     1,
     {"Auto-arrange", "Авторасстановка", "Automatisch aufstellen", "Placement auto",
      "Auto-rozstawienie", "Automatické rozestavení", "Automatikus felállítás",
      "Schieramento automatico", "Colocación automática", "自动布阵"},
     "OnJog-StartCombat"},
    {"start-combat",
     "input_start_combat",
     "combat_screen_tactic",
     "tap",
     "play.fill",
     1,
     {"Start combat", "Начать бой", "Kampf beginnen", "Commencer le combat", "Rozpocznij bitwę",
      "Zahájit bitvu", "Harc megkezdése", "Inizia il combattimento", "Iniciar combate", "开始战斗"},
     "OnJog-NextCreature"},
    {"creature-info",
     NULL,
     "combat_screen combat_screen_tactic",
     "next_tap_right",
     "info.circle",
     1,
     {"Creature info", "Сведения о существе", "Kreaturinfo", "Infos créature",
      "Informacje o istocie", "Informace o bytosti", "Lény adatai", "Informazioni sulla creatura",
      "Información de la criatura", "生物信息"},
     NULL},
    {"ctrl-melee",
     "ctrl",
     "combat_screen",
     "latch",
     "control",
     0,
     {"Ctrl: melee attack", "Ctrl: удар вблизи", "Strg: Nahkampf", "Ctrl : corps à corps",
      "Ctrl: walka wręcz", "Ctrl: útok zblízka", "Ctrl: közelharci támadás",
      "Ctrl: attacco in mischia", "Ctrl: ataque cuerpo a cuerpo", "Ctrl：近战攻击"},
     NULL},
    /* Screens where an army's stacks are dragged: a garrison's. A hero's
     * screen shows no panel at all. */
    {"shift-split",
     "shift",
     "garrison_screen",
     "latch",
     "shift",
     0,
     {"Shift: split stack", "Shift: разделить отряд", "Umschalt: Stapel teilen",
      "Maj : diviser la troupe", "Shift: podziel oddział", "Shift: rozdělit oddíl",
      "Shift: csapat megosztása", "Maiusc: dividi la truppa", "Mayús: dividir la tropa",
      "Shift：拆分部队"},
     NULL},
};

/* What a button says under it: a word or two, the full label in its tip. */
static const struct {
    const char *id;
    const char *text[HK_LANGS]; /* as label */
} SHORT[] = {
    {"mana", {"Mana", "Мана", "Mana", "Mana", "Mana", "Mana", "Mana", "Mana", "Maná", "魔法"}},
    {"quicksave",
     {"Quicksave", "Сохранить", "Speichern", "Sauver", "Zapisz", "Uložit", "Mentés", "Salva",
      "Guardar", "存档"}},
    {"quickload",
     {"Quickload", "Загрузить", "Laden", "Charger", "Wczytaj", "Načíst", "Betöltés", "Carica",
      "Cargar", "读档"}},
    {"sleep",
     {"Sleep", "Сон", "Schlaf", "Dormir", "Sen", "Spánek", "Alvás", "Dormi", "Dormir", "休息"}},
    {"dig",
     {"Dig", "Копать", "Graben", "Creuser", "Kop", "Kopat", "Ásás", "Scava", "Cavar", "挖掘"}},
    {"underground",
     {"Underground", "Подземелье", "Unterwelt", "Souterrain", "Podziemia", "Podzemí", "Föld alatt",
      "Sottosuolo", "Subsuelo", "地下"}},
    {"puzzle-map",
     {"Tear map", "Карта Слезы", "Tränenkarte", "Carte", "Mapa Łzy", "Mapa slzy", "Könny-térkép",
      "Mappa", "Mapa", "泪之图"}},
    {"camera-home",
     {"Camera", "Камера", "Kamera", "Caméra", "Kamera", "Kamera", "Kamera", "Visuale", "Cámara",
      "镜头"}},
    {"kingdom",
     {"Kingdom", "Королевство", "Königreich", "Royaume", "Królestwo", "Království", "Királyság",
      "Regno", "Reino", "王国"}},
    {"quest-log",
     {"Objectives", "Задания", "Ziele", "Objectifs", "Cele", "Úkoly", "Célok", "Obiettivi",
      "Objetivos", "目标"}},
    {"skill-wheel",
     {"Skills", "Навыки", "Fertigkeiten", "Compétences", "Umiejętności", "Dovednosti", "Ismeretek",
      "Capacità", "Habilidades", "技能"}},
    {"wait",
     {"Wait", "Ждать", "Warten", "Attendre", "Czekaj", "Čekat", "Várás", "Attendi", "Esperar",
      "等待"}},
    {"initiative",
     {"Initiative", "Инициатива", "Initiative", "Initiative", "Inicjatywa", "Iniciativa",
      "Kezdeményezés", "Iniziativa", "Iniciativa", "主动性"}},
    {"autoplace",
     {"Arrange", "Расставить", "Aufstellen", "Placer", "Rozstaw", "Rozestavit", "Felállítás",
      "Schiera", "Colocar", "布阵"}},
    {"start-combat",
     {"Start", "В бой", "Start", "Combat", "Start", "Do boje", "Harcba", "Combatti", "Combatir",
      "开战"}},
    {"creature-info",
     {"Info", "Сведения", "Info", "Infos", "Info", "Info", "Infó", "Info", "Info", "信息"}},
    {"ctrl-melee",
     {"Melee", "Вблизи", "Nahkampf", "Mêlée", "Wręcz", "Zblízka", "Közelharc", "Mischia", "Cuerpo",
      "近战"}},
    {"shift-split",
     {"Split", "Делить", "Teilen", "Diviser", "Podziel", "Rozdělit", "Megosztás", "Dividi",
      "Dividir", "拆分"}},
};

static const char *short_label(const char *id, int lang) {
    for (size_t i = 0; i < sizeof SHORT / sizeof SHORT[0]; ++i)
        if (!strcmp(SHORT[i].id, id))
            return SHORT[i].text[lang];
    return NULL;
}

/* Whether the space-separated `list` holds `word`. */
static int has_word(const char *list, const char *word) {
    const size_t len = strlen(word);
    for (const char *p = list; *p;) {
        while (*p == ' ')
            ++p;
        const char *e = p;
        while (*e && *e != ' ')
            ++e;
        if ((size_t)(e - p) == len && !memcmp(p, word, len))
            return 1;
        p = e;
    }
    return 0;
}

/* The bind of the first of `actions` the file binds. */
static const HkBind *find_any(const HkBind *binds, int n, const char *actions,
                              const char *const *sections, int nsections) {
    char action[48];
    for (const char *p = actions; *p;) {
        while (*p == ' ')
            ++p;
        const char *e = p;
        while (*e && *e != ' ')
            ++e;
        if (e > p) {
            copy_token(action, sizeof action, p, (size_t)(e - p));
            const HkBind *b = hk_find(binds, n, action, sections, nsections);
            if (b)
                return b;
        }
        p = e;
    }
    return NULL;
}

typedef struct {
    char *out;
    int cap, len;
} Buf;

static void put(Buf *b, const char *s) {
    const int n = (int)strlen(s);
    if (b->len + n < b->cap)
        memcpy(b->out + b->len, s, (size_t)n);
    b->len += n;
}

/* A JSON string; the words here hold no characters that need escaping but
 * quotes and backslashes, which a key's text could. */
static void put_str(Buf *b, const char *s) {
    put(b, "\"");
    char c[2] = {0, 0};
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\')
            put(b, "\\");
        c[0] = *s;
        put(b, c);
    }
    put(b, "\"");
}

int hk_command_count(void) {
    return (int)(sizeof COMMANDS / sizeof COMMANDS[0]);
}

const char *hk_command_buttons(int i) {
    return i >= 0 && i < hk_command_count() ? COMMANDS[i].buttons : NULL;
}

int hk_document(const HkBind *binds, int n, const char *const *sections, int nsections, int lang,
                uint32_t onscreen, char *out, int cap) {
    static const char *const LANG[HK_LANGS] = {"en", "ru", "de", "fr", "pl",
                                               "cs", "hu", "it", "es", "zh-Hans"};
    if (lang < HK_EN || lang >= HK_LANGS)
        lang = HK_EN;
    Buf b = {out, cap, 0};
    char num[16];
    int count = 0;
    put(&b, "{\"screen\":\"");
    for (int s = 0; s < nsections; ++s) {
        if (s)
            put(&b, " ");
        put(&b, sections[s]);
    }
    put(&b, "\",\"lang\":\"");
    put(&b, LANG[lang]);
    put(&b, "\",\"commands\":[");
    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; ++i) {
        const Command *c = &COMMANDS[i];
        int shown = 0;
        for (int s = 0; s < nsections && !shown; ++s)
            shown = has_word(c->sections, sections[s]);
        if (!shown || (i < 32 && (onscreen >> i & 1u)))
            continue;
        const HkBind *bind =
            c->actions ? find_any(binds, n, c->actions, sections, nsections) : NULL;
        if (c->actions && !bind)
            continue;
        put(&b, count++ ? ",{\"id\":" : "{\"id\":");
        put_str(&b, c->id);
        put(&b, ",\"label\":");
        put_str(&b, c->label[lang]);
        const char *brief = short_label(c->id, lang);
        if (brief) {
            put(&b, ",\"short\":");
            put_str(&b, brief);
        }
        put(&b, ",\"keys\":");
        put_str(&b, bind ? bind->text : "");
        put(&b, ",\"kind\":");
        put_str(&b, c->kind);
        snprintf(num, sizeof num, ",\"quick\":%d", c->quick);
        put(&b, num);
        put(&b, ",\"symbol\":");
        put_str(&b, c->symbol);
        if (bind) {
            snprintf(num, sizeof num, ",\"key\":%d", bind->key);
            put(&b, num);
            put(&b, ",\"mods\":[");
            for (int m = 0; m < 3 && bind->mods[m]; ++m) {
                snprintf(num, sizeof num, m ? ",%d" : "%d", bind->mods[m]);
                put(&b, num);
            }
            put(&b, "]");
        }
        put(&b, "}");
    }
    put(&b, "]}");
    if (!count)
        return 0;
    if (b.len < cap)
        out[b.len] = 0;
    return b.len + 1 <= cap ? b.len : b.len + 1;
}
