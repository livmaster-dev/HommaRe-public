/* native/skill_wheel.c - the hero the skill wheel opens on
 * (native-screens/SkillWheel.swift; native/native_screens.c opens it): the
 * one the game has in view when the player asks, read from the game's own
 * objects on the game thread.
 *
 * Where the wheel is offered: the adventure map (with a level-up box up or
 * not) and a hero's screen. There, the hero is
 *   - a level-up box's (NUI::CLevelUpBox, a modal window over the map: the
 *     top modal of the map's window screen), with the skills and perks the box
 *     offers;
 *   - a hero screen's (NUI::CHeroScreen2: the hero it shows);
 *   - else the map's chosen one (CAdventureController's record of the local
 *     player: its IHero*).
 * Of the hero (an IHero*, the runtime NWorld::CHero + 0x1c, as every game
 * interface passes it): its class (HERO_CLASS_* number), level, name, and its
 * skills and perks, one vector of {mastery, id}. The ids are the SkillID
 * numbers, the place of each skill in Skills.xdb, so the view names them from
 * its own read of the table; a perk is told from a skill there too.
 *
 * Nothing is called in the game: plain reads, each pointer checked. An
 * expansion without the addresses (its native/addresses.h) has no hero: the
 * wheel opens on the class it showed last. The addresses: research notes
 * skillwheel/re-{base,hof,toe}.md. */
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

/* The base game's (research notes skillwheel/re-base.md). */
#ifndef RECOMP_ADDRESS_HEADER
#define SW_SCREEN_STACK 0x0114b2e4 /* std::list<IScreen*>: the sentinel; top = sentinel->prev */
#define SW_VT_ADVENTURE 0x00fab2ac /* NUI::CAdventureScreen's IScreen */
#define SW_SUB_ADVENTURE 0x7b4     /* ... at that offset in the screen object */
#define SW_VT_HERO 0x00fb48d8      /* NUI::CHeroScreen2's IScreen */
#define SW_SUB_HERO 0x7d4
#define SW_HERO_SCREEN_HERO 0x1b0  /* CHeroScreen2: the IHero* it shows */
#define SW_SCREEN_WINDOWS 0xa0     /* screen object: its CWindowScreen, 0xf8 bytes in */
#define SW_WINDOWS_MODAL 0x1f4     /* CWindowScreen: the top modal window (CWindow*) */
#define SW_VT_LEVELUP 0x00fb89e4   /* NUI::CLevelUpBox (its CWindow, the full object) */
#define SW_LEVELUP_HERO 0x124      /* the box's IHero* */
#define SW_LEVELUP_CHOICES 0x130   /* vector of {id, mastery}: the choices offered */
#define SW_LEVELUP_LEVEL 0x140     /* the level the hero reaches */
#define SW_ADV_CONTROLLER 0x574    /* adventure screen: CAdventureController* */
#define SW_CTRL_RECORDS 0x1c       /* controller: per player id 0..8, 0x1c bytes each */
#define SW_RECORD_HERO 0x14        /* a record: the chosen hero's IHero* */
#define SW_CTRL_PLAYER 0x34        /* controller: the local IPlayer* */
#define SW_PLAYER_ID_FN 0x007d3000 /* IPlayer vt+0x2c, the player id: MOV EAX,[ECX+0x78] */
#define SW_PLAYER_ID 0x78
#define SW_HERO_LEVEL 0x4c  /* IHero: level */
#define SW_HERO_NAME 0x54   /*        name, a wide string {begin, end, cap} */
#define SW_HERO_CLASS 0x70  /*        class, HERO_CLASS_* (1 the first) */
#define SW_HERO_SKILLS 0x78 /*        vector of {mastery, id}: skills and perks */
#define SW_SKILL_COUNT 151  /* SkillID numbers: 0 NONE ... */
#endif

#ifdef SW_HERO_SKILLS

static uint32_t word(uint32_t a) {
    return ns_ptr(a) ? rd32(a) : 0;
}

/* The top screen's IScreen subobject, or 0. */
static uint32_t top_screen(void) {
    const uint32_t sentinel = word(SW_SCREEN_STACK);
    if (!sentinel || word(sentinel) == sentinel)
        return 0;
    const uint32_t last = word(sentinel + 4u);
    return last ? word(last + 8u) : 0;
}

/* The name as the game shows it: a wide string {begin, end, cap}; "" when
 * it does not read. */
static void hero_name(uint32_t hero, char *out, size_t cap) {
    out[0] = 0;
    const uint32_t b = word(hero + SW_HERO_NAME), e = word(hero + SW_HERO_NAME + 4u);
    if (ns_ptr(b) && e >= b && e - b <= 128u)
        ns_wstr_utf8(hero + SW_HERO_NAME, out, cap);
}

/* `,"hero":{...}` for an IHero*, into j; at `level` (a level-up box's, the
 * one it is reaching), else its own. */
static void write_hero(Json *j, uint32_t hero, int level) {
    char name[160];
    hero_name(hero, name, sizeof name);
    j_lit(j, ",\"hero\":{");
    j_printf(j, "\"class\":%d,\"level\":%d", (int)word(hero + SW_HERO_CLASS),
             level > 0 ? level : (int)word(hero + SW_HERO_LEVEL));
    if (name[0]) {
        j_lit(j, ",");
        j_kv_str(j, "name", name);
    }
    j_lit(j, ",\"skills\":[");
    const uint32_t b = word(hero + SW_HERO_SKILLS), e = word(hero + SW_HERO_SKILLS + 4u);
    int n = 0;
    if (ns_ptr(b) && e >= b && e - b <= 8u * SW_SKILL_COUNT) {
        for (uint32_t p = b; p < e; p += 8u) {
            const int mastery = (int)rd32(p), id = (int)rd32(p + 4u);
            if (id <= 0 || id >= SW_SKILL_COUNT || mastery <= 0)
                continue;
            j_printf(j, "%s{\"id\":%d,\"mastery\":%d}", n++ ? "," : "", id, mastery);
        }
    }
    j_lit(j, "]}");
}

int homm5_skill_wheel_hero(Json *j) {
    const uint32_t sub = top_screen();
    const uint32_t vt = word(sub);
    uint32_t hero = 0, box = 0;
    if (vt == SW_VT_HERO) {
        hero = word(sub - SW_SUB_HERO + SW_HERO_SCREEN_HERO);
    } else if (vt == SW_VT_ADVENTURE) {
        const uint32_t full = sub - SW_SUB_ADVENTURE;
        const uint32_t windows = word(full + SW_SCREEN_WINDOWS);
        const uint32_t modal = windows > 0xf8u ? word(windows - 0xf8u + SW_WINDOWS_MODAL) : 0;
        if (modal && word(modal) == SW_VT_LEVELUP) {
            box = modal;
            hero = word(box + SW_LEVELUP_HERO);
        } else {
            const uint32_t ctrl = word(full + SW_ADV_CONTROLLER);
            const uint32_t player = word(ctrl + SW_CTRL_PLAYER);
            const int pid = word(word(player) + 0x2cu) == SW_PLAYER_ID_FN
                                ? (int)word(player + SW_PLAYER_ID)
                                : -1;
            const uint32_t records = word(ctrl + SW_CTRL_RECORDS);
            if (pid >= 0 && pid < 9 && records)
                hero = word(records + (uint32_t)pid * 0x1cu + SW_RECORD_HERO);
        }
    } else {
        return -1; /* no wheel here */
    }
    if (ns_trace())
        fprintf(stderr, "[screens] skill wheel: screen %08x, hero %08x, level-up box %08x\n", vt,
                hero, box);
    if (!ns_ptr(hero))
        return 0;
    write_hero(j, hero, box ? (int)word(box + SW_LEVELUP_LEVEL) : 0);
    if (box) {
        j_lit(j, ",\"offer\":[");
        const uint32_t b = word(box + SW_LEVELUP_CHOICES), e = word(box + SW_LEVELUP_CHOICES + 4u);
        int n = 0;
        if (ns_ptr(b) && e >= b && e - b <= 8u * 16u) {
            for (uint32_t p = b; p < e; p += 8u) {
                const int id = (int)rd32(p);
                if (id > 0 && id < SW_SKILL_COUNT)
                    j_printf(j, "%s%d", n++ ? "," : "", id);
            }
        }
        j_lit(j, "]");
    }
    return 1;
}

#endif

/* The wheel's own buttons in the game's windows (tools/skill_wheel_ui_mod.py:
 * the level-up box's plate, the hero screen's sixth bookmark), both named
 * SkillWheelButton: a press shows the button's hidden child SkillWheelFlag
 * (UI_SHOW_WINDOW), seen here when the button is drawn next and put down
 * again. 1 when `win`, a window being drawn, was pressed so. */
void homm5_window_name(uint32_t win, char *out, size_t cap); /* native/phone_layout.c */
uint32_t homm5_window_child(uint32_t win, const char *name);
int homm5_window_take_shown(uint32_t win);

int homm5_skill_wheel_pressed(uint32_t win) {
    static uint32_t known; /* the last button seen: a name read once per window */
    if (win != known) {
        char name[32];
        homm5_window_name(win, name, sizeof name);
        if (strcmp(name, "SkillWheelButton") != 0)
            return 0;
        known = win;
    }
    const uint32_t flag = homm5_window_child(win, "SkillWheelFlag");
    if (!flag || !homm5_window_take_shown(flag))
        return 0;
    if (ns_trace())
        fprintf(stderr, "[screens] skill wheel: its button %08x pressed\n", win);
    return 1;
}

#ifndef SW_HERO_SKILLS

int homm5_skill_wheel_hero(Json *j) {
    (void)j;
    return 0;
}

#endif
