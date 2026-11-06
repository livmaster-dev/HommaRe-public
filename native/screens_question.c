/* native/screens_question.c - the game's questions drawn natively
 * (native/native_screens.c): "question", one popup for every CCommonMsgBox,
 * the box a screen puts in its own root to ask (quit the game, load over the
 * game, restart the mission, delete a save...). The box does not change the
 * screen stack, so the screen below may be native or the game's: the hook
 * reads the box each frame and does not draw it.
 *
 * The box keeps no strings: its setup (007c9d90, every box passes through
 * it) is overridden to note the title and the text. The answer is the box's
 * own OK or Cancel, which tell the screen that asked and remove the box.
 * The base game's addresses: research notes phone-menus/native/re-ingame-menus.md F. */
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

/* The base game's; an expansion has none yet. */
#ifndef RECOMP_ADDRESS_HEADER
#define NS_VT_MSGBOX 0x00fae2f4      /* CCommonMsgBox */
#define NS_VT_MSGBOX_TEXT 0x00fb7974 /* CCommonMsgBoxWithUpdatableText */
#define NS_MSGBOX_SETUP_ORIG fn_007c9d90 /* the translated box setup (overrides.h) */
#endif

/* Fields and slots an expansion moves: its addresses.h defines the ones that
 * differ (research notes phone-menus/native/expansion-*-addresses.md). */
#ifndef NS_B_YESNO
#define NS_B_YESNO 0x134
#endif
#ifndef NS_S_BOXES
#define NS_S_BOXES 0x14c
#endif

enum {
    B_WINDOW = 0xf8,   /* what the owner's list holds */
    B_OWNER = 0x104,   /* the screen whose root it is in */
    B_YESNO = NS_B_YESNO,   /* byte: Cancel shown */
    S_BOXES = NS_S_BOXES,   /* a screen's list of modal boxes (sentinel) */
    VT_CANCEL = 0x38,  /* thiscall (box, 0), RET 4: notify "no", remove */
    VT_OK = 0x3c,      /* thiscall (box, 0), RET 4: notify "yes", remove */
};

/* What each box's setup said, the newest last. */
typedef struct {
    uint32_t box;
    char title[256];
    char text[2048];
} Asked;

enum { ASKED = 4 };
static Asked g_asked[ASKED];
static int g_asked_count;

#if defined(NS_VT_MSGBOX)

void NS_MSGBOX_SETUP_ORIG(X86 *c);

/* 007c9d90 (an expansion's own address): thiscall (box; id, byte yes/no, wstr* title, wstr* text,
 * notify*, byte esc), RET 0x18. */
void homm5_msgbox_setup(X86 *c) {
    if (!ns_screens_on()) { /* the game's own boxes: nothing to keep */
        NS_MSGBOX_SETUP_ORIG(c);
        return;
    }
    const uint32_t box = c->r[R_ECX], sp = c->r[R_ESP];
    const uint32_t title = rd32(sp + 0xc), text = rd32(sp + 0x10);
    int at = 0;
    while (at < g_asked_count && g_asked[at].box != box)
        ++at;
    if (at == g_asked_count) {
        if (g_asked_count == ASKED) {
            memmove(&g_asked[0], &g_asked[1], sizeof g_asked[0] * (ASKED - 1));
            --g_asked_count;
        }
        at = g_asked_count++;
    } else if (at != g_asked_count - 1) {
        /* Set up again: the newest now. */
        Asked moved = g_asked[at];
        memmove(&g_asked[at], &g_asked[at + 1], sizeof g_asked[0] * (size_t)(g_asked_count - 1 - at));
        g_asked[g_asked_count - 1] = moved;
        at = g_asked_count - 1;
    }
    Asked *a = &g_asked[at];
    a->box = box;
    a->title[0] = a->text[0] = 0;
    if (ns_ptr(title))
        ns_wstr_utf8(title, a->title, sizeof a->title);
    if (ns_ptr(text))
        ns_wstr_utf8(text, a->text, sizeof a->text);
    if (ns_trace())
        fprintf(stderr, "[screens] box %08x: %s / %.200s\n", box, a->title, a->text);
    NS_MSGBOX_SETUP_ORIG(c);
}

/* Whether `box` is a message box in its owner's list now. */
static int live(uint32_t box) {
    if (!ns_ptr(box))
        return 0;
    const uint32_t vt = rd32(box);
    if (vt != NS_VT_MSGBOX && vt != NS_VT_MSGBOX_TEXT)
        return 0;
    const uint32_t owner = rd32(box + B_OWNER);
    if (!ns_ptr(owner))
        return 0;
    const uint32_t list = owner + S_BOXES;
    int guard = 32;
    for (uint32_t n = rd32(list); ns_ptr(n) && n != list && guard--; n = rd32(n))
        if (rd32(n + 8) == box + B_WINDOW)
            return 1;
    return 0;
}

uint32_t ns_question_box(void) {
    for (int i = g_asked_count - 1; i >= 0; --i)
        if (live(g_asked[i].box))
            return g_asked[i].box;
    return 0;
}

#else

uint32_t ns_question_box(void) {
    return 0;
}

#endif

static const Asked *asked(uint32_t box) {
    for (int i = 0; i < g_asked_count; ++i)
        if (g_asked[i].box == box)
            return &g_asked[i];
    return NULL;
}

static void question_command(Scratch *s, uint32_t box, const char *cmd) {
    char verb[16];
    ns_cmd_str(cmd, "do", verb, sizeof verb);
    const uint32_t zero = 0;
    if (strcmp(verb, "ok") == 0)
        ns_vcall(s, box, VT_OK, &zero, 1);
    else if (strcmp(verb, "cancel") == 0 && rd8(box + B_YESNO))
        ns_vcall(s, box, VT_CANCEL, &zero, 1);
}

static void question_document(Json *j, Scratch *s, uint32_t box) {
    (void)s;
    const Asked *a = asked(box);
    const int yesno = rd8(box + B_YESNO) != 0;
    j_lit(j, "{\"screen\":\"question\",");
    j_kv_str(j, "lang", ns_lang_code());
    j_printf(j, ",\"box\":%u,", box);
    j_kv_str(j, "title", a ? a->title : "");
    j_lit(j, ",");
    j_kv_str(j, "text", a ? a->text : "");
    j_lit(j, ",\"buttons\":[");
    j_button(j, 1, "ok", yesno ? ns_tr("Да", "Yes") : "OK", 1);
    if (yesno)
        j_button(j, 0, "cancel", ns_tr("Нет", "No"), 1);
    j_lit(j, "]}");
}

const NsScreen ns_question = {.id = "question", .command = question_command, .document = question_document};
