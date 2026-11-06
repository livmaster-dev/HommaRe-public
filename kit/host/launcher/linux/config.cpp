// config.cpp - see config.h.
#include "config.h"

#include "../../controls/json.h"
#include "l10n.h"

#include <cstdio>

namespace launcher::native {

namespace {

using controls::Json;

LocalizedText localized(const Json *v) {
    LocalizedText t;
    if (!v)
        return t;
    if (v->type == Json::String)
        t.values["en"] = v->s;
    for (const Json::Member &m : v->o)
        if (m.value.type == Json::String)
            t.values[m.key] = m.value.s;
    return t;
}

std::vector<std::string> strings(const Json *v) {
    std::vector<std::string> out;
    if (v)
        for (const Json &e : v->a)
            if (e.type == Json::String)
                out.push_back(e.s);
    return out;
}

} // namespace

std::string LocalizedText::text() const {
    auto it = values.find(language());
    if (it == values.end())
        it = values.find("en");
    if (it == values.end())
        it = values.begin();
    return it == values.end() ? "" : it->second;
}

bool read_text(const std::string &path, std::string *out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out->clear();
    char buf[1 << 16];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;)
        out->append(buf, n);
    fclose(f);
    return true;
}

bool parse_config(const std::string &text, Config *out, std::string *error) {
    Json root;
    if (!controls::json_parse(text, &root, error))
        return false;
    Config c;
    c.title = localized(root.get("title"));
    c.tagline = localized(root.get("tagline"));
    // "taglines": the tagline of each platform; this one's (Linux's, or
    // Windows' in the Windows app), when there is one, wins.
#ifdef _WIN32
    const char *platform = "windows";
#else
    const char *platform = "linux";
#endif
    if (const Json *taglines = root.get("taglines"))
        if (const Json *own = taglines->get(platform))
            c.tagline = localized(own);
    c.legal = localized(root.get("legal"));
    c.install_names = strings(root.get("installNames"));
    c.exclude = strings(root.get("exclude"));
    if (const Json *games = root.get("games"))
        for (const Json &g : games->a) {
            GameSpec s;
            s.id = g.str("id", "");
            s.module = g.str("module", "");
            s.title = localized(g.get("title"));
            s.subtitle = localized(g.get("subtitle"));
            s.blurb = localized(g.get("blurb"));
            s.year = g.str("year", "");
            s.version = g.str("version", "");
            s.gog_id = g.str("gogId", "");
            s.exe = strings(g.get("exe"));
            s.sha256 = strings(g.get("sha256"));
            s.required_dirs = strings(g.get("requiredDirs"));
            s.art = strings(g.get("art"));
            if (const Json *crop = g.get("artCrop"))
                for (const Json &v : crop->a)
                    s.art_crop.push_back(v.n);
            s.accent = g.str("accent", "");
            if (s.id.empty() || s.exe.empty()) {
                *error = "a game without an id or an executable";
                return false;
            }
            c.games.push_back(std::move(s));
        }
    if (const Json *installers = root.get("installers"))
        for (const Json &i : installers->a) {
            InstallerSpec s;
            s.id = i.str("id", "");
            s.gog_id = i.str("gogId", "");
            s.folder = i.str("folder", "");
            s.title = localized(i.get("title"));
            s.manifests = strings(i.get("manifests"));
            s.games = strings(i.get("games"));
            c.installers.push_back(std::move(s));
        }
    if (c.games.empty()) {
        *error = "no games";
        return false;
    }
    *out = std::move(c);
    return true;
}

bool parse_manifest(const std::string &text, Manifest *out) {
    Json root;
    std::string error;
    if (!controls::json_parse(text, &root, &error))
        return false;
    Manifest m;
    m.gog_id = root.str("gogId", "");
    m.installer = root.str("installer", "");
    const Json *files = root.get("files");
    if (!files)
        return false;
    for (const Json &f : files->a) {
        ManifestFile e;
        e.path = f.str("path", "");
        e.size = int64_t(f.num("size", -1));
        e.sha256 = f.str("sha256", "");
        if (e.path.empty() || e.size < 0 || e.sha256.size() != 64)
            return false;
        m.files.push_back(std::move(e));
    }
    *out = std::move(m);
    return true;
}

} // namespace launcher::native
