// codepage.cpp - see codepage.h.
#include "codepage.h"

#include "game_config.h"
#include "../platform/os.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef RECOMP_ANSI_CODEPAGE
#define RECOMP_ANSI_CODEPAGE 1252u
#endif

namespace win32cp {
namespace {

// Windows-1251, 0x80..0xbf (0xc0..0xff is U+0410..U+044F in order).
const uint16_t k1251[64] = {
    0x0402, 0x0403, 0x201a, 0x0453, 0x201e, 0x2026, 0x2020, 0x2021, 0x20ac, 0x2030, 0x0409,
    0x2039, 0x040a, 0x040c, 0x040b, 0x040f, 0x0452, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
    0x2013, 0x2014, 0xfffd, 0x2122, 0x0459, 0x203a, 0x045a, 0x045c, 0x045b, 0x045f, 0x00a0,
    0x040e, 0x045e, 0x0408, 0x00a4, 0x0490, 0x00a6, 0x00a7, 0x0401, 0x00a9, 0x0404, 0x00ab,
    0x00ac, 0x00ad, 0x00ae, 0x0407, 0x00b0, 0x00b1, 0x0406, 0x0456, 0x0491, 0x00b5, 0x00b6,
    0x00b7, 0x0451, 0x2116, 0x0454, 0x00bb, 0x0458, 0x0405, 0x0455, 0x0457,
};

void put_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xc0 | cp >> 6));
        out.push_back((char)(0x80 | (cp & 0x3f)));
    } else {
        out.push_back((char)(0xe0 | cp >> 12));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back((char)(0x80 | (cp & 0x3f)));
    }
}

// The page in effect: RECOMP_ACP, else what choose_for_install found, else
// the game's.
uint32_t g_page;

uint32_t page_of(uint32_t p) {
    return p == 1251 || p == 65001 ? p : 1252u;
}

// The first of "languages" in an install's goggame-<id>.info ("zh-Hans",
// "hu-HU"), or "".
struct InfoSearch {
    std::string dir, lang;
};

int scan_info(const char *name, void *user) {
    auto *search = static_cast<InfoSearch *>(user);
    const std::string n = name;
    if (n.compare(0, 8, "goggame-") != 0 || n.size() < 13 ||
        n.compare(n.size() - 5, 5, ".info") != 0)
        return 0;
    FILE *f = fopen((search->dir + "/" + n).c_str(), "rb");
    if (!f)
        return 0;
    char buf[4096];
    const size_t got = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[got] = 0;
    const char *at = strstr(buf, "\"languages\"");
    at = at ? strchr(at + 11, '"') : nullptr;
    if (!at)
        return 0;
    const char *end = strchr(at + 1, '"');
    if (end)
        search->lang.assign(at + 1, end);
    return end ? 1 : 0;
}

} // namespace

uint32_t acp() {
    if (!g_page) {
        const char *v = recomp_env("ACP");
        g_page =
            page_of(v && *v ? (uint32_t)strtoul(v, nullptr, 10) : (uint32_t)RECOMP_ANSI_CODEPAGE);
    }
    return g_page;
}

void choose_for_install(const std::string &root) {
    if (const char *v = recomp_env("ACP"); v && *v)
        return;
    // The install's root, or the folder above the EXE's (bin/ beside it).
    InfoSearch search{root, {}};
    os_listdir(root.c_str(), scan_info, &search);
    if (search.lang.empty() && root.rfind('/') != std::string::npos && root.rfind('/') > 0) {
        search.dir = root.substr(0, root.rfind('/'));
        os_listdir(search.dir.c_str(), scan_info, &search);
    }
    const std::string &l = search.lang;
    auto is = [&](const char *code) {
        const size_t n = strlen(code);
        return l.compare(0, n, code) == 0 && (l.size() == n || l[n] == '-');
    };
    // English keeps the game's page (a Russian release's 1251 has long named
    // the profiles of players of the English one too); a Cyrillic language
    // takes 1251; any other UTF-8, which spells every name the player types.
    if (l.empty() || is("en"))
        return;
    g_page = is("ru") || is("uk") || is("be") || is("bg") || is("sr") ? 1251u : 65001u;
}

uint32_t resolve(uint32_t cp) {
    if (cp == 0 || cp == 3)
        return acp();
    if (cp == 1 || cp == 2)
        return 1252;
    return cp;
}

bool supported(uint32_t cp) {
    cp = resolve(cp);
    return cp == 1251 || cp == 1252 || cp == 437 || cp == 850 || cp == 1200 || cp == 65001;
}

uint16_t to_unicode(uint32_t cp, uint8_t b) {
    if (resolve(cp) == 65001 && b >= 0x80)
        return 0xfffd; // a piece of a character
    if (resolve(cp) != 1251 || b < 0x80)
        return b;
    return b >= 0xc0 ? (uint16_t)(0x0410 + (b - 0xc0)) : k1251[b - 0x80];
}

bool from_unicode(uint32_t cp, uint16_t w, uint8_t *b) {
    if (resolve(cp) == 65001) { // one byte is ASCII only
        *b = (uint8_t)w;
        return w < 0x80;
    }
    if (resolve(cp) != 1251) {
        if (w >= 256)
            return false;
        *b = (uint8_t)w;
        return true;
    }
    if (w < 0x80) {
        *b = (uint8_t)w;
        return true;
    }
    if (w >= 0x0410 && w <= 0x044f) {
        *b = (uint8_t)(0xc0 + (w - 0x0410));
        return true;
    }
    for (int i = 0; i < 64; ++i)
        if (k1251[i] == w && w != 0xfffd) {
            *b = (uint8_t)(0x80 + i);
            return true;
        }
    return false;
}

std::string ansi_to_utf8(const std::string &s) {
    if (acp() != 1251)
        return s;
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char ch : s)
        put_utf8(out, to_unicode(0, ch));
    return out;
}

std::string utf8_to_ansi(const std::string &s) {
    if (acp() != 1251)
        return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = c;
        size_t n = 1;
        if (c >= 0xf0 && i + 3 < s.size()) {
            cp = 0x10000;
            n = 4;
        } else if (c >= 0xe0 && i + 2 < s.size()) {
            cp = (uint32_t)(c & 0x0f) << 12 | (uint32_t)(s[i + 1] & 0x3f) << 6 | (s[i + 2] & 0x3f);
            n = 3;
        } else if (c >= 0xc0 && i + 1 < s.size()) {
            cp = (uint32_t)(c & 0x1f) << 6 | (s[i + 1] & 0x3f);
            n = 2;
        }
        uint8_t b;
        out.push_back(cp < 0x10000 && from_unicode(0, (uint16_t)cp, &b) ? (char)b : '?');
        i += n;
    }
    return out;
}

} // namespace win32cp
