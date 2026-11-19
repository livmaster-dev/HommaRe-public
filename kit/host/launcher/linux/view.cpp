// view.cpp - see view.h.
#include "view.h"

#include "layout.h"
#include "l10n.h"
#include "text.h"
#include "version.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace launcher::native {

namespace {

Color rgb(uint32_t v, float a = 1) {
    return {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), uint8_t(std::lround(a * 255))};
}

Color rgbf(float r, float g, float b, float a = 1) {
    return {uint8_t(std::lround(r * 255)), uint8_t(std::lround(g * 255)),
            uint8_t(std::lround(b * 255)), uint8_t(std::lround(a * 255))};
}

Color alpha(Color c, float a) {
    c.a = uint8_t(std::lround(float(c.a) * a));
    return c;
}

// The game's look (SkinColor).
namespace skin_color {
const Color navy = rgb(0x1d2a55), navy_hover = rgb(0x3b57a8), gold = rgb(0xf6c94c),
            gold_hover = rgb(0xffe08a), gold_disabled = rgb(0x9c8045), gold_shadow = rgb(0x2a1a05),
            brown = rgb(0x6b3d12), ink = rgb(0x3a2a17), ink_soft = rgb(0x4a3a22),
            ink_muted = rgb(0x5a4529), soon = rgb(0x6f5d40), cream = rgb(0xf1e3c4),
            legal = rgb(0xd9c7a3), night = rgb(0x1a0f07), dialog_text = rgb(0xf6ead0),
            dialog_soft = rgb(0xeadcbc);
}

// The flat look (Theme).
namespace theme {
const Color ink = rgbf(0.035f, 0.04f, 0.055f), gold = rgbf(0.89f, 0.73f, 0.43f),
            gold_deep = rgbf(0.70f, 0.50f, 0.22f), text = rgbf(0.96f, 0.96f, 0.96f),
            dim = rgbf(0.70f, 0.70f, 0.70f), faint = rgbf(0.46f, 0.46f, 0.46f);
const Color stroke = rgbf(1, 1, 1, 0.12f);

Color accent(const std::string &hex) {
    if (hex.size() != 7 || hex[0] != '#')
        return gold;
    char *end = nullptr;
    const unsigned long v = strtoul(hex.c_str() + 1, &end, 16);
    return end && !*end ? rgb(uint32_t(v)) : gold;
}
} // namespace theme

// Layout units (points, or the skinned layout's own) onto the canvas's pixels.
struct Space {
    float ox = 0, oy = 0, s = 1;
    float x(float v) const {
        return ox + v * s;
    }
    float y(float v) const {
        return oy + v * s;
    }
    Rect r(float x0, float y0, float w, float h) const {
        const int l = int(std::lround(x(x0))), t = int(std::lround(y(y0)));
        return {l, t, int(std::lround(x(x0 + w))) - l, int(std::lround(y(y0 + h))) - t};
    }
};

struct Style {
    Style(Face face = Face::Sans, float size = 13, Color color = {255, 255, 255, 255},
          float tracking = 0, bool upper = false, float spacing = 0,
          std::optional<Color> shadow = {})
        : face(face), size(size), color(color), tracking(tracking), upper(upper), spacing(spacing),
          shadow(shadow) {}
    Face face;
    float size;
    Color color;
    float tracking;
    bool upper;
    float spacing; // between lines
    std::optional<Color> shadow;
};

std::string version_text() {
    std::string v = POP_RECOMP_VERSION;
    if (!v.empty() && v[0] == 'v')
        v.erase(0, 1);
    return v == "unknown" ? "" : v;
}

std::string joined(const std::vector<std::string> &parts, const char *sep) {
    std::string out;
    for (const std::string &p : parts)
        if (!p.empty())
            out += (out.empty() ? "" : sep) + p;
    return out;
}

// What the selected game's state means to the player (StatusInfo).
struct StatusInfo {
    std::string label, detail;
    Color color;
};

StatusInfo status_info(const Entry &e) {
    // The game's files are here but this app cannot play it yet.
    if (!e.playable_here && !e.interrupted() && e.state.kind != GameState::NotFound)
        return {t("portInTheWorks"), t("comingSoonDetail"), rgbf(0.62f, 0.66f, 0.95f)};
    const Color orange = rgbf(0.98f, 0.66f, 0.30f);
    switch (e.state.kind) {
    case GameState::Ready:
        return {t("ready"), e.root.empty() ? "" : location(e.root), rgbf(0.45f, 0.85f, 0.55f)};
    case GameState::WrongVersion:
        return {t("wrongVersion"), wrong_version_detail(e.state.digest), orange};
    case GameState::Incomplete:
        return {t("incomplete"), incomplete_detail(e.state.missing), orange};
    case GameState::Interrupted:
        return {t("interrupted"), interrupted_detail(e.state.setup), theme::gold};
    default:
#ifdef __ANDROID__
        // The installers come to the device with the Files app, as on an iPad.
        return {t("notFound"), t("notFoundDetail.iosInstaller"), theme::dim};
#else
        return {t("notFound"), t("notFoundDetail.mac"), theme::dim};
#endif
    }
}

// The same on the parchment (SkinStatus).
std::pair<std::string, Color> skin_status(const Entry &e) {
    if (!e.playable_here && !e.interrupted() && e.state.kind != GameState::NotFound)
        return {t("portInTheWorks"), rgb(0x7a4a12)};
    const std::string label = e.can_play() ? t("readyToPlay") : status_info(e).label;
    switch (e.state.kind) {
    case GameState::Ready:
        return {label, rgb(0x2f5a1c)};
    case GameState::NotFound:
        return {label, skin_color::ink_muted};
    case GameState::Interrupted:
        return {label, skin_color::brown};
    default:
        return {label, rgb(0x8a2e10)};
    }
}

} // namespace

// One frame being drawn.
struct View::Ui {
    View &v;
    Canvas &c;
    Model &m;
    float density;
    Space sp;
    Look look;
    const Skin *skin = nullptr;

    Font font(const Style &st) const {
        return {st.face, st.size * sp.s, st.tracking * sp.s};
    }
    std::string shown(const std::string &s, const Style &st) const {
        return st.upper ? upper(s) : s;
    }
    float width(const std::string &s, const Style &st) const {
        return text_width(shown(s, st), font(st)) / sp.s;
    }
    float line(const Style &st) const {
        return line_height(font(st)) / sp.s;
    }
    // One line with its top at y; `align` 0 leading, 0.5 centred, 1 trailing
    // within `w` (0: from x).
    void say(float x, float y, const std::string &s, const Style &st, float w = 0,
             float align = 0) {
        const Font f = font(st);
        const std::string text = shown(s, st);
        const float tw = text_width(text, f);
        const float px = sp.x(x) + (w > 0 ? (w * sp.s - tw) * align : 0);
        const float base = sp.y(y) + ascent(f);
        if (st.shadow)
            draw_text(c, px, base + std::max(1.0f, sp.s), text, f, *st.shadow);
        draw_text(c, px, base, text, f, st.color);
    }
    std::vector<std::string> wrap(const std::string &s, const Style &st, float w) const {
        return wrap_text(shown(s, st), w * sp.s, font(st));
    }
    float height(size_t lines, const Style &st) const {
        return lines ? float(lines) * line(st) + float(lines - 1) * st.spacing : 0;
    }
    void lines(float x, float y, float w, const std::vector<std::string> &ls, const Style &st,
               float align = 0) {
        Style plain = st;
        plain.upper = false; // wrapped already shown
        for (size_t i = 0; i < ls.size(); ++i)
            say(x, y + float(i) * (line(st) + st.spacing), ls[i], plain, w, align);
    }
    // The text cut to `w` with an ellipsis.
    std::string fit(const std::string &s, const Style &st, float w, Cut cut = Cut::Tail) const {
        return fit_text(shown(s, st), w * sp.s, font(st), cut);
    }
    // A one-line text shrunk (down to `least` of its size) to fit `w`.
    Style shrink(const std::string &s, Style st, float w, float least) const {
        const float full = width(s, st);
        // A little under the exact share: pixel rounding must not wrap it.
        if (full > w && full > 0)
            st.size *= std::max(least, w / full * 0.98f);
        return st;
    }

    bool hovered(const std::string &key) const {
        return v.hover_ == key;
    }
    bool pressed(const std::string &key) const {
        return v.pressed_ == key && v.hover_ == key;
    }
    void hit(Rect r, const std::string &key, std::function<void()> action, bool enabled = true) {
        v.regions_.push_back({r, key, std::move(action), enabled});
    }
    void hit(float x, float y, float w, float h, const std::string &key,
             std::function<void()> action, bool enabled = true) {
        hit(sp.r(x, y, w, h), key, std::move(action), enabled);
    }

    void picture(const Image &img, float x, float y, float w, float h, int opacity = 255,
                 Blend b = Blend::Over) {
        c.image(img, sp.r(x, y, w, h), opacity, b);
    }
    void nine(const Image &img, float x, float y, float w, float h, int top, int left, int bottom,
              int right, int opacity = 255, Blend b = Blend::Over, bool middle = true) {
        c.nine_slice(img, sp.r(x, y, w, h), top, left, bottom, right, sp.s, opacity, b, middle);
    }
    // `img` filling the box, cut to its shape; `align` 0 keeps its top, 0.5 its middle.
    void cover(const Image &img, Rect dst, float align = 0, int opacity = 255) {
        if (img.empty() || dst.w <= 0 || dst.h <= 0)
            return;
        const float da = float(dst.w) / float(dst.h), ia = float(img.w) / float(img.h);
        Rect src{0, 0, img.w, img.h};
        if (ia > da) {
            src.w = std::max(1, int(float(img.h) * da));
            src.x = (img.w - src.w) / 2;
        } else {
            src.h = std::max(1, int(float(img.w) / da));
            src.y = int(float(img.h - src.h) * align);
        }
        c.image(img, dst, opacity, Blend::Over, src);
    }

    // The soft shadow a shape casts, `radius` points of blur, `dy` down:
    // `shape` draws the shape (white) into a canvas at the box given.
    void cast(const std::string &key, Rect box, float radius, float dy, float opacity,
              const std::function<void(Canvas &, Rect)> &shape) {
        const int r = std::max(1, int(radius * sp.s));
        const std::string k = key + ":" + std::to_string(box.w) + "x" + std::to_string(box.h) +
                              ":" + std::to_string(r);
        auto it = v.cache_.find(k);
        if (it == v.cache_.end()) {
            Canvas tmp;
            tmp.resize(box.w + 4 * r, box.h + 4 * r, false);
            shape(tmp, {2 * r, 2 * r, box.w, box.h});
            tmp.blur({0, 0, tmp.width(), tmp.height()}, std::max(1, r / 2));
            Image img;
            img.w = tmp.width();
            img.h = tmp.height();
            img.px.assign(size_t(img.w) * size_t(img.h) * 4, 0);
            for (size_t i = 0; i < size_t(img.w) * size_t(img.h); ++i)
                img.px[i * 4 + 3] = tmp.pixels()[i * 4];
            it = v.cache_.emplace(k, std::move(img)).first;
        }
        const int oy = int(std::lround(dy * sp.s));
        c.image(it->second, {box.x - 2 * r, box.y - 2 * r + oy, it->second.w, it->second.h},
                int(255 * opacity));
    }
    // A picture's shape in white, for its shadow.
    const Image &white(const Image &img) {
        auto it = v.white_.find(&img);
        if (it != v.white_.end() && it->second.w == img.w && it->second.h == img.h)
            return it->second;
        Image w = img;
        for (size_t i = 0; i < w.px.size(); i += 4)
            w.px[i] = w.px[i + 1] = w.px[i + 2] = w.px[i + 3];
        return v.white_[&img] = std::move(w);
    }
};

namespace {

using Ui = View::Ui;

// ---------------------------------------------------------------------------
// The game's look
// ---------------------------------------------------------------------------

enum class Kind { Large, Small, Dialog };

struct ButtonMetrics {
    float font, height, min_width;
    int v, h; // cap insets, the design's border-image slices
    Tex normal, highlight, pushed, disabled;
};

ButtonMetrics metrics(Kind k) {
    switch (k) {
    case Kind::Large:
        return {21,
                58,
                184,
                20,
                24,
                Tex::Button,
                Tex::ButtonHighlight,
                Tex::ButtonPushed,
                Tex::ButtonDisabled};
    case Kind::Small:
        return {14,
                38,
                130,
                14,
                18,
                Tex::SmallButton,
                Tex::SmallButtonHighlight,
                Tex::SmallButtonPushed,
                Tex::SmallButtonDisabled};
    default:
        return {16,
                50,
                140,
                20,
                22,
                Tex::OkButton,
                Tex::OkButtonHighlight,
                Tex::OkButtonPushed,
                Tex::OkButtonDisabled};
    }
}

Style button_style(Kind k) {
    return {Face::Label, metrics(k).font, skin_color::gold};
}

float button_width(const Ui &u, Kind k, const std::string &label) {
    const ButtonMetrics m = metrics(k);
    return std::max(m.min_width, u.width(label, button_style(k)) + 2 * float(m.h));
}

// The game's button: its face for the state, its Highlight added on hover.
void skin_button(Ui &u, Kind k, float x, float y, float w, const std::string &label,
                 const std::string &key, std::function<void()> action, bool enabled = true) {
    const ButtonMetrics m = metrics(k);
    const Skin &skin = *u.skin;
    const bool hover = enabled && u.hovered(key), down = enabled && u.pressed(key);
    const Tex face = !enabled ? m.disabled : down ? m.pushed : m.normal;
    u.nine(skin[face], x, y, w, m.height, m.v, m.h, m.v, m.h);
    if (hover && !down)
        u.nine(skin[m.highlight], x, y, w, m.height, m.v, m.h, m.v, m.h, 255, Blend::Add);
    Style st = button_style(k);
    st.color = !enabled ? skin_color::gold_disabled
               : hover  ? skin_color::gold_hover
                        : skin_color::gold;
    st.shadow = skin_color::gold_shadow;
    const std::string text = u.fit(label, st, w - 2 * float(m.h));
    const float ty = y + (m.height - u.line(st)) / 2 + (down ? 1 : 0);
    u.say(x, ty, text, st, w, 0.5f);
    u.hit(x, y, w, m.height, key, std::move(action), enabled);
}

// A brown rule that fades out at both ends.
void divider(Ui &u, float x, float y, float w) {
    const Color c = skin_color::brown;
    u.c.gradient(u.sp.r(x, y, w, 2), {{0, alpha(c, 0)}, {0.5f, alpha(c, 0.55f)}, {1, alpha(c, 0)}},
                 true);
}

// The flat look's stand-in for a game's splash before its files are here.
void placeholder(Canvas &c, Rect r, Color accent) {
    c.gradient(r, {{0, alpha(accent, 0.35f)}, {1, theme::ink}});
    c.fill(r, alpha(theme::ink, 0)); // the gradient lies over what was there
    c.radial(r, float(r.x) + float(r.w) * 0.5f, float(r.y) + float(r.h) * 0.25f,
             260.0f * float(r.h) / 400.0f, alpha(accent, 0.45f), alpha(accent, 0));
}

// The selected game's splash in the dark-box frame.
void skin_art(Ui &u, const Entry &e, float x, float y, float w, float h) {
    const Skin &skin = *u.skin;
    const Rect box = u.sp.r(x, y, w, h);
    u.c.shadow({box.x, box.y + int(10 * u.sp.s), box.w, box.h}, int(15 * u.sp.s),
               rgbf(0, 0, 0, 0.55f));
    u.c.fill(box, rgb(0x120a05));
    const Rect inner = u.sp.r(x + 8, y + 8, w - 16, h - 16);
    if (e.art)
        u.cover(*e.art, inner, 0);
    else
        placeholder(u.c, inner, theme::accent(e.spec.accent));
    u.nine(skin[Tex::DarkBox], x, y, w, h, 8, 8, 8, 8, 255, Blend::Over, false);
}

void skin_soon_tag(Ui &u, float x, float y, const std::string &label) {
    const Style st{Face::Label, 11, skin_color::gold, 0.9f, true};
    const float w = u.width(label, st) + 20, h = u.line(st) + 10;
    u.nine((*u.skin)[Tex::DarkBox], x, y, w, h, 6, 6, 6, 6);
    u.say(x + 10, y + 5, label, st);
}

float soon_tag_width(const Ui &u, const std::string &label) {
    return u.width(label, {Face::Label, 11, skin_color::gold, 0.9f, true}) + 20;
}

// A setting's choice: the options page's check box and its name.
float check_width(const Ui &u, const std::string &title, float size) {
    return 27 + 12 + u.width(title, {Face::Menu, size});
}

float check_height(const Ui &u, float size) {
    return std::max(25.0f, u.line({Face::Menu, size})) + 8;
}

void skin_check(Ui &u, float x, float y, const std::string &title, bool on, float size,
                const std::string &key, std::function<void()> action) {
    const Skin &skin = *u.skin;
    const float h = check_height(u, size);
    const Image &box = skin[on ? Tex::CheckOn : Tex::CheckOff];
    u.picture(box, x, y + (h - 25) / 2, float(box.w), 25);
    const Style st{Face::Menu, size, u.hovered(key) ? skin_color::navy_hover : skin_color::navy};
    u.say(x + 27 + 12, y + (h - u.line(st)) / 2, title, st);
    u.hit(x, y, check_width(u, title, size), h, key, std::move(action));
}

// The selected game on its parchment strip, with Play and the folder
// actions; its height (drawn only with `draw`).
float skin_details(Ui &u, const Entry &e, float x, float y, float w, bool draw) {
    const StatusInfo info = status_info(e);
    const auto status = skin_status(e);
    const float iw = w - 80, text_w = std::min(iw, 690.0f);
    float cy = y + 32;
    std::vector<std::string> meta_parts = {e.spec.subtitle.text(), e.spec.year};
    if (!e.spec.version.empty())
        meta_parts.push_back(t("version") + " " + e.spec.version);
    const Style meta{Face::Label, 12, skin_color::brown, 1.7f, true};
    const std::string meta_text = joined(meta_parts, "  ·  ");
    std::vector<std::function<void()>> paint;
    if (draw && !meta_text.empty())
        u.say(x + 40, cy, u.fit(meta_text, meta, iw), {Face::Label, 12, skin_color::brown, 1.7f});
    cy += u.line(meta) + 8;
    const Style title = u.shrink(e.spec.title.text(), {Face::Menu, 38, skin_color::navy}, iw, 0.6f);
    if (draw)
        u.say(x + 40, cy, u.fit(e.spec.title.text(), title, iw), title);
    cy += u.line(title);
    if (const std::string blurb = e.spec.blurb.text(); !blurb.empty()) {
        const Style st{Face::Body, 15, skin_color::ink, 0, false, 3};
        const auto ls = u.wrap(blurb, st, text_w);
        cy += 8;
        if (draw)
            u.lines(x + 40, cy, text_w, ls, st);
        cy += u.height(ls.size(), st);
    }
    if (!e.can_play() && !info.detail.empty()) {
        const Style st{Face::Body, 13, skin_color::ink_muted};
        const auto ls = u.wrap(info.detail, st, text_w);
        cy += 8;
        if (draw)
            u.lines(x + 40, cy, text_w, ls, st);
        cy += u.height(ls.size(), st);
    }
    cy += 8 + 6 + 8;
    // The buttons, then the state at the trailing edge.
    std::string primary;
    std::function<void()> act;
    bool enabled = true;
    Model &m = u.m;
    if (installs(e)) {
        primary = t("installFromGog");
        act = [&m] { m.pick_installer(); };
    } else if (e.interrupted()) {
        primary = t("resumeInstall");
        act = [&m] { m.resume_install(); };
    } else {
        primary = e.playable_here ? t("play") : t("portInTheWorks");
        act = [&m] { m.play(); };
        enabled = e.can_play();
    }
    const float pw = button_width(u, Kind::Large, primary);
    const float fw = button_width(u, Kind::Small, t("chooseFolder"));
    const bool saves = !m.profile_dir().empty() && Model::can_reveal();
    const float sw = saves ? button_width(u, Kind::Small, t("saves")) : 0;
    const Style label{Face::Label, 13, status.second};
    const Style mono{Face::Mono, 12, skin_color::ink_muted};
    const bool path = e.can_play() && !e.root.empty();
    const float status_h = u.line(label) + (path ? 2 + u.line(mono) : 0);
    const float row_h = std::max(58.0f, status_h);
    if (draw) {
        float bx = x + 40;
        const float by = cy + (row_h - 58) / 2;
        skin_button(u, Kind::Large, bx, by, pw, primary, "primary", act, enabled);
        if (enabled)
            u.v.default_action_ = act;
        bx += pw + 12;
        skin_button(u, Kind::Small, bx, cy + (row_h - 38) / 2, fw, t("chooseFolder"), "folder",
                    [&m] { m.pick_folder(); });
        bx += fw + 12;
        if (saves) {
            skin_button(u, Kind::Small, bx, cy + (row_h - 38) / 2, sw, t("saves"), "saves",
                        [&m] { m.reveal_saves(); });
            bx += sw + 12;
        }
        const float right = x + 40 + iw, room = std::max(40.0f, right - (bx + 8));
        float sy = cy + (row_h - status_h) / 2;
        u.say(right - room, sy, u.fit(status.first, label, room), label, room, 1);
        sy += u.line(label) + 2;
        if (path) {
            const std::string where = u.fit(location(e.root), mono, room, Cut::Middle);
            const Style shown{Face::Mono, 12,
                              u.hovered("path") ? skin_color::ink : skin_color::ink_muted};
            u.say(right - room, sy, where, shown, room, 1);
            const float pw2 = u.width(where, mono);
            u.hit(right - pw2, sy, pw2, u.line(mono), "path", [&m] { m.reveal_selected(); });
        }
    }
    cy += row_h + 32;
    return std::max(264.0f, cy - y);
}

// The campaign scroll: the games, then the menu.
void skin_scroll(Ui &u, float x, float y, float w, float h) {
    const Skin &skin = *u.skin;
    Model &m = u.m;
    const Rect box = u.sp.r(x, y, w, h);
    u.cast("scroll", box, 16, 12, 0.55f, [&](Canvas &c, Rect r) {
        c.nine_slice(u.white(skin[Tex::Scroll]), r, 34, 14, 46, 14, u.sp.s);
    });
    u.nine(skin[Tex::Scroll], x, y, w, h, 34, 14, 46, 14);
    const float ix = x + 28, iw = w - 56;
    float cy = y + 40 + 8;
    const Style caps{Face::Caps, 15, skin_color::brown, 3, true};
    u.say(ix + 16, cy, u.fit(t("chooseGame"), caps, iw - 32),
          {Face::Caps, 15, skin_color::brown, 3});
    cy += u.line(caps) + 10;
    for (const Entry &e : m.entries()) {
        // Not installed (or, with its files here, not playable in this app).
        const bool soon =
            e.state.kind == GameState::NotFound || (!e.playable_here && !e.interrupted());
        const std::string tag =
            e.state.kind == GameState::NotFound ? t("notFound") : t("portInTheWorks");
        const float tag_w = soon ? soon_tag_width(u, tag) + 10 : 0;
        const std::string key = "game:" + e.spec.id;
        Style title{Face::Menu, 27, soon ? skin_color::soon : skin_color::navy};
        const float title_w = iw - 32 - tag_w;
        // One line where three quarters of the size or more fits it beside
        // the tag (the common faces standing in for Cormorant run wider),
        // two otherwise.
        if (u.width(e.spec.title.text(), title) * 0.75f <= title_w)
            title = u.shrink(e.spec.title.text(), title, title_w, 0.75f);
        std::vector<std::string> tl = u.wrap(e.spec.title.text(), title, title_w);
        if (tl.size() > 2) {
            tl.resize(2);
            tl[1] = u.fit(tl[1] + "…", title, title_w);
        }
        const Style sub{Face::MenuLight, 17, skin_color::ink_soft};
        const std::string sub_text = joined({e.spec.subtitle.text(), e.spec.year}, " · ");
        const float row_h = 10 + u.height(tl.size(), title) + 2 + u.line(sub) + 12;
        const bool selected = e.spec.id == m.selected_id();
        // Both are light the game adds onto the scroll; the selection softer
        // than in the game, whose list sits on a darker panel.
        if (selected)
            u.picture(skin[Tex::RowSelected], ix, cy, iw, row_h, 102, Blend::Add);
        else if (u.hovered(key))
            u.picture(skin[Tex::RowHover], ix, cy, iw, row_h, 255, Blend::Add);
        u.lines(ix + 16, cy + 10, title_w, tl, title);
        if (soon) {
            const float lw = u.width(tl.empty() ? "" : tl.back(), title);
            skin_soon_tag(u, ix + 16 + lw + 10,
                          cy + 10 + u.height(tl.size(), title) - u.line(title) +
                              (u.line(title) - u.line({Face::Label, 11}) - 10) / 2,
                          tag);
        }
        u.say(ix + 16, cy + 10 + u.height(tl.size(), title) + 2, u.fit(sub_text, sub, iw - 32),
              sub);
        const std::string id = e.spec.id;
        u.hit(ix, cy, iw, row_h, key, [&m, id] { m.select(id); });
        cy += row_h;
    }
    // The menu at the bottom of the scroll.
    struct Link {
        std::string title, key;
        std::function<void()> action;
    };
    View &v = u.v;
    const std::vector<Link> links = {
        {t("installFromGog"), "menu:install", [&m] { m.pick_installer(); }},
        {t("settings"), "menu:settings", [&v] { v.key(SDLK_COMMA, SDL_KMOD_CTRL); }},
        {t("quit"), "menu:quit", [&m] { m.quit(); }}};
    const Style link{Face::Menu, 25, skin_color::navy};
    const float link_h = u.line(link) + 8;
    float ly = y + h - 50 - float(links.size()) * link_h;
    divider(u, ix + 16, ly - 12, iw - 32);
    for (const Link &l : links) {
        const bool hover = u.hovered(l.key);
        Style st = link;
        st.color = hover ? skin_color::navy_hover : skin_color::navy;
        const std::string text = u.fit(l.title, st, iw - 32);
        if (hover) // a warm glow round the hovered line
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    if (dx || dy)
                        u.say(ix + 16 + float(dx) * 1.5f, ly + 4 + float(dy) * 1.5f, text,
                              {Face::Menu, 25, rgbf(1, 0.925f, 0.667f, 0.35f)});
        u.say(ix + 16, ly + 4, text, st);
        u.hit(ix, ly, iw, link_h, l.key, l.action);
        ly += link_h;
    }
}

// The tagline and version, the search under way and the legal line; its height.
float skin_footer(Ui &u, float x, float y, float w, bool draw) {
    const std::string tagline = joined({u.m.config().tagline.text(), version_text()}, " · ");
    const Style tag{Face::Menu, 19, skin_color::cream, 0, false, 0, rgbf(0, 0, 0)};
    const Style searching{Face::Body, 13, skin_color::legal, 0, false, 0, rgbf(0, 0, 0)};
    const Style legal{Face::Label, 11, skin_color::legal, 0, false, 1, rgbf(0, 0, 0)};
    const std::string legal_text =
        u.m.config().legal.empty() ? t("defaultLegal") : u.m.config().legal.text();
    float used = u.width(tagline, tag) + 24;
    if (u.m.scanning())
        used += u.width(t("searching"), searching) + 24;
    const float legal_w = std::clamp(w - used, 220.0f, 420.0f);
    const auto ls = u.wrap(legal_text, legal, legal_w);
    const float lh = u.height(ls.size(), legal);
    const float h = std::max(u.line(tag), lh);
    if (draw) {
        // The last lines' baselines meet.
        const float base = y + h - (lh > 0 ? u.line(legal) - ascent(u.font(legal)) / u.sp.s : 0);
        const float tag_top = base - ascent(u.font(tag)) / u.sp.s;
        const float tag_w = std::max(0.0f, w - legal_w - 24);
        u.say(x, tag_top, u.fit(tagline, tag, tag_w), tag);
        if (u.m.scanning()) {
            const float sx = x + u.width(tagline, tag) + 24, room = w - legal_w - 24 - (sx - x);
            if (room > 40)
                u.say(sx, base - ascent(u.font(searching)) / u.sp.s,
                      u.fit(t("searching"), searching, room), searching);
        }
        u.lines(x + w - legal_w, y + h - lh, legal_w, ls, legal, 1);
    }
    return h;
}

// Laid out at no less than 1200 x 760 and scaled down to fit (Look).
void skin_wide(Ui &u) {
    const Skin &skin = *u.skin;
    Model &m = u.m;
    const float W = u.look.width, H = u.look.height;
    const float content = W - 76, right = 392;
    const float left = std::max(300.0f, std::min(900.0f, content - right - 36));
    const float x0 = 40 + std::max(0.0f, (content - (left + 36 + right)) / 2);
    const float top = 28, bottom = H - 16;
    // The game's logo, cut to its opaque pixels, fitted at the leading edge.
    const Image &logo = skin[Tex::Logo];
    float logo_h = 0;
    if (!logo.empty()) {
        const float k = std::min(384.0f / float(logo.w), 101.0f / float(logo.h));
        const float lw = float(logo.w) * k;
        logo_h = float(logo.h) * k;
        const Rect box = u.sp.r(x0, top, lw, logo_h);
        u.cast("logo", box, 8, 4, 0.6f, [&](Canvas &c, Rect r) { c.image(u.white(logo), r); });
        u.c.image(logo, box);
    }
    float y = top + logo_h + 21;
    const Entry *e = m.selected();
    const float footer_h = skin_footer(u, x0 + 8, 0, left - 16, false);
    if (e) {
        const float details_h = skin_details(u, *e, x0, 0, left, false);
        const float art_h = std::max(120.0f, bottom - footer_h - 14 - details_h - 14 - y);
        skin_art(u, *e, x0 + 8, y, left - 16, art_h);
        y += art_h + 14;
        const Rect box = u.sp.r(x0, y, left, details_h);
        u.cast("parchment", box, 14, 10, 0.5f, [&](Canvas &c, Rect r) {
            c.nine_slice(u.white(skin[Tex::Parchment]), r, 22, 22, 22, 22, u.sp.s);
        });
        u.nine(skin[Tex::Parchment], x0, y, left, details_h, 22, 22, 22, 22);
        skin_details(u, *e, x0, y, left, true);
        y += details_h + 14;
    } else {
        y = bottom - footer_h;
    }
    skin_footer(u, x0 + 8, y, left - 16, true);
    skin_scroll(u, x0 + left + 36, top + 6, right, bottom - top - 6);
}

// The sky behind the game's 3D main menu (the blurred splash where there is
// none), darkening to the bottom: one picture per window size.
void skin_backdrop(Ui &u, const Entry *e) {
    const Skin &skin = *u.skin;
    const Image *art = e && e->art ? e->art.get() : nullptr;
    char key[160];
    snprintf(key, sizeof key, "backdrop:%dx%d:%p:%p:%s", u.c.width(), u.c.height(),
             static_cast<const void *>(&skin),
             skin.has(Tex::Sky) ? nullptr : static_cast<const void *>(art),
             skin.has(Tex::Sky) || !e ? "" : e->spec.id.c_str());
    auto it = u.v.cache_.find(key);
    if (it == u.v.cache_.end()) {
        for (auto old = u.v.cache_.begin(); old != u.v.cache_.end();)
            old =
                old->first.compare(0, 9, "backdrop:") == 0 ? u.v.cache_.erase(old) : std::next(old);
        Canvas tmp;
        tmp.resize(u.c.width(), u.c.height(), false);
        const Rect all{0, 0, tmp.width(), tmp.height()};
        tmp.clear(skin_color::night);
        Ui t2{u.v, tmp, u.m, u.density, u.sp, u.look, u.skin};
        if (skin.has(Tex::Sky)) {
            t2.cover(skin[Tex::Sky], all, 0.5f);
        } else if (e) {
            // The selected game's splash blurred, its placeholder before its
            // files are here.
            if (art)
                t2.cover(*art, all, 0.5f, 204);
            else
                placeholder(tmp, all, alpha(theme::accent(e->spec.accent), 0.8f));
            tmp.blur(all, int(20 * u.density));
        }
        const float H = float(tmp.height());
        tmp.gradient(all, {{0, rgbf(0.08f, 0.04f, 0.016f, 0)},
                           {0.45f, rgbf(0.08f, 0.04f, 0.016f, 0.25f)},
                           {1, rgbf(0.07f, 0.035f, 0.012f, 0.92f)}});
        (void)H;
        Image img;
        img.w = tmp.width();
        img.h = tmp.height();
        img.px.assign(tmp.pixels(), tmp.pixels() + size_t(img.w) * size_t(img.h) * 4);
        for (size_t i = 3; i < img.px.size(); i += 4)
            img.px[i] = 255;
        it = u.v.cache_.emplace(key, std::move(img)).first;
    }
    u.c.image(it->second, {0, 0, u.c.width(), u.c.height()});
}

// The options screen's scroll bar track, filled with gold up to `fraction`.
void skin_track(Ui &u, float x, float y, float w, double fraction) {
    u.nine((*u.skin)[Tex::Track], x, y, w, 24, 10, 12, 10, 12);
    const float fw = std::max(8.0f, (w - 12) * float(std::clamp(fraction, 0.0, 1.0)));
    u.c.rounded(u.sp.r(x + 6, y + 7, fw, 10), int(3 * u.sp.s),
                {{0, rgb(0xffe08a)}, {0.5f, rgb(0xe7a93a)}, {1, rgb(0x9b5d14)}});
}

const char *phase_key(Progress::Phase p) {
    switch (p) {
    case Progress::Copying:
        return "phaseCopying";
    case Progress::Checking:
        return "phaseChecking";
    case Progress::Extracting:
        return "phaseUnpacking";
    case Progress::Verifying:
        return "phaseVerifying";
    default:
        return "phasePatching";
    }
}

// Copying or installing, in the game's ornate dialog, the installer's name
// under it; laid out in a 900 x 460 box.
void skin_progress(Ui &u, const Progress &p) {
    const Skin &skin = *u.skin;
    Model &m = u.m;
    const Style name{Face::MenuLight, 19, skin_color::cream, 0, false, 0, rgbf(0, 0, 0)};
    const float y0 = (460 - (394 + 20 + u.line(name))) / 2, x0 = 135;
    const Rect box = u.sp.r(x0, y0, 630, 394);
    u.c.shadow({box.x + box.w / 12, box.y + int(18 * u.sp.s), box.w * 5 / 6, box.h * 5 / 6},
               int(15 * u.sp.s), rgbf(0, 0, 0, 0.6f));
    u.picture(skin[Tex::Dialog], x0, y0, 630, 394);
    const std::string title = p.phase == Progress::Copying ? t("importTitle") : t("installTitle");
    const Style ts = u.shrink(title, {Face::Menu, 28, skin_color::navy}, 518, 0.6f);
    u.say(x0 + 56, y0 + 50 + (48 - u.line(ts)) / 2, u.fit(title, ts, 518), ts, 518, 0.5f);
    // The icon, then the phase, the file, the bar and the counts.
    const bool icon = skin.has(Tex::MessageIcon);
    const float cx = x0 + 60 + 10 + (icon ? 104 + 14 : 0), cw = x0 + 60 + 510 - 20 - cx;
    const Style phase{Face::Menu, 22, skin_color::dialog_text};
    const Style current{Face::Body, 13, skin_color::dialog_soft};
    const Style counts{Face::Label, 12, skin_color::gold};
    const float stack = u.line(phase) + 8 + u.line(current) + 8 + 24 + 8 + u.line(counts);
    const float sy = y0 + 104 + (158 - stack) / 2;
    if (icon)
        u.picture(skin[Tex::MessageIcon], x0 + 70, y0 + 104 + (158 - 107) / 2, 104, 107);
    u.say(cx, sy, u.fit(t(phase_key(p.phase)), phase, cw), phase);
    u.say(cx, sy + u.line(phase) + 8,
          u.fit(p.current.empty() ? " " : p.current, current, cw, Cut::Middle), current);
    const float ty = sy + u.line(phase) + 8 + u.line(current) + 8;
    skin_track(u, cx, ty, cw, p.fraction());
    const std::string pct = std::to_string(int(std::lround(p.fraction() * 100))) + "%";
    u.say(cx, ty + 32,
          u.fit(file_of(p.files_done, p.files_total) + " · " +
                    progress_of(p.bytes_done, p.bytes_total),
                counts, cw - u.width(pct, counts) - 12),
          counts);
    u.say(cx, ty + 32, pct, counts, cw, 1);
    const float bw = button_width(u, Kind::Dialog, t("cancel"));
    auto cancel = [&m] { m.cancel_import(); };
    skin_button(u, Kind::Dialog, x0 + 56 + (518 - bw) / 2, y0 + 280, bw, t("cancel"), "cancel",
                cancel);
    u.v.cancel_action_ = cancel;
    u.say(0, y0 + 394 + 20, u.fit(p.name, name, 900, Cut::Middle), name, 900, 0.5f);
}

// ---------------------------------------------------------------------------
// The flat look
// ---------------------------------------------------------------------------

// The selected game's art, blurred and darkened behind everything.
void flat_backdrop(Ui &u, const Entry *e) {
    char key[160];
    snprintf(key, sizeof key, "flat:%dx%d:%p:%s", u.c.width(), u.c.height(),
             e && e->art ? static_cast<const void *>(e->art.get()) : nullptr,
             e ? e->spec.accent.c_str() : "");
    auto it = u.v.cache_.find(key);
    if (it == u.v.cache_.end()) {
        for (auto old = u.v.cache_.begin(); old != u.v.cache_.end();)
            old = old->first.compare(0, 5, "flat:") == 0 ? u.v.cache_.erase(old) : std::next(old);
        Canvas tmp;
        tmp.resize(u.c.width(), u.c.height(), false);
        const Rect all{0, 0, tmp.width(), tmp.height()};
        const float W = float(all.w), H = float(all.h), d = u.density;
        tmp.clear(theme::ink);
        if (e && e->art) {
            Ui t2{u.v, tmp, u.m, u.density, u.sp, u.look, nullptr};
            t2.cover(*e->art, all, 0.5f, 217);
            tmp.blur(all, int(23 * d));
        } else if (e) {
            // No art before the game's files are here: only the game's colour
            // as a glow from above.
            const Color a = theme::accent(e->spec.accent);
            tmp.radial(all, W / 2, 0, 700 * d, alpha(a, 0.28f), alpha(a, 0));
        }
        tmp.gradient(all, {{0, alpha(theme::ink, 0.35f)},
                           {0.5f, alpha(theme::ink, 0.80f)},
                           {1, alpha(theme::ink, 0.96f)}});
        tmp.radial(all, W / 2, H / 2, 900 * d, alpha(theme::ink, 0), alpha(theme::ink, 0.7f));
        Image img;
        img.w = tmp.width();
        img.h = tmp.height();
        img.px.assign(tmp.pixels(), tmp.pixels() + size_t(img.w) * size_t(img.h) * 4);
        for (size_t i = 3; i < img.px.size(); i += 4)
            img.px[i] = 255;
        it = u.v.cache_.emplace(key, std::move(img)).first;
    }
    u.c.image(it->second, {0, 0, u.c.width(), u.c.height()});
}

// A game's picture with its corners rounded (and, for the card, the gradient
// that carries its title): made once per size.
const Image &rounded_picture(Ui &u, const Entry &e, Rect r, float radius, bool card) {
    char key[200];
    snprintf(key, sizeof key, "%s:%s:%p:%dx%d", card ? "card" : "thumb", e.spec.id.c_str(),
             static_cast<const void *>(e.art.get()), r.w, r.h);
    auto it = u.v.cache_.find(key);
    if (it != u.v.cache_.end())
        return it->second;
    Canvas tmp;
    tmp.resize(r.w, r.h, false);
    const Rect all{0, 0, r.w, r.h};
    tmp.clear(theme::ink);
    if (e.art) {
        Ui t2{u.v, tmp, u.m, u.density, u.sp, u.look, nullptr};
        t2.cover(*e.art, all, 0);
    } else {
        placeholder(tmp, all, theme::accent(e.spec.accent));
    }
    if (card)
        tmp.gradient(all, {{0, alpha(theme::ink, 0)},
                           {0.35f, alpha(theme::ink, 0)},
                           {0.68f, alpha(theme::ink, 0.6f)},
                           {1, alpha(theme::ink, 0.94f)}});
    // The mask: a rounded box drawn white on black.
    Canvas mask;
    mask.resize(r.w, r.h, false);
    mask.rounded(all, int(radius * u.sp.s), {255, 255, 255, 255});
    Image img;
    img.w = r.w;
    img.h = r.h;
    img.px.resize(size_t(r.w) * size_t(r.h) * 4);
    for (size_t i = 0; i < size_t(r.w) * size_t(r.h); ++i) {
        const int a = mask.pixels()[i * 4];
        for (int k = 0; k < 3; ++k)
            img.px[i * 4 + size_t(k)] = uint8_t(tmp.pixels()[i * 4 + size_t(k)] * a / 255);
        img.px[i * 4 + 3] = uint8_t(a);
    }
    return u.v.cache_.emplace(key, std::move(img)).first->second;
}

void status_chip(Ui &u, float x, float y, const StatusInfo &info) {
    const Style st{Face::SansBold, 11, info.color, 0.6f, true};
    const float w = 8 + 6 + 5 + u.width(info.label, st) + 8, h = u.line(st) + 8;
    const Rect r = u.sp.r(x, y, w, h);
    u.c.rounded(r, r.h / 2, alpha(info.color, 0.14f));
    u.c.rounded(r, r.h / 2, alpha(info.color, 0.35f), std::max(1, int(0.5f * u.sp.s)));
    u.c.rounded(u.sp.r(x + 8, y + h / 2 - 3, 6, 6), int(3 * u.sp.s), info.color);
    u.say(x + 8 + 6 + 5, y + 4, info.label, st);
}

float chip_height(const Ui &u) {
    return u.line({Face::SansBold, 11}) + 8;
}

void gold_button(Ui &u, float x, float y, float w, const std::string &label, const std::string &key,
                 std::function<void()> action, bool enabled = true) {
    const bool down = enabled && u.pressed(key);
    const Rect r = u.sp.r(x, y + (down ? 1 : 0), w, 52);
    if (enabled)
        u.c.shadow({r.x + r.h / 2, r.y + int(4 * u.sp.s), r.w - r.h, r.h}, int(18 * u.sp.s),
                   alpha(theme::gold, down ? 0.15f : 0.4f));
    if (enabled)
        u.c.rounded(r, r.h / 2,
                    {{0, rgbf(0.98f, 0.86f, 0.58f)}, {0.5f, theme::gold}, {1, theme::gold_deep}});
    else
        u.c.rounded(r, r.h / 2, {{0, rgbf(0.42f, 0.42f, 0.42f)}, {1, rgbf(0.30f, 0.30f, 0.30f)}});
    u.c.rounded(r, r.h / 2, rgbf(1, 1, 1, enabled ? 0.3f : 0.1f), 1);
    const Style st{Face::SerifBold, 19, alpha(theme::ink, enabled ? 0.92f : 0.5f), 1.2f};
    u.say(x, y + (52 - u.line(st)) / 2 + (down ? 1 : 0), u.fit(label, st, w - 40), st, w, 0.5f);
    u.hit(x, y, w, 52, key, std::move(action), enabled);
}

float gold_width(const Ui &u, const std::string &label) {
    return u.width(label, {Face::SerifBold, 19, theme::ink, 1.2f}) + 68;
}

void ghost_button(Ui &u, float x, float y, float w, const std::string &label,
                  const std::string &key, std::function<void()> action) {
    const bool down = u.pressed(key);
    const Rect r = u.sp.r(x, y, w, 34);
    u.c.rounded(r, r.h / 2, rgbf(1, 1, 1, down ? 0.14f : u.hovered(key) ? 0.10f : 0.07f));
    u.c.rounded(r, r.h / 2, theme::stroke, 1);
    const Style st{Face::Sans, 13, alpha(theme::text, down ? 0.6f : 0.88f)};
    u.say(x, y + (34 - u.line(st)) / 2, label, st, w, 0.5f);
    u.hit(x, y, w, 34, key, std::move(action));
}

float ghost_width(const Ui &u, const std::string &label) {
    return u.width(label, {Face::Sans, 13}) + 28;
}

// The app's name and tagline over the list.
float wordmark(Ui &u, float x, float y, float w, bool draw) {
    const Style title{Face::SerifBold, 22, rgbf(0.95f, 0.82f, 0.55f), 2.5f, true};
    const Style tag{Face::SansBold, 11, theme::faint, 1.4f, true};
    const std::string tagline = u.m.config().tagline.text();
    if (draw) {
        u.say(x, y, u.fit(u.m.title(), title, w), {Face::SerifBold, 22, title.color, 2.5f});
        if (!tagline.empty())
            u.say(x, y + u.line(title) + 4, u.fit(tagline, tag, w),
                  {Face::SansBold, 11, theme::faint, 1.4f});
    }
    return u.line(title) + (tagline.empty() ? 0 : 4 + u.line(tag));
}

// The flat list's row: the splash, the title and the state.
float flat_row(Ui &u, const Entry &e, float x, float y, float w, bool draw) {
    const Style title{Face::SerifBold, 15, theme::text};
    const float tx = x + 10 + 76 + 14, tw = w - (tx - x) - 10;
    auto tl = u.wrap(e.spec.title.text(), title, tw);
    if (tl.size() > 2)
        tl.resize(2);
    const float text_h = u.height(tl.size(), title) + 5 + chip_height(u);
    const float h = 10 + std::max(56.0f, text_h) + 10;
    if (!draw)
        return h;
    Model &m = u.m;
    const bool selected = e.spec.id == m.selected_id();
    const Color accent = theme::accent(e.spec.accent);
    if (selected)
        u.c.rounded(u.sp.r(x, y, w, h), int(14 * u.sp.s), rgbf(1, 1, 1, 0.09f));
    else if (u.hovered("game:" + e.spec.id))
        u.c.rounded(u.sp.r(x, y, w, h), int(14 * u.sp.s), rgbf(1, 1, 1, 0.04f));
    const Rect thumb = u.sp.r(x + 10, y + (h - 56) / 2, 76, 56);
    u.c.image(rounded_picture(u, e, thumb, 8, false), thumb,
              e.state.kind == GameState::NotFound ? 150 : 255);
    u.c.rounded(thumb, int(8 * u.sp.s), selected ? alpha(accent, 0.8f) : theme::stroke,
                std::max(1, int(std::lround((selected ? 1.2f : 0.6f) * u.sp.s))));
    const float ty = y + (h - text_h) / 2;
    u.lines(tx, ty, tw, tl, title);
    status_chip(u, tx, ty + u.height(tl.size(), title) + 5, status_info(e));
    if (selected)
        u.c.rounded(u.sp.r(x - 1, y + h / 2 - 17, 3, 34), int(1.5f * u.sp.s), accent);
    const std::string id = e.spec.id;
    u.hit(x, y, w, h, "game:" + id, [&m, id] { m.select(id); });
    return h;
}

// The large card: the splash, and over its foot the subtitle, the title, the
// year, the version and the state.
void hero_card(Ui &u, const Entry &e, float x, float y, float w, float h) {
    const Color accent = theme::accent(e.spec.accent);
    const Rect r = u.sp.r(x, y, w, h);
    u.c.shadow({r.x, r.y + int(16 * u.sp.s), r.w, r.h}, int(30 * u.sp.s), rgbf(0, 0, 0, 0.45f));
    u.c.image(rounded_picture(u, e, r, 22, true), r);
    u.c.rounded(r, int(22 * u.sp.s), alpha(accent, 0.45f), 1);
    // From the bottom up.
    const StatusInfo info = status_info(e);
    float by = y + h - 24 - chip_height(u);
    float tx = x + 24;
    for (const std::string &tag : {e.spec.year, e.spec.version}) {
        if (tag.empty())
            continue;
        const Style st{Face::SansBold, 11, theme::dim};
        const float tw = u.width(tag, st) + 14;
        u.c.rounded(u.sp.r(tx, by + (chip_height(u) - u.line(st) - 6) / 2, tw, u.line(st) + 6),
                    int(5 * u.sp.s), rgbf(1, 1, 1, 0.08f));
        u.say(tx + 7, by + (chip_height(u) - u.line(st)) / 2, tag, st);
        tx += tw + 8;
    }
    status_chip(u, tx, by, info);
    const Style title = u.shrink(
        e.spec.title.text(), {Face::SerifBold, 34, theme::text, 0, false, 0, rgbf(0, 0, 0, 0.6f)},
        (w - 48) * 2, 0.6f);
    auto tl = u.wrap(e.spec.title.text(), title, w - 48);
    if (tl.size() > 2)
        tl.resize(2);
    by -= 6 + u.height(tl.size(), title);
    u.lines(x + 24, by, w - 48, tl, title);
    if (const std::string sub = e.spec.subtitle.text(); !sub.empty()) {
        const Style st{Face::SansBold, 12, accent, 2, true};
        by -= 6 + u.line(st);
        u.say(x + 24, by, u.fit(sub, st, w - 48), {Face::SansBold, 12, accent, 2});
    }
}

// The blurb and the state's advice (Details); its height.
float flat_details(Ui &u, const Entry &e, float x, float y, float w, bool draw) {
    const StatusInfo info = status_info(e);
    float cy = y;
    if (const std::string blurb = e.spec.blurb.text(); !blurb.empty()) {
        const Style st{Face::Sans, 15, theme::dim, 0, false, 3};
        const auto ls = u.wrap(blurb, st, w);
        if (draw)
            u.lines(x, cy, w, ls, st);
        cy += u.height(ls.size(), st) + 16;
    }
    // A path is cut in the middle; advice is read whole.
    const Style st =
        e.can_play() ? Style{Face::Mono, 12, theme::faint} : Style{Face::Sans, 13, theme::dim};
    std::vector<std::string> ls;
    if (e.can_play())
        ls = {u.fit(info.detail, st, w - 18, Cut::Middle)};
    else
        ls = u.wrap(info.detail, st, w - 18);
    if (draw) {
        u.c.rounded(u.sp.r(x, cy + (u.line(st) - 8) / 2, 8, 8), int(4 * u.sp.s), info.color);
        u.lines(x + 18, cy, w - 18, ls, st);
    }
    cy += u.height(ls.size(), st);
    return cy - y;
}

// Install, Resume install or Play, then the folder buttons (Actions); its height.
float flat_actions(Ui &u, const Entry &e, float x, float y, float w, bool draw) {
    Model &m = u.m;
    std::string label;
    std::function<void()> act;
    bool enabled = true;
    if (installs(e)) {
        label = t("installFromGog");
        act = [&m] { m.pick_installer(); };
    } else if (e.interrupted()) {
        label = t("resumeInstall");
        act = [&m] { m.resume_install(); };
    } else {
        label = t("play");
        act = [&m] { m.play(); };
        enabled = e.can_play();
    }
    struct Ghost {
        std::string label, key;
        std::function<void()> action;
    };
    std::vector<Ghost> ghosts = {{t("chooseFolder"), "folder", [&m] { m.pick_folder(); }}};
    if (!e.root.empty() && Model::can_reveal())
        ghosts.push_back({t("showInFinder"), "reveal", [&m] { m.reveal_selected(); }});
    if (!m.profile_dir().empty() && Model::can_reveal())
        ghosts.push_back({t("saves"), "saves", [&m] { m.reveal_saves(); }});
    const float pw = gold_width(u, label);
    float gw = 0;
    for (const Ghost &g : ghosts)
        gw += ghost_width(u, g.label) + 10;
    const bool row = pw + 12 + gw <= w;
    if (draw) {
        gold_button(u, x, y, pw, label, "primary", act, enabled);
        if (enabled)
            u.v.default_action_ = act;
        float gx = row ? x + pw + 12 : x;
        const float gy = row ? y + 9 : y + 52 + 12;
        for (const Ghost &g : ghosts) {
            const float bw = ghost_width(u, g.label);
            ghost_button(u, gx, gy, bw, g.label, g.key, g.action);
            gx += bw + 10;
        }
    }
    return row ? 52 : 52 + 12 + 34;
}

void flat_wide(Ui &u) {
    Model &m = u.m;
    const float W = u.look.width, H = u.look.height;
    const float rail = std::min(340.0f, std::max(270.0f, W * 0.28f));
    const float panel = 30 + rail + 18;
    u.c.gradient(u.sp.r(0, 0, panel, H),
                 {{0, alpha(theme::ink, 0.75f)}, {1, alpha(theme::ink, 0.35f)}}, true);
    float y = 34 + wordmark(u, 30, 34, rail, true) + 30;
    u.say(40, y, t("games"), {Face::SansBold, 11, theme::faint, 1.6f, true});
    y += u.line({Face::SansBold, 11}) + 8;
    for (const Entry &e : m.entries())
        y += flat_row(u, e, 30, y, rail, true) + 6;
    // The legal line at the foot, the search (or Look again) over it.
    const Style legal{Face::Sans, 10, alpha(theme::faint, 0.8f)};
    const auto ls = u.wrap(m.config().legal.empty() ? t("defaultLegal") : m.config().legal.text(),
                           legal, rail - 10);
    const float legal_y = H - 34 - u.height(ls.size(), legal);
    u.lines(40, legal_y, rail - 10, ls, legal);
    const Style small{Face::Sans, 12, u.hovered("refresh") ? theme::dim : theme::faint};
    const float ry = legal_y - 10 - u.line(small);
    if (m.scanning()) {
        u.say(40, ry, t("searching"), small);
    } else {
        const std::string label = "↻ " + t("refresh");
        u.say(40, ry, label, small);
        u.hit(40, ry, u.width(label, small), u.line(small), "refresh", [&m] { m.refresh(); });
    }
    const Entry *e = m.selected();
    if (!e)
        return;
    const float cx = panel + 36, cw = std::min(980.0f, W - panel) - 72;
    const float card_h = std::min(H * 0.58f, std::min(980.0f, W - rail - 120) / 1.5f);
    hero_card(u, *e, cx, 34, cw, card_h);
    y = 34 + card_h + 24;
    y += flat_details(u, *e, cx, y, cw, true) + 24;
    flat_actions(u, *e, cx, y, cw, true);
}

// A narrow window: the card, its details and the legal line in a column that
// scrolls, Play at the foot.
void flat_narrow(Ui &u) {
    Model &m = u.m;
    View &v = u.v;
    const float W = u.look.width, H = u.look.height;
    const Entry *e = m.selected();
    const float bar = e ? 10 + 52 + 8 : 0;
    const float card_h = std::min({W - 36, H * 0.52f, 640.0f});
    // Measured, then drawn shifted by the scroll.
    float content = 12 + wordmark(u, 22, 0, W - 44, false) + 20 + card_h + 44;
    if (e)
        content += flat_details(u, *e, 22, 0, W - 44, false) + 20;
    const Style legal{Face::Sans, 10, alpha(theme::faint, 0.8f)};
    const auto ls = u.wrap(m.config().legal.empty() ? t("defaultLegal") : m.config().legal.text(),
                           legal, W - 44);
    content += u.height(ls.size(), legal) + 12;
    v.scroll_max_ = std::max(0.0f, content - (H - bar));
    v.scroll_ = std::clamp(v.scroll_, 0.0f, v.scroll_max_);
    u.c.set_clip(u.sp.r(0, 0, W, H - bar));
    float y = 12 - v.scroll_;
    y += wordmark(u, 22, y, W - 44, true) + 20;
    if (e) {
        hero_card(u, *e, 18, y, W - 36, card_h);
        // Which of the games the card shows; a tap picks one.
        const size_t n = m.entries().size();
        float dx = W / 2 - float(n) * 13;
        for (const Entry &g : m.entries()) {
            const bool on = g.spec.id == m.selected_id();
            const float d = on ? 10 : 8;
            u.c.rounded(u.sp.r(dx + 13 - d / 2, y + card_h + 16 - d / 2, d, d), int(d / 2 * u.sp.s),
                        on ? theme::gold : alpha(theme::text, 0.4f));
            const std::string id = g.spec.id;
            u.hit(dx, y + card_h + 5, 26, 22, "dot:" + id, [&m, id] { m.select(id); });
            dx += 26;
        }
        y += card_h + 44;
        y += flat_details(u, *e, 22, y, W - 44, true) + 20;
    }
    u.lines(22, y, W - 44, ls, legal);
    u.c.reset_clip();
    if (e) {
        u.c.gradient(u.sp.r(0, H - bar - 20, W, bar + 20),
                     {{0, alpha(theme::ink, 0)}, {0.5f, alpha(theme::ink, 0.9f)}});
        const float bw = std::min(560.0f, W - 36);
        const std::string label = installs(*e)       ? t("installFromGog")
                                  : e->interrupted() ? t("resumeInstall")
                                                     : t("play");
        std::function<void()> act =
            installs(*e)       ? std::function<void()>([&m] { m.pick_installer(); })
            : e->interrupted() ? std::function<void()>([&m] { m.resume_install(); })
                               : std::function<void()>([&m] { m.play(); });
        const bool enabled = installs(*e) || e->interrupted() || e->can_play();
        gold_button(u, (W - bw) / 2, H - bar + 10 - 8, bw, label, "primary", act, enabled);
        if (enabled)
            v.default_action_ = act;
    }
}

void flat_progress(Ui &u, const Progress &p) {
    Model &m = u.m;
    const float W = u.look.width, H = u.look.height;
    u.c.fill({0, 0, u.c.width(), u.c.height()}, rgbf(0, 0, 0, 0.45f));
    const float w = std::min(460.0f, W - 48);
    const Style head{Face::SerifBold, 17, theme::text};
    const Style small{Face::Sans, 12, theme::faint};
    const Style mono{Face::Mono, 11, theme::faint};
    const float h = 22 + u.line(head) + 12 + 6 + 12 + 34 + 12 + u.line(mono) + 22;
    const float x = (W - w) / 2, y = (H - h) / 2;
    const Rect r = u.sp.r(x, y, w, h);
    u.c.shadow(r, int(40 * u.sp.s), rgbf(0, 0, 0, 0.6f));
    u.c.rounded(r, int(18 * u.sp.s), alpha(theme::ink, 0.92f));
    u.c.rounded(r, int(18 * u.sp.s), theme::stroke, 1);
    std::string headline;
    switch (p.phase) {
    case Progress::Copying:
        headline = t("importing", {{"name", p.name}});
        break;
    case Progress::Checking:
        headline = t("checking", {{"name", p.name}});
        break;
    case Progress::Extracting:
        headline = t("installing", {{"name", p.name}});
        break;
    case Progress::Verifying:
        headline = t("verifying", {{"name", p.name}});
        break;
    default:
        headline = t("patching", {{"name", p.name}});
        break;
    }
    float cy = y + 22;
    u.say(x + 22, cy, u.fit(headline, head, w - 44), head);
    cy += u.line(head) + 12;
    const Rect track = u.sp.r(x + 22, cy, w - 44, 6);
    u.c.rounded(track, track.h / 2, rgbf(1, 1, 1, 0.10f));
    u.c.rounded({track.x, track.y,
                 std::max(int(6 * u.sp.s), int(float(track.w) * float(p.fraction()))), track.h},
                track.h / 2,
                {{0, rgbf(0.98f, 0.86f, 0.58f)}, {0.5f, theme::gold}, {1, theme::gold_deep}});
    cy += 6 + 12;
    const float bw = ghost_width(u, t("cancel"));
    u.say(x + 22, cy + (34 - u.line(small)) / 2,
          u.fit(std::to_string(p.files_done) + " / " + std::to_string(p.files_total) + " · " +
                    progress_of(p.bytes_done, p.bytes_total),
                small, w - 44 - bw - 12),
          small);
    auto cancel = [&m] { m.cancel_import(); };
    ghost_button(u, x + w - 22 - bw, cy, bw, t("cancel"), "cancel", cancel);
    u.v.cancel_action_ = cancel;
    cy += 34 + 12;
    u.say(x + 22, cy, u.fit(p.current, mono, w - 44, Cut::Middle), mono);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct Row {
    std::string title;
    std::vector<std::string> choices;
    int chosen;
    std::function<void(int)> choose;
};

std::vector<Row> setting_rows(Model &m) {
    // The game's pacing and look (mods/settings_file.h), read when it starts.
    // Metal's HUD, MetalFX's render quality and the Mac trackpad's camera are
    // Apple's own and are left out here.
    return {
        {t("frameLimit"),
         {t("frameLimitGame"), "40", "60", "120"},
         m.frame_limit(),
         [&m](int i) { m.set_frame_limit(i); }},
        {t("energySaver"),
         {t("auto"), t("always"), t("never")},
         m.energy_saver(),
         [&m](int i) { m.set_energy_saver(i); }},
        {t("idleFrames"),
         {t("auto"), t("on"), t("off")},
         m.idle_frames(),
         [&m](int i) { m.set_idle_frames(i); }},
        {t("uiBounds"),
         {t("uiBoundsScreen"), "21:9", "16:9", "4:3"},
         m.ui_bounds(),
         [&m](int i) { m.set_ui_bounds(i); }},
        {t("startupVideos"),
         {t("showVideos"), t("skipVideos")},
         m.skip_intro() ? 1 : 0,
         [&m](int i) { m.set_skip_intro(i == 1); }},
    };
}

std::string language_name() {
    for (const Language &l : languages())
        if (l.code == chosen_language())
            return l.name;
    return t("languageSystem");
}

// The settings page: its natural height for `draw` false, laid out in a box
// 860 points wide.
float skin_settings(Ui &u, float H, bool draw) {
    const Skin &skin = *u.skin;
    Model &m = u.m;
    View &v = u.v;
    const float x = 70, w = 860 - 70 - 78;
    const Style label{Face::Menu, 22, skin_color::navy};
    if (draw) {
        const Rect page = u.sp.r(0, 0, 860, H);
        u.cast("page", page, 15, 18, 0.6f,
               [&](Canvas &c, Rect r) { c.image(u.white(skin[Tex::Page]), r); });
        u.picture(skin[Tex::Page], 0, 0, 860, H);
    }
    float y = 64;
    const Style title{Face::Menu, 40, skin_color::navy};
    if (draw)
        u.say(x, y, t("settings"), title);
    y += u.line(title) + 4 + 14;
    // A setting's name in its 220 points, on a second line where a
    // language's words are too long for one; its height.
    auto row_label = [&](float ry, const std::string &text) {
        const auto ls = u.wrap(text, label, 220);
        if (draw)
            u.lines(x, ry + 4, 220, ls, label);
        return u.height(ls.size(), label) + 8;
    };
    // The language: the one chosen in the options' own lettering, a menu of
    // the languages in their own names under it.
    {
        row_label(y, t("languageLabel"));
        const std::string name = language_name();
        const float lx = x + 220 + 16, nw = u.width(name, label);
        const float h = check_height(u, 22);
        if (draw) {
            const Color c =
                u.hovered("language") || v.menu_ ? skin_color::navy_hover : skin_color::navy;
            u.say(lx, y + (h - u.line(label)) / 2, name, {Face::Menu, 22, c});
            // A small triangle pointing down, for the menu's chevron.
            const float k = 22 * 0.55f;
            const Rect tri = u.sp.r(lx + nw + 8, y + h / 2 - k / 4, k, k / 2);
            for (int row = 0; row < tri.h; ++row) {
                const int inset = row * tri.w / (2 * std::max(1, tri.h));
                u.c.fill({tri.x + inset, tri.y + row, tri.w - 2 * inset, 1}, c);
            }
            u.hit(lx, y, nw + 8 + k, h, "language", [&v] { v.menu_ = !v.menu_; });
            v.menu_anchor_ = u.sp.r(lx, y + h, nw, 0);
        }
        y += h + 14;
    }
    if (draw)
        divider(u, x, y + 6, w);
    y += 14 + 14;
    const float flow_w = w - 220 - 16;
    for (const Row &r : setting_rows(m)) {
        std::vector<float> widths;
        for (const std::string &c : r.choices)
            widths.push_back(check_width(u, c, 22));
        const auto places = flow(widths, flow_w, 22);
        const int rows = places.empty() ? 1 : places.back().row + 1;
        const float ch = check_height(u, 22);
        const float lh = row_label(y, r.title);
        if (draw)
            for (size_t i = 0; i < r.choices.size(); ++i) {
                const int idx = int(i);
                const auto choose = r.choose;
                skin_check(u, x + 236 + places[i].x, y + float(places[i].row) * (ch + 4),
                           r.choices[i], r.chosen == idx, 22,
                           "set:" + r.title + ":" + std::to_string(i),
                           [choose, idx] { choose(idx); });
            }
        y += std::max(lh, float(rows) * ch + float(rows - 1) * 4) + 14;
    }
    // Animated models on the GPU (native/gpu_skin.cpp), read when the game starts.
    const float gh = row_label(y, t("gpuSkin"));
    if (draw)
        skin_check(u, x + 236, y, t("gpuSkinOn"), m.gpu_skin(), 22, "set:gpuskin",
                   [&m] { m.set_gpu_skin(!m.gpu_skin()); });
    y += std::max(gh, check_height(u, 22)) + 14;
    if (draw)
        divider(u, x, y + 6, w);
    y += 14 + 14;
    // The game folders, Add…, where installs go.
    {
        const float bw = button_width(u, Kind::Small, t("addFolder"));
        const float tw = w - bw - 14;
        float cy = y;
        if (draw)
            u.say(x, cy, t("gameFolders"), label);
        cy += u.line(label) + 4;
        const std::vector<std::string> roots = m.remembered_roots();
        const Style muted{Face::Body, 14, skin_color::ink_muted};
        const Style mono{Face::Mono, 13, rgb(0x4a3518)};
        if (roots.empty()) {
            if (draw)
                u.say(x, cy, u.fit(t("noFolders"), muted, tw), muted);
            cy += u.line(muted) + 4;
        }
        for (size_t i = 0; i < roots.size() && i < 4; ++i) {
            if (draw)
                u.say(x, cy, u.fit(location(roots[i]), mono, tw, Cut::Middle), mono);
            cy += u.line(mono) + 4;
        }
        const std::string base = Model::import_base();
        if (!base.empty()) {
            const Style small{Face::Body, 13, skin_color::ink_muted};
            auto ls = u.wrap(t("installsGoTo") + ": " + location(base), small, tw);
            if (ls.size() > 2) {
                ls.resize(2);
                ls[1] = u.fit(ls[1] + "…", small, tw);
            }
            cy += 2;
            if (draw)
                u.lines(x, cy, tw, ls, small);
            cy += u.height(ls.size(), small);
        }
        if (draw)
            skin_button(u, Kind::Small, x + w - bw, y, bw, t("addFolder"), "addfolder",
                        [&m] { m.pick_folder(); });
        y = std::max(cy, y + 38) + 14;
    }
    if (!m.profile_dir().empty()) {
        if (draw)
            divider(u, x, y + 6, w);
        y += 14 + 14;
        const float bw = button_width(u, Kind::Small, t("show"));
        const Style mono{Face::Mono, 13, rgb(0x4a3518)};
        const float bh = u.line(label) + 2 + u.line(mono);
        const float h = std::max(bh, 38.0f);
        if (draw) {
            u.say(x, y + (h - bh) / 2, t("savesFolder"), label);
            u.say(x, y + (h - bh) / 2 + u.line(label) + 2,
                  u.fit(location(m.profile_dir()), mono, w - bw - 14, Cut::Middle), mono);
            if (Model::can_reveal())
                skin_button(u, Kind::Small, x + w - bw, y + (h - 38) / 2, bw, t("show"), "showsaves",
                            [&m] { m.reveal_saves(); });
        }
        y += h + 14;
    }
    const float bw = button_width(u, Kind::Dialog, t("done"));
    const float natural = y + 50 + 60;
    if (draw) {
        auto done = [&v] { v.settings_ = false, v.menu_ = false; };
        skin_button(u, Kind::Dialog, (860 - bw) / 2, std::max(y, H - 60 - 50), bw, t("done"),
                    "done", done);
        u.v.default_action_ = done;
        u.v.cancel_action_ = done;
    }
    return natural;
}

// The languages' menu under the language's name.
void language_menu(Ui &u) {
    View &v = u.v;
    Model &m = u.m;
    const float s = u.density;
    const Style st{Face::Sans, 15, skin_color::cream};
    std::vector<std::pair<std::string, std::string>> items = {{"", t("languageSystem")}, {"-", ""}};
    for (const Language &l : languages())
        items.push_back({l.code, l.name});
    float w = 0;
    for (const auto &i : items)
        w = std::max(w, text_width(i.second, {st.face, st.size * s}) / s);
    w += 24 + 28;
    const float item_h = 26, divider_h = 9;
    float h = 12;
    for (const auto &i : items)
        h += i.first == "-" ? divider_h : item_h;
    Space menu{float(v.menu_anchor_.x), float(v.menu_anchor_.y) + 4 * s, s};
    // Kept on the screen.
    menu.ox = std::min(menu.ox, float(u.c.width()) - (w + 8) * s);
    if (menu.oy + h * s > float(u.c.height()) - 8 * s)
        menu.oy = std::max(8 * s, float(u.c.height()) - (h + 8) * s);
    Ui mu{v, u.c, m, u.density, menu, u.look, u.skin};
    // A click anywhere else closes it.
    mu.hit({0, 0, u.c.width(), u.c.height()}, "menu:outside", [&v] { v.menu_ = false; });
    const Rect box = menu.r(0, 0, w, h);
    u.c.shadow({box.x, box.y + int(6 * s), box.w, box.h}, int(14 * s), rgbf(0, 0, 0, 0.5f));
    u.c.rounded(box, int(8 * s), rgbf(0.13f, 0.09f, 0.06f, 0.97f));
    u.c.rounded(box, int(8 * s), alpha(skin_color::gold, 0.45f), 1);
    float y = 6;
    for (const auto &i : items) {
        if (i.first == "-") {
            u.c.fill(menu.r(10, y + 4, w - 20, 1), alpha(skin_color::cream, 0.2f));
            y += divider_h;
            continue;
        }
        const std::string key = "lang:" + i.first;
        if (mu.hovered(key))
            u.c.rounded(menu.r(4, y, w - 8, item_h), int(5 * s), alpha(skin_color::gold, 0.3f));
        if (i.first == chosen_language())
            mu.say(10, y + (item_h - mu.line(st)) / 2, "✓", st);
        mu.say(30, y + (item_h - mu.line(st)) / 2, i.second, st);
        const std::string code = i.first;
        mu.hit(0, y, w, item_h, key, [&v, &m, code] {
            m.set_language(code);
            v.menu_ = false;
        });
        y += item_h;
    }
    v.cancel_action_ = [&v] { v.menu_ = false; };
}

// The model's notice as an alert with OK.
void alert(Ui &u, const std::string &text) {
    Model &m = u.m;
    const bool skinned = u.skin != nullptr;
    const float W = u.look.width, H = u.look.height;
    u.c.fill({0, 0, u.c.width(), u.c.height()},
             skinned ? rgbf(0.063f, 0.031f, 0.012f, 0.62f) : rgbf(0, 0, 0, 0.45f));
    const float w = std::min(560.0f, W - 48);
    const Style st = skinned ? Style{Face::Body, 16, skin_color::ink, 0, false, 3}
                             : Style{Face::Sans, 14, theme::text, 0, false, 3};
    const auto ls = u.wrap(text, st, w - 72);
    const float bh = skinned ? 50 : 34;
    const float h = std::min(H - 32, 36 + u.height(ls.size(), st) + 24 + bh + 32);
    const float x = (W - w) / 2, y = (H - h) / 2;
    const Rect r = u.sp.r(x, y, w, h);
    auto ok = [&m] { m.clear_notice(); };
    if (skinned) {
        u.cast("alert", r, 14, 10, 0.5f, [&](Canvas &c, Rect b) {
            c.nine_slice(u.white((*u.skin)[Tex::Parchment]), b, 22, 22, 22, 22, u.sp.s);
        });
        u.nine((*u.skin)[Tex::Parchment], x, y, w, h, 22, 22, 22, 22);
    } else {
        u.c.shadow(r, int(30 * u.sp.s), rgbf(0, 0, 0, 0.6f));
        u.c.rounded(r, int(14 * u.sp.s), alpha(theme::ink, 0.96f));
        u.c.rounded(r, int(14 * u.sp.s), theme::stroke, 1);
    }
    u.c.set_clip(r);
    u.lines(x + 36, y + 36, w - 72, ls, st);
    u.c.reset_clip();
    const std::string label = t("ok");
    if (skinned) {
        const float bw = button_width(u, Kind::Dialog, label);
        skin_button(u, Kind::Dialog, x + (w - bw) / 2, y + h - 32 - bh, bw, label, "ok", ok);
    } else {
        const float bw = std::max(90.0f, ghost_width(u, label));
        ghost_button(u, x + (w - bw) / 2, y + h - 32 - bh, bw, label, "ok", ok);
    }
    u.v.default_action_ = ok;
    u.v.cancel_action_ = ok;
}

// What a drag over the window shows: where to let go and what happens.
void drop_overlay(Ui &u) {
    const float W = u.look.width, H = u.look.height;
    u.c.fill({0, 0, u.c.width(), u.c.height()}, alpha(theme::ink, 0.72f));
    const Rect r = u.sp.r(18, 18, W - 36, H - 36);
    const Color gold = alpha(theme::gold, 0.85f);
    const int t = std::max(1, int(2 * u.sp.s)), dash = int(10 * u.sp.s), gap = int(7 * u.sp.s),
              rad = int(22 * u.sp.s);
    for (int x = r.x + rad; x < r.x + r.w - rad; x += dash + gap) {
        const int len = std::min(dash, r.x + r.w - rad - x);
        u.c.fill({x, r.y, len, t}, gold);
        u.c.fill({x, r.y + r.h - t, len, t}, gold);
    }
    for (int y = r.y + rad; y < r.y + r.h - rad; y += dash + gap) {
        const int len = std::min(dash, r.y + r.h - rad - y);
        u.c.fill({r.x, y, t, len}, gold);
        u.c.fill({r.x + r.w - t, y, t, len}, gold);
    }
    const Style st{Face::SerifBold, 20, theme::text};
    const auto ls = u.wrap(::launcher::native::t("dropHint"), st, W - 80);
    u.lines(40, (H - u.height(ls.size(), st)) / 2, W - 80, ls, st, 0.5f);
}

} // namespace

void View::draw(Canvas &canvas, float density) {
    regions_.clear();
    default_action_ = nullptr;
    cancel_action_ = nullptr;
    canvas.reset_clip();
    canvas.clear({0, 0, 0, 255});
    const float W = float(canvas.width()) / density, H = float(canvas.height()) / density;
    const std::shared_ptr<const Skin> skin = model_.skin();
    const Look look = choose_look(W, H, skin != nullptr);
    Ui u{*this,
         canvas,
         model_,
         density,
         Space{0, 0, density * look.scale},
         look,
         look.skinned ? skin.get() : nullptr};
    if (!model_.has_scanned()) {
        // Until the first search has found the games (and with them the
        // textures of the game's own look): plain black, not a moment of the
        // flat look.
        return;
    }
    const Entry *e = model_.selected();
    // The window's own points, for what lies over the whole of it.
    Ui window{*this, canvas, model_, density, Space{0, 0, density}, choose_look(W, H, false),
              u.skin};
    window.look.width = W;
    window.look.height = H;
    if (u.skin) {
        skin_backdrop(u, e);
        skin_wide(u);
    } else {
        flat_backdrop(u, e);
        if (look.wide)
            flat_wide(u);
        else
            flat_narrow(u);
    }
    // The dialogs over the screen: scaled down when the window is small.
    auto modal = [&](float nw, float nh) {
        const float k = modal_scale(W, H, nw, nh);
        return Ui{*this,
                  canvas,
                  model_,
                  density,
                  Space{(float(canvas.width()) - nw * k * density) / 2,
                        (float(canvas.height()) - nh * k * density) / 2, k * density},
                  look,
                  u.skin};
    };
    if (const auto &p = model_.importing()) {
        settings_ = menu_ = false;
        if (u.skin) {
            canvas.fill({0, 0, canvas.width(), canvas.height()},
                        rgbf(0.063f, 0.031f, 0.012f, 0.62f));
            Ui mu = modal(900, 460);
            mu.hit({0, 0, canvas.width(), canvas.height()}, "modal", nullptr);
            skin_progress(mu, *p);
        } else {
            window.hit({0, 0, canvas.width(), canvas.height()}, "modal", nullptr);
            flat_progress(window, *p);
        }
    } else if (settings_ && u.skin) {
        canvas.fill({0, 0, canvas.width(), canvas.height()}, rgbf(0.063f, 0.031f, 0.012f, 0.62f));
        // Its height from a pass that only measures.
        Ui probe = modal(860, 860);
        const float natural = std::max(720.0f, skin_settings(probe, 0, false));
        Ui mu = modal(860, natural);
        mu.hit({0, 0, canvas.width(), canvas.height()}, "modal", nullptr);
        skin_settings(mu, natural, true);
        if (menu_)
            language_menu(mu);
    } else {
        settings_ = false;
        menu_ = false;
    }
    if (!model_.notice().empty())
        alert(window, model_.notice());
    if (drag_ && !model_.importing())
        drop_overlay(window);
}

const View::Region *View::at(float x, float y) const {
    for (auto it = regions_.rbegin(); it != regions_.rend(); ++it)
        if (it->r.contains(int(x), int(y)))
            return &*it;
    return nullptr;
}

bool View::update_hover() {
    const Region *r = at(mx_, my_);
    const std::string key = r && r->enabled && r->action ? r->key : "";
    if (key == hover_)
        return false;
    hover_ = key;
    return true;
}

bool View::pointer(float x, float y) {
    mx_ = x;
    my_ = y;
    return update_hover();
}

bool View::press(float x, float y) {
    pointer(x, y);
    const Region *r = at(x, y);
    pressed_ = r && r->enabled && r->action ? r->key : "";
    return true;
}

bool View::release(float x, float y) {
    pointer(x, y);
    const Region *r = at(x, y);
    const std::string was = pressed_;
    pressed_.clear();
    if (r && r->enabled && r->action && r->key == was) {
        // Copied: the action may redraw, which replaces the regions.
        const std::function<void()> action = r->action;
        action();
    }
    return true;
}

bool View::leave() {
    mx_ = my_ = -1;
    pressed_.clear();
    return update_hover();
}

bool View::wheel(float lines) {
    if (scroll_max_ <= 0)
        return false;
    scroll_ = std::clamp(scroll_ + lines * 40, 0.0f, scroll_max_);
    return true;
}

bool View::key(uint32_t key, uint16_t mods) {
    const bool ctrl = (mods & SDL_KMOD_CTRL) != 0;
    const bool modal = model_.importing() || settings_ || !model_.notice().empty();
    switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (default_action_) {
            const std::function<void()> act = default_action_;
            act();
        }
        return true;
    case SDLK_ESCAPE:
        if (cancel_action_) {
            const std::function<void()> act = cancel_action_;
            act();
        }
        return true;
    case SDLK_UP:
    case SDLK_DOWN:
        // Arrow keys move through the list.
        if (modal)
            return false;
        model_.step(key == SDLK_UP ? -1 : 1);
        return true;
    case SDLK_O:
        if (!ctrl || modal)
            return false;
        model_.pick_folder();
        return true;
    case SDLK_Q:
        if (!ctrl)
            return false;
        model_.quit();
        return true;
    case SDLK_COMMA:
        // Ctrl+, opens Settings, as an app's preferences.
        if (!ctrl || model_.importing() || !model_.skin())
            return false;
        settings_ = true;
        return true;
    default:
        return false;
    }
}

bool View::dragging(bool on) {
    if (drag_ == on)
        return false;
    drag_ = on && !model_.importing();
    return true;
}

} // namespace launcher::native
