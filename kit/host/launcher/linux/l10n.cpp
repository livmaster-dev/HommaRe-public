// l10n.cpp - see l10n.h.
#include "l10n.h"

#include "../../controls/json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace launcher::native {

// apple/launcher-strings.json, embedded by the build (cmake/EmbedFiles.cmake).
extern const unsigned char launcher_strings_json[];
extern const size_t launcher_strings_json_size;

namespace {

struct Catalog {
    std::vector<Language> languages;
    std::map<std::string, std::map<std::string, std::string>> strings;
};

const Catalog &catalog() {
    static const Catalog c = [] {
        Catalog out;
        controls::Json root;
        std::string error;
        const std::string text(reinterpret_cast<const char *>(launcher_strings_json),
                               launcher_strings_json_size);
        if (!controls::json_parse(text, &root, &error)) {
            fprintf(stderr, "[launcher] launcher-strings.json: %s\n", error.c_str());
            return out;
        }
        if (const controls::Json *langs = root.get("languages"))
            for (const controls::Json &l : langs->a)
                out.languages.push_back({l.str("code", ""), l.str("name", "")});
        if (const controls::Json *strings = root.get("strings"))
            for (const controls::Json::Member &m : strings->o)
                for (const controls::Json::Member &text : m.value.o)
                    if (text.value.type == controls::Json::String)
                        out.strings[m.key][text.key] = text.value.s;
        return out;
    }();
    return c;
}

bool known(const std::string &code) {
    for (const Language &l : catalog().languages)
        if (l.code == code)
            return true;
    return false;
}

std::string g_chosen;

std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

std::string fill(std::string text, const Args &args);

} // namespace

std::vector<std::pair<std::string, std::string>> preferred_languages() {
    std::vector<std::string> ids;
    auto get = [](const char *name) {
        const char *v = getenv(name);
        return std::string(v ? v : "");
    };
    std::string locale = get("LC_ALL");
    if (locale.empty())
        locale = get("LC_MESSAGES");
    if (locale.empty())
        locale = get("LANG");
    const std::string list = get("LANGUAGE");
    if (!list.empty() && !locale.empty() && locale != "C" && locale != "POSIX")
        for (size_t start = 0; start <= list.size();) {
            const size_t colon = std::min(list.find(':', start), list.size());
            ids.push_back(list.substr(start, colon - start));
            start = colon + 1;
        }
#ifdef _WIN32
    // Windows sets none of those: its display languages, most wanted first
    // ("ru-RU", "en-US"), unless the environment names one.
    ULONG count = 0, size = 0;
    if (locale.empty() && GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) &&
        size) {
        std::wstring names(size, L'\0');
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, names.data(), &size))
            for (const wchar_t *n = names.c_str(); *n; n += wcslen(n) + 1)
                ids.push_back(std::string(n, n + wcslen(n))); // tags are ASCII
    }
#endif
    ids.push_back(locale);
    std::vector<std::pair<std::string, std::string>> out;
    for (std::string id : ids) {
        id = id.substr(0, id.find_first_of(".@"));
        if (id.empty() || id == "C" || id == "POSIX")
            continue;
        const size_t sep = id.find_first_of("_-");
        out.push_back({id.substr(0, sep), sep == std::string::npos ? "" : id.substr(sep + 1)});
    }
    return out;
}

namespace {

std::string fill(std::string text, const Args &args) {
    for (const auto &[name, value] : args) {
        const std::string mark = std::string("{") + name + "}";
        for (size_t at = text.find(mark); at != std::string::npos;
             at = text.find(mark, at + value.size()))
            text.replace(at, mark.size(), value);
    }
    return text;
}

} // namespace

const std::vector<Language> &languages() {
    return catalog().languages;
}

std::string pick_language(const std::vector<std::pair<std::string, std::string>> &preferred) {
    for (const auto &[lang, region] : preferred) {
        const std::string first = lower(lang);
        if (first == "zh") {
            const std::string r = lower(region);
            if (r == "cn" || r == "sg" || r == "hans" || r.compare(0, 5, "hans_") == 0 ||
                r.compare(0, 5, "hans-") == 0)
                return "zh-Hans";
            continue;
        }
        if (known(first))
            return first;
    }
    return "en";
}

std::string language() {
    // RECOMP_LAUNCHER_LANG=<code> picks one for screenshots and tests.
    if (const char *forced = getenv("RECOMP_LAUNCHER_LANG"); forced && known(forced))
        return forced;
    if (!g_chosen.empty() && known(g_chosen))
        return g_chosen;
    static const std::string system = pick_language(preferred_languages());
    return system;
}

const std::string &chosen_language() {
    return g_chosen;
}

void choose_language(const std::string &code) {
    g_chosen = code;
}

std::string t(const char *key, const Args &args) {
    const auto &strings = catalog().strings;
    auto it = strings.end();
#ifdef _WIN32
    it = strings.find(std::string(key) + ".windows"); // then the desktop's, as on Linux
#endif
    if (it == strings.end())
        it = strings.find(std::string(key) + ".linux");
    if (it == strings.end())
        it = strings.find(key);
    if (it == strings.end())
        return key;
    auto text = it->second.find(language());
    if (text == it->second.end())
        text = it->second.find("en");
    return fill(text == it->second.end() ? key : text->second, args);
}

std::string bytes(int64_t count) {
    // ByteCountFormatter's file style, as the Mac shows sizes: powers of 1000,
    // more digits the larger the unit.
    struct Units {
        const char *code, *unit[5];
        char point;
    };
    static const Units table[] = {
        {"en", {"bytes", "KB", "MB", "GB", "TB"}, '.'},
        {"ru", {"байт", "КБ", "МБ", "ГБ", "ТБ"}, ','},
        {"cs", {"B", "kB", "MB", "GB", "TB"}, ','},
        {"de", {"Byte", "KB", "MB", "GB", "TB"}, ','},
        {"es", {"bytes", "KB", "MB", "GB", "TB"}, ','},
        {"fr", {"octets", "ko", "Mo", "Go", "To"}, ','},
        {"it", {"byte", "KB", "MB", "GB", "TB"}, ','},
        {"hu", {"bájt", "kB", "MB", "GB", "TB"}, ','},
        {"pl", {"B", "KB", "MB", "GB", "TB"}, ','},
        {"zh-Hans", {"字节", "KB", "MB", "GB", "TB"}, '.'},
    };
    const std::string lang = language();
    const Units *u = &table[0];
    for (const Units &row : table)
        if (lang == row.code)
            u = &row;
    double v = double(count < 0 ? 0 : count);
    int unit = 0;
    while (v >= 1000 && unit < 4) {
        v /= 1000;
        ++unit;
    }
    static const int decimals[5] = {0, 0, 1, 2, 2};
    char num[64];
    snprintf(num, sizeof num, "%.*f", decimals[unit], v);
    std::string s = num;
    if (const size_t dot = s.find('.'); dot != std::string::npos) {
        // Trailing zeros go, as the Mac's formatter drops them.
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
        else
            s[dot] = u->point;
    }
    return s + " " + u->unit[unit];
}

std::string location(const std::string &path) {
#ifdef _WIN32
    return path; // a Windows user knows C:/Users/<name>, not ~
#endif
    const char *home = getenv("HOME");
    const std::string h = home ? home : "";
    if (!h.empty() && h != "/" && path.compare(0, h.size() + 1, h + "/") == 0)
        return "~" + path.substr(h.size());
    return path;
}

std::string file_of(int done, int total) {
    return t("fileOf", {{"done", std::to_string(done)}, {"total", std::to_string(total)}});
}

std::string progress_of(int64_t done, int64_t total) {
    return t("progressOf", {{"done", bytes(done)}, {"total", bytes(total)}});
}

std::string interrupted_detail(const std::string &setup) {
    return t("interruptedDetail", {{"setup", setup.substr(setup.find_last_of('/') + 1)}});
}

std::string wrong_version_detail(const std::string &digest) {
    return t("wrongVersionDetail", {{"digest", digest.substr(0, 12)}});
}

std::string incomplete_detail(const std::vector<std::string> &missing) {
    std::string list;
    for (const std::string &m : missing)
        list += (list.empty() ? "" : ", ") + m;
    return t("incompleteDetail", {{"list", list}});
}

std::string verify_failed(const std::vector<std::string> &paths) {
    std::string list;
    for (size_t i = 0; i < paths.size() && i < 3; ++i)
        list += (i ? ", " : "") + paths[i];
    if (paths.size() > 3)
        list += "…";
    return t("verifyFailed", {{"count", std::to_string(paths.size())}, {"list", list}});
}

std::string no_space(int64_t need, int64_t free) {
    return t("noSpace", {{"need", bytes(need)}, {"free", bytes(free)}});
}

} // namespace launcher::native
