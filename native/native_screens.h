/* native/native_screens.h - what the native screens' files share
 * (native/native_screens.c and native/screens_*.c): the document's JSON
 * writer, the game's text, the commands' fields, guest calls from the draw
 * hook and the table of screens. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "x86.h"

/* ------------------------------------------------------------ the JSON -- */

typedef struct {
    char *s;
    int len, cap;
} Json;

void j_raw(Json *j, const char *s, int n);
/* A string literal only: its length is its size (the "" makes anything else,
 * a pointer or a conditional, a compile error). */
#define j_lit(j, lit) j_raw((j), "" lit, (int)sizeof(lit) - 1)
void j_printf(Json *j, const char *fmt, ...);
void j_str(Json *j, const char *s);              /* a JSON string of UTF-8 s */
void j_kv_str(Json *j, const char *key, const char *value);
void j_bool(Json *j, const char *key, int value); /* "key":true */
void j_fact(Json *j, int first, const char *label, const char *value);   /* [label, value] */
void j_button(Json *j, int first, const char *name, const char *label, int enabled);

/* ----------------------------------------------------- text and commands -- */

/* Guest UTF-16 text (n units at `at`) into UTF-8, the game's markup left out. */
void ns_text_utf8(uint32_t at, uint32_t n, char *out, size_t cap);
/* A wide string {begin, end, cap} at `w` the same way. */
void ns_wstr_utf8(uint32_t w, char *out, size_t cap);

/* A command's string / int field ({"do":"press","name":"create","index":3}). */
void ns_cmd_str(const char *cmd, const char *key, char *out, size_t cap);
int ns_cmd_int(const char *cmd, const char *key, int fallback);

/* The views' own words in the game's language (screens_words.c), set by the
 * screen being read from its first difficulty's name: ns_set_lang returns
 * ns_ru, ns_lang_code the view's "lang". German, French and Polish come from
 * the words' table, else English. */
enum { NS_EN, NS_RU, NS_DE, NS_FR, NS_PL };
extern int ns_lang;
extern int ns_ru;
int ns_set_lang(const char *easy);
const char *ns_lang_code(void);
const char *ns_tr(const char *ru, const char *en);

int ns_trace(void); /* RECOMP_TRACE_NATIVE_SCREENS */
int ns_ptr(uint32_t a); /* a plausible guest pointer */

/* ------------------------------------------------------ guest calls -- */

/* Guest scratch for calls made from the draw hook: data from `base` up, the
 * calls' stack below it. */
typedef struct {
    X86 *c;
    uint32_t base, used;
} Scratch;

Scratch ns_scratch(X86 *c);
uint32_t ns_call(Scratch *s, uint32_t fn, uint32_t ecx, uint32_t edx, const uint32_t *args,
                 int nargs);
uint32_t ns_vcall(Scratch *s, uint32_t self, uint32_t slot, const uint32_t *args, int nargs);
uint32_t ns_alloc(Scratch *s, uint32_t n);
/* The game's narrow string {begin, end, cap} of `text` in the scratch (for
 * calls that only read a name). */
uint32_t ns_str(Scratch *s, const char *text);
/* A game formatter that returns a const wstr* (fastcall ECX), into UTF-8. */
void ns_text_of(Scratch *s, uint32_t fn, uint32_t ecx, char *out, size_t cap);
/* A screen's button reaction ("create", "back"), as its button would run it:
 * `handler` called with ECX = `reactions`, the screen's
 * IProgrammedReactionsAndChecks subobject. */
void ns_react(Scratch *s, uint32_t reactions, uint32_t handler, const char *name);
/* The same with the sender's name the handler compares (a campaign's button,
 * "CampaignBtn2"). */
void ns_react_as(Scratch *s, uint32_t reactions, uint32_t handler, const char *sender,
                 const char *name);
/* Difficulty `index`'s name (0 the easiest). */
void ns_difficulty_name(Scratch *s, int index, char *out, size_t cap);
/* A text file of the game by its path ("/UI/MPWait/PlayersList/Item/bonus0.txt"),
 * as its windows show it; read once, "" when there is none. */
const char *ns_game_text(Scratch *s, const char *path);
/* A label in the game's words (its text file at `path`), or the view's own
 * words when the install has none. */
const char *ns_label(Scratch *s, const char *path, const char *ru, const char *en);
/* `text`'s first letter as a capital (Latin or Cyrillic), in place. */
char *ns_capital(char *text);
/* The game's heap: free(`p`) (what a formatter's out string holds). */
void ns_free(Scratch *s, uint32_t p);
/* A NUL-terminated UTF-16 copy of UTF-8 `utf8` in the scratch (what an edit
 * line's SetText takes); at most 63 characters. */
uint32_t ns_wide(Scratch *s, const char *utf8);

/* ------------------------------------------------------- windows -- */

/* The IWindow of an interface pointer a screen keeps (an IButton*): its
 * virtual base. */
uint32_t ns_iwindow(uint32_t p);
/* IWindow vt+0x8c FindChild(name, recursive): 0 when there is none. */
uint32_t ns_find(Scratch *s, uint32_t iwindow, const char *name);
/* IWindow vt+0x40 IsEnabled / vt+0x48 IsVisible. */
int ns_enabled(Scratch *s, uint32_t iwindow);
int ns_visible(Scratch *s, uint32_t iwindow);
/* A window's text from the game's data (a button's caption), in the game's
 * language, markup left out: IWindow vt+0xb4. */
void ns_caption(Scratch *s, uint32_t iwindow, char *out, size_t cap);

/* ------------------------------------------------------ the screens -- */

/* A game screen the host draws natively, known by its IScreen subobject's
 * vtable on the screen stack (the full object `sub` bytes before it). Each
 * frame its root would draw, while it is on top and claimed: `command` for
 * each queued command, `step` (optional), then `document`. */
typedef struct {
    const char *id;
    uint32_t vtable, sub;
    /* Optional: whether the screen is drawn natively now (one screen with
     * several panels may be so only on some); NULL: always. Asked for every
     * window drawn: guest memory only, no calls. */
    int (*claims)(uint32_t screen);
    void (*command)(Scratch *s, uint32_t screen, const char *cmd);
    void (*step)(Scratch *s, uint32_t screen);
    void (*document)(Json *j, Scratch *s, uint32_t screen);
    /* Optional: a window of the screen the game still draws under the native
     * view (the menu's logo), by its <Name>; the root is drawn then, and of
     * its windows only these and the windows that hold them. */
    int (*keeps)(uint32_t screen, const char *name);
} NsScreen;

/* native/screens_question.c: a game question (CCommonMsgBox) up now, its
 * box, or 0; and the popup for it, read with the box for its screen. */
uint32_t ns_question_box(void);

/* Whether native screens are on this run (the launcher's Menus, the env). */
int ns_screens_on(void);

/* The native view goes now, for a press that starts loading (a game, a
 * save, a mission): the game goes on drawing the screen while it loads, so
 * the view would stay until the load ended. The screen's document is held
 * back until another screen or a question comes. */
void ns_leave(void);

/* screens_lan.c: a network screen's chat ("chat": its lines) and setting an
 * IEditLine's text, for the LAN players' screen in screens_custom.c. */
void ns_lan_chat(Json *j, uint32_t model, int enabled);
void ns_lan_set_edit(Scratch *s, uint32_t edit, const char *utf8);
extern const NsScreen ns_question;

/* native/phone_layout.c: a window's <Name>. */
void homm5_window_name(uint32_t win, char *out, size_t cap);

/* What a game moves (an expansion's addresses.h, included here first, defines
 * the ones that differ): a screen's root WindowScreen, and interface slots. */
#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif
#ifndef NS_SCREEN_ROOT
#define NS_SCREEN_ROOT 0xa0
#endif
#ifndef NS_IW_FIND
#define NS_IW_FIND 0x8c          /* IWindow FindChild(name, recursive) */
#endif
#ifndef NS_IW_CAPTION
#define NS_IW_CAPTION 0xb4       /* IWindow: its text from the data */
#endif
#ifndef NS_SLIDER_SET_POS
#define NS_SLIDER_SET_POS 0x0c   /* ISlider SetPos(float) */
#endif
#ifndef NS_SLIDER_SET_SEGMENT
#define NS_SLIDER_SET_SEGMENT 0x2c
#endif
#ifndef NS_SLIDER_GET_SEGMENT
#define NS_SLIDER_GET_SEGMENT 0x30
#endif

enum {
    NS_ROOT_DRAW = 0x14,   /* from a root to the draw part 00e3dd70 gets */
};
