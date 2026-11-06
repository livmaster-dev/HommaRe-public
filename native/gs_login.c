/* native/gs_login.c - Multi Player > ubi-gs never sends the player's password
 * (runtime/gs_login.h). Shared by the three games; addresses come from the
 * game's RECOMP_ADDRESS_HEADER, the base game's being the defaults.
 *
 * The GS SDK's login entry (the base game's 00419ae0, thiscall: name,
 * password, game version, a flag; RET 0x10) copies the strings into the SDK
 * and builds the router's LOGIN from them (0042a380: message 0x66, [name,
 * password, version, flag]). NUbi::CStateLoggedOut::ProcessLogin (00b30f60)
 * calls it, through a thunk (004193f0), with the account name and password
 * from the login dialog (MPRegister). When the server list came from a server
 * the player chose, the replacement here hands it a token instead
 * (recomp_gs_login_password), so the password never reaches the SDK, the
 * message or the wire. The SDK's own copy of the password (+0x14, 16
 * characters) is never read again; the message carries the whole string.
 *
 * NUbi::CStateWaitLoginRouterResult::ProcessLoginRouterResult (00b31380,
 * thiscall: the reply, whose byte +0xc is '&' when the router accepted)
 * reports the answer, which tells the runtime whether a registration took. */
#include <stdint.h>
#include <string.h>

#include "native_seam.h"
#include "x86.h"

#ifdef RECOMP_OVERRIDE_HEADER
#include RECOMP_OVERRIDE_HEADER
#endif
#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif
#ifndef HOMM5_FN_GS_LOGIN
#define HOMM5_FN_GS_LOGIN fn_00419ae0
#define HOMM5_FN_GS_LOGIN_RESULT fn_00b31380
#endif

void HOMM5_FN_GS_LOGIN(X86 *c);
void HOMM5_FN_GS_LOGIN_RESULT(X86 *c);

enum {
    TEXT_MAX = 256,   /* longer than the dialog lets anyone type */
    REPLY_RESULT = 0x0c,
    RESULT_OK = 0x26, /* '&', GSSUCCESS */
};

/* A guest string into buf; 0 when it is not one of under TEXT_MAX bytes. */
static int guest_text(uint32_t at, char *buf) {
    if (!at)
        return 0;
    for (uint32_t i = 0; i < TEXT_MAX; ++i) {
        buf[i] = (char)rd8(at + i);
        if (!buf[i])
            return 1;
    }
    return 0;
}

void homm5_gs_login(X86 *c) {
    const uint32_t esp = c->r[R_ESP];
    const uint32_t name = rd32(esp + 4u), password = rd32(esp + 8u);
    char user[TEXT_MAX], pw[TEXT_MAX], out[64];
    const int swap = guest_text(name, user) && guest_text(password, pw) &&
                     recomp_gs_login_password(user, pw, out, sizeof out);
    memset(pw, 0, sizeof pw);
    if (!swap) {
        HOMM5_FN_GS_LOGIN(c);
        return;
    }
    /* The replacement on the guest stack, under a copy of the call's frame
     * (return address and the four arguments), so the call's own stack
     * grows below both. The RET 0x10 pops that copy; ESP is then put where
     * the original call would have left it. */
    const uint32_t text = (esp - 0x80u) & ~15u;
    const uint32_t n = (uint32_t)strlen(out) + 1u;
    for (uint32_t i = 0; i < n; ++i)
        wr8(text + i, (uint8_t)out[i]);
    memset(out, 0, sizeof out);
    const uint32_t frame = text - 0x14u;
    wr32(frame, rd32(esp));
    wr32(frame + 4u, name);
    wr32(frame + 8u, text);
    wr32(frame + 12u, rd32(esp + 12u));
    wr32(frame + 16u, rd32(esp + 16u));
    c->r[R_ESP] = frame;
    HOMM5_FN_GS_LOGIN(c);
    c->r[R_ESP] = esp + 0x14u;
    for (uint32_t i = 0; i < n; ++i)
        wr8(text + i, 0);
}

void homm5_gs_login_result(X86 *c) {
    const uint32_t reply = rd32(c->r[R_ESP] + 4u);
    if (reply)
        recomp_gs_login_result(rd8(reply + REPLY_RESULT) == RESULT_OK);
    HOMM5_FN_GS_LOGIN_RESULT(c);
}
