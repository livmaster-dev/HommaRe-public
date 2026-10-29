// ui_mods.cpp - a game's interface mods made on the player's device (ui_mods.h).
//
// A recipe (tools/ui_recipe.py, which documents the format) names the paks
// the mod's files come from, in load order, and for each file the SHA-256 of
// the copy it was made from and the lines it changes; the few files a mod adds
// whole are written out in it. The mod is built here from the player's own
// paks, a file whose copy differs (another build of the game) is left out,
// and the result goes into the profile, the asset overlay's first tier, as
// game.toml's profile_files used to put a mod built from the developer's game.
// A stamp beside it (the recipe's and the paks' fingerprints) saves the work
// on the next start.
#include "ui_mods.h"

#include "../runtime/sha256.h"
#include "../third_party/miniz/miniz.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Edit {
    size_t at = 0, removed = 0;
    std::vector<std::string> lines;
};

struct File {
    std::string name, sha256, source;
    bool whole = false;
    bool turn_left = false; // the source, a picture, turned a quarter to the left
    std::string text;
    std::vector<Edit> edits;
};

struct Recipe {
    std::string name, out, date, over;
    std::vector<std::string> layers;
    std::vector<File> files;
    std::string digest; // of the recipe file
};

bool read_all(const fs::path &path, std::string &out) {
    FILE *f = fopen(path.string().c_str(), "rb");
    if (!f)
        return false;
    out.clear();
    char chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof chunk, f)) > 0;)
        out.append(chunk, n);
    fclose(f);
    return true;
}

bool write_all(const fs::path &path, const std::string &bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    FILE *f = fopen(path.string().c_str(), "wb");
    if (!f)
        return false;
    const bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return fclose(f) == 0 && ok;
}

std::vector<std::string> fields(const std::string &line) {
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        const size_t tab = line.find('\t', at);
        out.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
        if (tab == std::string::npos)
            return out;
        at = tab + 1;
    }
}

// The lines of `s`, each with its LF (a CR before it stays in the line).
std::vector<std::string> split_lines(const std::string &s) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < s.size()) {
        const size_t nl = s.find('\n', at);
        const size_t end = nl == std::string::npos ? s.size() : nl + 1;
        out.push_back(s.substr(at, end - at));
        at = end;
    }
    return out;
}

bool parse(const fs::path &path, Recipe &r) {
    std::string text;
    if (!read_all(path, text))
        return false;
    r.digest = sha256_hex(text.data(), text.size());
    for (const std::string &raw : split_lines(text)) {
        std::string line = raw;
        if (!line.empty() && line.back() == '\n')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        const char kind = line[0];
        if (kind == '+' || kind == '*' || kind == '=') {
            std::string put = line.substr(1) + (kind == '+' ? "\n" : kind == '*' ? "\r\n" : "");
            if (r.files.empty())
                return false;
            File &f = r.files.back();
            if (f.whole)
                f.text += put;
            else if (!f.edits.empty())
                f.edits.back().lines.push_back(std::move(put));
            else
                return false;
            continue;
        }
        const std::vector<std::string> v = fields(line);
        const std::string &key = v[0];
        if (key == "recipe") {
            if (v.size() < 2 || (v[1] != "1" && v[1] != "2"))
                return false; // a format this build does not know
        } else if (key == "name" && v.size() > 1) {
            r.name = v[1];
        } else if (key == "out" && v.size() > 1) {
            r.out = v[1];
        } else if (key == "date" && v.size() > 1) {
            r.date = v[1];
        } else if (key == "over" && v.size() > 1) {
            r.over = v[1];
        } else if (key == "layer" && v.size() > 1) {
            r.layers.push_back(v[1]);
        } else if (key == "file" && v.size() > 2) {
            File f;
            f.name = v[1];
            f.sha256 = v[2];
            f.source = v.size() > 3 ? v[3] : v[1];
            r.files.push_back(std::move(f));
        } else if (key == "new" && v.size() > 1) {
            File f;
            f.name = v[1];
            f.whole = true;
            r.files.push_back(std::move(f));
        } else if (key == "turn" && v.size() > 1 && v[1] == "left" && !r.files.empty() && !r.files.back().whole) {
            r.files.back().turn_left = true;
        } else if (key == "@" && v.size() > 2 && !r.files.empty() && !r.files.back().whole) {
            Edit e;
            e.at = strtoull(v[1].c_str(), nullptr, 10);
            e.removed = strtoull(v[2].c_str(), nullptr, 10);
            r.files.back().edits.push_back(std::move(e));
        } else {
            return false;
        }
    }
    return !r.name.empty() && !r.out.empty() && !r.layers.empty();
}

bool apply_edits(const std::string &source, const std::vector<Edit> &edits, std::string &out) {
    std::vector<std::string> lines = split_lines(source);
    for (auto e = edits.rbegin(); e != edits.rend(); ++e) {
        if (e->at > lines.size() || e->removed > lines.size() - e->at)
            return false;
        lines.erase(lines.begin() + (ptrdiff_t)e->at, lines.begin() + (ptrdiff_t)(e->at + e->removed));
        lines.insert(lines.begin() + (ptrdiff_t)e->at, e->lines.begin(), e->lines.end());
    }
    out.clear();
    for (const std::string &l : lines)
        out += l;
    return true;
}

// An uncompressed 32-bit DDS without mips turned a quarter to the left: the
// new row r is the old column width-1-r, top down (tools/ui_recipe.py
// turn_left).
bool turn_left(const std::string &dds, std::string &out) {
    auto u32at = [&](size_t at) {
        uint32_t v = 0;
        memcpy(&v, dds.data() + at, 4);
        return v;
    };
    if (dds.size() < 128 || dds.compare(0, 4, "DDS ") != 0)
        return false;
    const uint32_t height = u32at(12), width = u32at(16), bits = u32at(88);
    const uint64_t px = (uint64_t)width * height * 4;
    if (bits != 32 || dds.size() < 128 + px)
        return false;
    out = dds;
    memcpy(out.data() + 12, &width, 4);
    memcpy(out.data() + 16, &height, 4);
    char *to = out.data() + 128;
    for (uint32_t r = 0; r < width; ++r) {
        const uint32_t col = width - 1 - r;
        for (uint32_t y = 0; y < height; ++y, to += 4)
            memcpy(to, dds.data() + 128 + ((uint64_t)y * width + col) * 4, 4);
    }
    return true;
}

bool same_letters(const std::string &a, const std::string &b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return tolower((unsigned char)x) == tolower((unsigned char)y); });
}

// `rel` under `root`, each part matched without case where the exact one is
// missing (a copied install may spell DataLS as datals).
fs::path resolve(const fs::path &root, const std::string &rel) {
    fs::path at = root;
    std::error_code ec;
    for (const fs::path &part : fs::path(rel)) {
        fs::path exact = at / part;
        if (fs::exists(exact, ec)) {
            at = exact;
            continue;
        }
        bool found = false;
        for (const auto &e : fs::directory_iterator(at, ec)) {
            if (same_letters(e.path().filename().string(), part.string())) {
                at = e.path();
                found = true;
                break;
            }
        }
        if (!found)
            return exact;
    }
    return at;
}

using Files = std::map<std::string, std::string>;

uint32_t u16(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}
uint32_t u32(const unsigned char *p) {
    return u16(p) | u16(p + 2) << 16;
}

std::string lower(std::string s) {
    for (char &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

bool read_at(FILE *f, uint64_t at, void *buf, size_t n) {
    return fseeko(f, (off_t)at, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

// The wanted files (`want`: lower-cased name -> name) one pak holds, into
// `found`. The central directory is walked by its size, not its entry count:
// Tribes of the East's data.pak has more entries than a zip's 16-bit count
// holds (it wraps), which miniz's reader trusts.
void read_pak(const fs::path &path, const std::map<std::string, std::string> &want, Files &found) {
    FILE *f = fopen(path.string().c_str(), "rb");
    if (!f)
        return;
    std::vector<unsigned char> tail, cd;
    if (fseeko(f, 0, SEEK_END) == 0) {
        const uint64_t size = (uint64_t)ftello(f);
        const uint64_t span = std::min<uint64_t>(size, 65536 + 22);
        tail.resize((size_t)span);
        // The end-of-directory record, last in the file but for its comment.
        if (span >= 22 && read_at(f, size - span, tail.data(), tail.size())) {
            for (size_t i = tail.size() - 22 + 1; i-- > 0;) {
                if (u32(&tail[i]) != 0x06054b50)
                    continue;
                cd.resize(u32(&tail[i + 12]));
                if (!read_at(f, u32(&tail[i + 16]), cd.data(), cd.size()))
                    cd.clear();
                break;
            }
        }
    }
    for (size_t at = 0; at + 46 <= cd.size() && u32(&cd[at]) == 0x02014b50;) {
        const uint32_t method = u16(&cd[at + 10]), packed = u32(&cd[at + 20]), size = u32(&cd[at + 24]);
        const uint32_t name_len = u16(&cd[at + 28]), extra = u16(&cd[at + 30]), comment = u16(&cd[at + 32]);
        const uint32_t local = u32(&cd[at + 42]);
        if (at + 46 + name_len > cd.size())
            break;
        auto it = want.find(lower(std::string((const char *)&cd[at + 46], name_len)));
        at += 46 + name_len + extra + comment;
        if (it == want.end() || (method != 0 && method != 8))
            continue;
        unsigned char head[30];
        if (!read_at(f, local, head, sizeof head) || u32(head) != 0x04034b50)
            continue;
        std::vector<unsigned char> data(packed);
        if (!read_at(f, local + 30 + u16(head + 26) + u16(head + 28), data.data(), data.size()))
            continue;
        std::string bytes(size, '\0');
        if (method == 0) {
            if (packed != size)
                continue;
            memcpy(bytes.data(), data.data(), size);
        } else if (tinfl_decompress_mem_to_mem(bytes.data(), size, data.data(), data.size(), 0) != size) {
            continue;
        }
        found[it->second] = std::move(bytes);
    }
    fclose(f);
}

// The wanted files from the layers, a later pak's copy over an earlier one's
// (names matched without case, as the game matches them).
Files read_layers(const fs::path &root, const std::vector<std::string> &layers, const std::vector<std::string> &names) {
    std::map<std::string, std::string> want;
    for (const std::string &name : names)
        want[lower(name)] = name;
    Files found;
    for (const std::string &layer : layers)
        read_pak(resolve(root, layer), want, found);
    return found;
}

Files read_zip(const fs::path &path) {
    Files out;
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path.string().c_str(), 0))
        return out;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory)
            continue;
        size_t size = 0;
        void *p = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!p)
            continue;
        out[st.m_filename].assign((const char *)p, size);
        mz_free(p);
    }
    mz_zip_reader_end(&zip);
    return out;
}

// The game's pak reader takes an entry's sizes from its local header, so
// they go there (as host/text_overrides.cpp writes its pak).
struct MemoryFile {
    const std::string *bytes;
};
size_t read_memory(void *opaque, mz_uint64 ofs, void *buf, size_t n) {
    const std::string &b = *static_cast<MemoryFile *>(opaque)->bytes;
    if (ofs >= b.size())
        return 0;
    n = std::min(n, (size_t)(b.size() - ofs));
    memcpy(buf, b.data() + ofs, n);
    return n;
}

// "YYYY-MM-DD HH:MM:SS" as the local time the zip's DOS dates hold.
MZ_TIME_T parse_date(const std::string &s) {
    std::tm when{};
    if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &when.tm_year, &when.tm_mon, &when.tm_mday, &when.tm_hour,
               &when.tm_min, &when.tm_sec) != 6)
        return 0;
    when.tm_year -= 1900;
    when.tm_mon -= 1;
    when.tm_isdst = -1;
    return (MZ_TIME_T)std::mktime(&when);
}

std::string build_zip(const Files &files, MZ_TIME_T stamp) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        return {};
    bool ok = true;
    for (const auto &[name, bytes] : files) {
        MemoryFile file{&bytes};
        ok = ok && mz_zip_writer_add_read_buf_callback(
                       &zip, name.c_str(), read_memory, &file, bytes.size(), &stamp, nullptr, 0,
                       (mz_uint)MZ_DEFAULT_LEVEL | MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE | MZ_ZIP_FLAG_ASCII_FILENAME,
                       nullptr, 0, nullptr, 0);
    }
    void *buf = nullptr;
    size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
    std::string out = ok ? std::string((const char *)buf, size) : std::string();
    if (buf)
        mz_free(buf); // the finished archive is the caller's
    mz_zip_writer_end(&zip);
    return out;
}

// What the mod is made from: the recipe, the paks (size and date) and the
// mod it goes over.
std::string fingerprint(const Recipe &r, const fs::path &root, const std::string &over) {
    std::string key = "recipe " + r.digest + "\n";
    std::error_code ec;
    for (const std::string &layer : r.layers) {
        const fs::path p = resolve(root, layer);
        const auto size = fs::file_size(p, ec);
        const auto time = fs::last_write_time(p, ec).time_since_epoch().count();
        key += layer + " " + (ec ? std::string("-") : std::to_string(size) + " " + std::to_string((long long)time)) + "\n";
    }
    return key + "over " + over + "\n";
}

// "phone/x" -> ("x", true).
std::pair<std::string, bool> profile_path(const std::string &out) {
    if (out.compare(0, 6, "phone/") == 0)
        return {out.substr(6), true};
    return {out, false};
}

} // namespace

void host_ui_mods(const std::string &game_root, const std::string &profile_dir, const std::string &recipe_dir,
                  bool phone) {
    std::error_code ec;
    if (game_root.empty() || profile_dir.empty() || recipe_dir.empty() || !fs::is_directory(recipe_dir, ec))
        return;
    std::vector<Recipe> recipes;
    for (const auto &e : fs::directory_iterator(recipe_dir, ec)) {
        if (e.path().extension() != ".recipe")
            continue;
        Recipe r;
        if (parse(e.path(), r))
            recipes.push_back(std::move(r));
        else
            fprintf(stderr, "[host] ui mods: %s is not a recipe this build reads\n", e.path().string().c_str());
    }
    // A recipe over another comes after it.
    std::stable_sort(recipes.begin(), recipes.end(),
                     [](const Recipe &a, const Recipe &b) { return a.over.empty() && !b.over.empty(); });

    const fs::path profile(profile_dir), root(game_root);
    std::map<std::string, std::string> keys; // out -> fingerprint, for the recipes over it
    std::map<std::string, Files> made;       // out -> files, when built this start
    for (const Recipe &r : recipes) {
        const auto [rel, phone_only] = profile_path(r.out);
        const fs::path out = profile / rel;
        const fs::path stamp = profile / ".ui-mods" / (r.name + ".stamp");
        if (phone_only && !phone) {
            if (fs::remove(out, ec))
                fprintf(stderr, "[host] ui mods: %s taken out (phone interface off)\n", rel.c_str());
            fs::remove(stamp, ec);
            continue;
        }
        const std::string key = fingerprint(r, root, r.over.empty() ? std::string() : keys[r.over]);
        keys[r.out] = key;
        std::string old_key;
        if (fs::exists(out, ec) && read_all(stamp, old_key) && old_key == key)
            continue;

        Files over;
        if (!r.over.empty()) {
            auto it = made.find(r.over);
            over = it != made.end() ? it->second : read_zip(profile / profile_path(r.over).first);
        }
        std::vector<std::string> names;
        for (const File &f : r.files)
            if (!f.whole && !over.count(f.source))
                names.push_back(f.source);
        const Files layers = read_layers(root, r.layers, names);

        Files files;
        int left = 0;
        for (const File &f : r.files) {
            if (f.whole) {
                files[f.name] = f.text;
                continue;
            }
            // The mod it goes over first, as the generator took it.
            const std::string *src = nullptr;
            if (auto it = over.find(f.source); it != over.end())
                src = &it->second;
            else if (auto in = layers.find(f.source); in != layers.end())
                src = &in->second;
            std::string bytes, turned;
            bool fits = src && sha256_hex(src->data(), src->size()) == f.sha256;
            if (fits && f.turn_left) {
                fits = turn_left(*src, turned);
                src = &turned;
            }
            if (!fits || !apply_edits(*src, f.edits, bytes)) {
                ++left; // missing here, or another build of the game
                continue;
            }
            files[f.name] = std::move(bytes);
        }
        const std::string next = files.empty() ? std::string() : build_zip(files, parse_date(r.date));
        std::string current;
        if (next.empty()) {
            fs::remove(out, ec);
            fprintf(stderr, "[host] ui mods: %s: none of its %zu files fit this install\n", r.name.c_str(),
                    r.files.size());
        } else if (!(read_all(out, current) && current == next) && !write_all(out, next)) {
            fprintf(stderr, "[host] ui mods: could not write %s\n", out.string().c_str());
            continue;
        } else {
            const std::string note = left ? " (" + std::to_string(left) + " left out)" : "";
            fprintf(stderr, "[host] ui mods: %s: %zu files%s into %s\n", r.name.c_str(), files.size(), note.c_str(),
                    out.string().c_str());
        }
        write_all(stamp, key);
        made[r.out] = std::move(files);
    }
}

void host_ui_mods_retire(const std::string &profile_dir, const std::string &other_dir,
                         const std::string &kept_dir) {
    std::error_code ec;
    if (profile_dir.empty() || other_dir.empty() || !fs::is_directory(other_dir, ec))
        return;
    const fs::path profile(profile_dir);
    for (const auto &e : fs::directory_iterator(other_dir, ec)) {
        if (e.path().extension() != ".recipe" || fs::exists(fs::path(kept_dir) / e.path().filename(), ec))
            continue;
        Recipe r;
        if (!parse(e.path(), r) || r.out.empty())
            continue;
        const fs::path out = profile / profile_path(r.out).first;
        if (fs::remove(out, ec))
            fprintf(stderr, "[host] ui mods: %s taken out (the other device form's)\n", out.string().c_str());
        fs::remove(profile / ".ui-mods" / (r.name + ".stamp"), ec);
    }
}
