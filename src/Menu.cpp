#include "Menu.hpp"

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
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// gdiplus.h wants min/max, which NOMINMAX removes.
namespace Gdiplus {
    using std::max;
    using std::min;
}
#include <gdiplus.h>

namespace pb::menu {
namespace {

namespace gp = Gdiplus;
using RectF = gp::RectF;

// ===========================================================================
// What the window shows
// ===========================================================================

constexpr int MAX_TICKS = 24;
constexpr int SLIDER_COUNT = 2; // buffer range, randomize
constexpr int TOGGLE_COUNT = 4;
constexpr int MODE_COUNT = 8;
constexpr int BUCKETS = 20;     // 5% slices of the level for the Analytics theme

// Numbers you can scroll or step: the buffer's min and max, and randomize.
constexpr int T_MIN = 0;
constexpr int T_MAX = 1;
constexpr int T_RANDOM = 2;
constexpr int TARGET_COUNT = 3;

constexpr char const* TOGGLE_KEYS[TOGGLE_COUNT] { "enabled", "buffer-releases", "buffer-ship", "show-feedback" };
constexpr wchar_t const* TOGGLE_NAMES[TOGGLE_COUNT] { L"Enabled", L"Releases", L"Ship", L"Timing popup" };
constexpr wchar_t const* TOGGLE_SHORT[TOGGLE_COUNT] { L"Enabled", L"Releases", L"Ship", L"Popup" };
constexpr wchar_t const* TOGGLE_LOWER[TOGGLE_COUNT] { L"enabled", L"releases", L"ship", L"popup" };
constexpr wchar_t const* TOGGLE_DESCS[TOGGLE_COUNT] {
    L"Turns the buffer on or off. Same as the toggle key (F6).",
    L"Also buffers early releases for wave, ship and robot, where letting go too early kills you.",
    L"Lets the buffer work in ship mode. Off by default since ship predictions are less reliable.",
    L"Shows in-game how early each buffered click was.",
};
constexpr wchar_t const* SLIDER_DESCS[SLIDER_COUNT] {
    L"Holds early clicks that would kill you and fires them later, only using delays inside this range. Drag either handle.",
    L"Off = always the smallest safe delay (+1 tick margin). Above 0 = a random safe delay up to this many ticks above the smallest one.",
};
constexpr wchar_t const* TARGET_DESCS[TARGET_COUNT] {
    L"Smallest delay the buffer will use, in ticks (1 tick = 4.2 ms at 240 TPS).",
    L"Largest delay the buffer will use, in ticks. 0 turns buffering off.",
    L"Off = always the smallest safe delay (+1 tick margin). Above 0 = a random safe delay up to this many ticks above the smallest one.",
};
constexpr wchar_t const* THEME_BUTTON_DESC = L"Change how this window looks. Your pick is saved.";
constexpr wchar_t const* MODE_NAMES[MODE_COUNT] { L"cube", L"ship", L"ball", L"ufo", L"wave", L"robot", L"spider", L"swing" };

// ===========================================================================
// Themes
// ===========================================================================

enum class Kind { Clean, Terminal, Hud, Neon, Retro, TwoPane, Analytics, Stepper, Slate, Lines };

struct Palette {
    uint32_t bg, panel, hover, border, text, muted, faint, track, fill, handle, accent, onAccent, danger, rnd, divider;
};

struct Theme {
    char const* key;     // value of the "theme" setting
    wchar_t const* name; // shown in the dropdown
    wchar_t const* desc;
    Kind kind;
    float w, h;          // window size in logical pixels
    float radius;        // card corners
    int corners;         // Windows 11 corners: 1 square, 2 round, 3 small round
    Palette c;
};

constexpr Palette CLEAN_COLORS {
    0x1b1b1b, 0x222222, 0x2a2a2a, 0x000000, 0xf0f0f0, 0x9a9a9a, 0x8a8a8a, 0x3a3a3a,
    0xf0f0f0, 0xffffff, 0x7ade2d, 0x111111, 0xff6b6b, 0x9a9a9a, 0x444444,
};

constexpr Theme THEMES[] {
    { "Minimal", L"Minimal", L"No boxes, just lines and text.",
      Kind::Lines, 454.f, 612.f, 0.f, 3,
      { 0x121212, 0x121212, 0x1a1a1a, 0x2a2a2a, 0xececec, 0x9a9a9a, 0x8a8a8a, 0x333333,
        0xececec, 0xececec, 0xececec, 0x121212, 0xff6b6b, 0x8a8a8a, 0x1e1e1e } },
};
constexpr int THEME_COUNT = static_cast<int>(sizeof(THEMES) / sizeof(THEMES[0]));

constexpr UINT WM_PB_SHOW = WM_APP + 1;
constexpr UINT WM_PB_HIDE = WM_APP + 2;
constexpr UINT WM_PB_EVENT = WM_APP + 3;
constexpr UINT WM_PB_OPEN = WM_APP + 4;
constexpr UINT WM_PB_SYNC = WM_APP + 5;
constexpr UINT WM_PB_TOGGLE = WM_APP + 6;
constexpr UINT WM_DPI_CHANGED = 0x02E0; // WM_DPICHANGED

constexpr UINT_PTR TIMER_REPEAT = 1; // holding a - / + button
constexpr UINT_PTR TIMER_MENU = 2;   // mouse left the window with the theme list open

// WDA_EXCLUDEFROMCAPTURE, spelled out for older SDK headers.
constexpr DWORD EXCLUDE_FROM_CAPTURE = 0x00000011;

// ===========================================================================
// State
// ===========================================================================

struct Values {
    int sliders[SLIDER_COUNT] { 2, 0 }; // buffer max ticks, randomize ticks
    int minTicks = 1;                   // buffer min ticks (left handle)
    bool toggles[TOGGLE_COUNT] { true, true, false, true };
    int theme = 0;
    bool hasPos = false;
    int x = 0;
    int y = 0;
    int sizePercent = 100;
};

// One line in the log: a buffered input, or a plain note like "Buffer ON".
struct Item {
    std::wstring time;
    std::wstring note; // set for plain notes
    int percent = -1;
    int ticks = 0;
    float ms = 0.f;
    int mode = 0;
    bool release = false;
    bool random = false;
};

struct Attempt {
    int number = 0;  // 0 = outside a level
    int death = -1;  // level % it ended at, -1 if it didn't end in a death (yet)
    int saves = 0;
    std::vector<Item> items; // oldest first
};

// Sent from the game thread to the window thread.
struct Event {
    enum Type { Note, Save, NewAttempt, Death } type = Note;
    Item item;
    int number = 0; // attempt number, or death %
    bool newLevel = false;
};

constexpr size_t MAX_ATTEMPTS = 40;
constexpr size_t MAX_ITEMS = 60; // per attempt

std::atomic<HWND> g_window = nullptr;
std::atomic<HWND> g_game = nullptr;
std::atomic<bool> g_started = false;

// Everything below is only touched on the window thread.
Values g_values;
std::deque<Attempt> g_attempts; // newest first
int g_heat[BUCKETS] {};
int g_heatModes[BUCKETS][MODE_COUNT] {};
int g_levelSaves = 0;

int g_hover = -1;       // see hitTest()
int g_dragging = -1;    // slider being dragged, -1 if none
bool g_dragMin = false; // dragging the buffer's left (min) handle
int g_focus = T_MAX;    // number the arrow keys change
int g_holdStep = -1;    // - / + button being held (hit code), -1 if none
bool g_menuOpen = false;
int g_menuKey = -1;     // theme highlighted with the arrow keys
int g_wheel = 0;        // leftover wheel delta (smooth-scrolling mice)
bool g_placed = false;
float g_scale = 1.f;
int g_shown = -1;       // theme the geometry and window size are set up for

// Settings are only read and written on the main thread.
int themeIndex(std::string const& key) {
    for (int i = 0; i < THEME_COUNT; ++i) {
        if (key == THEMES[i].key) return i;
    }
    return 0;
}

Values readSettings() {
    auto mod = Mod::get();
    Values v;
    v.sliders[0] = static_cast<int>(mod->getSettingValue<int64_t>("buffer-ticks"));
    v.sliders[1] = static_cast<int>(mod->getSettingValue<int64_t>("randomize"));
    v.minTicks = static_cast<int>(mod->getSettingValue<int64_t>("buffer-min-ticks"));
    for (int i = 0; i < TOGGLE_COUNT; ++i) v.toggles[i] = mod->getSettingValue<bool>(TOGGLE_KEYS[i]);
    v.theme = themeIndex(mod->getSettingValue<std::string>("theme"));
    v.hasPos = mod->getSavedValue<bool>("window-has-pos", false);
    v.x = static_cast<int>(mod->getSavedValue<int64_t>("window-x", 0));
    v.y = static_cast<int>(mod->getSavedValue<int64_t>("window-y", 0));
    v.sizePercent = static_cast<int>(mod->getSettingValue<int64_t>("window-size"));
    return v;
}

bool g_writing = false; // main thread: the window is saving, skip the change echoes

// Only writes what changed, so one click is one setting-change event.
void writeSettings(Values const& v) {
    auto mod = Mod::get();
    g_writing = true;
    auto setInt = [&](char const* key, int value) {
        if (mod->getSettingValue<int64_t>(key) != value) mod->setSettingValue<int64_t>(key, value);
    };
    setInt("buffer-ticks", v.sliders[0]);
    setInt("randomize", v.sliders[1]);
    setInt("buffer-min-ticks", v.minTicks);
    for (int i = 0; i < TOGGLE_COUNT; ++i) {
        if (mod->getSettingValue<bool>(TOGGLE_KEYS[i]) != v.toggles[i]) mod->setSettingValue<bool>(TOGGLE_KEYS[i], v.toggles[i]);
    }
    std::string theme = THEMES[std::clamp(v.theme, 0, THEME_COUNT - 1)].key;
    if (mod->getSettingValue<std::string>("theme") != theme) mod->setSettingValue<std::string>("theme", theme);
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

Theme const& shown() { return THEMES[std::clamp(g_shown, 0, THEME_COUNT - 1)]; }

// ===========================================================================
// Values
// ===========================================================================

int lowTicks() {
    int hi = g_values.sliders[0];
    return hi > 0 ? std::min(g_values.minTicks, hi) : g_values.minTicks;
}

void setMax(int v) {
    v = std::clamp(v, 0, MAX_TICKS);
    g_values.sliders[0] = v;
    // Max pushed below min: drag the min along.
    if (v > 0 && g_values.minTicks > v) g_values.minTicks = v;
}

// `push`: raising min past max drags max along (- / + buttons, scrolling).
// Otherwise min stops at max (the range slider's left handle).
void setMin(int v, bool push) {
    v = std::clamp(v, 1, MAX_TICKS);
    int& hi = g_values.sliders[0];
    if (hi > 0 && v > hi) {
        if (push) hi = v;
        else v = hi;
    }
    g_values.minTicks = v;
}

void setRandom(int v) { g_values.sliders[1] = std::clamp(v, 0, MAX_TICKS); }

int targetValue(int target) {
    switch (target) {
        case T_MIN: return lowTicks();
        case T_MAX: return g_values.sliders[0];
        default: return g_values.sliders[1];
    }
}

void setTarget(int target, int v, bool push) {
    switch (target) {
        case T_MIN: setMin(v, push); break;
        case T_MAX: setMax(v); break;
        default: setRandom(v); break;
    }
}

void adjust(HWND wnd, int target, int delta, bool commit) {
    setTarget(target, targetValue(target) + delta, true);
    g_focus = target;
    InvalidateRect(wnd, nullptr, FALSE);
    if (commit) save();
}

// ===========================================================================
// Text formatting
// ===========================================================================

enum class Fmt { Full, Ticks, Short, Ms, Ascii, Spaced };

wchar_t const* plural(int n) { return n == 1 ? L"" : L"s"; }
double ticksToMs(int ticks) { return ticks * 1000.0 / 240.0; }

std::wstring rangeText(Fmt f) {
    int hi = g_values.sliders[0];
    if (hi <= 0) return L"off";
    int lo = lowTicks();
    wchar_t b[80];
    if (lo >= hi) {
        switch (f) {
            case Fmt::Full: std::swprintf(b, 80, L"%d tick%ls \u00b7 %.1f ms", hi, plural(hi), ticksToMs(hi)); break;
            case Fmt::Ticks: std::swprintf(b, 80, L"%d tick%ls", hi, plural(hi)); break;
            case Fmt::Ms: std::swprintf(b, 80, L"%.1f ms", ticksToMs(hi)); break;
            case Fmt::Ascii: std::swprintf(b, 80, L"%d tick%ls  %.1fms", hi, plural(hi), ticksToMs(hi)); break;
            default: std::swprintf(b, 80, L"%d", hi); break;
        }
        return b;
    }
    switch (f) {
        case Fmt::Full: std::swprintf(b, 80, L"%d\u2013%d ticks \u00b7 %.1f\u2013%.1f ms", lo, hi, ticksToMs(lo), ticksToMs(hi)); break;
        case Fmt::Ticks: std::swprintf(b, 80, L"%d\u2013%d ticks", lo, hi); break;
        case Fmt::Ms: std::swprintf(b, 80, L"%.1f\u2013%.1f ms", ticksToMs(lo), ticksToMs(hi)); break;
        case Fmt::Ascii: std::swprintf(b, 80, L"%d-%d ticks  %.1f-%.1fms", lo, hi, ticksToMs(lo), ticksToMs(hi)); break;
        case Fmt::Spaced: std::swprintf(b, 80, L"%d \u2013 %d", lo, hi); break;
        default: std::swprintf(b, 80, L"%d\u2013%d", lo, hi); break;
    }
    return b;
}

std::wstring randomText(Fmt f) {
    int v = g_values.sliders[1];
    if (v <= 0) return L"off";
    wchar_t b[48];
    switch (f) {
        case Fmt::Full: std::swprintf(b, 48, L"up to +%d tick%ls", v, plural(v)); break;
        case Fmt::Ascii: std::swprintf(b, 48, L"+0..%d tick%ls", v, plural(v)); break;
        case Fmt::Ticks: std::swprintf(b, 48, L"+%d tick%ls", v, plural(v)); break;
        default: std::swprintf(b, 48, L"+%d", v); break;
    }
    return b;
}

enum class ItemFmt { Full, Arrows, Terminal, Plain, Columns, Hud };

std::wstring itemText(Item const& it, ItemFmt f) {
    if (!it.note.empty()) return it.note;
    wchar_t const* mode = MODE_NAMES[std::clamp(it.mode, 0, MODE_COUNT - 1)];
    wchar_t const* action = it.release ? L"release" : L"press";
    wchar_t const* arrow = it.release ? L"\u25bc" : L"\u25b2";
    wchar_t b[96];
    switch (f) {
        case ItemFmt::Full:
            std::swprintf(b, 96, L"+%d tick%ls (%.1f ms) %ls %ls", it.ticks, plural(it.ticks), it.ms, mode, action);
            break;
        case ItemFmt::Arrows:
            std::swprintf(b, 96, L"+%d tick%ls %.1f ms %ls %ls", it.ticks, plural(it.ticks), it.ms, mode, arrow);
            break;
        case ItemFmt::Terminal:
            std::swprintf(b, 96, L"+%dt %4.1fms %ls %ls", it.ticks, it.ms, mode, action);
            break;
        case ItemFmt::Plain:
            std::swprintf(b, 96, L"+%d tick%ls %.1f ms %ls %ls", it.ticks, plural(it.ticks), it.ms, mode, action);
            break;
        case ItemFmt::Columns:
            std::swprintf(b, 96, L"+%d  %4.1f ms  %ls %ls", it.ticks, it.ms, mode, action);
            break;
        case ItemFmt::Hud:
            std::swprintf(b, 96, L"+%d %ls %ls", it.ticks, mode, arrow);
            break;
    }
    return b;
}

std::wstring percentText(int p) { return p >= 0 ? std::to_wstring(p) + L"%" : std::wstring(); }

// ===========================================================================
// Drawing primitives
// ===========================================================================

gp::Color col(uint32_t rgb, int alpha = 255) {
    return gp::Color(
        static_cast<BYTE>(std::clamp(alpha, 0, 255)),
        static_cast<BYTE>((rgb >> 16) & 0xff), static_cast<BYTE>((rgb >> 8) & 0xff), static_cast<BYTE>(rgb & 0xff)
    );
}

// a + (b - a) * t, per channel.
uint32_t mix(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int shift) {
        float x = static_cast<float>((a >> shift) & 0xff);
        float y = static_cast<float>((b >> shift) & 0xff);
        return static_cast<uint32_t>(std::lround(x + (y - x) * t)) << shift;
    };
    return ch(16) | ch(8) | ch(0);
}

// Older attempts in the log fade out a bit.
int ageAlpha(int age) { return std::max(120, 255 - age * 45); }

RectF box(float x, float y, float w, float h) { return RectF(x, y, w, h); }
RectF inset(RectF r, float d) { return RectF(r.X + d, r.Y + d, r.Width - 2.f * d, r.Height - 2.f * d); }

void roundPath(gp::GraphicsPath& path, RectF r, float radius) {
    float d = std::min(radius * 2.f, std::min(r.Width, r.Height));
    path.AddArc(r.X, r.Y, d, d, 180.f, 90.f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.f, 90.f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.f, 90.f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.f, 90.f);
    path.CloseFigure();
}

void fill(gp::Graphics& g, RectF r, gp::Color c, float radius = 0.f) {
    if (r.Width <= 0.f || r.Height <= 0.f) return;
    gp::SolidBrush brush(c);
    if (radius < 0.5f) {
        g.FillRectangle(&brush, r);
        return;
    }
    gp::GraphicsPath path;
    roundPath(path, r, radius);
    g.FillPath(&brush, &path);
}

void stroke(gp::Graphics& g, RectF r, gp::Color c, float radius = 0.f, float width = 1.f) {
    gp::Pen pen(c, width);
    if (radius < 0.5f) {
        g.DrawRectangle(&pen, r);
        return;
    }
    gp::GraphicsPath path;
    roundPath(path, r, radius);
    g.DrawPath(&pen, &path);
}

void circle(gp::Graphics& g, float cx, float cy, float r, gp::Color c) {
    gp::SolidBrush brush(c);
    g.FillEllipse(&brush, cx - r, cy - r, r * 2.f, r * 2.f);
}

void ring(gp::Graphics& g, float cx, float cy, float r, gp::Color c, float width) {
    gp::Pen pen(c, width);
    g.DrawEllipse(&pen, cx - r, cy - r, r * 2.f, r * 2.f);
}

void line(gp::Graphics& g, float x0, float y0, float x1, float y1, gp::Color c, float width = 1.f) {
    gp::Pen pen(c, width);
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapRound);
    g.DrawLine(&pen, x0, y0, x1, y1);
}

void chevron(gp::Graphics& g, float cx, float cy, gp::Color c, float s = 3.5f, float width = 1.5f) {
    gp::Pen pen(c, width);
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapRound);
    pen.SetLineJoin(gp::LineJoinRound);
    gp::PointF pts[3] { { cx - s, cy - s / 2.f }, { cx, cy + s / 2.f }, { cx + s, cy - s / 2.f } };
    g.DrawLines(&pen, pts, 3);
}

void checkMark(gp::Graphics& g, float cx, float cy, gp::Color c, float s = 5.f, float width = 2.f) {
    gp::Pen pen(c, width);
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapRound);
    pen.SetLineJoin(gp::LineJoinRound);
    gp::PointF pts[3] { { cx - s, cy }, { cx - s * 0.3f, cy + s * 0.7f }, { cx + s, cy - s * 0.7f } };
    g.DrawLines(&pen, pts, 3);
}

void closeGlyph(gp::Graphics& g, RectF r, gp::Color c) {
    float cx = r.X + r.Width / 2.f;
    float cy = r.Y + r.Height / 2.f;
    line(g, cx - 5.f, cy - 5.f, cx + 5.f, cy + 5.f, c, 1.5f);
    line(g, cx + 5.f, cy - 5.f, cx - 5.f, cy + 5.f, c, 1.5f);
}

void minimizeGlyph(gp::Graphics& g, RectF r, gp::Color c) {
    float cx = r.X + r.Width / 2.f;
    float cy = r.Y + r.Height / 2.f;
    line(g, cx - 5.f, cy, cx + 5.f, cy, c, 1.5f);
}

// ===========================================================================
// Fonts and text (created once, kept for the life of the process)
// ===========================================================================

enum class Face { Ui, Semi, Bold, Light, Mono, MonoBold, Display };

enum Align : int { A_LEFT = 0, A_CENTER = 1, A_RIGHT = 2 };

gp::FontFamily* g_families[5] {}; // Segoe UI, Semibold, Light, Consolas, display
struct FontEntry {
    Face face;
    float size;
    gp::Font* font;
};
std::vector<FontEntry> g_fonts;
gp::StringFormat* g_line[3] {};  // one line, vertically centered, "..." when too long
gp::StringFormat* g_top = nullptr; // one line, top aligned (for baseline-aligned runs)
gp::StringFormat* g_wrap[3] {};  // wrapping, vertically centered
gp::StringFormat* g_wrapTop = nullptr;

gp::FontFamily* loadFamily(std::initializer_list<wchar_t const*> names, bool mono) {
    for (auto name : names) {
        auto family = new gp::FontFamily(name);
        if (family->GetLastStatus() == gp::Ok && family->IsAvailable()) return family;
        delete family;
    }
    return (mono ? gp::FontFamily::GenericMonospace() : gp::FontFamily::GenericSansSerif())->Clone();
}

void initText() {
    g_families[0] = loadFamily({ L"Segoe UI" }, false);
    g_families[1] = loadFamily({ L"Segoe UI Semibold", L"Segoe UI" }, false);
    g_families[2] = loadFamily({ L"Segoe UI Light", L"Segoe UI" }, false);
    g_families[3] = loadFamily({ L"Consolas", L"Cascadia Mono", L"Courier New" }, true);
    g_families[4] = loadFamily({ L"Lilita One", L"Segoe UI Black", L"Arial Black", L"Segoe UI" }, false);

    gp::StringAlignment aligns[3] { gp::StringAlignmentNear, gp::StringAlignmentCenter, gp::StringAlignmentFar };
    for (int i = 0; i < 3; ++i) {
        auto f = new gp::StringFormat(gp::StringFormat::GenericTypographic());
        f->SetAlignment(aligns[i]);
        f->SetLineAlignment(gp::StringAlignmentCenter);
        f->SetFormatFlags((f->GetFormatFlags() & ~gp::StringFormatFlagsLineLimit) | gp::StringFormatFlagsNoWrap | gp::StringFormatFlagsMeasureTrailingSpaces);
        f->SetTrimming(gp::StringTrimmingEllipsisCharacter);
        g_line[i] = f;

        auto w = new gp::StringFormat();
        w->SetAlignment(aligns[i]);
        w->SetLineAlignment(gp::StringAlignmentCenter);
        w->SetTrimming(gp::StringTrimmingEllipsisWord);
        g_wrap[i] = w;
    }
    g_top = new gp::StringFormat(gp::StringFormat::GenericTypographic());
    g_top->SetLineAlignment(gp::StringAlignmentNear);
    g_top->SetFormatFlags((g_top->GetFormatFlags() & ~gp::StringFormatFlagsLineLimit) | gp::StringFormatFlagsNoWrap | gp::StringFormatFlagsMeasureTrailingSpaces);
    g_top->SetTrimming(gp::StringTrimmingEllipsisCharacter);

    g_wrapTop = new gp::StringFormat();
    g_wrapTop->SetLineAlignment(gp::StringAlignmentNear);
    g_wrapTop->SetTrimming(gp::StringTrimmingEllipsisWord);
}

gp::FontFamily* familyOf(Face face) {
    switch (face) {
        case Face::Semi: return g_families[1];
        case Face::Light: return g_families[2];
        case Face::Mono:
        case Face::MonoBold: return g_families[3];
        case Face::Display: return g_families[4];
        default: return g_families[0];
    }
}

int styleOf(Face face) {
    int style = (face == Face::Bold || face == Face::MonoBold) ? gp::FontStyleBold : gp::FontStyleRegular;
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

void text(gp::Graphics& g, std::wstring_view s, Face face, float size, gp::Color c, RectF r, int align = A_LEFT) {
    if (s.empty()) return;
    gp::SolidBrush brush(c);
    g.DrawString(s.data(), static_cast<INT>(s.size()), font(face, size), r, g_line[align], &brush);
}

// Text with a hard drop shadow (Retro theme).
void shadowed(gp::Graphics& g, std::wstring_view s, Face face, float size, gp::Color c, RectF r, int align = A_LEFT, float off = 2.f) {
    RectF sr(r.X + off, r.Y + off, r.Width, r.Height);
    text(g, s, face, size, gp::Color(c.GetA(), 0, 0, 0), sr, align);
    text(g, s, face, size, c, r, align);
}

void wrap(gp::Graphics& g, std::wstring_view s, Face face, float size, gp::Color c, RectF r, int align = A_CENTER, bool top = false) {
    if (s.empty()) return;
    gp::SolidBrush brush(c);
    gp::StringFormat* fmt = top ? g_wrapTop : g_wrap[align];
    g.DrawString(s.data(), static_cast<INT>(s.size()), font(face, size), r, fmt, &brush);
}

float measure(gp::Graphics& g, std::wstring_view s, Face face, float size) {
    if (s.empty()) return 0.f;
    RectF out;
    g.MeasureString(s.data(), static_cast<INT>(s.size()), font(face, size), gp::PointF(0.f, 0.f), g_top, &out);
    return out.Width;
}

// A row of differently styled pieces sharing one baseline.
struct Run {
    std::wstring_view s;
    Face face;
    float size;
    gp::Color color;
};

float runs(gp::Graphics& g, std::initializer_list<Run> parts, RectF r, int align = A_LEFT) {
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
    float x = align == A_LEFT ? r.X : align == A_RIGHT ? r.X + r.Width - total : r.X + (r.Width - total) / 2.f;
    x = std::max(x, r.X);
    float right = r.X + r.Width;
    for (auto const& p : parts) {
        if (p.s.empty()) continue;
        float room = right - x;
        if (room <= 4.f) break;
        float w = measure(g, p.s, p.face, p.size);
        float top = baseline - ascentOf(p.face, p.size);
        gp::SolidBrush brush(p.color);
        auto f = font(p.face, p.size);
        if (w > room + 0.5f) {
            // Doesn't fit: cut it with "..." and stop.
            RectF pr(x, top, room, lineHeightOf(p.face, p.size));
            g.DrawString(p.s.data(), static_cast<INT>(p.s.size()), f, pr, g_top, &brush);
            break;
        }
        g.DrawString(p.s.data(), static_cast<INT>(p.s.size()), f, gp::PointF(x, top), g_top, &brush);
        x += w;
    }
    return total;
}

// Letter-spaced caps (Minimal theme).
float spacedWidth(gp::Graphics& g, std::wstring_view s, Face face, float size, float spacing) {
    float w = 0.f;
    for (size_t i = 0; i < s.size(); ++i) w += measure(g, s.substr(i, 1), face, size);
    return w + spacing * static_cast<float>(s.size() > 0 ? s.size() - 1 : 0);
}

void spaced(gp::Graphics& g, std::wstring_view s, Face face, float size, gp::Color c, RectF r, float spacing, int align = A_LEFT) {
    float total = spacedWidth(g, s, face, size, spacing);
    float x = align == A_LEFT ? r.X : align == A_RIGHT ? r.X + r.Width - total : r.X + (r.Width - total) / 2.f;
    for (size_t i = 0; i < s.size(); ++i) {
        auto ch = s.substr(i, 1);
        float w = measure(g, ch, face, size);
        text(g, ch, face, size, c, RectF(x, r.Y, w + 4.f, r.Height));
        x += w + spacing;
    }
}

std::wstring upper(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towupper(ch));
    return s;
}

std::wstring lower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

// ===========================================================================
// Geometry (one set per theme; painting and hit testing both use it)
// ===========================================================================

struct Geo {
    float titleH = 40.f;
    RectF close, minimize, themeBtn, menu;
    float itemH = 28.f;
    bool sliders = true;
    RectF sliderRow[SLIDER_COUNT]; // hover / hit area
    RectF head[SLIDER_COUNT];      // name and value line
    float x0[SLIDER_COUNT] {}, x1[SLIDER_COUNT] {}, ty[SLIDER_COUNT] {};
    RectF toggle[TOGGLE_COUNT];
    bool targets = false; // scrollable numbers (HUD tiles, Big Numbers tiles)
    RectF target[TARGET_COUNT];
    bool steps = false;   // - / + buttons
    RectF step[TARGET_COUNT][2];
    bool heat = false;    // Analytics bars
    RectF bars;
    RectF tileA, tileB;   // HUD number tiles
    RectF status, sliderCard, toggleCard, heatCard, logCard, footer;
};

Geo g_geo;

// Two columns of themes.
constexpr int MENU_ROWS = (THEME_COUNT + 1) / 2;

RectF menuItem(int i) {
    auto const& m = g_geo.menu;
    float colW = (m.Width - 8.f) / 2.f;
    return RectF(m.X + 4.f + (i / MENU_ROWS) * colW, m.Y + 6.f + (i % MENU_ROWS) * g_geo.itemH, colW, g_geo.itemH);
}

float barWidth() { return (g_geo.bars.Width - (BUCKETS - 1) * 2.f) / BUCKETS; }

std::wstring themeLabel(Theme const& t) {
    switch (t.kind) {
        case Kind::Terminal: return lower(t.name);
        case Kind::Lines: return upper(t.name);
        default: return t.name;
    }
}

float themeButtonWidth(gp::Graphics& g, Theme const& t) {
    auto label = themeLabel(t);
    switch (t.kind) {
        case Kind::Hud: return 26.f;
        case Kind::Lines: return spacedWidth(g, label, Face::Semi, 10.f, 1.5f) + 24.f;
        case Kind::Terminal: return measure(g, label, Face::Mono, 12.f) + 30.f;
        case Kind::Retro: return measure(g, label, Face::Display, 14.f) + 36.f;
        default: return measure(g, label, Face::Ui, 12.f) + 32.f;
    }
}

void computeGeo(Theme const& t) {
    Geo G;
    float W = t.w;
    float H = t.h;

    float bw = 80.f;
    if (HDC dc = GetDC(nullptr)) {
        {
            gp::Graphics mg(dc);
            bw = themeButtonWidth(mg, t);
        }
        ReleaseDC(nullptr, dc);
    }
    auto buttons = [&](float right, float top, float size, float height, float gap) {
        G.close = box(right - size, top, size, height);
        G.minimize = box(G.close.X - gap - size, top, size, height);
        G.themeBtn = box(G.minimize.X - gap - bw, top, bw, height);
    };
    auto sliderX = [&](float a, float b) {
        for (int i = 0; i < SLIDER_COUNT; ++i) {
            G.x0[i] = a;
            G.x1[i] = b;
        }
    };

    switch (t.kind) {
        case Kind::Clean: {
            G.titleH = 40.f;
            buttons(W - 12.f, 8.f, 28.f, 24.f, 4.f);
            G.sliderCard = box(12, 48, 430, 154);
            G.sliderRow[0] = box(12, 54, 430, 80);
            G.head[0] = box(28, 60, 398, 20);
            G.ty[0] = 98;
            G.sliderRow[1] = box(12, 134, 430, 62);
            G.head[1] = box(28, 142, 398, 20);
            G.ty[1] = 178;
            sliderX(28, 426);
            G.toggleCard = box(12, 210, 430, 152);
            for (int i = 0; i < TOGGLE_COUNT; ++i) G.toggle[i] = box(12, 214.f + 36.f * i, 430, 36);
            G.footer = box(12, H - 12 - 40, 430, 40);
            G.logCard = box(12, 370, 430, G.footer.Y - 6 - 370);
            break;
        }
        case Kind::Terminal: {
            G.titleH = 34.f;
            buttons(W - 6.f, 5.f, 30.f, 24.f, 2.f);
            G.sliderRow[0] = box(0, 35, W, 52);
            G.head[0] = box(16, 47, W - 32, 18);
            G.ty[0] = 82;
            G.sliderRow[1] = box(0, 87, W, 62);
            G.head[1] = box(16, 99, W - 32, 18);
            G.ty[1] = 134;
            {
                float cw = (W - 32.f) / 27.f; // "[" + 25 values + "]"
                sliderX(16.f + 1.5f * cw, 16.f + 25.5f * cw);
            }
            {
                float cw = (W - 32.f - 12.f) / 2.f;
                for (int i = 0; i < TOGGLE_COUNT; ++i) {
                    G.toggle[i] = box(16.f + (i % 2) * (cw + 12.f), 167.f + (i / 2) * 32.f, cw, 28);
                }
            }
            G.footer = box(0, H - 48, W, 48);
            G.logCard = box(16, 249, W - 32, G.footer.Y - 8 - 249);
            break;
        }
        case Kind::Hud: {
            G.titleH = 46.f;
            G.close = box(W - 10 - 26, 12, 26, 24);
            G.minimize = box(G.close.X - 2 - 26, 12, 26, 24);
            G.themeBtn = box(G.minimize.X - 2 - 26, 12, 26, 24);
            G.toggle[0] = box(10, 11, 108, 26); // the "BUFFER ON" pill
            G.sliders = false;
            G.tileA = box(10, 46, 136, 76);
            G.tileB = box(154, 46, 136, 76);
            G.targets = true;
            G.target[T_MIN] = box(G.tileA.X, G.tileA.Y, G.tileA.Width / 2, G.tileA.Height);
            G.target[T_MAX] = box(G.tileA.X + G.tileA.Width / 2, G.tileA.Y, G.tileA.Width / 2, G.tileA.Height);
            G.target[T_RANDOM] = G.tileB;
            {
                float cw = (280.f - 12.f) / 3.f;
                for (int k = 1; k < TOGGLE_COUNT; ++k) G.toggle[k] = box(10.f + (k - 1) * (cw + 6.f), 130, cw, 30);
            }
            G.footer = box(10, H - 10 - 44, 280, 44);
            G.logCard = box(10, 168, 280, G.footer.Y - 6 - 168);
            break;
        }
        case Kind::Neon: {
            G.titleH = 42.f;
            buttons(W - 14.f, 9.f, 28.f, 24.f, 4.f);
            G.sliderCard = box(14, 42, 426, 150);
            G.sliderRow[0] = box(14, 42, 426, 72);
            G.head[0] = box(32, 56, 390, 18);
            G.ty[0] = 95;
            G.sliderRow[1] = box(14, 114, 426, 76);
            G.head[1] = box(32, 122, 390, 18);
            G.ty[1] = 161;
            sliderX(32, 422);
            for (int i = 0; i < TOGGLE_COUNT; ++i) G.toggle[i] = box(14.f + (i % 2) * 217.f, 202.f + (i / 2) * 52.f, 209, 44);
            G.footer = box(14, H - 12 - 36, 426, 36);
            G.logCard = box(14, 308, 426, G.footer.Y - 8 - 308);
            break;
        }
        case Kind::Retro: {
            G.titleH = 50.f;
            G.close = box(W - 14 - 32, 11, 32, 30);
            G.minimize = box(G.close.X - 6 - 32, 11, 32, 30);
            G.themeBtn = box(G.minimize.X - 6 - bw, 11, bw, 30);
            G.sliderCard = box(14, 52, 426, 152);
            G.sliderRow[0] = box(14, 52, 426, 72);
            G.head[0] = box(33, 62, 388, 26);
            G.ty[0] = 107;
            G.sliderRow[1] = box(14, 124, 426, 78);
            G.head[1] = box(33, 132, 388, 26);
            G.ty[1] = 177;
            sliderX(43, 411);
            for (int i = 0; i < TOGGLE_COUNT; ++i) G.toggle[i] = box(14.f + (i % 2) * 217.f, 214.f + (i / 2) * 52.f, 209, 44);
            G.footer = box(14, H - 12 - 34, 426, 34);
            G.logCard = box(14, 320, 426, G.footer.Y - 8 - 320);
            break;
        }
        case Kind::TwoPane: {
            G.titleH = 40.f;
            buttons(W - 12.f, 8.f, 28.f, 24.f, 4.f);
            G.sliderCard = box(12, 48, 300, 136);
            G.sliderRow[0] = box(12, 48, 300, 66);
            G.head[0] = box(28, 60, 268, 18);
            G.ty[0] = 97;
            G.sliderRow[1] = box(12, 114, 300, 70);
            G.head[1] = box(28, 122, 268, 18);
            G.ty[1] = 159;
            sliderX(28, 296);
            G.toggleCard = box(12, 192, 300, 152);
            for (int i = 0; i < TOGGLE_COUNT; ++i) G.toggle[i] = box(12, 196.f + 36.f * i, 300, 36);
            G.footer = box(16, 354, 292, H - 12 - 354);
            G.logCard = box(320, 48, W - 12 - 320, H - 12 - 48);
            break;
        }
        case Kind::Analytics: {
            G.titleH = 40.f;
            buttons(W - 12.f, 8.f, 28.f, 24.f, 4.f);
            G.sliderCard = box(12, 48, 430, 100);
            G.sliderRow[0] = box(20, 52, 205, 50);
            G.head[0] = box(28, 58, 189, 16);
            G.ty[0] = 90;
            G.x0[0] = 31;
            G.x1[0] = 214;
            G.sliderRow[1] = box(229, 52, 205, 50);
            G.head[1] = box(237, 58, 189, 16);
            G.ty[1] = 90;
            G.x0[1] = 240;
            G.x1[1] = 423;
            for (int k = 0; k < TOGGLE_COUNT; ++k) G.toggle[k] = box(28.f + k * 101.f, 110, 95, 28);
            G.heatCard = box(12, 156, 430, 158);
            G.heat = true;
            G.bars = box(28, 194, 398, 34);
            G.footer = box(12, H - 12 - 34, 430, 34);
            G.logCard = box(12, 322, 430, G.footer.Y - 8 - 322);
            break;
        }
        case Kind::Stepper: {
            G.titleH = 40.f;
            buttons(W - 12.f, 8.f, 28.f, 24.f, 4.f);
            G.sliders = false;
            G.targets = true;
            G.steps = true;
            float tw = (430.f - 16.f) / 3.f;
            for (int i = 0; i < TARGET_COUNT; ++i) {
                RectF tile = box(12.f + i * (tw + 8.f), 48, tw, 150);
                G.target[i] = tile;
                float bx = tile.X + (tw - 94.f) / 2.f;
                G.step[i][0] = box(bx, tile.Y + 104, 44, 30);
                G.step[i][1] = box(bx + 50, tile.Y + 104, 44, 30);
            }
            {
                float cw = (430.f - 18.f) / 4.f;
                for (int k = 0; k < TOGGLE_COUNT; ++k) G.toggle[k] = box(12.f + k * (cw + 6.f), 206, cw, 52);
            }
            G.footer = box(12, H - 12 - 30, 430, 30);
            G.logCard = box(12, 266, 430, G.footer.Y - 8 - 266);
            break;
        }
        case Kind::Slate: {
            G.titleH = 40.f;
            buttons(W - 12.f, 8.f, 28.f, 24.f, 4.f);
            G.status = box(12, 40, 430, 32);
            G.sliderCard = box(12, 82, 430, 140);
            G.sliderRow[0] = box(12, 82, 430, 66);
            G.head[0] = box(28, 94, 398, 18);
            G.ty[0] = 133;
            G.sliderRow[1] = box(12, 148, 430, 74);
            G.head[1] = box(28, 156, 398, 18);
            G.ty[1] = 195;
            sliderX(28, 426);
            G.toggleCard = box(12, 232, 430, 144);
            for (int i = 0; i < TOGGLE_COUNT; ++i) G.toggle[i] = box(12, 236.f + 34.f * i, 430, 34);
            G.footer = box(12, H - 12 - 30, 430, 30);
            G.logCard = box(12, 386, 430, G.footer.Y - 8 - 386);
            break;
        }
        case Kind::Lines: {
            G.titleH = 48.f;
            G.close = box(W - 22 - 28, 12, 28, 24);
            G.minimize = box(G.close.X - 2 - 28, 12, 28, 24);
            G.themeBtn = box(G.minimize.X - 6 - bw, 12, bw, 24);
            G.sliderRow[0] = box(22, 49, 410, 85);
            G.head[0] = box(22, 62, 410, 30);
            G.ty[0] = 111;
            G.sliderRow[1] = box(22, 135, 410, 85);
            G.head[1] = box(22, 148, 410, 30);
            G.ty[1] = 197;
            sliderX(29, 425);
            for (int i = 0; i < TOGGLE_COUNT; ++i) G.toggle[i] = box(22, 221.f + 38.f * i, 410, 38);
            G.footer = box(22, H - 14 - 30, 410, 30);
            G.logCard = box(22, 386, 410, G.footer.Y - 6 - 386);
            break;
        }
    }

    // Minimal only: no theme button (off-screen and zero-sized, so it can't be
    // drawn or clicked).
    G.themeBtn = RectF(-1000.f, -1000.f, 0.f, 0.f);

    // The theme list drops down under its button, kept inside the window.
    G.itemH = t.kind == Kind::Retro ? 30.f : 28.f;
    float mw = std::min(W - 16.f, 348.f);
    float mx = std::clamp(G.themeBtn.X + G.themeBtn.Width - mw, 8.f, W - 8.f - mw);
    G.menu = box(mx, G.themeBtn.Y + G.themeBtn.Height + 4.f, mw, 12.f + G.itemH * MENU_ROWS);

    g_geo = G;
}

float thumbX(int i, int v) {
    return g_geo.x0[i] + (g_geo.x1[i] - g_geo.x0[i]) * (static_cast<float>(v) / MAX_TICKS);
}

int valueAt(int i, float x) {
    float span = g_geo.x1[i] - g_geo.x0[i];
    if (span <= 0.f) return 0;
    float t = (x - g_geo.x0[i]) / span;
    return std::clamp(static_cast<int>(std::lround(t * MAX_TICKS)), 0, MAX_TICKS);
}

// ===========================================================================
// Hit testing
// ===========================================================================

constexpr int HIT_NONE = -1;
constexpr int HIT_CLOSE = 0;
constexpr int HIT_MINIMIZE = 1;
constexpr int HIT_THEME = 2;
constexpr int HIT_TOGGLE = 10;     // + toggle
constexpr int HIT_SLIDER = 20;     // + slider
constexpr int HIT_TARGET = 30;     // + target
constexpr int HIT_STEP = 40;       // + target * 2 + (0 = minus, 1 = plus)
constexpr int HIT_MENU_PANEL = 59;
constexpr int HIT_MENU = 60;       // + theme
constexpr int HIT_BUCKET = 100;    // + bucket

bool within(int hit, int base, int count) { return hit >= base && hit < base + count; }

int hitTest(float x, float y) {
    auto const& G = g_geo;
    if (g_menuOpen) {
        if (G.menu.Contains(x, y)) {
            for (int i = 0; i < THEME_COUNT; ++i) {
                if (menuItem(i).Contains(x, y)) return HIT_MENU + i;
            }
            return HIT_MENU_PANEL;
        }
        if (G.themeBtn.Contains(x, y)) return HIT_THEME;
        return HIT_NONE;
    }
    if (G.close.Contains(x, y)) return HIT_CLOSE;
    if (G.minimize.Contains(x, y)) return HIT_MINIMIZE;
    if (G.themeBtn.Contains(x, y)) return HIT_THEME;
    for (int i = 0; i < TOGGLE_COUNT; ++i) {
        if (G.toggle[i].Contains(x, y)) return HIT_TOGGLE + i;
    }
    if (G.sliders) {
        for (int i = 0; i < SLIDER_COUNT; ++i) {
            if (G.sliderRow[i].Contains(x, y)) return HIT_SLIDER + i;
        }
    }
    if (G.steps) {
        for (int i = 0; i < TARGET_COUNT; ++i) {
            for (int d = 0; d < 2; ++d) {
                if (G.step[i][d].Contains(x, y)) return HIT_STEP + i * 2 + d;
            }
        }
    }
    if (G.targets) {
        for (int i = 0; i < TARGET_COUNT; ++i) {
            if (G.target[i].Width > 0.f && G.target[i].Contains(x, y)) return HIT_TARGET + i;
        }
    }
    if (G.heat) {
        RectF area(G.bars.X, G.bars.Y - 4.f, G.bars.Width, G.bars.Height + 20.f);
        if (area.Contains(x, y)) {
            int b = static_cast<int>((x - G.bars.X) / (barWidth() + 2.f));
            return HIT_BUCKET + std::clamp(b, 0, BUCKETS - 1);
        }
    }
    return HIT_NONE;
}

bool hovering(int hit) { return g_hover == hit; }

// The hover / drag / held state that decides the description text.
int activeHit() {
    if (g_dragging >= 0) return HIT_SLIDER + g_dragging;
    if (g_holdStep >= 0) return g_holdStep;
    if (g_menuOpen && !within(g_hover, HIT_MENU, THEME_COUNT) && g_menuKey >= 0) return HIT_MENU + g_menuKey;
    return g_hover;
}

std::wstring bucketText(int b) {
    wchar_t buf[96];
    int count = g_heat[b];
    if (count == 0) {
        std::swprintf(buf, 96, L"%d\u2013%d%%: no saves yet", b * 5, b * 5 + 5);
        return buf;
    }
    int best = 0;
    for (int m = 1; m < MODE_COUNT; ++m) {
        if (g_heatModes[b][m] > g_heatModes[b][best]) best = m;
    }
    std::swprintf(buf, 96, L"%d\u2013%d%%: %d save%ls, mostly %ls", b * 5, b * 5 + 5, count, plural(count), MODE_NAMES[best]);
    return buf;
}

std::wstring description() {
    int h = activeHit();
    if (within(h, HIT_MENU, THEME_COUNT)) {
        auto const& t = THEMES[h - HIT_MENU];
        return std::wstring(t.name) + L": " + t.desc;
    }
    if (h == HIT_THEME) return THEME_BUTTON_DESC;
    if (within(h, HIT_SLIDER, SLIDER_COUNT)) return SLIDER_DESCS[h - HIT_SLIDER];
    if (within(h, HIT_TOGGLE, TOGGLE_COUNT)) return TOGGLE_DESCS[h - HIT_TOGGLE];
    if (within(h, HIT_TARGET, TARGET_COUNT)) return TARGET_DESCS[h - HIT_TARGET];
    if (within(h, HIT_STEP, TARGET_COUNT * 2)) return TARGET_DESCS[(h - HIT_STEP) / 2];
    if (within(h, HIT_BUCKET, BUCKETS)) return bucketText(h - HIT_BUCKET);
    return {};
}

// ===========================================================================
// Shared pieces
// ===========================================================================

void card(gp::Graphics& g, Theme const& t, RectF r, bool hover = false) {
    fill(g, r, col(hover ? t.c.hover : t.c.panel), t.radius);
    stroke(g, r, col(t.c.border), t.radius);
}

// Title bar buttons: theme dropdown, minimize, close.
void drawButtons(gp::Graphics& g, Theme const& t) {
    auto const& G = g_geo;
    auto const& c = t.c;
    bool hc = hovering(HIT_CLOSE);
    bool hm = hovering(HIT_MINIMIZE);
    bool ht = hovering(HIT_THEME) || g_menuOpen;
    auto label = themeLabel(t);
    float cy = G.themeBtn.Y + G.themeBtn.Height / 2.f;

    switch (t.kind) {
        case Kind::Retro: {
            auto chunky = [&](RectF r, uint32_t color, bool hover) {
                fill(g, r, col(hover ? mix(color, 0xffffff, 0.18f) : color), 8.f);
                stroke(g, inset(r, 1.f), col(0x000000), 7.f, 2.f);
            };
            chunky(G.themeBtn, 0x2f6fdc, ht);
            chunky(G.minimize, 0x3fbd3f, hm);
            chunky(G.close, 0xe04040, hc);
            shadowed(g, L"\u2013", Face::Display, 18.f, col(0xffffff), G.minimize, A_CENTER, 1.5f);
            shadowed(g, L"\u00d7", Face::Display, 18.f, col(0xffffff), G.close, A_CENTER, 1.5f);
            shadowed(g, label, Face::Display, 14.f, col(0xffffff), box(G.themeBtn.X + 10, G.themeBtn.Y, G.themeBtn.Width - 30, G.themeBtn.Height), A_LEFT, 1.5f);
            float vx = G.themeBtn.X + G.themeBtn.Width - 15.f;
            chevron(g, vx + 1.f, cy + 1.f, col(0x000000), 4.f, 2.5f);
            chevron(g, vx, cy, col(0xffffff), 4.f, 2.5f);
            return;
        }
        case Kind::Terminal: {
            if (hm) fill(g, G.minimize, col(c.hover));
            if (hc) fill(g, G.close, col(0xc42b1c));
            if (ht) fill(g, G.themeBtn, col(c.hover));
            text(g, L"_", Face::Mono, 13.f, col(hm ? c.text : c.muted), G.minimize, A_CENTER);
            text(g, L"x", Face::Mono, 13.f, col(hc ? 0xffffff : c.muted), G.close, A_CENTER);
            text(g, label, Face::Mono, 12.f, col(ht ? c.text : c.faint), box(G.themeBtn.X + 8, G.themeBtn.Y, G.themeBtn.Width - 24, G.themeBtn.Height));
            chevron(g, G.themeBtn.X + G.themeBtn.Width - 11.f, cy, col(ht ? c.accent : c.faint));
            return;
        }
        case Kind::Lines: {
            minimizeGlyph(g, G.minimize, col(hm ? c.text : c.faint));
            closeGlyph(g, G.close, col(hc ? c.danger : c.faint));
            spaced(g, label, Face::Semi, 10.f, col(ht ? c.text : c.muted), box(G.themeBtn.X + 4, G.themeBtn.Y, G.themeBtn.Width, G.themeBtn.Height), 1.5f);
            chevron(g, G.themeBtn.X + G.themeBtn.Width - 9.f, cy, col(ht ? c.text : c.muted), 3.f, 1.2f);
            return;
        }
        default: {
            float rad = (t.kind == Kind::Neon || t.kind == Kind::Slate) ? 6.f : 4.f;
            if (hm) fill(g, G.minimize, col(c.hover), rad);
            if (hc) fill(g, G.close, col(0xc42b1c), rad);
            if (ht) fill(g, G.themeBtn, col(c.hover), rad);
            minimizeGlyph(g, G.minimize, col(hm ? c.text : c.muted));
            closeGlyph(g, G.close, col(hc ? 0xffffff : c.muted));
            if (t.kind == Kind::Hud) {
                // Icon only: a 2x2 grid of swatches.
                float cx = G.themeBtn.X + G.themeBtn.Width / 2.f;
                gp::Color ic = col(ht ? c.text : c.muted);
                for (int k = 0; k < 4; ++k) {
                    float sx = cx - 5.f + (k % 2) * 6.f;
                    float sy = cy - 5.f + (k / 2) * 6.f;
                    fill(g, box(sx, sy, 4.f, 4.f), k == 0 ? col(c.accent) : ic, 1.f);
                }
                return;
            }
            text(g, label, Face::Ui, 12.f, col(ht ? c.text : c.muted), box(G.themeBtn.X + 10, G.themeBtn.Y, G.themeBtn.Width - 26, G.themeBtn.Height));
            chevron(g, G.themeBtn.X + G.themeBtn.Width - 12.f, cy, col(ht ? c.text : c.muted));
            return;
        }
    }
}

// A slider track with one handle (randomize) or two (the buffer range).
void drawSlider(gp::Graphics& g, Theme const& t, int i) {
    auto const& G = g_geo;
    auto const& c = t.c;
    float x0 = G.x0[i];
    float x1 = G.x1[i];
    float y = G.ty[i];
    bool range = i == 0;
    int hi = g_values.sliders[i];
    bool on = hi > 0;
    int lo = range ? std::min(lowTicks(), hi) : 0;
    bool twoHandles = range && on && lo < hi;
    float lx = range && on ? thumbX(i, lo) : x0;
    float hx = thumbX(i, hi);
    bool active = hovering(HIT_SLIDER + i) || g_dragging == i;

    switch (t.kind) {
        case Kind::Terminal: {
            // [====|====|=========] drawn one character per value.
            float cw = (x1 - x0) / MAX_TICKS;
            float left = x0 - 1.5f * cw;
            for (int k = 0; k <= MAX_TICKS + 2; ++k) {
                wchar_t const* ch = L"=";
                uint32_t color = c.track;
                if (k == 0 || k == MAX_TICKS + 2) {
                    ch = k == 0 ? L"[" : L"]";
                    color = active ? c.muted : c.track;
                }
                else {
                    int v = k - 1;
                    if (range) {
                        if (!on) {
                            if (v == 0) { ch = L"|"; color = c.muted; }
                        }
                        else if (v == lo || v == hi) { ch = L"|"; color = c.accent; }
                        else if (v > lo && v < hi) color = c.accent;
                    }
                    else {
                        if (v == hi) { ch = L"|"; color = on ? c.accent : c.muted; }
                        else if (v < hi) color = c.accent;
                    }
                }
                text(g, ch, Face::Mono, 13.f, col(color), box(left + k * cw, y - 9.f, cw, 18.f), A_CENTER);
            }
            return;
        }
        case Kind::Neon: {
            fill(g, box(x0, y - 2, x1 - x0, 4), col(c.track), 2);
            if (hx > lx) fill(g, box(lx, y - 2, hx - lx, 4), col(c.accent), 2);
            auto thumb = [&](float x) {
                circle(g, x, y, 13.f, col(c.accent, active ? 90 : 60));
                circle(g, x, y, 9.f, col(c.handle));
            };
            if (twoHandles) thumb(lx);
            thumb(hx);
            return;
        }
        case Kind::Retro: {
            RectF track = box(x0 - 10, y - 5, x1 - x0 + 20, 10);
            fill(g, track, col(c.track), 5);
            if (hx > lx) fill(g, box(lx, y - 3, hx - lx, 7), col(c.fill), 3);
            stroke(g, track, col(0x000000), 5, 2);
            auto thumb = [&](float x) {
                RectF r = box(x - 10, y - 11, 20, 22);
                fill(g, r, col(active ? 0xffffff : 0xf2f2f2), 6);
                stroke(g, inset(r, 1), col(0x000000), 5, 2);
            };
            if (twoHandles) thumb(lx);
            thumb(hx);
            return;
        }
        case Kind::Slate: {
            fill(g, box(x0, y - 2, x1 - x0, 4), col(c.track), 2);
            if (hx > lx) fill(g, box(lx, y - 2, hx - lx, 4), col(c.accent), 2);
            auto thumb = [&](float x, int v) {
                circle(g, x, y, 11.f, col(c.bg));
                ring(g, x, y, 10.f, col(active ? mix(c.accent, 0xffffff, 0.25f) : c.accent), 2.f);
                text(g, std::to_wstring(v), Face::Bold, 10.f, col(c.text), box(x - 11, y - 11, 22, 22), A_CENTER);
            };
            if (twoHandles) thumb(lx, lo);
            thumb(hx, hi);
            return;
        }
        case Kind::Lines: {
            fill(g, box(x0, y - 1, x1 - x0, 2), col(c.track));
            if (hx > lx) fill(g, box(lx, y - 1, hx - lx, 2), col(c.fill));
            uint32_t knob = active ? 0xffffff : c.handle;
            if (twoHandles) circle(g, lx, y, 7.f, col(knob));
            circle(g, hx, y, 7.f, col(knob));
            return;
        }
        default: {
            float tw = t.kind == Kind::Analytics ? 7.f : 8.f;
            float th = t.kind == Kind::Analytics ? 16.f : 18.f;
            fill(g, box(x0, y - 2, x1 - x0, 4), col(c.track), 2);
            if (hx > lx) fill(g, box(lx, y - 2, hx - lx, 4), col(c.fill), 2);
            if (twoHandles) fill(g, box(lx - tw / 2, y - th / 2, tw, th), col(c.handle), 4);
            fill(g, box(hx - tw / 2, y - th / 2, tw, th), col(c.handle), 4);
            return;
        }
    }
}

void sliderHover(gp::Graphics& g, Theme const& t, int i) {
    if (!hovering(HIT_SLIDER + i) && g_dragging != i) return;
    RectF r = g_geo.sliderRow[i];
    fill(g, box(r.X + 4, r.Y + 1, r.Width - 8, r.Height - 2), col(t.c.hover), std::min(t.radius, 4.f));
}

void drawSwitch(gp::Graphics& g, Theme const& t, float x, float y, bool on, float w = 38.f) {
    fill(g, box(x, y, w, 20), col(on ? t.c.accent : t.c.track), 10);
    float kx = on ? x + w - 10.f : x + 10.f;
    uint32_t knob = on ? (t.kind == Kind::Slate ? t.c.bg : 0xffffff) : t.c.muted;
    circle(g, kx, y + 10.f, 7.f, col(knob));
}

void toggleRows(gp::Graphics& g, Theme const& t, bool hint) {
    auto const& c = t.c;
    for (int i = 0; i < TOGGLE_COUNT; ++i) {
        RectF r = g_geo.toggle[i];
        bool on = g_values.toggles[i];
        if (hovering(HIT_TOGGLE + i)) fill(g, box(r.X + 4, r.Y + 2, r.Width - 8, r.Height - 4), col(c.hover), std::min(t.radius, 4.f));
        if (i > 0) line(g, r.X + 14, r.Y, r.X + r.Width - 14, r.Y, col(c.bg));
        RectF label = box(r.X + 16, r.Y, r.Width - 80, r.Height);
        uint32_t tc = t.kind == Kind::Slate && !on ? mix(c.text, c.muted, 0.45f) : c.text;
        if (hint && i == 1) {
            runs(g, { { L"Releases", Face::Ui, 13.f, col(tc) }, { L"  \u00b7  wave, ship, robot", Face::Ui, 13.f, col(c.muted) } }, label);
        }
        else {
            text(g, TOGGLE_NAMES[i], Face::Ui, 13.f, col(tc), label);
        }
        float sw = t.kind == Kind::Slate ? 36.f : 38.f;
        drawSwitch(g, t, r.X + r.Width - 16 - sw, r.Y + (r.Height - 20) / 2, on, sw);
    }
}

void footerText(gp::Graphics& g, Theme const& t, RectF r, std::wstring_view idle, float size = 12.f, Face face = Face::Ui) {
    auto desc = description();
    if (!desc.empty()) wrap(g, desc, face, size + 1.f, col(t.c.text), r);
    else wrap(g, idle, face, size, col(t.c.muted), r);
}

// ===========================================================================
// Log
// ===========================================================================

enum class LogLook { Clean, Neon, Slate, Retro, Lines, Terminal, Stepper };

void logHeader(gp::Graphics& g, Theme const& t, LogLook look, Attempt const& a, int age, RectF r) {
    auto const& c = t.c;
    int alpha = ageAlpha(age);
    std::wstring n = std::to_wstring(a.number);
    switch (look) {
        case LogLook::Neon:
            text(g, L"ATTEMPT " + n, Face::Bold, 11.f, col(c.accent, age == 0 ? 255 : alpha - 60), r);
            return;
        case LogLook::Slate: {
            std::wstring s = L"ATTEMPT " + n;
            uint32_t tc = age == 0 ? c.accent : c.muted;
            text(g, s, Face::Bold, 11.f, col(tc, alpha), r);
            float w = measure(g, s, Face::Bold, 11.f);
            float my = r.Y + r.Height / 2.f;
            line(g, r.X + w + 8.f, my, r.X + r.Width, my, col(c.divider));
            return;
        }
        case LogLook::Retro:
            shadowed(g, L"Attempt " + n, Face::Display, 15.f, col(age == 0 ? 0xffd84a : 0xe6c04a, alpha), r, A_LEFT, 1.5f);
            return;
        case LogLook::Lines:
            spaced(g, L"ATTEMPT " + n, Face::Ui, 11.f, col(0x6a6a6a), r, 1.5f);
            return;
        case LogLook::Terminal: {
            std::wstring s = L"-- attempt " + n + L" ";
            float dash = std::max(1.f, measure(g, L"-", Face::Mono, 12.f));
            int count = static_cast<int>((r.Width - measure(g, s, Face::Mono, 12.f)) / dash);
            if (count > 0) s.append(static_cast<size_t>(count), L'-');
            text(g, s, Face::Mono, 12.f, col(c.faint), r);
            return;
        }
        default: {
            // "---- Attempt 12 ----" with lines on both sides
            std::wstring s = L"Attempt " + n;
            float w = measure(g, s, Face::Bold, 11.f);
            float mid = r.X + r.Width / 2.f;
            float my = r.Y + r.Height / 2.f;
            line(g, r.X, my, mid - w / 2.f - 8.f, my, col(c.divider));
            line(g, mid + w / 2.f + 8.f, my, r.X + r.Width, my, col(c.divider));
            text(g, s, Face::Bold, 11.f, col(c.faint, alpha), r, A_CENTER);
            return;
        }
    }
}

void logItem(gp::Graphics& g, Theme const& t, LogLook look, Item const& it, int age, RectF r) {
    auto const& c = t.c;
    int alpha = ageAlpha(age);
    bool note = !it.note.empty();
    auto pct = percentText(it.percent);

    if (look == LogLook::Terminal) {
        std::wstring time = it.time + L" ";
        std::wstring p = pct.empty() ? std::wstring() : pct + L" ";
        std::wstring body = itemText(it, ItemFmt::Terminal);
        runs(g, {
            { time, Face::Mono, 12.f, col(c.faint, alpha) },
            { p, Face::Mono, 12.f, col(c.danger, alpha) },
            { body, Face::Mono, 12.f, col(note ? c.muted : c.text, alpha) },
            { it.random ? L" rnd" : L"", Face::Mono, 12.f, col(c.rnd, alpha) },
        }, r);
        return;
    }

    // Every theme: time | level % | what happened.
    float x = r.X;
    text(g, it.time, Face::Mono, 12.f, col(c.faint, alpha), box(x, r.Y, 62, r.Height));
    x += 62.f;
    if (!pct.empty()) {
        text(g, pct, Face::MonoBold, 12.f, col(c.danger, alpha), box(x, r.Y, 42, r.Height));
        x += 42.f;
    }

    RectF rest = box(x, r.Y, r.X + r.Width - x, r.Height);
    if (note) {
        text(g, it.note, Face::Mono, 12.f, col(c.muted, alpha), rest);
        return;
    }
    ItemFmt fmt = ItemFmt::Full;
    wchar_t const* marker = L" (random)";
    switch (look) {
        case LogLook::Neon: fmt = ItemFmt::Arrows; marker = L"  rnd"; break;
        case LogLook::Slate: marker = L"  rnd"; break;
        case LogLook::Stepper: marker = L"  rnd"; break;
        case LogLook::Retro: fmt = ItemFmt::Plain; break;
        case LogLook::Lines: fmt = ItemFmt::Columns; marker = L"  rnd"; break;
        default: break;
    }
    runs(g, {
        { itemText(it, fmt), Face::Mono, 12.f, col(c.text, alpha) },
        { it.random ? marker : L"", Face::Mono, 12.f, col(c.rnd, alpha) },
    }, rest);
}

// Newest attempt first, its newest line first.
void drawLog(gp::Graphics& g, Theme const& t, RectF area, LogLook look, bool skipFirstHeader = false) {
    constexpr float LH = 18.f;
    float y = area.Y;
    float bottom = area.Y + area.Height;
    bool any = false;
    for (size_t ai = 0; ai < g_attempts.size(); ++ai) {
        auto const& a = g_attempts[ai];
        int age = static_cast<int>(ai);
        if (a.number > 0 && !(skipFirstHeader && ai == 0)) {
            float gapAbove = 0.f;
            if (ai > 0 && (look == LogLook::Neon || look == LogLook::Slate || look == LogLook::Terminal)) gapAbove = 5.f;
            if (ai > 0 && (look == LogLook::Retro || look == LogLook::Lines)) gapAbove = 8.f;
            float hh = look == LogLook::Retro ? 22.f : LH;
            // Don't leave a header at the very bottom with nothing under it.
            if (y + gapAbove + hh + (a.items.empty() ? 0.f : LH) > bottom) return;
            logHeader(g, t, look, a, age, box(area.X, y + gapAbove, area.Width, hh));
            y += gapAbove + hh;
        }
        for (auto it = a.items.rbegin(); it != a.items.rend(); ++it) {
            if (y + LH > bottom) return;
            logItem(g, t, look, *it, age, box(area.X, y, area.Width, LH));
            y += LH;
            any = true;
        }
    }
    if (y + LH > bottom) return;
    if (!any) {
        bool term = look == LogLook::Terminal;
        text(g, term ? L"-- nothing buffered yet --" : L"Nothing yet", term ? Face::Mono : Face::Ui, 12.f,
             col(term ? t.c.faint : t.c.muted), box(area.X, y, area.Width, LH));
        y += LH;
    }
    if (look == LogLook::Terminal && y + LH + 4.f <= bottom) {
        // Prompt with a block cursor.
        y += 4.f;
        text(g, L">", Face::Mono, 12.f, col(t.c.accent), box(area.X, y, 12, LH));
        fill(g, box(area.X + 12, y + 2, 8, 14), col(t.c.accent));
    }
}

// ===========================================================================
// Themes
// ===========================================================================

void paintClean(gp::Graphics& g, Theme const& t) {
    auto const& G = g_geo;
    auto const& c = t.c;
    circle(g, 20.f, G.titleH / 2.f, 4.f, col(g_values.toggles[0] ? c.accent : c.track));
    text(g, L"Input Buffer", Face::Bold, 15.f, col(c.text), box(30, 0, 200, G.titleH));
    drawButtons(g, t);

    card(g, t, G.sliderCard);
    for (int i = 0; i < SLIDER_COUNT; ++i) sliderHover(g, t, i);
    line(g, G.sliderCard.X + 14, G.sliderRow[1].Y, G.sliderCard.X + G.sliderCard.Width - 14, G.sliderRow[1].Y, col(c.bg));
    for (int i = 0; i < SLIDER_COUNT; ++i) {
        bool on = g_values.sliders[i] > 0;
        gp::Color tc = col(on ? c.text : c.muted);
        text(g, i == 0 ? L"Buffer" : L"Randomize", Face::Semi, 13.f, tc, G.head[i]);
        text(g, i == 0 ? rangeText(Fmt::Full) : randomText(Fmt::Full), Face::Ui, 12.f, tc, G.head[i], A_RIGHT);
        drawSlider(g, t, i);
    }
    // Tick scale under the buffer slider.
    for (int v = 0; v <= MAX_TICKS; v += 6) {
        float x = thumbX(0, v);
        int align = v == 0 ? A_LEFT : v == MAX_TICKS ? A_RIGHT : A_CENTER;
        float bx = align == A_LEFT ? x - 2.f : align == A_RIGHT ? x - 28.f : x - 15.f;
        text(g, std::to_wstring(v), Face::Ui, 10.f, col(c.faint), box(bx, G.ty[0] + 13, 30, 14), align);
    }

    card(g, t, G.toggleCard);
    toggleRows(g, t, true);

    card(g, t, G.logCard);
    text(g, L"Log", Face::Semi, 13.f, col(c.text), box(G.logCard.X + 16, G.logCard.Y + 6, 120, 22));
    drawLog(g, t, box(G.logCard.X + 16, G.logCard.Y + 32, G.logCard.Width - 32, G.logCard.Height - 40), LogLook::Clean);

    footerText(g, t, G.footer, L"Hover a setting to see what it does  \u00b7  F7 to show / hide  \u00b7  scroll a slider to adjust");
}

void paintTerminal(gp::Graphics& g, Theme const& t) {
    auto const& G = g_geo;
    auto const& c = t.c;
    float W = t.w;
    runs(g, { { L"> input_buffer", Face::Mono, 13.f, col(c.accent) }, { L".exe", Face::Mono, 13.f, col(c.faint) } }, box(12, 0, 240, G.titleH));
    line(g, 0, G.titleH + 0.5f, W, G.titleH + 0.5f, col(c.border));
    drawButtons(g, t);

    for (int i = 0; i < SLIDER_COUNT; ++i) {
        if (hovering(HIT_SLIDER + i) || g_dragging == i) fill(g, G.sliderRow[i], col(c.hover));
        text(g, i == 0 ? L"buffer" : L"randomize", Face::Mono, 13.f, col(c.muted), G.head[i]);
        bool on = g_values.sliders[i] > 0;
        text(g, i == 0 ? rangeText(Fmt::Ascii) : randomText(Fmt::Ascii), Face::Mono, 13.f, col(on ? c.accent : c.muted), G.head[i], A_RIGHT);
        drawSlider(g, t, i);
    }
    line(g, 0, 157.5f, W, 157.5f, col(c.border));

    for (int i = 0; i < TOGGLE_COUNT; ++i) {
        RectF r = G.toggle[i];
        bool on = g_values.toggles[i];
        if (hovering(HIT_TOGGLE + i)) fill(g, box(r.X - 6, r.Y, r.Width + 6, r.Height), col(c.hover));
        std::wstring name = std::wstring(L" ") + TOGGLE_LOWER[i];
        runs(g, { { on ? L"[x]" : L"[ ]", Face::Mono, 13.f, col(on ? c.accent : c.muted) }, { name, Face::Mono, 13.f, col(on ? c.text : c.muted) } }, r);
    }
    line(g, 0, 239.5f, W, 239.5f, col(c.border));

    drawLog(g, t, G.logCard, LogLook::Terminal);

    line(g, 0, G.footer.Y + 0.5f, W, G.footer.Y + 0.5f, col(c.border));
    RectF inner = box(16, G.footer.Y + 2, W - 32, G.footer.Height - 4);
    auto desc = description();
    if (!desc.empty()) {
        wrap(g, desc, Face::Mono, 11.f, col(c.text), inner, A_LEFT);
    }
    else {
        text(g, L"F6 toggle \u00b7 F7 hide", Face::Mono, 11.f, col(c.faint), inner);
        text(g, L"hidden from capture", Face::Mono, 11.f, col(c.faint), inner, A_RIGHT);
    }
}

void paintHud(gp::Graphics& g, Theme const& t) {
    auto& G = g_geo;
    auto const& c = t.c;

    // "BUFFER ON" pill = the Enabled toggle.
    bool enabled = g_values.toggles[0];
    RectF p = G.toggle[0];
    bool hp = hovering(HIT_TOGGLE);
    uint32_t pillColor = enabled ? (hp ? mix(c.accent, 0xffffff, 0.15f) : c.accent) : (hp ? c.hover : c.track);
    fill(g, p, col(pillColor), 13);
    circle(g, p.X + 14, p.Y + p.Height / 2.f, 3.f, col(enabled ? c.onAccent : c.muted));
    text(g, enabled ? L"BUFFER ON" : L"BUFFER OFF", Face::Bold, 12.f, col(enabled ? c.onAccent : c.muted), box(p.X + 23, p.Y, p.Width - 26, p.Height));
    drawButtons(g, t);

    // Buffer tile: "1-4", scroll the left number for min, the right for max.
    RectF a = G.tileA;
    bool overBuffer = hovering(HIT_TARGET + T_MIN) || hovering(HIT_TARGET + T_MAX);
    card(g, t, a, overBuffer);
    text(g, L"Buffer", Face::Ui, 11.f, col(c.muted), box(a.X + 10, a.Y + 8, a.Width - 20, 16));
    int hi = g_values.sliders[0];
    int lo = lowTicks();
    RectF num = box(a.X + 10, a.Y + 23, a.Width - 20, 32);
    if (hi <= 0 || lo >= hi) {
        std::wstring s = hi <= 0 ? L"off" : std::to_wstring(hi);
        text(g, s, Face::Bold, 24.f, col(hi <= 0 ? c.muted : (hovering(HIT_TARGET + T_MAX) ? c.accent : c.text)), num);
        G.target[T_MIN] = RectF();
        G.target[T_MAX] = a;
    }
    else {
        std::wstring ls = std::to_wstring(lo);
        std::wstring hs = std::to_wstring(hi);
        float lw = measure(g, ls, Face::Bold, 24.f);
        float dw = measure(g, L"\u2013", Face::Bold, 24.f);
        runs(g, {
            { ls, Face::Bold, 24.f, col(hovering(HIT_TARGET + T_MIN) ? c.accent : c.text) },
            { L"\u2013", Face::Bold, 24.f, col(c.muted) },
            { hs, Face::Bold, 24.f, col(hovering(HIT_TARGET + T_MAX) ? c.accent : c.text) },
        }, num);
        float split = num.X + lw + dw / 2.f;
        G.target[T_MIN] = box(a.X, a.Y, split - a.X, a.Height);
        G.target[T_MAX] = box(split, a.Y, a.X + a.Width - split, a.Height);
    }
    std::wstring sub = L"buffer off";
    if (hi > 0) {
        wchar_t b[48];
        std::swprintf(b, 48, L"ticks \u00b7 \u2264%.1f ms", ticksToMs(hi));
        sub = b;
    }
    text(g, sub, Face::Ui, 11.f, col(c.muted), box(a.X + 10, a.Y + 54, a.Width - 20, 16));

    RectF b = G.tileB;
    bool overRandom = hovering(HIT_TARGET + T_RANDOM);
    card(g, t, b, overRandom);
    int rv = g_values.sliders[1];
    text(g, L"Randomize", Face::Ui, 11.f, col(c.muted), box(b.X + 10, b.Y + 8, b.Width - 20, 16));
    text(g, rv > 0 ? L"+" + std::to_wstring(rv) : std::wstring(L"off"), Face::Bold, 24.f,
         col(rv <= 0 ? c.muted : (overRandom ? c.accent : c.text)), box(b.X + 10, b.Y + 23, b.Width - 20, 32));
    text(g, rv > 0 ? L"ticks max" : L"smallest safe", Face::Ui, 11.f, col(c.muted), box(b.X + 10, b.Y + 54, b.Width - 20, 16));

    for (int k = 1; k < TOGGLE_COUNT; ++k) {
        RectF r = G.toggle[k];
        bool on = g_values.toggles[k];
        bool h = hovering(HIT_TOGGLE + k);
        uint32_t bg = on ? mix(c.panel, c.accent, h ? 0.22f : 0.15f) : (h ? c.hover : c.panel);
        fill(g, r, col(bg), 6);
        stroke(g, r, col(c.border), 6);
        text(g, TOGGLE_SHORT[k], on ? Face::Semi : Face::Ui, 12.f, col(on ? c.accent : c.muted), r, A_CENTER);
    }

    // Log: one block per attempt, "62%  +3 wave (up arrow)  rnd".
    card(g, t, G.logCard);
    RectF area = box(G.logCard.X + 10, G.logCard.Y + 6, G.logCard.Width - 20, G.logCard.Height - 12);
    float y = area.Y;
    float bottom = area.Y + area.Height;
    bool any = false;
    for (size_t ai = 0; ai < g_attempts.size() && y < bottom; ++ai) {
        auto const& at = g_attempts[ai];
        int age = static_cast<int>(ai);
        int alpha = ageAlpha(age);
        if (at.number > 0) {
            float hh = ai == 0 ? 22.f : 28.f;
            if (y + hh + (at.items.empty() ? 0.f : 17.f) > bottom) break;
            RectF hr = box(area.X, y + hh - 22.f, area.Width, 22.f);
            text(g, L"Attempt " + std::to_wstring(at.number), Face::Semi, 12.f, col(ai == 0 ? c.text : c.muted, alpha), hr);
            text(g, std::to_wstring(at.saves) + (at.saves == 1 ? L" save" : L" saves"), Face::Ui, 12.f, col(c.muted, alpha), hr, A_RIGHT);
            y += hh;
        }
        bool full = false;
        for (auto it = at.items.rbegin(); it != at.items.rend(); ++it) {
            if (y + 17.f > bottom) { full = true; break; }
            RectF r = box(area.X, y, area.Width, 17.f);
            // time | % | "+3 wave (arrow)" | rnd
            text(g, it->time, Face::Mono, 11.f, col(c.faint, alpha), r);
            RectF rest = box(r.X + 62.f, r.Y, r.Width - 62.f, r.Height);
            if (!it->note.empty()) {
                text(g, it->note, Face::Mono, 12.f, col(c.muted, alpha), rest);
            }
            else {
                text(g, percentText(it->percent), Face::MonoBold, 12.f, col(c.danger, alpha), rest);
                text(g, itemText(*it, ItemFmt::Hud), Face::Mono, 12.f, col(c.text, alpha), box(rest.X + 36.f, r.Y, rest.Width - 62.f, r.Height));
                text(g, it->random ? L"rnd" : L"\u00b7", Face::Mono, 12.f, col(it->random ? c.rnd : 0x555555, alpha), r, A_RIGHT);
            }
            y += 17.f;
            any = true;
        }
        if (full) break;
    }
    if (!any && y + 17.f <= bottom) text(g, L"Nothing yet", Face::Ui, 12.f, col(c.muted), box(area.X, y, area.Width, 17.f));

    auto desc = description();
    if (!desc.empty()) {
        wrap(g, desc, Face::Ui, 12.f, col(c.text), G.footer);
    }
    else {
        gp::Color m = col(c.muted);
        runs(g, {
            { L"\u25b2", Face::Mono, 11.f, m }, { L" press  \u00b7  ", Face::Ui, 11.f, m },
            { L"\u25bc", Face::Mono, 11.f, m }, { L" release  \u00b7  scroll a number", Face::Ui, 11.f, m },
        }, G.footer, A_CENTER);
    }
}

void paintNeon(gp::Graphics& g, Theme const& t) {
    auto const& G = g_geo;
    auto const& c = t.c;
    runs(g, { { L"INPUT ", Face::Bold, 15.f, col(c.text) }, { L"BUFFER", Face::Bold, 15.f, col(c.accent) } }, box(16, 0, 200, G.titleH));
    drawButtons(g, t);

    card(g, t, G.sliderCard);
    for (int i = 0; i < SLIDER_COUNT; ++i) {
        bool on = g_values.sliders[i] > 0;
        text(g, i == 0 ? L"Buffer" : L"Randomize", Face::Semi, 13.f, col(on ? c.text : c.muted), G.head[i]);
        if (!on) {
            text(g, L"off", Face::Semi, 13.f, col(c.muted), G.head[i], A_RIGHT);
        }
        else if (i == 0) {
            auto ticks = rangeText(Fmt::Ticks);
            auto ms = L" \u00b7 " + rangeText(Fmt::Ms);
            runs(g, { { ticks, Face::Semi, 13.f, col(c.accent) }, { ms, Face::Ui, 13.f, col(c.muted) } }, G.head[i], A_RIGHT);
        }
        else {
            text(g, randomText(Fmt::Ticks), Face::Semi, 13.f, col(c.accent), G.head[i], A_RIGHT);
        }
        drawSlider(g, t, i);
    }

    for (int i = 0; i < TOGGLE_COUNT; ++i) {
        RectF r = G.toggle[i];
        bool on = g_values.toggles[i];
        bool h = hovering(HIT_TOGGLE + i);
        uint32_t bg = on ? mix(c.panel, c.accent, h ? 0.17f : 0.12f) : (h ? c.hover : c.panel);
        fill(g, r, col(bg), 10);
        stroke(g, r, col(on ? mix(c.panel, c.accent, 0.4f) : c.border), 10);
        text(g, TOGGLE_NAMES[i], Face::Ui, 13.f, col(on ? c.text : c.muted), box(r.X + 14, r.Y, r.Width - 40, r.Height));
        float dx = r.X + r.Width - 18.f;
        float dy = r.Y + r.Height / 2.f;
        if (on) circle(g, dx, dy, 7.f, col(c.accent, 50));
        circle(g, dx, dy, 4.f, col(on ? c.accent : mix(c.panel, c.muted, 0.3f)));
    }

    card(g, t, G.logCard);
    text(g, L"Log", Face::Semi, 13.f, col(c.text), box(G.logCard.X + 16, G.logCard.Y + 8, 120, 20));
    drawLog(g, t, box(G.logCard.X + 16, G.logCard.Y + 32, G.logCard.Width - 32, G.logCard.Height - 40), LogLook::Neon);

    footerText(g, t, G.footer, L"Hover a setting to see what it does  \u00b7  F7 to show / hide");
}

void retroPanel(gp::Graphics& g, RectF r, uint32_t color, int alpha = 255) {
    fill(g, r, col(color, alpha), 12);
    if (alpha == 255) stroke(g, inset(r, 4.5f), col(0xb8692c), 8, 3);
    stroke(g, inset(r, 1.5f), col(0x000000), 11, 3);
}

void paintRetro(gp::Graphics& g, Theme const& t) {
    auto const& G = g_geo;
    auto const& c = t.c;
    stroke(g, box(1.5f, 1.5f, t.w - 3.f, t.h - 3.f), col(0x000000), 0, 3);
    shadowed(g, L"Input Buffer", Face::Display, 24.f, col(0xffd84a), box(16, 6, 240, 44));
    drawButtons(g, t);

    retroPanel(g, G.sliderCard, c.panel);
    for (int i = 0; i < SLIDER_COUNT; ++i) {
        bool on = g_values.sliders[i] > 0;
        shadowed(g, i == 0 ? L"Buffer" : L"Randomize", Face::Display, 17.f, col(0xffffff), G.head[i]);
        std::wstring v = i == 0 ? rangeText(Fmt::Short) : randomText(Fmt::Short);
        if (i == 0 && on && lowTicks() < g_values.sliders[0]) {
            v = std::to_wstring(lowTicks()) + L" - " + std::to_wstring(g_values.sliders[0]);
        }
        shadowed(g, v, Face::Display, 17.f, col(on ? c.fill : 0xd0d0d0), G.head[i], A_RIGHT);
        drawSlider(g, t, i);
    }

    for (int i = 0; i < TOGGLE_COUNT; ++i) {
        RectF r = G.toggle[i];
        bool on = g_values.toggles[i];
        bool h = hovering(HIT_TOGGLE + i);
        uint32_t base = on ? c.accent : 0x6b6b6b;
        fill(g, r, col(h ? mix(base, 0xffffff, 0.15f) : base), 10);
        stroke(g, inset(r, 1.5f), col(0x000000), 9, 3);
        std::wstring_view name = TOGGLE_SHORT[i];
        float tw = measure(g, name, Face::Display, 16.f);
        float total = on ? tw + 22.f : tw;
        float x = r.X + (r.Width - total) / 2.f;
        float cy = r.Y + r.Height / 2.f;
        if (on) {
            checkMark(g, x + 8.f + 1.5f, cy + 1.5f, col(0x000000), 6.f, 3.f);
            checkMark(g, x + 8.f, cy, col(0xffffff), 6.f, 3.f);
            x += 22.f;
        }
        shadowed(g, name, Face::Display, 16.f, col(on ? 0xffffff : 0xd0d0d0), box(x, r.Y, tw + 6.f, r.Height));
    }

    retroPanel(g, G.logCard, 0x000000, 115);
    drawLog(g, t, box(G.logCard.X + 16, G.logCard.Y + 10, G.logCard.Width - 32, G.logCard.Height - 20), LogLook::Retro);

    auto desc = description();
    if (!desc.empty()) wrap(g, desc, Face::Semi, 13.f, col(0xffffff), G.footer);
    else wrap(g, L"Hover a setting to see what it does  \u00b7  F7 to show / hide", Face::Semi, 12.f, col(c.faint), G.footer);
}

void paintTwoPane(gp::Graphics& g, Theme const& t) {
    auto const& G = g_geo;
    auto const& c = t.c;
    text(g, L"Input Buffer", Face::Bold, 15.f, col(c.text), box(16, 0, 200, G.titleH));
    drawButtons(g, t);

    card(g, t, G.sliderCard);
    for (int i = 0; i < SLIDER_COUNT; ++i) sliderHover(g, t, i);
    for (int i = 0; i < SLIDER_COUNT; ++i) {
        bool on = g_values.sliders[i] > 0;
        gp::Color tc = col(on ? c.text : c.muted);
        text(g, i == 0 ? L"Buffer" : L"Randomize", Face::Semi, 13.f, tc, G.head[i]);
        text(g, i == 0 ? rangeText(Fmt::Ticks) : randomText(Fmt::Full), Face::Ui, 12.f, tc, G.head[i], A_RIGHT);
        drawSlider(g, t, i);
    }

    card(g, t, G.toggleCard);
    toggleRows(g, t, false);

    auto desc = description();
    if (!desc.empty()) wrap(g, desc, Face::Ui, 13.f, col(c.text), G.footer, A_LEFT, true);
    else wrap(g, L"Hover a setting to see what it does. F7 to show / hide.", Face::Ui, 12.f, col(c.muted), G.footer, A_LEFT, true);

    // Log: one block per attempt with a mini level bar.
    RectF pane = G.logCard;
    card(g, t, pane);
    float lx = pane.X + 16;
    float lw = pane.Width - 32;
    text(g, L"Log", Face::Semi, 13.f, col(c.text), box(lx, pane.Y + 10, 120, 20));
    wchar_t total[48];
    std::swprintf(total, 48, L"this level \u00b7 %d save%ls", g_levelSaves, plural(g_levelSaves));
    text(g, total, Face::Ui, 12.f, col(c.muted), box(lx, pane.Y + 10, lw, 20), A_RIGHT);

    float y = pane.Y + 40;
    float bottom = pane.Y + pane.Height - 10;
    bool any = false;
    for (size_t ai = 0; ai < g_attempts.size(); ++ai) {
        auto const& a = g_attempts[ai];
        int alpha = ageAlpha(static_cast<int>(ai));
        if (a.number > 0) {
            if (y + 34 + (a.items.empty() ? 0 : 18) > bottom) return;
            text(g, L"Attempt " + std::to_wstring(a.number), Face::Bold, 12.f, col(c.text, alpha), box(lx, y, lw / 2, 18));
            std::wstring status;
            if (a.death >= 0) status = L"died at " + std::to_wstring(a.death) + L"%";
            else if (ai == 0) status = L"this attempt";
            text(g, status, Face::Ui, 11.f, col(c.muted, alpha), box(lx + lw / 2, y, lw / 2, 18), A_RIGHT);
            y += 22;
            float by = y + 3;
            fill(g, box(lx, by, lw, 6), col(0x333333, alpha), 3);
            if (a.death >= 0) fill(g, box(lx, by, lw * std::clamp(a.death, 0, 100) / 100.f, 6), col(0x4a4a4a, alpha), 3);
            for (auto const& it : a.items) {
                if (it.note.empty() && it.percent >= 0) {
                    float mx = lx + lw * std::clamp(it.percent, 0, 100) / 100.f;
                    fill(g, box(mx - 1.5f, by - 3, 3, 12), col(c.danger, alpha));
                }
            }
            y += 16;
        }
        for (auto it = a.items.rbegin(); it != a.items.rend(); ++it) {
            if (y + 18 > bottom) return;
            RectF r = box(lx, y, lw, 18);
            if (!it->note.empty()) {
                text(g, it->time + L"  " + it->note, Face::Mono, 12.f, col(c.muted, alpha), r);
            }
            else {
                text(g, it->time, Face::Mono, 12.f, col(c.faint, alpha), box(lx, y, 62, 18));
                text(g, percentText(it->percent), Face::MonoBold, 12.f, col(c.danger, alpha), box(lx + 62, y, 46, 18));
                runs(g, {
                    { itemText(*it, ItemFmt::Full), Face::Mono, 12.f, col(c.text, alpha) },
                    { it->random ? L"   random" : L"", Face::Mono, 12.f, col(c.muted, alpha) },
                }, box(lx + 108, y, lw - 108, 18));
            }
            y += 18;
            any = true;
        }
        y += 6;
        if (ai + 1 < g_attempts.size() && y < bottom) line(g, lx, y, lx + lw, y, col(c.bg));
        y += 8;
    }
    if (!any && y + 18 <= bottom) text(g, L"Nothing yet", Face::Ui, 12.f, col(c.muted), box(lx, y, lw, 18));
}

void paintAnalytics(gp::Graphics& g, Theme const& t) {
    auto const& G = g_geo;
    auto const& c = t.c;
    text(g, L"Input Buffer", Face::Bold, 15.f, col(c.text), box(16, 0, 200, G.titleH));
    drawButtons(g, t);

    card(g, t, G.sliderCard);
    for (int i = 0; i < SLIDER_COUNT; ++i) sliderHover(g, t, i);
    for (int i = 0; i < SLIDER_COUNT; ++i) {
        bool on = g_values.sliders[i] > 0;
        gp::Color tc = col(on ? c.text : c.muted);
        text(g, i == 0 ? L"Buffer" : L"Randomize", Face::Semi, 12.f, tc, G.head[i]);
        text(g, i == 0 ? rangeText(Fmt::Short) : randomText(Fmt::Short), Face::Ui, 12.f, tc, G.head[i], A_RIGHT);
        drawSlider(g, t, i);
    }
    for (int k = 0; k < TOGGLE_COUNT; ++k) {
        RectF r = G.toggle[k];
        bool on = g_values.toggles[k];
        bool h = hovering(HIT_TOGGLE + k);
        if (k == 0 && on) {
            fill(g, r, col(h ? mix(c.accent, 0xffffff, 0.15f) : c.accent), 14);
            text(g, TOGGLE_SHORT[k], Face::Bold, 12.f, col(c.onAccent), r, A_CENTER);
        }
        else if (on) {
            fill(g, r, col(mix(c.panel, c.accent, h ? 0.2f : 0.13f)), 14);
            text(g, TOGGLE_SHORT[k], Face::Semi, 12.f, col(c.accent), r, A_CENTER);
        }
        else {
            fill(g, r, col(h ? 0x333333 : 0x2a2a2a), 14);
            text(g, TOGGLE_SHORT[k], Face::Ui, 12.f, col(c.muted), r, A_CENTER);
        }
        stroke(g, r, col(c.border), 14);
    }

    // Where in the level the buffer saved you.
    card(g, t, G.heatCard);
    RectF hc = G.heatCard;
    text(g, L"Where you need it", Face::Semi, 13.f, col(c.text), box(hc.X + 16, hc.Y + 10, 220, 18));
    wchar_t total[48];
    std::swprintf(total, 48, L"this level \u00b7 %d save%ls", g_levelSaves, plural(g_levelSaves));
    text(g, total, Face::Ui, 11.f, col(c.muted), box(hc.X + 16, hc.Y + 10, hc.Width - 32, 18), A_RIGHT);

    int most = 0;
    for (int b = 0; b < BUCKETS; ++b) most = std::max(most, g_heat[b]);
    float bw = barWidth();
    uint32_t low = mix(c.track, c.danger, 0.2f);
    for (int b = 0; b < BUCKETS; ++b) {
        float k = most > 0 ? static_cast<float>(g_heat[b]) / most : 0.f;
        float h = G.bars.Height * std::max(0.1f, k);
        uint32_t color = g_heat[b] > 0 ? mix(low, c.danger, k) : c.track;
        if (hovering(HIT_BUCKET + b)) color = mix(color, 0xffffff, 0.25f);
        fill(g, box(G.bars.X + b * (bw + 2.f), G.bars.Y + G.bars.Height - h, bw, h), col(color));
    }
    for (int q = 0; q <= 4; ++q) {
        float x = G.bars.X + G.bars.Width * q / 4.f;
        int align = q == 0 ? A_LEFT : q == 4 ? A_RIGHT : A_CENTER;
        float bx = align == A_LEFT ? x : align == A_RIGHT ? x - 40.f : x - 20.f;
        text(g, std::to_wstring(q * 25) + L"%", Face::Ui, 10.f, col(c.faint), box(bx, G.bars.Y + G.bars.Height + 2, 40, 14), align);
    }

    // Top 3 spots.
    int top[3] { -1, -1, -1 };
    for (int b = 0; b < BUCKETS; ++b) {
        if (g_heat[b] == 0) continue;
        for (int r = 0; r < 3; ++r) {
            if (top[r] < 0 || g_heat[b] > g_heat[top[r]]) {
                for (int s = 2; s > r; --s) top[s] = top[s - 1];
                top[r] = b;
                break;
            }
        }
    }
    float tw = (G.bars.Width - 12.f) / 3.f;
    for (int r = 0; r < 3; ++r) {
        RectF tile = box(G.bars.X + r * (tw + 6.f), hc.Y + 94, tw, 52);
        fill(g, tile, col(c.hover), 6);
        if (top[r] < 0) {
            text(g, L"\u2014", Face::Bold, 18.f, col(c.faint), box(tile.X + 10, tile.Y + 6, tile.Width - 20, 24));
            text(g, r == 0 && g_levelSaves == 0 ? L"no saves yet" : L"", Face::Ui, 11.f, col(c.muted), box(tile.X + 10, tile.Y + 30, tile.Width - 20, 16));
            continue;
        }
        int b = top[r];
        wchar_t range[24];
        std::swprintf(range, 24, L"%d\u2013%d%%", b * 5, b * 5 + 5);
        text(g, range, Face::Bold, 18.f, col(mix(c.danger, c.muted, r * 0.22f)), box(tile.X + 10, tile.Y + 6, tile.Width - 20, 24));
        int best = 0;
        for (int m = 1; m < MODE_COUNT; ++m) {
            if (g_heatModes[b][m] > g_heatModes[b][best]) best = m;
        }
        wchar_t sub[48];
        std::swprintf(sub, 48, L"%d save%ls \u00b7 %ls", g_heat[b], plural(g_heat[b]), MODE_NAMES[best]);
        text(g, sub, Face::Ui, 11.f, col(c.muted), box(tile.X + 10, tile.Y + 30, tile.Width - 20, 16));
    }

    card(g, t, G.logCard);
    text(g, L"Log", Face::Semi, 13.f, col(c.text), box(G.logCard.X + 16, G.logCard.Y + 6, 120, 22));
    drawLog(g, t, box(G.logCard.X + 16, G.logCard.Y + 32, G.logCard.Width - 32, G.logCard.Height - 40), LogLook::Clean);

    footerText(g, t, G.footer, L"Hover a setting or a bar to see more  \u00b7  F7 to show / hide");
}

void paintStepper(gp::Graphics& g, Theme const& t) {
    auto const& G = g_geo;
    auto const& c = t.c;
    text(g, L"Input Buffer", Face::Bold, 15.f, col(c.text), box(16, 0, 200, G.titleH));
    drawButtons(g, t);

    int hi = g_values.sliders[0];
    int rv = g_values.sliders[1];
    int lo = lowTicks();
    wchar_t const* labels[TARGET_COUNT] { L"MIN", L"MAX", L"RANDOM" };
    for (int i = 0; i < TARGET_COUNT; ++i) {
        RectF tile = G.target[i];
        bool h = hovering(HIT_TARGET + i) || within(g_hover, HIT_STEP + i * 2, 2) || within(g_holdStep, HIT_STEP + i * 2, 2);
        card(g, t, tile, h);
        spaced(g, labels[i], Face::Ui, 11.f, col(c.muted), box(tile.X, tile.Y + 8, tile.Width, 16), 1.f, A_CENTER);

        std::wstring num;
        std::wstring sub;
        uint32_t nc = c.text;
        auto ms = [](int ticks) {
            wchar_t b[24];
            std::swprintf(b, 24, L"%.1f ms", ticksToMs(ticks));
            return std::wstring(b);
        };
        if (i == T_MIN) {
            num = std::to_wstring(lo);
            if (hi <= 0) nc = c.muted;
            sub = ms(lo);
        }
        else if (i == T_MAX) {
            num = hi > 0 ? std::to_wstring(hi) : std::wstring(L"OFF");
            if (hi <= 0) nc = c.muted;
            sub = hi > 0 ? ms(hi) : std::wstring(L"buffer off");
        }
        else {
            num = rv > 0 ? L"+" + std::to_wstring(rv) : std::wstring(L"OFF");
            nc = rv > 0 ? c.accent : c.muted;
            sub = rv > 0 ? L"ticks" : L"smallest safe";
        }
        text(g, num, Face::Bold, 44.f, col(nc), box(tile.X, tile.Y + 24, tile.Width, 54), A_CENTER);
        text(g, sub, Face::Ui, 11.f, col(c.muted), box(tile.X, tile.Y + 80, tile.Width, 16), A_CENTER);

        for (int d = 0; d < 2; ++d) {
            RectF b = G.step[i][d];
            int code = HIT_STEP + i * 2 + d;
            bool held = g_holdStep == code;
            bool hb = hovering(code);
            fill(g, b, col(held ? 0x3c3c3c : hb ? 0x363636 : 0x2c2c2c), 6);
            stroke(g, b, col(c.border), 6);
            float cx = b.X + b.Width / 2.f;
            float cy = b.Y + b.Height / 2.f;
            line(g, cx - 5, cy, cx + 5, cy, col(c.text), 1.6f);
            if (d == 1) line(g, cx, cy - 5, cx, cy + 5, col(c.text), 1.6f);
        }
    }

    for (int k = 0; k < TOGGLE_COUNT; ++k) {
        RectF r = G.toggle[k];
        bool on = g_values.toggles[k];
        bool h = hovering(HIT_TOGGLE + k);
        uint32_t bg = on ? mix(c.panel, c.accent, h ? 0.2f : 0.14f) : (h ? c.hover : c.panel);
        fill(g, r, col(bg), 8);
        stroke(g, r, col(c.border), 8);
        text(g, on ? L"ON" : L"OFF", Face::Semi, 12.f, col(on ? c.accent : 0x8a8a8a), box(r.X, r.Y + 8, r.Width, 18), A_CENTER);
        text(g, TOGGLE_SHORT[k], Face::Ui, 11.f, col(on ? 0xc8c8c8 : c.muted), box(r.X, r.Y + 27, r.Width, 16), A_CENTER);
    }

    card(g, t, G.logCard);
    text(g, L"Log", Face::Semi, 13.f, col(c.text), box(G.logCard.X + 16, G.logCard.Y + 6, 120, 22));
    drawLog(g, t, box(G.logCard.X + 16, G.logCard.Y + 32, G.logCard.Width - 32, G.logCard.Height - 40), LogLook::Stepper);

    footerText(g, t, G.footer, L"Scroll a number or use \u2212 / +  \u00b7  F7 to hide");
}

void paintSlate(gp::Graphics& g, Theme const& t) {
    auto const& G = g_geo;
    auto const& c = t.c;
    text(g, L"Input Buffer", Face::Bold, 15.f, col(c.text), box(16, 0, 200, G.titleH));
    drawButtons(g, t);

    // Status line: everything at a glance.
    RectF s = G.status;
    fill(g, s, col(c.panel), t.radius);
    bool enabled = g_values.toggles[0];
    circle(g, s.X + 16, s.Y + s.Height / 2.f, 4.f, col(enabled ? c.accent : c.muted));
    std::wstring rest = L"  \u00b7  " + (g_values.sliders[0] > 0 ? rangeText(Fmt::Ticks) : std::wstring(L"buffer off"));
    rest += g_values.sliders[1] > 0 ? L"  \u00b7  random +" + std::to_wstring(g_values.sliders[1]) : std::wstring(L"  \u00b7  no random");
    if (g_values.toggles[1]) rest += L"  \u00b7  releases";
    if (g_values.toggles[2]) rest += L"  \u00b7  ship";
    runs(g, {
        { enabled ? L"Active" : L"Off", Face::Bold, 12.f, col(c.text) },
        { rest, Face::Ui, 12.f, col(mix(c.text, c.muted, 0.35f)) },
    }, box(s.X + 28, s.Y, s.Width - 40, s.Height));

    card(g, t, G.sliderCard);
    for (int i = 0; i < SLIDER_COUNT; ++i) sliderHover(g, t, i);
    for (int i = 0; i < SLIDER_COUNT; ++i) {
        bool on = g_values.sliders[i] > 0;
        text(g, i == 0 ? L"Buffer range" : L"Randomize", Face::Semi, 13.f, col(on ? c.text : c.muted), G.head[i]);
        text(g, i == 0 ? rangeText(Fmt::Ms) : randomText(Fmt::Full), Face::Ui, 12.f, col(mix(c.text, c.muted, 0.35f)), G.head[i], A_RIGHT);
        drawSlider(g, t, i);
    }

    card(g, t, G.toggleCard);
    toggleRows(g, t, false);

    card(g, t, G.logCard);
    drawLog(g, t, box(G.logCard.X + 16, G.logCard.Y + 8, G.logCard.Width - 32, G.logCard.Height - 16), LogLook::Slate);

    footerText(g, t, G.footer, L"Hover a setting to see what it does  \u00b7  F7 to show / hide");
}

void paintLines(gp::Graphics& g, Theme const& t) {
    auto const& G = g_geo;
    auto const& c = t.c;
    float W = t.w;
    spaced(g, L"INPUT BUFFER", Face::Semi, 13.f, col(c.text), box(22, 0, 260, G.titleH), 2.f);
    drawButtons(g, t);
    line(g, 22, G.titleH + 0.5f, W - 22, G.titleH + 0.5f, col(c.border));

    for (int i = 0; i < SLIDER_COUNT; ++i) {
        bool active = hovering(HIT_SLIDER + i) || g_dragging == i;
        bool on = g_values.sliders[i] > 0;
        spaced(g, i == 0 ? L"BUFFER" : L"RANDOMIZE", Face::Ui, 11.f, col(active ? c.text : c.muted), G.head[i], 1.5f);
        std::wstring big = !on ? std::wstring(L"off") : i == 0 ? rangeText(Fmt::Spaced) : randomText(Fmt::Short);
        runs(g, {
            { big, Face::Light, 20.f, col(on ? c.text : c.muted) },
            { on ? L" ticks" : L"", Face::Ui, 12.f, col(c.muted) },
        }, G.head[i], A_RIGHT);
        drawSlider(g, t, i);
        float ly = G.sliderRow[i].Y + G.sliderRow[i].Height + 0.5f;
        line(g, 22, ly, W - 22, ly, col(c.border));
    }

    for (int i = 0; i < TOGGLE_COUNT; ++i) {
        RectF r = G.toggle[i];
        bool on = g_values.toggles[i];
        bool h = hovering(HIT_TOGGLE + i);
        text(g, TOGGLE_NAMES[i], Face::Ui, 13.f, col(on || h ? c.text : 0x8a8a8a), r);
        text(g, on ? L"ON" : L"OFF", on ? Face::Semi : Face::Ui, 12.f, col(on ? c.text : (h ? c.muted : 0x8a8a8a)), r, A_RIGHT);
        float ly = r.Y + r.Height + 0.5f;
        line(g, 22, ly, W - 22, ly, col(i + 1 < TOGGLE_COUNT ? c.divider : c.border));
    }

    RectF lc = G.logCard;
    bool firstIsAttempt = !g_attempts.empty() && g_attempts.front().number > 0;
    std::wstring label = firstIsAttempt ? L"LOG \u00b7 ATTEMPT " + std::to_wstring(g_attempts.front().number) : std::wstring(L"LOG");
    spaced(g, label, Face::Ui, 11.f, col(c.muted), box(lc.X, lc.Y, lc.Width, 20), 1.5f);
    drawLog(g, t, box(lc.X, lc.Y + 24, lc.Width, lc.Height - 24), LogLook::Lines, firstIsAttempt);

    footerText(g, t, G.footer, L"Hover a setting to see what it does  \u00b7  F7 to show / hide");
}

// The theme list.
void paintScene(gp::Graphics& g);

// Live thumbnail of a theme, drawn next to (or under) the theme list.
void paintPreview(gp::Graphics& g, Theme const& t, int index) {
    Theme const& p = THEMES[index];
    RectF m = g_geo.menu;
    float leftRoom = m.X - 16.f;
    float belowRoom = t.h - (m.Y + m.Height) - 12.f;
    float s;
    float x;
    float y;
    if (leftRoom >= 160.f) {
        s = std::min({ leftRoom / p.w, (t.h - m.Y - 8.f) / p.h, 0.6f });
        x = m.X - 8.f - p.w * s;
        y = m.Y;
    }
    else if (belowRoom >= 110.f) {
        s = std::min({ (t.w - 16.f) / p.w, belowRoom / p.h, 0.6f });
        x = (t.w - p.w * s) / 2.f;
        y = m.Y + m.Height + 8.f;
    }
    else {
        return;
    }
    float pw = p.w * s;
    float ph = p.h * s;
    int bw = static_cast<int>(std::lround(pw * g_scale));
    int bh = static_cast<int>(std::lround(ph * g_scale));
    if (bw < 16 || bh < 16) return;

    // Paint the other theme off-screen with its own layout, then put everything back.
    Geo savedGeo = g_geo;
    int savedShown = g_shown;
    float savedScale = g_scale;
    int savedHover = g_hover;
    g_shown = index;
    computeGeo(p);
    g_scale = savedScale * s;
    g_menuOpen = false;
    g_hover = HIT_NONE;
    gp::Bitmap bmp(bw, bh, PixelFormat32bppARGB);
    {
        gp::Graphics pg(&bmp);
        paintScene(pg);
    }
    g_geo = savedGeo;
    g_shown = savedShown;
    g_scale = savedScale;
    g_hover = savedHover;
    g_menuOpen = true;

    RectF r = box(x, y, pw, ph);
    fill(g, box(x + 2, y + 6, pw, ph), col(0x000000, 70), 6.f);
    g.DrawImage(&bmp, r);
    stroke(g, r, col(mix(t.c.border, t.c.text, 0.35f)));
}

void paintMenu(gp::Graphics& g, Theme const& t) {
    auto const& G = g_geo;
    auto const& c = t.c;
    RectF m = G.menu;
    bool retro = t.kind == Kind::Retro;
    bool flat = c.panel == c.bg; // Terminal / Minimal: lift the list off the background
    float rad = retro ? 10.f : std::min(t.radius, 8.f);
    uint32_t panel = retro ? c.panel : flat ? mix(c.bg, c.text, 0.05f) : c.panel;
    uint32_t hover = retro ? mix(c.panel, 0xffffff, 0.12f) : flat ? mix(c.bg, c.text, 0.11f) : mix(c.hover, c.text, 0.04f);

    fill(g, box(m.X, m.Y + 6, m.Width, m.Height), col(0x000000, 45), rad + 2.f);
    fill(g, box(m.X, m.Y + 2, m.Width, m.Height + 1), col(0x000000, 60), rad + 1.f);
    fill(g, m, col(panel), rad);
    if (retro) stroke(g, inset(m, 1.5f), col(0x000000), rad - 1.f, 3.f);
    else stroke(g, m, col(flat ? mix(c.border, c.text, 0.15f) : c.border), rad);

    for (int i = 0; i < THEME_COUNT; ++i) {
        RectF r = menuItem(i);
        auto const& item = THEMES[i];
        bool h = hovering(HIT_MENU + i) || (g_menuKey == i && !within(g_hover, HIT_MENU, THEME_COUNT));
        bool current = i == g_shown;
        if (h) fill(g, r, col(hover), std::max(0.f, rad - 4.f));

        // Swatch: the theme's background with its accent.
        RectF sw = box(r.X + 8, r.Y + (r.Height - 16) / 2.f, 16, 16);
        float srad = t.kind == Kind::Terminal ? 0.f : 4.f;
        fill(g, sw, col(item.c.bg), srad);
        stroke(g, sw, col(mix(panel, c.text, 0.3f)), srad);
        circle(g, sw.X + 8, sw.Y + 8, 4.f, col(item.c.accent));

        // Narrow windows (HUD): smaller text so long names still fit.
        bool narrow = r.Width < 160.f;
        RectF label = box(r.X + 30, r.Y, r.Width - (current ? 50.f : 34.f), r.Height);
        uint32_t tc = current || h ? c.text : mix(c.text, c.muted, 0.4f);
        switch (t.kind) {
            case Kind::Terminal: text(g, lower(item.name), Face::Mono, narrow ? 11.f : 12.f, col(tc), label); break;
            case Kind::Lines: text(g, upper(item.name), Face::Semi, narrow ? 10.f : 11.f, col(tc), label); break;
            case Kind::Retro: shadowed(g, item.name, Face::Display, narrow ? 13.f : 14.f, col(0xffffff), label, A_LEFT, 1.5f); break;
            default: text(g, item.name, current ? Face::Semi : Face::Ui, narrow ? 12.f : 13.f, col(tc), label); break;
        }
        if (current) {
            uint32_t mark = retro ? c.fill : c.accent;
            checkMark(g, r.X + r.Width - 14.f, r.Y + r.Height / 2.f, col(mark), 4.5f, 2.f);
        }
    }

    int preview = within(g_hover, HIT_MENU, THEME_COUNT) ? g_hover - HIT_MENU : g_menuKey;
    if (preview >= 0 && preview < THEME_COUNT) paintPreview(g, t, preview);
}

// The whole window for the current theme, in logical pixels.
void paintScene(gp::Graphics& g) {
    Theme const& t = shown();
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
    g.ScaleTransform(g_scale, g_scale);
    g.Clear(col(t.c.bg));
    if (t.kind != Kind::Retro) stroke(g, box(0, 0, t.w - 1.f, t.h - 1.f), col(t.c.border));

    switch (t.kind) {
        case Kind::Clean: paintClean(g, t); break;
        case Kind::Terminal: paintTerminal(g, t); break;
        case Kind::Hud: paintHud(g, t); break;
        case Kind::Neon: paintNeon(g, t); break;
        case Kind::Retro: paintRetro(g, t); break;
        case Kind::TwoPane: paintTwoPane(g, t); break;
        case Kind::Analytics: paintAnalytics(g, t); break;
        case Kind::Stepper: paintStepper(g, t); break;
        case Kind::Slate: paintSlate(g, t); break;
        case Kind::Lines: paintLines(g, t); break;
    }
    if (g_menuOpen) paintMenu(g, t);
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
    if (pw <= 0 || ph <= 0 || g_shown < 0) return;

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
        case Event::NewAttempt: {
            if (e.newLevel) {
                std::memset(g_heat, 0, sizeof(g_heat));
                std::memset(g_heatModes, 0, sizeof(g_heatModes));
                g_levelSaves = 0;
            }
            // An attempt where nothing happened: reuse it instead of stacking headers.
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
        }
        case Event::Death:
            if (!g_attempts.empty() && g_attempts.front().number > 0 && g_attempts.front().death < 0) {
                g_attempts.front().death = e.number;
            }
            break;
        case Event::Save: {
            int p = e.item.percent;
            if (p >= 0) {
                int b = std::clamp(p * BUCKETS / 100, 0, BUCKETS - 1);
                g_heat[b]++;
                g_heatModes[b][std::clamp(e.item.mode, 0, MODE_COUNT - 1)]++;
            }
            g_levelSaves++;
            addItem(std::move(e.item));
            g_attempts.front().saves++;
            break;
        }
        case Event::Note:
            addItem(std::move(e.item));
            break;
    }
}

// ===========================================================================
// Window
// ===========================================================================

using DwmSetFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

// dwmapi is loaded once and stays loaded (it's a system DLL the game already uses).
DwmSetFn dwmSet() {
    static DwmSetFn fn = [] {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<DwmSetFn>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
    }();
    return fn;
}

// Windows 11 corners, dark frame and border color to match the theme.
void styleFrame(HWND wnd, Theme const& t) {
    auto set = dwmSet();
    if (!set) return;
    int corners = t.corners;
    set(wnd, 33, &corners, sizeof(corners)); // DWMWA_WINDOW_CORNER_PREFERENCE
    BOOL dark = TRUE;
    set(wnd, 20, &dark, sizeof(dark));       // DWMWA_USE_IMMERSIVE_DARK_MODE
    COLORREF border = RGB((t.c.border >> 16) & 0xff, (t.c.border >> 8) & 0xff, t.c.border & 0xff);
    set(wnd, 34, &border, sizeof(border));   // DWMWA_BORDER_COLOR
}

bool isOnScreen(int x, int y) {
    POINT pt { x + 40, y + 20 };
    return MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) != nullptr;
}

BOOL CALLBACK findSecondMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    MONITORINFO info { sizeof(info) };
    if (GetMonitorInfoW(monitor, &info) && !(info.dwFlags & MONITORINFOF_PRIMARY)) {
        *reinterpret_cast<RECT*>(data) = info.rcWork;
        return FALSE; // found one, stop
    }
    return TRUE;
}

// First time the window shows: last saved spot, else the second monitor,
// else next to the game.
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
        int x = second.left + (second.right - second.left - w) / 2;
        int y = second.top + (second.bottom - second.top - h) / 2;
        SetWindowPos(wnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }

    RECT game {};
    HWND gameWnd = g_game.load();
    if (gameWnd && GetWindowRect(gameWnd, &game)) {
        SetWindowPos(wnd, nullptr, game.left + 40, game.top + 60, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// Sizes the window for its theme and monitor: 1x on 1080p, 1.33x on 1440p,
// 2x on 4K (by the monitor's shorter side), times the Window Size setting,
// but never bigger than the screen.
void applyLayout(HWND wnd) {
    int theme = std::clamp(g_values.theme, 0, THEME_COUNT - 1);
    Theme const& t = THEMES[theme];
    float monitorScale = 1.f;
    MONITORINFO info { sizeof(info) };
    bool haveMonitor = GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &info) != 0;
    if (haveMonitor) {
        float mw = static_cast<float>(info.rcMonitor.right - info.rcMonitor.left);
        float mh = static_cast<float>(info.rcMonitor.bottom - info.rcMonitor.top);
        monitorScale = std::clamp(std::min(mw, mh) / 1080.f, 0.75f, 3.f);
    }
    float user = std::clamp(g_values.sizePercent / 100.f, 0.5f, 2.f);
    float scale = std::clamp(monitorScale * user, 0.5f, 4.f);
    if (haveMonitor) {
        float fit = std::min(
            (info.rcWork.right - info.rcWork.left) * 0.95f / t.w,
            (info.rcWork.bottom - info.rcWork.top) * 0.95f / t.h
        );
        scale = std::max(0.5f, std::min(scale, fit));
    }
    bool themeChanged = theme != g_shown;
    if (!themeChanged && std::abs(scale - g_scale) < 0.01f) return;

    g_scale = scale;
    g_shown = theme;
    if (themeChanged) {
        computeGeo(t);
        styleFrame(wnd, t);
        g_menuOpen = false;
        g_hover = HIT_NONE;
    }

    int w = static_cast<int>(std::lround(t.w * g_scale));
    int h = static_cast<int>(std::lround(t.h * g_scale));
    RECT r;
    GetWindowRect(wnd, &r);
    int x = r.left;
    int y = r.top;
    // A bigger theme shouldn't hang off the edge of the screen.
    if (themeChanged && haveMonitor && g_placed) {
        RECT const& work = info.rcWork;
        x = std::max<int>(work.left, std::min<int>(x, work.right - w));
        y = std::max<int>(work.top, std::min<int>(y, work.bottom - h));
    }
    SetWindowPos(wnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    if ((x != r.left || y != r.top) && g_placed) savePosition(wnd);
    InvalidateRect(wnd, nullptr, FALSE);
}

void showWindow(HWND wnd, Values* values) {
    g_values = *values;
    delete values;
    applyLayout(wnd);
    if (!g_placed) {
        place(wnd, g_values);
        g_placed = true;
        applyLayout(wnd); // it may have moved to a monitor with a different size
    }
    g_hover = HIT_NONE;
    // Don't steal focus from the game, but come to the front (also restores it if minimized).
    ShowWindow(wnd, SW_SHOWNOACTIVATE);
    SetWindowPos(wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(wnd, nullptr, FALSE);
}

// True if the game window sits on top of this one (single monitor setups).
bool coveredByGame(HWND wnd) {
    HWND game = g_game.load();
    if (!game || game == wnd || !IsWindowVisible(game) || IsIconic(game)) return false;
    RECT a;
    RECT b;
    RECT both;
    if (!GetWindowRect(wnd, &a) || !GetWindowRect(game, &b) || !IntersectRect(&both, &a, &b)) return false;
    for (HWND w = GetWindow(wnd, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (w == game) return true;
    }
    return false;
}

void closeMenu(HWND wnd) {
    KillTimer(wnd, TIMER_MENU);
    if (!g_menuOpen) return;
    g_menuOpen = false;
    g_menuKey = -1;
    InvalidateRect(wnd, nullptr, FALSE);
}

void pickTheme(HWND wnd, int index) {
    closeMenu(wnd);
    index = std::clamp(index, 0, THEME_COUNT - 1);
    if (index == g_values.theme && index == g_shown) return;
    g_values.theme = index;
    applyLayout(wnd);
    save();
}

// Ends a drag or a held - / + button, saving the new value.
void endPress(HWND wnd) {
    bool changed = g_dragging >= 0 || g_holdStep >= 0;
    g_dragging = -1;
    g_dragMin = false;
    if (g_holdStep >= 0) KillTimer(wnd, TIMER_REPEAT);
    g_holdStep = -1;
    if (GetCapture() == wnd) ReleaseCapture();
    if (changed) {
        save();
        InvalidateRect(wnd, nullptr, FALSE);
    }
}

void hide(HWND wnd) {
    endPress(wnd);
    closeMenu(wnd);
    bool hadFocus = GetForegroundWindow() == wnd;
    ShowWindow(wnd, SW_HIDE);
    if (hadFocus) {
        if (HWND game = g_game.load()) SetForegroundWindow(game);
    }
}

void dragTo(HWND wnd, float x) {
    if (g_dragging == 0 && g_dragMin) setMin(valueAt(0, x), false);
    else if (g_dragging == 0) setMax(valueAt(0, x));
    else setRandom(valueAt(g_dragging, x));
    InvalidateRect(wnd, nullptr, FALSE);
}

// Which number the wheel changes when scrolling over a slider: for the buffer
// range, whichever handle is closer to the mouse.
int sliderTarget(int slider, float x) {
    if (slider == 1) return T_RANDOM;
    int hi = g_values.sliders[0];
    if (hi <= 0) return T_MAX;
    int lo = lowTicks();
    float dl = std::abs(x - thumbX(0, lo));
    float dh = std::abs(x - thumbX(0, hi));
    // Same spot (min = max): left of the handle grabs min, right grabs max.
    return (dl < dh || (dl == dh && x < thumbX(0, lo))) ? T_MIN : T_MAX;
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
            // Hide if it's showing; show (or bring back on top of the game) otherwise.
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

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE; // clicking never takes keyboard focus from the game

        case WM_DPI_CHANGED:
            applyLayout(wnd);
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                int h = g_hover;
                bool clickable = h == HIT_CLOSE || h == HIT_MINIMIZE || h == HIT_THEME
                    || within(h, HIT_TOGGLE, TOGGLE_COUNT) || within(h, HIT_SLIDER, SLIDER_COUNT)
                    || within(h, HIT_STEP, TARGET_COUNT * 2) || within(h, HIT_MENU, THEME_COUNT)
                    || (within(h, HIT_TARGET, TARGET_COUNT) && shown().kind == Kind::Hud);
                if (clickable) {
                    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649))); // IDC_HAND
                    return TRUE;
                }
            }
            break;

        case WM_PB_OPEN: {
            auto values = reinterpret_cast<Values*>(lParam);
            if (IsWindowVisible(wnd)) {
                delete values;
                return 0;
            }
            showWindow(wnd, values);
            return 0;
        }

        case WM_PB_HIDE:
            hide(wnd);
            return 0;

        case WM_PB_SYNC: {
            auto values = reinterpret_cast<Values*>(lParam);
            if (g_dragging < 0 && g_holdStep < 0) {
                for (int i = 0; i < SLIDER_COUNT; ++i) g_values.sliders[i] = values->sliders[i];
                g_values.minTicks = values->minTicks;
                for (int i = 0; i < TOGGLE_COUNT; ++i) g_values.toggles[i] = values->toggles[i];
                g_values.theme = values->theme;
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

        case WM_NCHITTEST: {
            // Drag the window by its title bar.
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(wnd, &pt);
            float x = pt.x / g_scale;
            float y = pt.y / g_scale;
            // No dragging while the theme list is open (a click anywhere closes it).
            if (!g_menuOpen && y < g_geo.titleH && hitTest(x, y) == HIT_NONE) return HTCAPTION;
            return HTCLIENT;
        }

        case WM_NCLBUTTONDOWN:
            closeMenu(wnd); // clicking the title bar closes the theme list
            break;

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) closeMenu(wnd);
            break;

        case WM_EXITSIZEMOVE:
            applyLayout(wnd); // moved to a different monitor?
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
            if (g_menuOpen) KillTimer(wnd, TIMER_MENU);
            if (g_dragging >= 0) {
                dragTo(wnd, x);
                return 0;
            }
            updateHover(wnd, x, y);
            TRACKMOUSEEVENT tme { sizeof(tme), TME_LEAVE, wnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            // Mouse went back to the game: close the theme list after a moment.
            if (g_menuOpen) SetTimer(wnd, TIMER_MENU, 400, nullptr);
            if (g_hover != HIT_NONE && g_holdStep < 0) {
                g_hover = HIT_NONE;
                InvalidateRect(wnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            float x = logicalX(lParam);
            float y = logicalY(lParam);
            int hit = hitTest(x, y);
            bool right = msg == WM_RBUTTONDOWN;

            if (g_menuOpen) {
                if (within(hit, HIT_MENU, THEME_COUNT)) pickTheme(wnd, hit - HIT_MENU);
                else if (hit != HIT_MENU_PANEL) closeMenu(wnd); // a click outside just closes the list
                updateHover(wnd, x, y);
                return 0;
            }
            bool hud = shown().kind == Kind::Hud;
            if (right) {
                // HUD: right click lowers a number.
                if (hud && within(hit, HIT_TARGET, TARGET_COUNT)) adjust(wnd, hit - HIT_TARGET, -1, true);
                return 0;
            }

            if (hit == HIT_CLOSE) {
                hide(wnd);
            }
            else if (hit == HIT_MINIMIZE) {
                ShowWindow(wnd, SW_SHOWMINNOACTIVE); // keeps the game focused
            }
            else if (hit == HIT_THEME) {
                g_menuOpen = true;
                g_menuKey = g_shown;
                updateHover(wnd, x, y);
                InvalidateRect(wnd, nullptr, FALSE);
            }
            else if (within(hit, HIT_SLIDER, SLIDER_COUNT)) {
                int i = hit - HIT_SLIDER;
                // Only grab when clicking near the bar itself.
                if (std::abs(y - g_geo.ty[i]) <= 16.f && x >= g_geo.x0[i] - 14.f && x <= g_geo.x1[i] + 14.f) {
                    g_dragging = i;
                    g_dragMin = i == 0 && sliderTarget(0, x) == T_MIN;
                    g_focus = i == 1 ? T_RANDOM : (g_dragMin ? T_MIN : T_MAX);
                    SetCapture(wnd);
                    dragTo(wnd, x);
                }
                else {
                    g_focus = i == 1 ? T_RANDOM : T_MAX;
                }
            }
            else if (within(hit, HIT_STEP, TARGET_COUNT * 2)) {
                int target = (hit - HIT_STEP) / 2;
                int delta = (hit - HIT_STEP) % 2 ? 1 : -1;
                adjust(wnd, target, delta, false);
                g_holdStep = hit;
                SetCapture(wnd);
                SetTimer(wnd, TIMER_REPEAT, 400, nullptr); // held: repeat after a moment
            }
            else if (hud && within(hit, HIT_TARGET, TARGET_COUNT)) {
                adjust(wnd, hit - HIT_TARGET, 1, true); // HUD: left click raises a number
            }
            else if (within(hit, HIT_TOGGLE, TOGGLE_COUNT)) {
                g_values.toggles[hit - HIT_TOGGLE] = !g_values.toggles[hit - HIT_TOGGLE];
                InvalidateRect(wnd, nullptr, FALSE);
                save();
            }
            return 0;
        }

        case WM_TIMER:
            if (wParam == TIMER_MENU) {
                closeMenu(wnd);
                return 0;
            }
            if (wParam == TIMER_REPEAT) {
                if (g_holdStep >= 0 && g_hover == g_holdStep) {
                    int target = (g_holdStep - HIT_STEP) / 2;
                    int delta = (g_holdStep - HIT_STEP) % 2 ? 1 : -1;
                    adjust(wnd, target, delta, false);
                }
                if (g_holdStep >= 0) SetTimer(wnd, TIMER_REPEAT, 70, nullptr);
                else KillTimer(wnd, TIMER_REPEAT);
                return 0;
            }
            break;

        case WM_LBUTTONUP:
            if (g_dragging >= 0 || g_holdStep >= 0) endPress(wnd);
            return 0;

        case WM_CAPTURECHANGED:
            if ((g_dragging >= 0 || g_holdStep >= 0) && reinterpret_cast<HWND>(lParam) != wnd) endPress(wnd);
            return 0;

        case WM_MOUSEWHEEL: {
            if (g_menuOpen || g_dragging >= 0 || g_holdStep >= 0) return 0;
            g_wheel += GET_WHEEL_DELTA_WPARAM(wParam);
            int steps = g_wheel / WHEEL_DELTA;
            g_wheel -= steps * WHEEL_DELTA;
            if (steps == 0) return 0;
            // Only scrolling over a slider or a number changes it. Hit test where
            // the wheel event happened (screen coordinates), not the last hover.
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(wnd, &pt);
            float x = pt.x / g_scale;
            int hit = hitTest(x, pt.y / g_scale);
            int target = -1;
            if (within(hit, HIT_SLIDER, SLIDER_COUNT)) target = sliderTarget(hit - HIT_SLIDER, x);
            else if (within(hit, HIT_TARGET, TARGET_COUNT)) target = hit - HIT_TARGET;
            else if (within(hit, HIT_STEP, TARGET_COUNT * 2)) target = (hit - HIT_STEP) / 2;
            if (target >= 0) adjust(wnd, target, steps, true);
            return 0;
        }

        case WM_KEYDOWN:
            if (g_menuOpen) {
                switch (wParam) {
                    case VK_ESCAPE: closeMenu(wnd); return 0;
                    case VK_LEFT:
                    case VK_RIGHT:
                        g_menuKey = (g_menuKey + MENU_ROWS) % (MENU_ROWS * 2);
                        if (g_menuKey >= THEME_COUNT) g_menuKey = THEME_COUNT - 1;
                        InvalidateRect(wnd, nullptr, FALSE);
                        return 0;
                    case VK_UP:
                        g_menuKey = (g_menuKey + THEME_COUNT - 1) % THEME_COUNT;
                        InvalidateRect(wnd, nullptr, FALSE);
                        return 0;
                    case VK_DOWN:
                        g_menuKey = (g_menuKey + 1) % THEME_COUNT;
                        InvalidateRect(wnd, nullptr, FALSE);
                        return 0;
                    case VK_RETURN:
                    case VK_SPACE:
                        pickTheme(wnd, g_menuKey);
                        return 0;
                }
                return 0;
            }
            switch (wParam) {
                case VK_ESCAPE:
                case VK_F7:
                    hide(wnd);
                    return 0;
                case VK_LEFT:
                case VK_DOWN:
                    adjust(wnd, g_focus, -1, true);
                    return 0;
                case VK_RIGHT:
                case VK_UP:
                    adjust(wnd, g_focus, 1, true);
                    return 0;
            }
            break;

        case WM_CLOSE:
            hide(wnd); // keep the window around, just hide it
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

    auto instance = GetModuleHandleW(nullptr);

    HDC screen = GetDC(nullptr);
    g_scale = GetDeviceCaps(screen, LOGPIXELSX) / 96.f; // first guess, applyLayout fixes it on show
    ReleaseDC(nullptr, screen);
    if (!(g_scale >= 1.f && g_scale <= 4.f)) g_scale = 1.f;

    // Per-monitor DPI for this window only, so it stays sharp on every monitor
    // whatever DPI mode the game runs in (Windows 10 1607+, skipped otherwise).
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetContextFn = HANDLE(WINAPI*)(HANDLE);
        if (auto setContext = reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetThreadDpiAwarenessContext"))) {
            setContext(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4))); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        }
    }

    WNDCLASSW cls {};
    cls.style = CS_DROPSHADOW;
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    cls.lpszClassName = L"InputBufferLiteWindow";
    RegisterClassW(&cls);

    int pw = static_cast<int>(std::lround(THEMES[0].w * g_scale));
    int ph = static_cast<int>(std::lround(THEMES[0].h * g_scale));

    // A normal app window (own taskbar button) so it can live on another monitor.
    // NOACTIVATE: clicking it never takes keyboard focus away from the game.
    HWND wnd = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName, L"Input Buffer Lite", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, pw, ph, nullptr, nullptr, instance, nullptr
    );
    if (!wnd) {
        g_started = false;
        return;
    }

    // Hide from capture. Falls back to a black box on Windows older than 10 (2004).
    if (!SetWindowDisplayAffinity(wnd, EXCLUDE_FROM_CAPTURE)) {
        SetWindowDisplayAffinity(wnd, WDA_MONITOR);
    }

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
// doesn't exist yet, it's created in the background and picks the request up,
// so the game never waits for it.
void request(UINT msg) {
    if (HWND wnd = g_window.load()) {
        auto values = new Values(readSettings());
        if (!PostMessageW(wnd, msg, 0, reinterpret_cast<LPARAM>(values))) delete values;
        return;
    }
    startThread();
    delete g_pending.exchange(new Pending { msg, readSettings() });
    // The window may have finished in the meantime: hand the request over here.
    if (HWND wnd = g_window.load()) takePending(wnd);
}

void rememberGameWindow(HWND wnd) {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == wnd) return;
    // Only GD's own window counts (not a launcher or Discord in front at startup).
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) g_game = fg;
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
    rememberGameWindow(g_window.load());
    request(WM_PB_TOGGLE);
}

void open() {
    rememberGameWindow(g_window.load());
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

void log(std::string const& line) {
    if (!g_window.load()) return;
    auto e = new Event;
    e->type = Event::Note;
    e->item.time = timestamp();
    e->item.note = widen(line);
    post(e);
}

void saved(int ticks, double ms, int mode, bool release, bool random, int percent) {
    if (!g_window.load()) return;
    auto e = new Event;
    e->type = Event::Save;
    e->item.time = timestamp();
    e->item.percent = percent;
    e->item.ticks = ticks;
    e->item.ms = static_cast<float>(ms);
    e->item.mode = mode;
    e->item.release = release;
    e->item.random = random;
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

} // namespace pb::menu

#else

namespace pb::menu {

void toggle() {
    openSettingsPopup(Mod::get(), false);
}

void open() {}

void prepare() {}

void sync() {}

void log(std::string const&) {}

void saved(int, double, int, bool, bool, int) {}

void attempt(int, bool) {}

void death(int) {}

} // namespace pb::menu

#endif
