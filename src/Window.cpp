#include "Window.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>

using namespace geode::prelude;

#ifdef GEODE_IS_WINDOWS

#include <Windows.h>
#include <windowsx.h>
#include <objidl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// gdiplus.h wants min/max, which NOMINMAX removes.
namespace Gdiplus {
    using std::max;
    using std::min;
}
#include <gdiplus.h>

namespace pb::window {
namespace {

namespace gp = Gdiplus;
using RectF = gp::RectF;

// ===========================================================================
// What's in the window
// ===========================================================================

constexpr int MAX_TICKS = 24;
constexpr int SLIDERS = 2;   // buffer, randomize
constexpr int TOGGLES = 4;
constexpr int MODES = 8;

constexpr char const* TOGGLE_KEYS[TOGGLES] { "enabled", "buffer-releases", "buffer-ship", "show-feedback" };
constexpr wchar_t const* TOGGLE_NAMES[TOGGLES] { L"Enabled", L"Releases", L"Ship", L"Timing popup" };
constexpr wchar_t const* TOGGLE_HELP[TOGGLES] {
    L"Turns the buffer on or off. The toggle key (F6) also cycles 2, 4, 8 and 16 ticks, then off.",
    L"Also holds back early releases in wave and robot (and ship when Ship is on), where letting go too early kills you.",
    L"Lets the buffer work in ship mode. Off by default: ship predictions are less reliable.",
    L"Shows in game how early each held-back click was.",
};
constexpr wchar_t const* SLIDER_NAMES[SLIDERS] { L"BUFFER", L"RANDOMIZE" };
constexpr wchar_t const* SLIDER_HELP[SLIDERS] {
    L"Largest delay the buffer may add to a click that would kill you, in ticks (1 tick = 4.2 ms). Wave stops at 8. 0 = off.",
    L"0 = always the smallest safe delay. Above 0 = any safe delay up to this many ticks above the smallest one.",
};
constexpr wchar_t const* IDLE_HELP = L"Hover something to see what it does  ·  F7 shows / hides this window";
constexpr wchar_t const* MODE_NAMES[MODES] { L"cube", L"ship", L"ball", L"ufo", L"wave", L"robot", L"spider", L"swing" };

// Colors (black and white, no gray accents)
constexpr uint32_t C_BG = 0x121212;
constexpr uint32_t C_HOVER = 0x1a1a1a;
constexpr uint32_t C_LINE = 0x2a2a2a;
constexpr uint32_t C_DIVIDER = 0x1e1e1e;
constexpr uint32_t C_TEXT = 0xececec;
constexpr uint32_t C_MUTED = 0x9a9a9a;
constexpr uint32_t C_FAINT = 0x6a6a6a;
constexpr uint32_t C_TRACK = 0x333333;
constexpr uint32_t C_DANGER = 0xff6b6b;

// Layout, in logical pixels (scaled for the monitor)
constexpr float W = 454.f;
constexpr float H = 680.f;
constexpr float PAD = 22.f;
constexpr float TITLE_H = 48.f;
constexpr float STATS_Y = 49.f;
constexpr float STATS_H = 60.f;
constexpr float SLIDER_Y = STATS_Y + STATS_H + 1.f;
constexpr float SLIDER_H = 82.f;
constexpr float TOGGLE_Y = SLIDER_Y + SLIDERS * (SLIDER_H + 1.f);
constexpr float TOGGLE_H = 38.f;
constexpr float LOG_Y = TOGGLE_Y + TOGGLES * TOGGLE_H + 14.f;
constexpr float FOOTER_H = 30.f;
constexpr float TRACK_X0 = PAD + 7.f;
constexpr float TRACK_X1 = W - PAD - 7.f;

constexpr UINT WM_PB_SHOW = WM_APP + 1;
constexpr UINT WM_PB_TOGGLE = WM_APP + 2;
constexpr UINT WM_PB_OPEN = WM_APP + 3;
constexpr UINT WM_PB_SYNC = WM_APP + 4;
constexpr UINT WM_PB_EVENT = WM_APP + 5;
constexpr UINT WM_DPI_CHANGED = 0x02E0;

// ===========================================================================
// State
// ===========================================================================

struct Values {
    int sliders[SLIDERS] { 2, 3 }; // max ticks, randomize
    bool toggles[TOGGLES] { true, true, false, true };
    bool hasPos = false;
    int x = 0;
    int y = 0;
    int sizePercent = 100;
};

struct Item {
    std::wstring time;
    std::wstring note; // set for plain notes ("Buffer on: ...")
    Save save;
};

struct Attempt {
    int number = 0;  // 0 = outside a level
    int death = -1;  // level % it ended at
    int saves = 0;
    std::vector<Item> items; // oldest first
};

struct Event {
    enum Type { Note, Saved, NewAttempt, Death } type = Note;
    Item item;
    int number = 0;
    bool newLevel = false;
};

constexpr size_t MAX_ATTEMPTS = 40;
constexpr size_t MAX_ITEMS = 60;

std::atomic<HWND> g_window = nullptr;
std::atomic<HWND> g_game = nullptr;
std::atomic<bool> g_started = false;
bool g_writing = false; // main thread: the window is saving settings

// Window thread only.
Values g_values;
std::deque<Attempt> g_attempts; // newest first
int g_levelSaves = 0;
double g_levelMs = 0.0;
int g_hover = -1;
int g_dragging = -1;
int g_focus = 0; // slider the arrow keys change
int g_wheel = 0;
float g_scale = 1.f;
bool g_placed = false;

// ===========================================================================
// Settings (main thread)
// ===========================================================================

Values readSettings() {
    auto mod = Mod::get();
    Values v;
    v.sliders[0] = static_cast<int>(mod->getSettingValue<int64_t>("buffer-ticks"));
    v.sliders[1] = static_cast<int>(mod->getSettingValue<int64_t>("randomize"));
    for (int i = 0; i < TOGGLES; ++i) v.toggles[i] = mod->getSettingValue<bool>(TOGGLE_KEYS[i]);
    v.hasPos = mod->getSavedValue<bool>("window-has-pos", false);
    v.x = static_cast<int>(mod->getSavedValue<int64_t>("window-x", 0));
    v.y = static_cast<int>(mod->getSavedValue<int64_t>("window-y", 0));
    v.sizePercent = static_cast<int>(mod->getSettingValue<int64_t>("window-size"));
    return v;
}

// Writes only what changed, so one click is one setting change.
void writeSettings(Values const& v) {
    auto mod = Mod::get();
    g_writing = true;
    auto setInt = [&](char const* key, int value) {
        if (mod->getSettingValue<int64_t>(key) != value) mod->setSettingValue<int64_t>(key, value);
    };
    setInt("buffer-ticks", v.sliders[0]);
    setInt("randomize", v.sliders[1]);
    for (int i = 0; i < TOGGLES; ++i) {
        if (mod->getSettingValue<bool>(TOGGLE_KEYS[i]) != v.toggles[i]) mod->setSettingValue<bool>(TOGGLE_KEYS[i], v.toggles[i]);
    }
    g_writing = false;
}

void save() {
    Values copy = g_values;
    queueInMainThread([copy] { writeSettings(copy); });
}

void savePosition(HWND wnd) {
    RECT r;
    if (!GetWindowRect(wnd, &r)) return;
    int x = r.left;
    int y = r.top;
    queueInMainThread([x, y] {
        auto mod = Mod::get();
        mod->setSavedValue<bool>("window-has-pos", true);
        mod->setSavedValue<int64_t>("window-x", x);
        mod->setSavedValue<int64_t>("window-y", y);
    });
}

// ===========================================================================
// Drawing helpers
// ===========================================================================

gp::Color col(uint32_t rgb, int alpha = 255) {
    return gp::Color(static_cast<BYTE>(std::clamp(alpha, 0, 255)), static_cast<BYTE>((rgb >> 16) & 0xff),
        static_cast<BYTE>((rgb >> 8) & 0xff), static_cast<BYTE>(rgb & 0xff));
}

RectF box(float x, float y, float w, float h) { return RectF(x, y, w, h); }

void fill(gp::Graphics& g, RectF r, gp::Color c) {
    if (r.Width <= 0.f || r.Height <= 0.f) return;
    gp::SolidBrush brush(c);
    g.FillRectangle(&brush, r);
}

void line(gp::Graphics& g, float x0, float y0, float x1, float y1, gp::Color c, float width = 1.f) {
    gp::Pen pen(c, width);
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapRound);
    g.DrawLine(&pen, x0, y0, x1, y1);
}

void hline(gp::Graphics& g, float y, uint32_t c = C_LINE) {
    gp::Pen pen(col(c), 1.f);
    g.DrawLine(&pen, PAD, y + 0.5f, W - PAD, y + 0.5f);
}

void circle(gp::Graphics& g, float cx, float cy, float r, gp::Color c) {
    gp::SolidBrush brush(c);
    g.FillEllipse(&brush, cx - r, cy - r, r * 2.f, r * 2.f);
}

// Older attempts in the log fade a little.
int ageAlpha(int age) { return std::max(120, 255 - age * 45); }

enum class Face { Ui, Semi, Light, Mono, MonoBold };
enum Align : int { LEFT = 0, CENTER = 1, RIGHT = 2 };

gp::FontFamily* g_family[4] {}; // Segoe UI, Semibold, Light, Consolas
struct FontEntry {
    Face face;
    float size;
    gp::Font* font;
};
std::vector<FontEntry> g_fonts;
gp::StringFormat* g_line[3] {}; // one line, vertically centered, "..." when too long
gp::StringFormat* g_top = nullptr; // one line, top aligned
gp::StringFormat* g_wrap = nullptr; // wrapping, centered

gp::FontFamily* loadFamily(std::initializer_list<wchar_t const*> names, bool mono) {
    for (auto name : names) {
        auto family = new gp::FontFamily(name);
        if (family->GetLastStatus() == gp::Ok && family->IsAvailable()) return family;
        delete family;
    }
    return (mono ? gp::FontFamily::GenericMonospace() : gp::FontFamily::GenericSansSerif())->Clone();
}

void initText() {
    g_family[0] = loadFamily({ L"Segoe UI" }, false);
    g_family[1] = loadFamily({ L"Segoe UI Semibold", L"Segoe UI" }, false);
    g_family[2] = loadFamily({ L"Segoe UI Light", L"Segoe UI" }, false);
    g_family[3] = loadFamily({ L"Consolas", L"Cascadia Mono", L"Courier New" }, true);

    gp::StringAlignment aligns[3] { gp::StringAlignmentNear, gp::StringAlignmentCenter, gp::StringAlignmentFar };
    for (int i = 0; i < 3; ++i) {
        auto f = new gp::StringFormat(gp::StringFormat::GenericTypographic());
        f->SetAlignment(aligns[i]);
        f->SetLineAlignment(gp::StringAlignmentCenter);
        f->SetFormatFlags((f->GetFormatFlags() & ~gp::StringFormatFlagsLineLimit) | gp::StringFormatFlagsNoWrap | gp::StringFormatFlagsMeasureTrailingSpaces);
        f->SetTrimming(gp::StringTrimmingEllipsisCharacter);
        g_line[i] = f;
    }
    g_top = new gp::StringFormat(gp::StringFormat::GenericTypographic());
    g_top->SetLineAlignment(gp::StringAlignmentNear);
    g_top->SetFormatFlags((g_top->GetFormatFlags() & ~gp::StringFormatFlagsLineLimit) | gp::StringFormatFlagsNoWrap | gp::StringFormatFlagsMeasureTrailingSpaces);
    g_wrap = new gp::StringFormat();
    g_wrap->SetAlignment(gp::StringAlignmentCenter);
    g_wrap->SetLineAlignment(gp::StringAlignmentCenter);
    g_wrap->SetTrimming(gp::StringTrimmingEllipsisWord);
}

gp::FontFamily* familyOf(Face face) {
    switch (face) {
        case Face::Semi: return g_family[1];
        case Face::Light: return g_family[2];
        case Face::Mono:
        case Face::MonoBold: return g_family[3];
        default: return g_family[0];
    }
}

int styleOf(Face face) {
    int style = face == Face::MonoBold ? gp::FontStyleBold : gp::FontStyleRegular;
    if (!familyOf(face)->IsStyleAvailable(style)) style = gp::FontStyleRegular;
    return style;
}

gp::Font* font(Face face, float size) {
    for (auto const& e : g_fonts) {
        if (e.face == face && e.size == size) return e.font;
    }
    auto f = new gp::Font(familyOf(face), size, styleOf(face), gp::UnitPixel);
    g_fonts.push_back({ face, size, f });
    return f;
}

float ascentOf(Face face, float size) {
    auto fam = familyOf(face);
    int style = styleOf(face);
    return size * fam->GetCellAscent(style) / std::max<UINT16>(1, fam->GetEmHeight(style));
}

float lineHeightOf(Face face, float size) {
    auto fam = familyOf(face);
    int style = styleOf(face);
    return size * fam->GetLineSpacing(style) / std::max<UINT16>(1, fam->GetEmHeight(style));
}

void text(gp::Graphics& g, std::wstring_view s, Face face, float size, gp::Color c, RectF r, int align = LEFT) {
    if (s.empty()) return;
    gp::SolidBrush brush(c);
    g.DrawString(s.data(), static_cast<INT>(s.size()), font(face, size), r, g_line[align], &brush);
}

float measure(gp::Graphics& g, std::wstring_view s, Face face, float size) {
    if (s.empty()) return 0.f;
    RectF out;
    g.MeasureString(s.data(), static_cast<INT>(s.size()), font(face, size), gp::PointF(0.f, 0.f), g_top, &out);
    return out.Width;
}

// Pieces in different styles sharing one baseline.
struct Run {
    std::wstring_view s;
    Face face;
    float size;
    gp::Color color;
};

void runs(gp::Graphics& g, std::initializer_list<Run> parts, RectF r, int align = LEFT) {
    float total = 0.f;
    float tallest = 0.f;
    float baseAscent = 0.f;
    for (auto const& p : parts) {
        if (p.s.empty()) continue;
        total += measure(g, p.s, p.face, p.size);
        float lh = lineHeightOf(p.face, p.size);
        if (lh > tallest) {
            tallest = lh;
            baseAscent = ascentOf(p.face, p.size);
        }
    }
    float baseline = r.Y + (r.Height - tallest) / 2.f + baseAscent;
    float x = align == LEFT ? r.X : align == RIGHT ? r.X + r.Width - total : r.X + (r.Width - total) / 2.f;
    x = std::max(x, r.X);
    for (auto const& p : parts) {
        if (p.s.empty()) continue;
        float w = measure(g, p.s, p.face, p.size);
        if (x + w > r.X + r.Width + 0.5f) break;
        gp::SolidBrush brush(p.color);
        g.DrawString(p.s.data(), static_cast<INT>(p.s.size()), font(p.face, p.size),
            gp::PointF(x, baseline - ascentOf(p.face, p.size)), g_top, &brush);
        x += w;
    }
}

// Letter-spaced capitals.
void spaced(gp::Graphics& g, std::wstring_view s, Face face, float size, gp::Color c, RectF r, float spacing = 1.5f, int align = LEFT) {
    float total = 0.f;
    for (size_t i = 0; i < s.size(); ++i) total += measure(g, s.substr(i, 1), face, size);
    total += spacing * static_cast<float>(s.empty() ? 0 : s.size() - 1);
    float x = align == LEFT ? r.X : align == RIGHT ? r.X + r.Width - total : r.X + (r.Width - total) / 2.f;
    for (size_t i = 0; i < s.size(); ++i) {
        auto ch = s.substr(i, 1);
        float w = measure(g, ch, face, size);
        text(g, ch, face, size, c, RectF(x, r.Y, w + 4.f, r.Height));
        x += w + spacing;
    }
}

std::wstring fmtw(wchar_t const* format, ...) {
    wchar_t buf[160];
    va_list args;
    va_start(args, format);
    std::vswprintf(buf, 160, format, args);
    va_end(args);
    return buf;
}

wchar_t const* plural(int n) { return n == 1 ? L"" : L"s"; }

// ===========================================================================
// Geometry and hit testing
// ===========================================================================

constexpr int HIT_NONE = -1;
constexpr int HIT_CLOSE = 0;
constexpr int HIT_MINIMIZE = 1;
constexpr int HIT_TOGGLE = 10; // + toggle
constexpr int HIT_SLIDER = 20; // + slider

RectF closeRect() { return box(W - PAD - 28.f, 12.f, 28.f, 24.f); }
RectF minimizeRect() { return box(W - PAD - 58.f, 12.f, 28.f, 24.f); }
RectF sliderRow(int i) { return box(PAD, SLIDER_Y + i * (SLIDER_H + 1.f), W - 2 * PAD, SLIDER_H); }
float trackY(int i) { return sliderRow(i).Y + 58.f; }
RectF toggleRow(int i) { return box(PAD, TOGGLE_Y + i * TOGGLE_H, W - 2 * PAD, TOGGLE_H); }
RectF footerRect() { return box(PAD, H - 14.f - FOOTER_H, W - 2 * PAD, FOOTER_H); }
RectF logRect() { return box(PAD, LOG_Y, W - 2 * PAD, footerRect().Y - 8.f - LOG_Y); }

float thumbX(int v) { return TRACK_X0 + (TRACK_X1 - TRACK_X0) * (static_cast<float>(v) / MAX_TICKS); }

int valueAt(float x) {
    float t = (x - TRACK_X0) / (TRACK_X1 - TRACK_X0);
    return std::clamp(static_cast<int>(std::lround(t * MAX_TICKS)), 0, MAX_TICKS);
}

bool within(int hit, int base, int count) { return hit >= base && hit < base + count; }

int hitTest(float x, float y) {
    if (closeRect().Contains(x, y)) return HIT_CLOSE;
    if (minimizeRect().Contains(x, y)) return HIT_MINIMIZE;
    for (int i = 0; i < TOGGLES; ++i) {
        if (toggleRow(i).Contains(x, y)) return HIT_TOGGLE + i;
    }
    for (int i = 0; i < SLIDERS; ++i) {
        if (sliderRow(i).Contains(x, y)) return HIT_SLIDER + i;
    }
    return HIT_NONE;
}

std::wstring help() {
    int h = g_dragging >= 0 ? HIT_SLIDER + g_dragging : g_hover;
    if (within(h, HIT_SLIDER, SLIDERS)) return SLIDER_HELP[h - HIT_SLIDER];
    if (within(h, HIT_TOGGLE, TOGGLES)) return TOGGLE_HELP[h - HIT_TOGGLE];
    return {};
}

// ===========================================================================
// Painting
// ===========================================================================

void paintTitle(gp::Graphics& g) {
    bool on = g_values.toggles[0] && g_values.sliders[0] > 0;
    spaced(g, L"INPUT BUFFER", Face::Semi, 13.f, col(C_TEXT), box(PAD, 0, 200, TITLE_H), 2.f);
    // state dot: filled when the buffer is on
    float dx = PAD + 152.f;
    if (on) circle(g, dx, TITLE_H / 2.f, 3.5f, col(C_TEXT));
    else {
        gp::Pen pen(col(C_FAINT), 1.2f);
        g.DrawEllipse(&pen, dx - 3.5f, TITLE_H / 2.f - 3.5f, 7.f, 7.f);
    }

    auto mr = minimizeRect();
    auto cr = closeRect();
    float my = mr.Y + mr.Height / 2.f;
    float mx = mr.X + mr.Width / 2.f;
    line(g, mx - 5.f, my, mx + 5.f, my, col(g_hover == HIT_MINIMIZE ? C_TEXT : C_FAINT), 1.5f);
    float cx = cr.X + cr.Width / 2.f;
    float cy = cr.Y + cr.Height / 2.f;
    auto cc = col(g_hover == HIT_CLOSE ? C_DANGER : C_FAINT);
    line(g, cx - 5.f, cy - 5.f, cx + 5.f, cy + 5.f, cc, 1.5f);
    line(g, cx + 5.f, cy - 5.f, cx - 5.f, cy + 5.f, cc, 1.5f);
    hline(g, TITLE_H);
}

// Saves this attempt, this level, and how early they were on average.
void paintStats(gp::Graphics& g) {
    int attemptSaves = 0;
    int attemptNumber = 0;
    if (!g_attempts.empty()) {
        attemptSaves = g_attempts.front().saves;
        attemptNumber = g_attempts.front().number;
    }
    std::wstring values[3] {
        std::to_wstring(attemptSaves),
        std::to_wstring(g_levelSaves),
        g_levelSaves > 0 ? fmtw(L"%.1f", g_levelMs / g_levelSaves) : std::wstring(L"–"),
    };
    std::wstring labels[3] {
        attemptNumber > 0 ? fmtw(L"ATTEMPT %d", attemptNumber) : std::wstring(L"ATTEMPT"),
        L"THIS LEVEL",
        L"AVG MS EARLY",
    };
    float colW = (W - 2 * PAD) / 3.f;
    for (int i = 0; i < 3; ++i) {
        float x = PAD + i * colW;
        text(g, values[i], Face::Light, 22.f, col(C_TEXT), box(x, STATS_Y + 6.f, colW, 28.f));
        spaced(g, labels[i], Face::Ui, 9.5f, col(C_FAINT), box(x, STATS_Y + 34.f, colW, 18.f), 1.3f);
    }
    hline(g, STATS_Y + STATS_H);
}

void paintSliders(gp::Graphics& g) {
    for (int i = 0; i < SLIDERS; ++i) {
        RectF row = sliderRow(i);
        bool active = g_hover == HIT_SLIDER + i || g_dragging == i;
        if (active) fill(g, box(row.X - 6.f, row.Y + 1.f, row.Width + 12.f, row.Height - 2.f), col(C_HOVER));
        int v = g_values.sliders[i];
        bool on = v > 0;
        RectF head = box(row.X, row.Y + 12.f, row.Width, 30.f);
        spaced(g, SLIDER_NAMES[i], Face::Ui, 11.f, col(active ? C_TEXT : C_MUTED), head);
        if (!on) {
            text(g, L"off", Face::Light, 20.f, col(C_MUTED), head, RIGHT);
        }
        else if (i == 0) {
            auto big = std::to_wstring(v);
            auto small = fmtw(L" tick%ls  ·  %.1f ms", plural(v), v * 1000.0 / 240.0);
            runs(g, { { big, Face::Light, 20.f, col(C_TEXT) }, { small, Face::Ui, 12.f, col(C_MUTED) } }, head, RIGHT);
        }
        else {
            auto big = fmtw(L"+%d", v);
            auto small = fmtw(L" tick%ls", plural(v));
            runs(g, { { big, Face::Light, 20.f, col(C_TEXT) }, { small, Face::Ui, 12.f, col(C_MUTED) } }, head, RIGHT);
        }

        float y = trackY(i);
        float hx = thumbX(v);
        fill(g, box(TRACK_X0, y - 1.f, TRACK_X1 - TRACK_X0, 2.f), col(C_TRACK));
        if (hx > TRACK_X0) fill(g, box(TRACK_X0, y - 1.f, hx - TRACK_X0, 2.f), col(C_TEXT));
        circle(g, hx, y, 7.f, col(active ? 0xffffff : C_TEXT));
        hline(g, row.Y + row.Height);
    }
}

void paintToggles(gp::Graphics& g) {
    for (int i = 0; i < TOGGLES; ++i) {
        RectF r = toggleRow(i);
        bool on = g_values.toggles[i];
        bool hover = g_hover == HIT_TOGGLE + i;
        if (hover) fill(g, box(r.X - 6.f, r.Y + 1.f, r.Width + 12.f, r.Height - 2.f), col(C_HOVER));
        text(g, TOGGLE_NAMES[i], Face::Ui, 13.f, col(on || hover ? C_TEXT : C_MUTED), r);
        text(g, on ? L"ON" : L"OFF", on ? Face::Semi : Face::Ui, 12.f, col(on ? C_TEXT : C_MUTED), r, RIGHT);
        hline(g, r.Y + r.Height, i + 1 < TOGGLES ? C_DIVIDER : C_LINE);
    }
}

void paintItem(gp::Graphics& g, Item const& it, int age, RectF r) {
    int alpha = ageAlpha(age);
    text(g, it.time, Face::Mono, 12.f, col(C_FAINT, alpha), box(r.X, r.Y, 62.f, r.Height));
    float x = r.X + 62.f;
    if (!it.note.empty()) {
        text(g, it.note, Face::Mono, 12.f, col(C_MUTED, alpha), box(x, r.Y, r.X + r.Width - x, r.Height));
        return;
    }
    auto const& s = it.save;
    if (s.percent >= 0) {
        text(g, fmtw(L"%d%%", s.percent), Face::MonoBold, 12.f, col(C_TEXT, alpha), box(x, r.Y, 42.f, r.Height));
    }
    x += 42.f;
    auto body = fmtw(L"+%d  %4.1f ms  %ls %ls", s.ticks, s.ms, MODE_NAMES[std::clamp(s.mode, 0, MODES - 1)],
        s.release ? L"release" : L"press");
    runs(g, { { body, Face::Mono, 12.f, col(C_TEXT, alpha) }, { s.random ? L"  rnd" : L"", Face::Mono, 12.f, col(C_FAINT, alpha) } },
        box(x, r.Y, r.X + r.Width - x, r.Height));
}

// Newest attempt first, its newest line first.
void paintLog(gp::Graphics& g) {
    RectF area = logRect();
    bool current = !g_attempts.empty() && g_attempts.front().number > 0;
    std::wstring title = current ? fmtw(L"LOG · ATTEMPT %d", g_attempts.front().number) : std::wstring(L"LOG");
    spaced(g, title, Face::Ui, 11.f, col(C_MUTED), box(area.X, area.Y, area.Width, 20.f));

    constexpr float LH = 18.f;
    float y = area.Y + 24.f;
    float bottom = area.Y + area.Height;
    bool any = false;
    for (size_t ai = 0; ai < g_attempts.size(); ++ai) {
        auto const& a = g_attempts[ai];
        int age = static_cast<int>(ai);
        if (a.number > 0 && ai > 0) {
            if (y + 8.f + LH + (a.items.empty() ? 0.f : LH) > bottom) return;
            y += 8.f;
            RectF hr = box(area.X, y, area.Width, LH);
            spaced(g, fmtw(L"ATTEMPT %d", a.number), Face::Ui, 11.f, col(C_FAINT), hr);
            if (a.death >= 0) text(g, fmtw(L"died at %d%%", a.death), Face::Ui, 11.f, col(C_DANGER, 200), hr, RIGHT);
            y += LH;
        }
        for (auto it = a.items.rbegin(); it != a.items.rend(); ++it) {
            if (y + LH > bottom) return;
            paintItem(g, *it, age, box(area.X, y, area.Width, LH));
            y += LH;
            any = true;
        }
    }
    if (!any && y + LH <= bottom) {
        text(g, L"Nothing held back yet", Face::Ui, 12.f, col(C_MUTED), box(area.X, y, area.Width, LH));
    }
}

void paintFooter(gp::Graphics& g) {
    RectF r = footerRect();
    auto h = help();
    gp::SolidBrush brush(col(h.empty() ? C_FAINT : C_TEXT));
    std::wstring_view s = h.empty() ? std::wstring_view(IDLE_HELP) : std::wstring_view(h);
    g.DrawString(s.data(), static_cast<INT>(s.size()), font(Face::Ui, h.empty() ? 12.f : 12.5f), r, g_wrap, &brush);
}

void paintScene(gp::Graphics& g) {
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
    g.ScaleTransform(g_scale, g_scale);
    g.Clear(col(C_BG));
    {
        gp::Pen pen(col(C_LINE), 1.f);
        g.DrawRectangle(&pen, box(0, 0, W - 1.f, H - 1.f));
    }
    paintTitle(g);
    paintStats(g);
    paintSliders(g);
    paintToggles(g);
    paintLog(g);
    paintFooter(g);
}

// Back buffer, reused between paints.
HDC g_memDC = nullptr;
HBITMAP g_memBitmap = nullptr;
HGDIOBJ g_memOld = nullptr;
int g_memW = 0;
int g_memH = 0;

void paint(HWND wnd, HDC target) {
    RECT client;
    GetClientRect(wnd, &client);
    int pw = client.right - client.left;
    int ph = client.bottom - client.top;
    if (pw <= 0 || ph <= 0) return;
    if (!g_memDC || pw != g_memW || ph != g_memH) {
        if (g_memDC) {
            SelectObject(g_memDC, g_memOld);
            DeleteObject(g_memBitmap);
            DeleteDC(g_memDC);
        }
        g_memDC = CreateCompatibleDC(target);
        g_memBitmap = CreateCompatibleBitmap(target, pw, ph);
        g_memOld = SelectObject(g_memDC, g_memBitmap);
        g_memW = pw;
        g_memH = ph;
    }
    {
        gp::Graphics g(g_memDC);
        paintScene(g);
    }
    BitBlt(target, 0, 0, pw, ph, g_memDC, 0, 0, SRCCOPY);
}

// ===========================================================================
// Log events
// ===========================================================================

void addItem(Item&& item) {
    if (g_attempts.empty()) g_attempts.emplace_front();
    auto& a = g_attempts.front();
    a.items.push_back(std::move(item));
    if (a.items.size() > MAX_ITEMS) a.items.erase(a.items.begin());
}

void handleEvent(Event& e) {
    switch (e.type) {
        case Event::NewAttempt:
            if (e.newLevel) {
                g_levelSaves = 0;
                g_levelMs = 0.0;
            }
            // an attempt where nothing happened is reused instead of stacking headers
            if (!g_attempts.empty() && g_attempts.front().items.empty()) {
                auto& a = g_attempts.front();
                a.number = e.number;
                a.death = -1;
                a.saves = 0;
            }
            else {
                Attempt a;
                a.number = e.number;
                g_attempts.push_front(std::move(a));
                while (g_attempts.size() > MAX_ATTEMPTS) g_attempts.pop_back();
            }
            break;
        case Event::Death:
            if (!g_attempts.empty() && g_attempts.front().number > 0 && g_attempts.front().death < 0) {
                g_attempts.front().death = e.number;
            }
            break;
        case Event::Saved:
            g_levelSaves++;
            g_levelMs += e.item.save.ms;
            addItem(std::move(e.item));
            g_attempts.front().saves++;
            break;
        case Event::Note:
            addItem(std::move(e.item));
            break;
    }
}

// ===========================================================================
// Window
// ===========================================================================

using DwmSetFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

void styleFrame(HWND wnd) {
    static DwmSetFn set = [] {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<DwmSetFn>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
    }();
    if (!set) return;
    int corners = 3; // small round (Windows 11)
    set(wnd, 33, &corners, sizeof(corners));
    BOOL dark = TRUE;
    set(wnd, 20, &dark, sizeof(dark));
    COLORREF border = RGB((C_LINE >> 16) & 0xff, (C_LINE >> 8) & 0xff, C_LINE & 0xff);
    set(wnd, 34, &border, sizeof(border));
}

bool isOnScreen(int x, int y) {
    POINT pt { x + 40, y + 20 };
    return MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) != nullptr;
}

BOOL CALLBACK findSecondMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    MONITORINFO info { sizeof(info) };
    if (GetMonitorInfoW(monitor, &info) && !(info.dwFlags & MONITORINFOF_PRIMARY)) {
        *reinterpret_cast<RECT*>(data) = info.rcWork;
        return FALSE;
    }
    return TRUE;
}

// First show: the last saved spot, else the middle of a second monitor, else next to the game.
void place(HWND wnd, Values const& v) {
    RECT r;
    GetWindowRect(wnd, &r);
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (v.hasPos && isOnScreen(v.x, v.y)) {
        SetWindowPos(wnd, nullptr, v.x, v.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    RECT second {};
    EnumDisplayMonitors(nullptr, nullptr, findSecondMonitor, reinterpret_cast<LPARAM>(&second));
    if (second.right > second.left) {
        SetWindowPos(wnd, nullptr, second.left + (second.right - second.left - w) / 2,
            second.top + (second.bottom - second.top - h) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    RECT game {};
    HWND gameWnd = g_game.load();
    if (gameWnd && GetWindowRect(gameWnd, &game)) {
        SetWindowPos(wnd, nullptr, game.left + 40, game.top + 60, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// 1x on 1080p, 1.33x on 1440p, 2x on 4K (by the monitor's shorter side), times
// the Window Size setting, never bigger than the screen.
void applyLayout(HWND wnd, bool force = false) {
    float scale = 1.f;
    MONITORINFO info { sizeof(info) };
    bool haveMonitor = GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &info) != 0;
    if (haveMonitor) {
        float mw = static_cast<float>(info.rcMonitor.right - info.rcMonitor.left);
        float mh = static_cast<float>(info.rcMonitor.bottom - info.rcMonitor.top);
        scale = std::clamp(std::min(mw, mh) / 1080.f, 0.75f, 3.f);
    }
    scale *= std::clamp(g_values.sizePercent / 100.f, 0.5f, 2.f);
    scale = std::clamp(scale, 0.5f, 4.f);
    if (haveMonitor) {
        float fit = std::min((info.rcWork.right - info.rcWork.left) * 0.95f / W, (info.rcWork.bottom - info.rcWork.top) * 0.95f / H);
        scale = std::max(0.5f, std::min(scale, fit));
    }
    if (!force && std::abs(scale - g_scale) < 0.01f) return;
    g_scale = scale;
    RECT r;
    GetWindowRect(wnd, &r);
    SetWindowPos(wnd, nullptr, r.left, r.top, static_cast<int>(std::lround(W * g_scale)),
        static_cast<int>(std::lround(H * g_scale)), SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(wnd, nullptr, FALSE);
}

void showWindow(HWND wnd, Values* values) {
    g_values = *values;
    delete values;
    applyLayout(wnd, !g_placed);
    if (!g_placed) {
        place(wnd, g_values);
        g_placed = true;
        applyLayout(wnd); // it may be on a monitor with a different size now
    }
    g_hover = HIT_NONE;
    // come to the front without taking focus from the game (also un-minimizes)
    ShowWindow(wnd, SW_SHOWNOACTIVATE);
    SetWindowPos(wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(wnd, nullptr, FALSE);
}

// True if the game sits on top of this window (one monitor).
bool coveredByGame(HWND wnd) {
    HWND game = g_game.load();
    if (!game || game == wnd || !IsWindowVisible(game) || IsIconic(game)) return false;
    RECT a, b, both;
    if (!GetWindowRect(wnd, &a) || !GetWindowRect(game, &b) || !IntersectRect(&both, &a, &b)) return false;
    for (HWND w = GetWindow(wnd, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (w == game) return true;
    }
    return false;
}

void endDrag(HWND wnd) {
    if (g_dragging < 0) return;
    g_dragging = -1;
    if (GetCapture() == wnd) ReleaseCapture();
    save();
    InvalidateRect(wnd, nullptr, FALSE);
}

void hide(HWND wnd) {
    endDrag(wnd);
    bool hadFocus = GetForegroundWindow() == wnd;
    ShowWindow(wnd, SW_HIDE);
    if (hadFocus) {
        if (HWND game = g_game.load()) SetForegroundWindow(game);
    }
}

void setSlider(HWND wnd, int i, int v) {
    v = std::clamp(v, 0, MAX_TICKS);
    if (g_values.sliders[i] == v) return;
    g_values.sliders[i] = v;
    InvalidateRect(wnd, nullptr, FALSE);
}

float logicalX(LPARAM lParam) { return GET_X_LPARAM(lParam) / g_scale; }
float logicalY(LPARAM lParam) { return GET_Y_LPARAM(lParam) / g_scale; }

void updateHover(HWND wnd, float x, float y) {
    int hover = hitTest(x, y);
    if (hover != g_hover) {
        g_hover = hover;
        InvalidateRect(wnd, nullptr, FALSE);
    }
}

LRESULT CALLBACK windowProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PB_SHOW:
            showWindow(wnd, reinterpret_cast<Values*>(lParam));
            return 0;

        case WM_PB_TOGGLE: {
            auto values = reinterpret_cast<Values*>(lParam);
            if (IsWindowVisible(wnd) && !IsIconic(wnd) && !coveredByGame(wnd)) {
                delete values;
                hide(wnd);
            }
            else {
                showWindow(wnd, values);
            }
            return 0;
        }

        case WM_PB_OPEN: {
            auto values = reinterpret_cast<Values*>(lParam);
            if (IsWindowVisible(wnd)) delete values;
            else showWindow(wnd, values);
            return 0;
        }

        case WM_PB_SYNC: {
            auto values = reinterpret_cast<Values*>(lParam);
            if (g_dragging < 0) {
                for (int i = 0; i < SLIDERS; ++i) g_values.sliders[i] = values->sliders[i];
                for (int i = 0; i < TOGGLES; ++i) g_values.toggles[i] = values->toggles[i];
                g_values.sizePercent = values->sizePercent;
                applyLayout(wnd);
                InvalidateRect(wnd, nullptr, FALSE);
            }
            delete values;
            return 0;
        }

        case WM_PB_EVENT: {
            auto e = reinterpret_cast<Event*>(lParam);
            handleEvent(*e);
            delete e;
            if (IsWindowVisible(wnd)) InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE; // clicking never takes keyboard focus from the game

        case WM_DPI_CHANGED:
            applyLayout(wnd);
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && g_hover != HIT_NONE) {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649))); // hand
                return TRUE;
            }
            break;

        case WM_NCHITTEST: {
            // drag the window by its title bar
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(wnd, &pt);
            float x = pt.x / g_scale;
            float y = pt.y / g_scale;
            if (y < TITLE_H && hitTest(x, y) == HIT_NONE) return HTCAPTION;
            return HTCLIENT;
        }

        case WM_EXITSIZEMOVE:
            applyLayout(wnd); // moved to another monitor?
            savePosition(wnd);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(wnd, &ps);
            paint(wnd, dc);
            EndPaint(wnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            float x = logicalX(lParam);
            float y = logicalY(lParam);
            if (g_dragging >= 0) {
                setSlider(wnd, g_dragging, valueAt(x));
                return 0;
            }
            updateHover(wnd, x, y);
            TRACKMOUSEEVENT tme { sizeof(tme), TME_LEAVE, wnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            if (g_hover != HIT_NONE && g_dragging < 0) {
                g_hover = HIT_NONE;
                InvalidateRect(wnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            float x = logicalX(lParam);
            float y = logicalY(lParam);
            int hit = hitTest(x, y);
            if (hit == HIT_CLOSE) {
                hide(wnd);
            }
            else if (hit == HIT_MINIMIZE) {
                ShowWindow(wnd, SW_SHOWMINNOACTIVE);
            }
            else if (within(hit, HIT_TOGGLE, TOGGLES)) {
                g_values.toggles[hit - HIT_TOGGLE] = !g_values.toggles[hit - HIT_TOGGLE];
                InvalidateRect(wnd, nullptr, FALSE);
                save();
            }
            else if (within(hit, HIT_SLIDER, SLIDERS)) {
                int i = hit - HIT_SLIDER;
                g_focus = i;
                // grab only near the track itself
                if (std::abs(y - trackY(i)) <= 16.f && x >= TRACK_X0 - 14.f && x <= TRACK_X1 + 14.f) {
                    g_dragging = i;
                    SetCapture(wnd);
                    setSlider(wnd, i, valueAt(x));
                }
            }
            return 0;
        }

        case WM_LBUTTONUP:
            endDrag(wnd);
            return 0;

        case WM_CAPTURECHANGED:
            if (g_dragging >= 0 && reinterpret_cast<HWND>(lParam) != wnd) endDrag(wnd);
            return 0;

        case WM_MOUSEWHEEL: {
            if (g_dragging >= 0) return 0;
            g_wheel += GET_WHEEL_DELTA_WPARAM(wParam);
            int steps = g_wheel / WHEEL_DELTA;
            g_wheel -= steps * WHEEL_DELTA;
            if (steps == 0) return 0;
            // only scrolling over a slider changes it (where the wheel happened, not the last hover)
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(wnd, &pt);
            int hit = hitTest(pt.x / g_scale, pt.y / g_scale);
            if (within(hit, HIT_SLIDER, SLIDERS)) {
                int i = hit - HIT_SLIDER;
                g_focus = i;
                setSlider(wnd, i, g_values.sliders[i] + steps);
                save();
            }
            return 0;
        }

        case WM_KEYDOWN:
            switch (wParam) {
                case VK_ESCAPE:
                case VK_F7:
                    hide(wnd);
                    return 0;
                case VK_LEFT:
                case VK_DOWN:
                    setSlider(wnd, g_focus, g_values.sliders[g_focus] - 1);
                    save();
                    return 0;
                case VK_RIGHT:
                case VK_UP:
                    setSlider(wnd, g_focus, g_values.sliders[g_focus] + 1);
                    save();
                    return 0;
                case VK_TAB:
                    g_focus = (g_focus + 1) % SLIDERS;
                    return 0;
            }
            break;

        case WM_CLOSE:
            hide(wnd); // keep it around, just hidden
            return 0;
    }
    return DefWindowProcW(wnd, msg, wParam, lParam);
}

// A show request made before the window existed.
struct Pending {
    UINT msg;
    Values values;
};
std::atomic<Pending*> g_pending = nullptr;

void takePending(HWND wnd) {
    if (auto p = g_pending.exchange(nullptr)) {
        auto values = new Values(p->values);
        UINT msg = p->msg;
        delete p;
        if (!PostMessageW(wnd, msg, 0, reinterpret_cast<LPARAM>(values))) delete values;
    }
}

void windowThread() {
    gp::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (gp::GdiplusStartup(&token, &input, nullptr) != gp::Ok) {
        g_started = false;
        return;
    }
    initText();

    // per-monitor DPI for this window only, so it stays sharp on every monitor
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetContextFn = HANDLE(WINAPI*)(HANDLE);
        if (auto setContext = reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetThreadDpiAwarenessContext"))) {
            setContext(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4))); // PER_MONITOR_AWARE_V2
        }
    }

    auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW cls {};
    cls.style = CS_DROPSHADOW;
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // arrow
    cls.lpszClassName = L"PredictiveInputBufferWindow";
    RegisterClassW(&cls);

    // A normal app window (own taskbar button) so it can live on another monitor.
    // NOACTIVATE: clicking it never takes keyboard focus away from the game.
    HWND wnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName, L"Predictive Input Buffer",
        WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(W), static_cast<int>(H),
        nullptr, nullptr, instance, nullptr);
    if (!wnd) {
        g_started = false;
        return;
    }
    styleFrame(wnd);
    g_window = wnd;
    takePending(wnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void startThread() {
    if (!g_started.exchange(true)) std::thread(windowThread).detach();
}

// Sends a show / toggle / open request with fresh settings. If the window
// doesn't exist yet it's made in the background and picks the request up.
void request(UINT msg) {
    if (HWND wnd = g_window.load()) {
        auto values = new Values(readSettings());
        if (!PostMessageW(wnd, msg, 0, reinterpret_cast<LPARAM>(values))) delete values;
        return;
    }
    startThread();
    delete g_pending.exchange(new Pending { msg, readSettings() });
    if (HWND wnd = g_window.load()) takePending(wnd);
}

void rememberGameWindow() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_window.load()) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) g_game = fg; // only GD's own window counts
}

std::wstring widen(std::string const& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(0, len)), L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::wstring timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_s(&local, &now);
    wchar_t stamp[16];
    std::wcsftime(stamp, 16, L"%H:%M:%S", &local);
    return stamp;
}

void post(Event* e) {
    HWND wnd = g_window.load();
    if (!wnd || !PostMessageW(wnd, WM_PB_EVENT, 0, reinterpret_cast<LPARAM>(e))) delete e;
}

} // namespace

void toggle() {
    rememberGameWindow();
    request(WM_PB_TOGGLE);
}

void open() {
    rememberGameWindow();
    request(WM_PB_OPEN);
}

void prepare() {
    startThread();
}

void sync() {
    HWND wnd = g_window.load();
    if (!wnd || g_writing) return;
    auto values = new Values(readSettings());
    if (!PostMessageW(wnd, WM_PB_SYNC, 0, reinterpret_cast<LPARAM>(values))) delete values;
}

void note(std::string const& line) {
    if (!g_window.load()) return;
    auto e = new Event;
    e->type = Event::Note;
    e->item.time = timestamp();
    e->item.note = widen(line);
    post(e);
}

void saved(Save const& save) {
    if (!g_window.load()) return;
    auto e = new Event;
    e->type = Event::Saved;
    e->item.time = timestamp();
    e->item.save = save;
    post(e);
}

void attempt(int number, bool newLevel) {
    if (!g_window.load()) return;
    auto e = new Event;
    e->type = Event::NewAttempt;
    e->number = number;
    e->newLevel = newLevel;
    post(e);
}

void death(int percent) {
    if (!g_window.load()) return;
    auto e = new Event;
    e->type = Event::Death;
    e->number = percent;
    post(e);
}

} // namespace pb::window

#else

namespace pb::window {

void toggle() { openSettingsPopup(Mod::get(), false); }
void open() {}
void prepare() {}
void sync() {}
void note(std::string const&) {}
void saved(Save const&) {}
void attempt(int, bool) {}
void death(int) {}

} // namespace pb::window

#endif
