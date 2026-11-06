// text.cpp - see text.h.
#include "text.h"

#include "../../../platform/os.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>

// Static, as the runtime's GDI has its own copy; most of it goes unused here.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../../third_party/stb/stb_truetype.h"
#pragma GCC diagnostic pop

namespace gdi { // the kit's Open Sans, embedded for the runtime (third_party/fonts/opensans)
extern const unsigned char bundled_sans_regular[];
extern const size_t bundled_sans_regular_size;
extern const unsigned char bundled_sans_semibold[];
extern const size_t bundled_sans_semibold_size;
} // namespace gdi

namespace launcher::native {

namespace {

struct LoadedFont {
    std::vector<uint8_t> bytes; // empty for the embedded faces
    stbtt_fontinfo info{};
    std::string path;
    // The size it is drawn at against the design's face: the common faces
    // standing in for Cormorant Garamond are larger at the same size.
    float factor = 1;
};

// A face's files, best first, and its stand-in when there is none.
struct Candidate {
    const char *file;
    float factor;
};
struct FaceSpec {
    std::vector<Candidate> files;
    bool bold; // the kit's Open Sans stands in as Semibold
};

const FaceSpec &spec(Face face) {
    static const FaceSpec specs[] = {
        // Menu: Cormorant Garamond as in the design, else the nearest
        // common serif (scaled, as those are wider), as the Mac does.
        {{{"CormorantGaramond-BoldItalic.ttf", 1},
          {"CormorantGaramond-BoldItalic.otf", 1},
          {"LiberationSerif-BoldItalic.ttf", 0.84f},
          {"NotoSerif-BoldItalic.ttf", 0.8f},
          {"DejaVuSerif-BoldItalic.ttf", 0.76f},
          {"FreeSerifBoldItalic.ttf", 0.84f}},
         true},
        {{{"CormorantGaramond-MediumItalic.ttf", 1},
          {"CormorantGaramond-MediumItalic.otf", 1},
          {"LiberationSerif-Italic.ttf", 0.84f},
          {"NotoSerif-Italic.ttf", 0.8f},
          {"DejaVuSerif-Italic.ttf", 0.76f},
          {"FreeSerifItalic.ttf", 0.84f}},
         false},
        {{{"CormorantGaramond-SemiBold.ttf", 1},
          {"CormorantGaramond-SemiBold.otf", 1},
          {"LiberationSerif-Bold.ttf", 0.84f},
          {"NotoSerif-Bold.ttf", 0.8f},
          {"DejaVuSerif-Bold.ttf", 0.76f},
          {"FreeSerifBold.ttf", 0.84f}},
         true},
        // Label: PT Sans Bold (Fedora's pt-sans-fonts, Debian's fonts-paratype).
        {{{"PTSans-Bold.ttf", 1},
          {"PTS75F.ttf", 1},
          {"LiberationSans-Bold.ttf", 0.92f},
          {"NotoSans-Bold.ttf", 0.9f},
          {"DejaVuSans-Bold.ttf", 0.86f}},
         true},
        // Body: PT Serif Italic.
        {{{"PTSerif-Italic.ttf", 1},
          {"PTF56F.ttf", 1},
          {"LiberationSerif-Italic.ttf", 1.02f},
          {"NotoSerif-Italic.ttf", 0.95f},
          {"DejaVuSerif-Italic.ttf", 0.9f}},
         false},
        {{{"DejaVuSansMono.ttf", 1},
          {"LiberationMono-Regular.ttf", 1},
          {"NotoSansMono-Regular.ttf", 1}},
         false},
        {{}, false},
        {{}, true},
        {{{"LiberationSerif-Bold.ttf", 1},
          {"NotoSerif-Bold.ttf", 0.95f},
          {"DejaVuSerif-Bold.ttf", 0.9f}},
         true},
    };
    return specs[int(face)];
}

// CJK faces stb_truetype can draw, and faces with the symbols the others lack.
const Candidate kCjk[] = {{"DroidSansFallbackFull.ttf", 1}, {"DroidSansFallback.ttf", 1},
                          {"wqy-microhei.ttc", 1},          {"wqy-zenhei.ttc", 1},
                          {"NotoSansCJK-Regular.ttc", 1},   {"NotoSansCJKsc-Regular.otf", 1},
                          {"NotoSansSC-Regular.otf", 1},    {"SourceHanSansSC-Regular.otf", 1},
                          {"SourceHanSans-Regular.ttc", 1}};
const Candidate kSymbols[] = {
    {"DejaVuSans.ttf", 1}, {"LiberationSans-Regular.ttf", 1}, {"FreeSans.ttf", 1}};

std::string g_font_dir;
std::vector<std::unique_ptr<LoadedFont>> g_fonts;

std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

// Every font file in the app's font folder and the system's, by lower-case
// name; the first found wins.
const std::map<std::string, std::string> &font_files() {
    static const std::map<std::string, std::string> files = [] {
        std::map<std::string, std::string> out;
        std::vector<std::string> roots;
        if (!g_font_dir.empty())
            roots.push_back(g_font_dir);
#ifdef _WIN32
        if (const char *windows = getenv("WINDIR"); windows && *windows)
            roots.push_back(std::string(windows) + "/Fonts");
        if (const char *local = getenv("LOCALAPPDATA"); local && *local)
            roots.push_back(std::string(local) + "/Microsoft/Windows/Fonts"); // per-user installs
#endif
        const char *home = getenv("HOME");
        const char *data = getenv("XDG_DATA_HOME");
        if (data && *data)
            roots.push_back(std::string(data) + "/fonts");
        else if (home && *home)
            roots.push_back(std::string(home) + "/.local/share/fonts");
        if (home && *home)
            roots.push_back(std::string(home) + "/.fonts");
        roots.push_back("/usr/local/share/fonts");
        roots.push_back("/usr/share/fonts");
        std::function<void(const std::string &, int)> walk = [&](const std::string &dir,
                                                                 int depth) {
            std::vector<std::string> names;
            os_listdir(
                dir.c_str(),
                [](const char *name, void *user) {
                    if (name[0] != '.')
                        static_cast<std::vector<std::string> *>(user)->push_back(name);
                    return 0;
                },
                &names);
            std::sort(names.begin(), names.end());
            for (const std::string &n : names) {
                const std::string path = dir + "/" + n;
                OsStat st{};
                if (os_stat(path.c_str(), &st) != 0)
                    continue;
                if (st.is_dir) {
                    if (depth < 4)
                        walk(path, depth + 1);
                } else {
                    out.emplace(lower(n), path);
                }
            }
        };
        for (const std::string &r : roots)
            walk(r, 0);
        return out;
    }();
    return files;
}

LoadedFont *load_embedded(bool bold) {
    static LoadedFont *fonts[2] = {nullptr, nullptr};
    LoadedFont *&f = fonts[bold ? 1 : 0];
    if (f)
        return f;
    auto font = std::make_unique<LoadedFont>();
    const unsigned char *data = bold ? gdi::bundled_sans_semibold : gdi::bundled_sans_regular;
    if (!stbtt_InitFont(&font->info, data, stbtt_GetFontOffsetForIndex(data, 0)))
        return nullptr;
    // Open Sans is a wide face: a little smaller to sit with the others.
    font->factor = 0.94f;
    f = font.get();
    g_fonts.push_back(std::move(font));
    return f;
}

// The font in `path` (a collection's face whose name says Simplified Chinese
// where it holds several); nullptr when stb_truetype cannot read it.
LoadedFont *load_file(const std::string &path, float factor) {
    for (const auto &f : g_fonts)
        if (f->path == path)
            return f.get();
    auto font = std::make_unique<LoadedFont>();
    FILE *file = fopen(path.c_str(), "rb");
    if (!file)
        return nullptr;
    char buf[1 << 16];
    for (size_t n; (n = fread(buf, 1, sizeof buf, file)) > 0;)
        font->bytes.insert(font->bytes.end(), buf, buf + n);
    fclose(file);
    const unsigned char *data = font->bytes.data();
    const int count = std::max(1, stbtt_GetNumberOfFonts(data));
    int chosen = -1;
    for (int i = 0; i < count && chosen < 0; ++i) {
        stbtt_fontinfo info;
        if (!stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, i)))
            continue;
        int length = 0;
        const char *name =
            stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MICROSOFT,
                                    STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
        std::string family;
        for (int k = 1; name && k < length; k += 2) // UTF-16BE, ASCII names
            family.push_back(name[k]);
        if (count == 1 || family.find(" SC") != std::string::npos)
            chosen = i;
    }
    if (chosen < 0)
        chosen = 0;
    if (!stbtt_InitFont(&font->info, data, stbtt_GetFontOffsetForIndex(data, chosen)))
        return nullptr; // e.g. CFF2 outlines
    font->path = path;
    font->factor = factor;
    LoadedFont *f = font.get();
    g_fonts.push_back(std::move(font));
    return f;
}

LoadedFont *first_of(const Candidate *list, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        auto it = font_files().find(lower(list[i].file));
        if (it != font_files().end())
            if (LoadedFont *f = load_file(it->second, list[i].factor))
                return f;
    }
    return nullptr;
}

// The faces a face's characters come from, in turn.
const std::vector<LoadedFont *> &chain(Face face) {
    static std::vector<LoadedFont *> chains[int(Face::Count)];
    static bool ready[int(Face::Count)] = {};
    std::vector<LoadedFont *> &c = chains[int(face)];
    if (ready[int(face)])
        return c;
    ready[int(face)] = true;
    const FaceSpec &s = spec(face);
    if (LoadedFont *f = first_of(s.files.data(), s.files.size()))
        c.push_back(f);
    if (LoadedFont *f = load_embedded(s.bold))
        c.push_back(f);
    if (LoadedFont *f = first_of(kCjk, std::size(kCjk)))
        c.push_back(f);
    if (LoadedFont *f = first_of(kSymbols, std::size(kSymbols)))
        c.push_back(f);
    return c;
}

float pixel_scale(const LoadedFont *f, const Font &font) {
    return stbtt_ScaleForMappingEmToPixels(&f->info, font.size * f->factor);
}

struct Glyph {
    LoadedFont *font = nullptr;
    int index = 0;
};

Glyph glyph(Face face, uint32_t cp) {
    const std::vector<LoadedFont *> &c = chain(face);
    for (LoadedFont *f : c)
        if (int g = stbtt_FindGlyphIndex(&f->info, int(cp)))
            return {f, g};
    return {c.empty() ? nullptr : c.front(), 0};
}

struct Bitmap {
    std::vector<uint8_t> px;
    int w = 0, h = 0, x = 0, y = 0;
};

std::unordered_map<uint64_t, Bitmap> g_bitmaps;

const Bitmap &bitmap(const Glyph &g, float scale, float shift) {
    size_t font_index = 0;
    for (; font_index < g_fonts.size() && g_fonts[font_index].get() != g.font; ++font_index) {
    }
    const int sub = std::clamp(int(shift * 4), 0, 3);
    const uint64_t key = (uint64_t(font_index) << 56) |
                         (uint64_t(uint32_t(g.index) & 0xfffff) << 36) |
                         (uint64_t(uint32_t(scale * 65536.0f)) << 2) | uint64_t(sub);
    auto it = g_bitmaps.find(key);
    if (it != g_bitmaps.end())
        return it->second;
    if (g_bitmaps.size() > 20000)
        g_bitmaps.clear();
    Bitmap b;
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(&g.font->info, g.index, scale, scale, float(sub) / 4, 0, &x0,
                                    &y0, &x1, &y1);
    b.w = x1 - x0;
    b.h = y1 - y0;
    b.x = x0;
    b.y = y0;
    if (b.w > 0 && b.h > 0) {
        b.px.resize(size_t(b.w) * size_t(b.h));
        stbtt_MakeGlyphBitmapSubpixel(&g.font->info, b.px.data(), b.w, b.h, b.w, scale, scale,
                                      float(sub) / 4, 0, g.index);
    }
    return g_bitmaps.emplace(key, std::move(b)).first->second;
}

// Each character's glyph and its advance to the next (kerning and tracking in).
struct Laid {
    std::vector<Glyph> glyphs;
    std::vector<float> advance;
};

Laid lay(const std::vector<uint32_t> &cps, const Font &font) {
    Laid out;
    out.glyphs.reserve(cps.size());
    out.advance.reserve(cps.size());
    for (size_t i = 0; i < cps.size(); ++i) {
        const Glyph g = glyph(font.face, cps[i]);
        float adv = 0;
        if (g.font) {
            const float scale = pixel_scale(g.font, font);
            int advance = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&g.font->info, g.index, &advance, &lsb);
            adv = float(advance) * scale;
            if (i + 1 < cps.size()) {
                const Glyph next = glyph(font.face, cps[i + 1]);
                if (next.font == g.font)
                    adv += float(stbtt_GetGlyphKernAdvance(&g.font->info, g.index, next.index)) *
                           scale;
            }
        }
        out.glyphs.push_back(g);
        out.advance.push_back(adv + font.tracking);
    }
    return out;
}

float sum(const std::vector<float> &v, size_t from, size_t to) {
    float s = 0;
    for (size_t i = from; i < to; ++i)
        s += v[i];
    return s;
}

bool cjk(uint32_t cp) {
    return (cp >= 0x2e80 && cp <= 0x9fff) || (cp >= 0xac00 && cp <= 0xd7af) ||
           (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xff00 && cp <= 0xffef) ||
           (cp >= 0x20000 && cp <= 0x2fa1f);
}

// CJK punctuation that closes: never at the start of a line.
bool closing(uint32_t cp) {
    switch (cp) {
    case 0x3001:
    case 0x3002:
    case 0xff0c:
    case 0xff0e:
    case 0xff1a:
    case 0xff1b:
    case 0xff01:
    case 0xff1f:
    case 0xff09:
    case 0x300d:
    case 0x300f:
    case 0x3011:
        return true;
    default:
        return false;
    }
}

} // namespace

void set_font_dir(const std::string &dir) {
    g_font_dir = dir;
}

std::string face_file(Face face) {
    const std::vector<LoadedFont *> &c = chain(face);
    return c.empty() ? "" : c.front()->path;
}

std::vector<uint32_t> decode_utf8(const std::string &s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + size_t(n) >= s.size()) {
            out.push_back(0xfffd);
            ++i;
            continue;
        }
        uint32_t cp = n == 0 ? c : n == 1 ? (c & 0x1f) : n == 2 ? (c & 0x0f) : (c & 0x07);
        bool ok = true;
        for (int k = 1; k <= n; ++k) {
            const uint8_t d = uint8_t(s[i + size_t(k)]);
            ok = ok && (d >> 6) == 2;
            cp = (cp << 6) | (d & 0x3f);
        }
        out.push_back(ok ? cp : 0xfffd);
        i += ok ? size_t(n) + 1 : 1;
    }
    return out;
}

std::string encode_utf8(const std::vector<uint32_t> &cps) {
    std::string out;
    for (uint32_t cp : cps) {
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xc0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xe0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(char(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(char(0xf0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(char(0x80 | (cp & 0x3f)));
        }
    }
    return out;
}

float ascent(const Font &font) {
    const std::vector<LoadedFont *> &c = chain(font.face);
    if (c.empty())
        return font.size;
    int a, d, gap;
    stbtt_GetFontVMetrics(&c.front()->info, &a, &d, &gap);
    return float(a) * pixel_scale(c.front(), font);
}

float descent(const Font &font) {
    const std::vector<LoadedFont *> &c = chain(font.face);
    if (c.empty())
        return 0;
    int a, d, gap;
    stbtt_GetFontVMetrics(&c.front()->info, &a, &d, &gap);
    return float(-d) * pixel_scale(c.front(), font);
}

float line_height(const Font &font) {
    const std::vector<LoadedFont *> &c = chain(font.face);
    if (c.empty())
        return font.size * 1.2f;
    int a, d, gap;
    stbtt_GetFontVMetrics(&c.front()->info, &a, &d, &gap);
    return float(a - d + gap) * pixel_scale(c.front(), font);
}

float text_width(const std::string &utf8, const Font &font) {
    const Laid l = lay(decode_utf8(utf8), font);
    // The last letter's tracking is not part of the text.
    return std::max(0.0f,
                    sum(l.advance, 0, l.advance.size()) - (l.advance.empty() ? 0 : font.tracking));
}

float draw_text(Canvas &canvas, float x, float y, const std::string &utf8, const Font &font,
                Color color) {
    const std::vector<uint32_t> cps = decode_utf8(utf8);
    const Laid l = lay(cps, font);
    const int baseline = int(std::lround(y));
    float pen = x;
    for (size_t i = 0; i < cps.size(); ++i) {
        const Glyph &g = l.glyphs[i];
        if (g.font && cps[i] != ' ') {
            const float scale = pixel_scale(g.font, font);
            const int ix = int(std::floor(pen));
            const Bitmap &b = bitmap(g, scale, pen - float(ix));
            if (!b.px.empty())
                canvas.coverage(ix + b.x, baseline + b.y, b.w, b.h, b.px.data(), b.w, color);
        }
        pen += l.advance[i];
    }
    return pen - x;
}

std::vector<std::string> wrap_text(const std::string &utf8, float width, const Font &font) {
    std::vector<std::string> lines;
    const std::vector<uint32_t> all = decode_utf8(utf8);
    size_t start = 0;
    while (start <= all.size()) {
        size_t end = start;
        while (end < all.size() && all[end] != '\n')
            ++end;
        const std::vector<uint32_t> para(all.begin() + long(start), all.begin() + long(end));
        const Laid l = lay(para, font);
        size_t from = 0;
        while (from < para.size()) {
            // The furthest break that still fits.
            size_t brk = 0, i = from;
            float w = 0;
            for (; i < para.size(); ++i) {
                if (w + l.advance[i] - font.tracking > width && i > from)
                    break;
                w += l.advance[i];
                const bool space = para[i] == ' ';
                const bool next_cjk =
                    i + 1 < para.size() && cjk(para[i + 1]) && !closing(para[i + 1]);
                if (space ||
                    ((cjk(para[i]) || next_cjk) && !(i + 1 < para.size() && closing(para[i + 1]))))
                    brk = i + 1;
            }
            if (i >= para.size()) {
                lines.push_back(encode_utf8({para.begin() + long(from), para.end()}));
                break;
            }
            size_t cut = brk > from ? brk : std::max(i, from + 1);
            size_t keep = cut;
            while (keep > from && para[keep - 1] == ' ')
                --keep;
            lines.push_back(encode_utf8({para.begin() + long(from), para.begin() + long(keep)}));
            from = cut;
            while (from < para.size() && para[from] == ' ')
                ++from;
        }
        if (para.empty())
            lines.push_back("");
        start = end + 1;
    }
    return lines;
}

std::string fit_text(const std::string &utf8, float width, const Font &font, Cut cut) {
    if (text_width(utf8, font) <= width)
        return utf8;
    const std::vector<uint32_t> cps = decode_utf8(utf8);
    const Laid l = lay(cps, font);
    const float dots = text_width("…", font) + font.tracking;
    const float room = width - dots;
    if (cut == Cut::Tail) {
        size_t n = 0;
        float w = 0;
        while (n < cps.size() && w + l.advance[n] <= room)
            w += l.advance[n++];
        while (n > 0 && cps[n - 1] == ' ')
            --n;
        return encode_utf8({cps.begin(), cps.begin() + long(n)}) + "…";
    }
    // Middle: as much of each end as fits, the end a little favoured (a
    // path's file name).
    size_t head = 0, tail = cps.size();
    float w = 0;
    bool from_end = true, can_end = true, can_head = true;
    while (can_end || can_head) {
        if (from_end && can_end) {
            if (tail > head && w + l.advance[tail - 1] <= room)
                w += l.advance[--tail];
            else
                can_end = false;
        } else if (!from_end && can_head) {
            if (head < tail && w + l.advance[head] <= room)
                w += l.advance[head++];
            else
                can_head = false;
        }
        from_end = !from_end;
    }
    return encode_utf8({cps.begin(), cps.begin() + long(head)}) + "…" +
           encode_utf8({cps.begin() + long(tail), cps.end()});
}

std::string upper(const std::string &utf8) {
    std::vector<uint32_t> cps = decode_utf8(utf8);
    for (uint32_t &c : cps) {
        if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7) ||
            (c >= 0x3b1 && c <= 0x3c9 && c != 0x3c2) || (c >= 0x430 && c <= 0x44f))
            c -= 0x20;
        else if (c == 0xff)
            c = 0x178;
        else if (c >= 0x450 && c <= 0x45f)
            c -= 0x50;
        else if ((c >= 0x100 && c <= 0x137 && (c & 1)) || (c >= 0x139 && c <= 0x148 && !(c & 1)) ||
                 (c >= 0x14a && c <= 0x177 && (c & 1)) || (c >= 0x179 && c <= 0x17e && !(c & 1)))
            c -= 1;
    }
    return encode_utf8(cps);
}

} // namespace launcher::native
