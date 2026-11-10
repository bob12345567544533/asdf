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
constexpr int MAIN_SLIDERS = 2; // buffer, randomize (big rows)
constexpr int MODE_SLIDERS = 8; // per-mode limits (small grid)
constexpr int SLIDERS = MAIN_SLIDERS + MODE_SLIDERS;
constexpr int TOGGLE_GROUP = 8;  // switches per group (a grid of two columns)
constexpr int TOGGLE_GROUPS = 2;
constexpr int TOGGLES = TOGGLE_GROUP * TOGGLE_GROUPS;
constexpr int MODES = 8;
constexpr int TABS = 2;
constexpr int TAB_LIVE = 0;     // what's happening: timing, deaths, the log
constexpr int TAB_SETTINGS = 1; // everything you can change

constexpr wchar_t const* TAB_NAMES[TABS] { L"LIVE", L"SETTINGS" };
constexpr wchar_t const* GROUP_NAMES[TOGGLE_GROUPS] { L"SWITCHES", L"HOW IT JUDGES A CLICK" };
constexpr char const* TOGGLE_KEYS[TOGGLES] {
    "enabled", "buffer-releases", "buffer-ship", "hold-fix", "coach", "show-feedback", "death-check", "practice-only",
    "finer-timing-v3", "best-fix", "confidence-check", "smart-safety", "moving-objects", "through-pads", "through-orbs", "tap-model",
};
constexpr bool TOGGLE_DEFAULTS[TOGGLES] { true, true, false, true, true, true, true, false,
                                          true, true, true, true, true, true, true, true };
constexpr wchar_t const* TOGGLE_NAMES[TOGGLES] {
    L"Enabled", L"Releases", L"Ship", L"Hold fix", L"Coach", L"Timing popup", L"Death check", L"Practice only",
    L"Finer timing", L"Best fix", L"Only when sure", L"Smart safety", L"Moving objects", L"Pads + portals", L"Orbs", L"Learn my taps",
};
constexpr wchar_t const* TOGGLE_HELP[TOGGLES] {
    L"Turns the buffer on or off. The toggle key (F6) also cycles 2, 4, 8 and 16 ticks, then off.",
    L"Also holds back early releases in wave and robot (and ship when Ship is on), where letting go too early kills you.",
    L"Lets the buffer work in ship mode. Off by default: ship predictions are less reliable.",
    L"Wave (and ship when Ship is on): when holding on a little too long would kill you and letting go right then is safe, lets go for you.",
    L"After each death, works out how early or late your click was and shows it on the LIVE tab. Works with the buffer off too. Starts with the next level you open.",
    L"Shows in game how early each held-back click was.",
    L"Says so when you die within 0.6 s of the buffer changing one of your inputs, so you know if it helped or hurt.",
    L"Only helps in practice mode.",
    L"Delays in quarter ticks, so a click moves only as much as it has to.",
    L"Compares the safe delays further ahead and takes the one that keeps you alive longest.",
    L"Only helps when a quarter tick later would be safe too. If not, your click goes through untouched.",
    L"Cube-like modes: prefers delays where the jump fully clears or lands.",
    L"The ghost moves blocks and spikes that are moving along with it instead of treating them as still.",
    L"The ghost uses jump pads, speed portals and gravity portals itself, so the buffer still helps near them.",
    L"The ghost uses yellow, pink, red, blue, green and black orbs itself (each kind after you've hit it once).",
    L"Learns how long you hold your clicks in each mode, so the ghost lets go when you would.",
};
constexpr char const* SLIDER_KEYS[SLIDERS] { "buffer-ticks", "randomize", "max-cube", "max-ship", "max-ball", "max-ufo",
                                            "max-wave", "max-robot", "max-spider", "max-swing" };
constexpr wchar_t const* SLIDER_NAMES[SLIDERS] { L"BUFFER", L"RANDOMIZE", L"CUBE", L"SHIP", L"BALL", L"UFO",
                                                 L"WAVE", L"ROBOT", L"SPIDER", L"SWING" };
constexpr wchar_t const* MODE_HELP =
    L"Most this mode may be delayed, in ticks. BUFFER still caps every mode: the lower of the two wins (faded = capped by BUFFER).";
constexpr wchar_t const* SLIDER_HELP[MAIN_SLIDERS] {
    L"Largest delay the buffer may add to a click that would kill you, in ticks (1 tick = 4.2 ms). Each gamemode can be capped lower below. 0 = off.",
    L"0 = always the smallest safe delay. Above 0 = any safe delay up to this many ticks above the smallest one.",
};
constexpr wchar_t const* TIMING_HELP =
    L"Every death the coach could work out, placed by how far off your click was. Left of the middle = you clicked too early, right = too late. Brighter = more recent.";
constexpr wchar_t const* DEATHS_HELP = L"Where in the level you died. Taller = more deaths at that spot, red = the last one.";
constexpr wchar_t const* LIVE_HELP = L"F7 show / hide  ·  F6 cycle the buffer  ·  mouse wheel scrolls the log";
constexpr wchar_t const* SETTINGS_HELP = L"Hover something to see what it does";
constexpr wchar_t const* MODE_NAMES[MODES] { L"cube", L"ship", L"ball", L"ufo", L"wave", L"robot", L"spider", L"swing" };

// Colors: black and white, plus one colour each for early, late and bad news.
constexpr uint32_t C_BG = 0x121212;
constexpr uint32_t C_HOVER = 0x1a1a1a;
constexpr uint32_t C_LINE = 0x2a2a2a;
constexpr uint32_t C_DIVIDER = 0x1e1e1e;
constexpr uint32_t C_TEXT = 0xececec;
constexpr uint32_t C_MUTED = 0x9a9a9a;
constexpr uint32_t C_FAINT = 0x6a6a6a;
constexpr uint32_t C_TRACK = 0x333333;
constexpr uint32_t C_DANGER = 0xff6b6b;
constexpr uint32_t C_LATE = 0xffb86b;  // coach: you were late
constexpr uint32_t C_EARLY = 0x7cc4ff; // coach: you were early

// Layout, in logical pixels (scaled for the monitor)
constexpr float W = 454.f;
constexpr float H = 900.f;
constexpr float PAD = 22.f;
constexpr float TITLE_H = 48.f;
constexpr float TAB_Y = TITLE_H + 1.f;
constexpr float TAB_H = 36.f;
constexpr float TOP = TAB_Y + TAB_H + 1.f; // where a tab's own content starts
constexpr float FOOTER_H = 46.f; // room for three lines of help
// LIVE
constexpr float STATS_Y = TOP;
constexpr float STATS_H = 60.f;
constexpr float TIMING_Y = STATS_Y + STATS_H + 1.f;
constexpr float TIMING_H = 124.f;
constexpr float DEATHS_Y = TIMING_Y + TIMING_H + 1.f;
constexpr float DEATHS_H = 70.f;
constexpr float LOG_Y = DEATHS_Y + DEATHS_H + 12.f;
constexpr float TIMING_RANGE = 80.f; // ms from the middle of the timing bar to either end
constexpr size_t TIMING_MAX = 40;    // deaths it shows
// SETTINGS
constexpr float SLIDER_Y = TOP;
constexpr float SLIDER_H = 82.f;
constexpr float MODE_Y = SLIDER_Y + MAIN_SLIDERS * (SLIDER_H + 1.f);
constexpr float MODE_HEAD = 30.f;
constexpr float MODE_CELL_H = 46.f;
constexpr float MODE_GAP = 24.f;  // between the two columns
constexpr int MODE_ROWS = MODE_SLIDERS / 2;
constexpr float TOGGLE_Y = MODE_Y + MODE_HEAD + MODE_ROWS * MODE_CELL_H + 8.f;
constexpr float TOGGLE_HEAD = 28.f;
constexpr float TOGGLE_H = 34.f;
constexpr float TOGGLE_GROUP_H = TOGGLE_HEAD + (TOGGLE_GROUP / 2) * TOGGLE_H + 6.f;
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
    int sliders[SLIDERS] { 2, 3, 24, 8, 24, 6, 8, 24, 24, 6 }; // max ticks, randomize, per-mode limits
    bool toggles[TOGGLES] {};
    bool hasPos = false;
    int x = 0;
    int y = 0;
    int sizePercent = 100;
    int tab = TAB_LIVE;

    Values() {
        for (int i = 0; i < TOGGLES; ++i) toggles[i] = TOGGLE_DEFAULTS[i];
    }
};

struct Item {
    std::wstring time;
    std::wstring note; // set for plain notes ("Buffer on: ...")
    int tone = Plain;  // ... and how they're coloured
    Save save;
};

struct Attempt {
    int number = 0;  // 0 = outside a level
    int death = -1;  // level % it ended at
    int saves = 0;
    std::vector<Item> items; // oldest first
};

struct Event {
    enum Type { Note, Saved, NewAttempt, Death, Accuracy, CoachStats, Timing } type = Note;
    Item item;
    int number = 0;
    bool newLevel = false;
    Coach coach;
    float ms = 0.f;
};

constexpr size_t MAX_ATTEMPTS = 60;
constexpr size_t MAX_ITEMS = 80;

std::atomic<HWND> g_window = nullptr;
std::atomic<HWND> g_game = nullptr;
std::atomic<bool> g_started = false;
bool g_writing = false; // main thread: the window is saving settings

// Window thread only.
Values g_values;
std::deque<Attempt> g_attempts; // newest first
int g_levelSaves = 0;
double g_levelMs = 0.0;
int g_checks = 0; // self-check: predictions compared this session
int g_hits = 0;   // ... and how many were right
Coach g_coach;    // the coach's numbers for this level
std::vector<float> g_timing; // ... and each death it worked out: ms off (below 0 = early), oldest first
int g_deaths[101] {};        // deaths at each level % (this level)
int g_deathCount = 0;
int g_lastDeath = -1;
int g_tab = TAB_LIVE;
int g_logScroll = 0; // log rows scrolled away from the newest
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
    for (int i = 0; i < SLIDERS; ++i) v.sliders[i] = static_cast<int>(mod->getSettingValue<int64_t>(SLIDER_KEYS[i]));
    for (int i = 0; i < TOGGLES; ++i) v.toggles[i] = mod->getSettingValue<bool>(TOGGLE_KEYS[i]);
    v.hasPos = mod->getSavedValue<bool>("window-has-pos", false);
    v.x = static_cast<int>(mod->getSavedValue<int64_t>("window-x", 0));
    v.y = static_cast<int>(mod->getSavedValue<int64_t>("window-y", 0));
    v.sizePercent = static_cast<int>(mod->getSettingValue<int64_t>("window-size"));
    v.tab = std::clamp(static_cast<int>(mod->getSavedValue<int64_t>("window-tab", TAB_LIVE)), 0, TABS - 1);
    return v;
}

// Writes only what changed, so one click is one setting change.
void writeSettings(Values const& v) {
    auto mod = Mod::get();
    g_writing = true;
    auto setInt = [&](char const* key, int value) {
        if (mod->getSettingValue<int64_t>(key) != value) mod->setSettingValue<int64_t>(key, value);
    };
    for (int i = 0; i < SLIDERS; ++i) setInt(SLIDER_KEYS[i], v.sliders[i]);
    for (int i = 0; i < TOGGLES; ++i) {
        if (mod->getSettingValue<bool>(TOGGLE_KEYS[i]) != v.toggles[i]) mod->setSettingValue<bool>(TOGGLE_KEYS[i], v.toggles[i]);
    }
    g_writing = false;
}

void save() {
    Values copy = g_values;
    queueInMainThread([copy] { writeSettings(copy); });
}

void saveTab() {
    int tab = g_tab;
    queueInMainThread([tab] { Mod::get()->setSavedValue<int64_t>("window-tab", tab); });
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
// More drawing helpers
// ===========================================================================

// A line of a given colour across part of the window.
void hseg(gp::Graphics& g, float x0, float x1, float y, uint32_t c = C_LINE) {
    gp::Pen pen(col(c), 1.f);
    g.DrawLine(&pen, x0, y + 0.5f, x1, y + 0.5f);
}

// A bar with fully round ends.
void pill(gp::Graphics& g, RectF r, gp::Color c) {
    if (r.Width < r.Height || r.Height <= 0.f) return;
    gp::SolidBrush brush(c);
    gp::GraphicsPath path;
    float d = r.Height;
    path.AddArc(r.X, r.Y, d, d, 90.f, 180.f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.f, 180.f);
    path.CloseFigure();
    g.FillPath(&brush, &path);
}

float spacedWidth(gp::Graphics& g, std::wstring_view s, Face face, float size, float spacing = 1.5f) {
    float total = 0.f;
    for (size_t i = 0; i < s.size(); ++i) total += measure(g, s.substr(i, 1), face, size);
    return total + spacing * static_cast<float>(s.empty() ? 0 : s.size() - 1);
}

uint32_t toneColor(int tone) {
    return tone == Late ? C_LATE : tone == Early ? C_EARLY : tone == Bad ? C_DANGER : C_MUTED;
}

// ===========================================================================
// Geometry and hit testing
// ===========================================================================

constexpr int HIT_NONE = -1;
constexpr int HIT_CLOSE = 0;
constexpr int HIT_MINIMIZE = 1;
constexpr int HIT_TAB = 2;     // + tab
constexpr int HIT_TIMING = 5;
constexpr int HIT_DEATHS = 6;
constexpr int HIT_LOG = 7;
constexpr int HIT_TOGGLE = 10; // + toggle
constexpr int HIT_SLIDER = 40; // + slider

RectF closeRect() { return box(W - PAD - 28.f, 12.f, 28.f, 24.f); }
RectF minimizeRect() { return box(W - PAD - 58.f, 12.f, 28.f, 24.f); }
float tabX(int i) { return PAD + i * 68.f; } // where a tab's label starts
RectF tabRect(int i) { return box(tabX(i) - 8.f, TAB_Y, i == 0 ? 60.f : 104.f, TAB_H); }
bool isMode(int i) { return i >= MAIN_SLIDERS; }
RectF sliderRow(int i) {
    if (!isMode(i)) return box(PAD, SLIDER_Y + i * (SLIDER_H + 1.f), W - 2 * PAD, SLIDER_H);
    int k = i - MAIN_SLIDERS;
    float cw = (W - 2 * PAD - MODE_GAP) / 2.f;
    return box(PAD + (k % 2) * (cw + MODE_GAP), MODE_Y + MODE_HEAD + (k / 2) * MODE_CELL_H, cw, MODE_CELL_H);
}
float trackY(int i) { return sliderRow(i).Y + (isMode(i) ? 33.f : 58.f); }
float trackX0(int i) { return isMode(i) ? sliderRow(i).X + 6.f : TRACK_X0; }
float trackX1(int i) { return isMode(i) ? sliderRow(i).X + sliderRow(i).Width - 6.f : TRACK_X1; }
float toggleGroupY(int group) { return TOGGLE_Y + group * TOGGLE_GROUP_H; }
RectF toggleRow(int i) {
    int k = i % TOGGLE_GROUP;
    float cw = (W - 2 * PAD - MODE_GAP) / 2.f;
    return box(PAD + (k % 2) * (cw + MODE_GAP), toggleGroupY(i / TOGGLE_GROUP) + TOGGLE_HEAD + (k / 2) * TOGGLE_H, cw, TOGGLE_H);
}
RectF footerRect() { return box(PAD, H - 14.f - FOOTER_H, W - 2 * PAD, FOOTER_H); }
RectF timingRect() { return box(PAD, TIMING_Y, W - 2 * PAD, TIMING_H); }
RectF deathsRect() { return box(PAD, DEATHS_Y, W - 2 * PAD, DEATHS_H); }
RectF logRect() { return box(PAD, LOG_Y, W - 2 * PAD, footerRect().Y - 10.f - LOG_Y); }

float thumbX(int i, int v) { return trackX0(i) + (trackX1(i) - trackX0(i)) * (static_cast<float>(v) / MAX_TICKS); }

int valueAt(int i, float x) {
    float t = (x - trackX0(i)) / (trackX1(i) - trackX0(i));
    return std::clamp(static_cast<int>(std::lround(t * MAX_TICKS)), 0, MAX_TICKS);
}

bool within(int hit, int base, int count) { return hit >= base && hit < base + count; }

int hitTest(float x, float y) {
    if (closeRect().Contains(x, y)) return HIT_CLOSE;
    if (minimizeRect().Contains(x, y)) return HIT_MINIMIZE;
    for (int i = 0; i < TABS; ++i) {
        if (tabRect(i).Contains(x, y)) return HIT_TAB + i;
    }
    if (g_tab == TAB_LIVE) {
        if (timingRect().Contains(x, y)) return HIT_TIMING;
        if (deathsRect().Contains(x, y)) return HIT_DEATHS;
        if (logRect().Contains(x, y)) return HIT_LOG;
        return HIT_NONE;
    }
    for (int i = 0; i < TOGGLES; ++i) {
        if (toggleRow(i).Contains(x, y)) return HIT_TOGGLE + i;
    }
    for (int i = 0; i < SLIDERS; ++i) {
        if (sliderRow(i).Contains(x, y)) return HIT_SLIDER + i;
    }
    return HIT_NONE;
}

// Something you can click (gets the hand cursor).
bool clickable(int hit) {
    return hit == HIT_CLOSE || hit == HIT_MINIMIZE || within(hit, HIT_TAB, TABS) || within(hit, HIT_TOGGLE, TOGGLES)
        || within(hit, HIT_SLIDER, SLIDERS);
}

std::wstring help() {
    int h = g_dragging >= 0 ? HIT_SLIDER + g_dragging : g_hover;
    if (within(h, HIT_SLIDER, SLIDERS)) return isMode(h - HIT_SLIDER) ? MODE_HELP : SLIDER_HELP[h - HIT_SLIDER];
    if (within(h, HIT_TOGGLE, TOGGLES)) return TOGGLE_HELP[h - HIT_TOGGLE];
    if (h == HIT_TIMING) return TIMING_HELP;
    if (h == HIT_DEATHS) return DEATHS_HELP;
    return {};
}

// ===========================================================================
// Painting
// ===========================================================================

void paintTitle(gp::Graphics& g) {
    bool on = g_values.toggles[0] && g_values.sliders[0] > 0;
    spaced(g, L"INPUT BUFFER", Face::Semi, 13.f, col(C_TEXT), box(PAD, 0, 200, TITLE_H), 2.f);
    // state: a filled dot and the range when the buffer is on
    float dx = PAD + spacedWidth(g, L"INPUT BUFFER", Face::Semi, 13.f, 2.f) + 14.f;
    if (on) circle(g, dx, TITLE_H / 2.f, 3.5f, col(C_TEXT));
    else {
        gp::Pen pen(col(C_FAINT), 1.2f);
        g.DrawEllipse(&pen, dx - 3.5f, TITLE_H / 2.f - 3.5f, 7.f, 7.f);
    }
    int v = g_values.sliders[0];
    auto state = on ? fmtw(L"up to %d tick%ls", v, plural(v)) : std::wstring(L"off");
    text(g, state, Face::Ui, 12.f, col(on ? C_MUTED : C_FAINT), box(dx + 12.f, 0, 140.f, TITLE_H));

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

void paintTabs(gp::Graphics& g) {
    for (int i = 0; i < TABS; ++i) {
        bool active = g_tab == i;
        bool hover = g_hover == HIT_TAB + i;
        float x = tabX(i);
        spaced(g, TAB_NAMES[i], active ? Face::Semi : Face::Ui, 11.f, col(active || hover ? C_TEXT : C_FAINT),
            box(x, TAB_Y, 120.f, TAB_H), 1.6f);
        if (active) {
            float w = spacedWidth(g, TAB_NAMES[i], Face::Semi, 11.f, 1.6f);
            fill(g, box(x, TAB_Y + TAB_H - 1.f, w, 2.f), col(C_TEXT));
        }
    }
    hline(g, TAB_Y + TAB_H);
}

// Saves this attempt, this level, how early they were on average, and how right the ghost is.
void paintStats(gp::Graphics& g) {
    int attemptSaves = 0;
    int attemptNumber = 0;
    if (!g_attempts.empty()) {
        attemptSaves = g_attempts.front().saves;
        attemptNumber = g_attempts.front().number;
    }
    std::wstring values[4] {
        std::to_wstring(attemptSaves),
        std::to_wstring(g_levelSaves),
        g_levelSaves > 0 ? fmtw(L"%.1f", g_levelMs / g_levelSaves) : std::wstring(L"–"),
        g_checks > 0 ? fmtw(L"%d%%", static_cast<int>(std::lround(100.0 * g_hits / g_checks))) : std::wstring(L"–"),
    };
    std::wstring labels[4] {
        attemptNumber > 0 ? fmtw(L"ATTEMPT %d", attemptNumber) : std::wstring(L"ATTEMPT"),
        L"THIS LEVEL",
        L"AVG MS EARLY",
        g_checks > 0 ? fmtw(L"ACCURACY · %d", g_checks) : std::wstring(L"ACCURACY"),
    };
    float colW = (W - 2 * PAD) / 4.f;
    for (int i = 0; i < 4; ++i) {
        float x = PAD + i * colW;
        text(g, values[i], Face::Light, 22.f, col(C_TEXT), box(x, STATS_Y + 6.f, colW, 28.f));
        spaced(g, labels[i], Face::Ui, 9.5f, col(C_FAINT), box(x, STATS_Y + 34.f, colW, 18.f), 1.3f);
    }
    hline(g, STATS_Y + STATS_H);
}

// The coach: your last death in big, and every death it worked out on a bar
// from "too early" (left) to "too late" (right).
void paintTiming(gp::Graphics& g) {
    float y = TIMING_Y;
    bool hover = g_hover == HIT_TIMING;
    spaced(g, L"TIMING", Face::Ui, 11.f, col(hover ? C_TEXT : C_MUTED), box(PAD, y + 8.f, 120.f, 18.f));

    // averages, top right
    auto const& c = g_coach;
    if (c.early > 0 || c.late > 0) {
        auto early = c.early > 0 ? fmtw(L"%d× early · %.0f ms", c.early, c.earlyMs / c.early) : std::wstring();
        auto late = c.late > 0 ? fmtw(L"%d× late · %.0f ms", c.late, c.lateMs / c.late) : std::wstring();
        std::wstring gap = (c.early > 0 && c.late > 0) ? L"      " : L"";
        runs(g, { { early, Face::Ui, 11.5f, col(C_EARLY) }, { gap, Face::Ui, 11.5f, col(C_FAINT) }, { late, Face::Ui, 11.5f, col(C_LATE) } },
            box(PAD + 120.f, y + 8.f, W - 2 * PAD - 120.f, 18.f), RIGHT);
    }

    // the last one, big
    RectF hero = box(PAD, y + 30.f, W - 2 * PAD, 40.f);
    if (g_timing.empty()) {
        text(g, L"–", Face::Light, 28.f, col(C_FAINT), box(hero.X, hero.Y, 30.f, hero.Height));
        text(g, L"after a death: how early or late your click was", Face::Ui, 12.f, col(C_FAINT),
            box(hero.X + 30.f, hero.Y + 2.f, hero.Width - 30.f, hero.Height));
    }
    else {
        float ms = g_timing.back();
        bool late = ms >= 0.f;
        auto big = fmtw(L"%.0f", std::abs(ms));
        auto unit = late ? std::wstring(L" ms late") : std::wstring(L" ms early");
        auto ticks = fmtw(L"   %.0f tick%ls", std::max(1.f, std::round(std::abs(ms) * 0.24f)),
            std::round(std::abs(ms) * 0.24f) > 1.f ? L"s" : L"");
        uint32_t color = late ? C_LATE : C_EARLY;
        runs(g, { { big, Face::Light, 30.f, col(color) }, { unit, Face::Ui, 14.f, col(color) }, { ticks, Face::Ui, 12.f, col(C_FAINT) } }, hero);
        text(g, L"LAST DEATH", Face::Ui, 9.5f, col(C_FAINT), hero, RIGHT);
    }

    // the bar
    float x0 = PAD + 4.f;
    float x1 = W - PAD - 4.f;
    float cx = (x0 + x1) / 2.f;
    float by = y + 90.f;
    fill(g, box(x0, by - 1.f, x1 - x0, 2.f), col(C_TRACK));
    for (int t = -3; t <= 3; ++t) {
        if (t == 0) continue;
        float x = cx + (x1 - cx) * (t * 20.f / TIMING_RANGE);
        line(g, x, by - 3.f, x, by + 3.f, col(C_TRACK), 1.f);
    }
    size_t n = g_timing.size();
    for (size_t i = 0; i < n; ++i) {
        float ms = g_timing[i];
        float x = cx + (x1 - cx) * std::clamp(ms / TIMING_RANGE, -1.f, 1.f);
        bool newest = i + 1 == n;
        int alpha = newest ? 255 : 60 + static_cast<int>(150.f * static_cast<float>(i + 1) / static_cast<float>(n));
        float half = newest ? 11.f : 7.f;
        line(g, x, by - half, x, by + half, col(ms >= 0.f ? C_LATE : C_EARLY, alpha), newest ? 2.5f : 2.f);
    }
    line(g, cx, by - 10.f, cx, by + 10.f, col(C_TEXT, 200), 1.f); // on time
    RectF labels = box(x0, by + 12.f, x1 - x0, 16.f);
    spaced(g, L"EARLY", Face::Ui, 9.5f, col(C_EARLY, 190), labels, 1.3f);
    spaced(g, L"LATE", Face::Ui, 9.5f, col(C_LATE, 190), labels, 1.3f, RIGHT);
    text(g, fmtw(L"%.0f ms", TIMING_RANGE / 2.f), Face::Ui, 9.5f, col(C_FAINT), box(cx + (x1 - cx) / 2.f - 30.f, labels.Y, 60.f, 16.f), CENTER);
    text(g, fmtw(L"%.0f ms", TIMING_RANGE / 2.f), Face::Ui, 9.5f, col(C_FAINT), box(cx - (x1 - cx) / 2.f - 30.f, labels.Y, 60.f, 16.f), CENTER);
    hline(g, TIMING_Y + TIMING_H);
}

// Where in the level you die: one mark per level %, taller the more often.
void paintDeaths(gp::Graphics& g) {
    float y = DEATHS_Y;
    bool hover = g_hover == HIT_DEATHS;
    spaced(g, L"DEATHS", Face::Ui, 11.f, col(hover ? C_TEXT : C_MUTED), box(PAD, y + 8.f, 120.f, 18.f));
    int worst = -1;
    for (int p = 0; p <= 100; ++p) {
        if (g_deaths[p] > 0 && (worst < 0 || g_deaths[p] >= g_deaths[worst])) worst = p;
    }
    if (g_deathCount > 0) {
        auto total = fmtw(L"%d this level", g_deathCount);
        auto most = worst >= 0 && g_deaths[worst] >= 2 ? fmtw(L"      most at %d%% · %d×", worst, g_deaths[worst]) : std::wstring();
        runs(g, { { total, Face::Ui, 11.5f, col(C_MUTED) }, { most, Face::Ui, 11.5f, col(C_TEXT) } },
            box(PAD + 120.f, y + 8.f, W - 2 * PAD - 120.f, 18.f), RIGHT);
    }
    float x0 = PAD + 4.f;
    float x1 = W - PAD - 4.f;
    float sy = y + 50.f;
    for (int q = 0; q <= 4; ++q) {
        float x = x0 + (x1 - x0) * q / 4.f;
        line(g, x, sy, x, sy + 4.f, col(C_TRACK), 1.f);
    }
    fill(g, box(x0, sy - 1.f, x1 - x0, 2.f), col(C_TRACK));
    for (int p = 0; p <= 100; ++p) {
        int count = g_deaths[p];
        if (count <= 0 || p == g_lastDeath) continue;
        float x = x0 + (x1 - x0) * p / 100.f;
        float h = 5.f + 3.f * std::min(count, 5);
        line(g, x, sy - h, x, sy - 1.f, col(C_TEXT, std::min(255, 110 + 35 * count)), 2.f);
    }
    if (g_lastDeath >= 0 && g_lastDeath <= 100 && g_deaths[g_lastDeath] > 0) {
        float x = x0 + (x1 - x0) * g_lastDeath / 100.f;
        float h = 5.f + 3.f * std::min(g_deaths[g_lastDeath], 5);
        line(g, x, sy - h, x, sy - 1.f, col(C_DANGER), 2.f);
    }
    RectF labels = box(x0, sy + 5.f, x1 - x0, 14.f);
    text(g, L"0%", Face::Ui, 9.5f, col(C_FAINT), labels);
    text(g, L"50%", Face::Ui, 9.5f, col(C_FAINT), labels, CENTER);
    text(g, L"100%", Face::Ui, 9.5f, col(C_FAINT), labels, RIGHT);
    hline(g, DEATHS_Y + DEATHS_H);
}

void paintModeLimits(gp::Graphics& g) {
    spaced(g, L"PER MODE", Face::Ui, 11.f, col(C_MUTED), box(PAD, MODE_Y + 8.f, W - 2 * PAD, 18.f));
    for (int i = MAIN_SLIDERS; i < SLIDERS; ++i) {
        RectF r = sliderRow(i);
        bool active = g_hover == HIT_SLIDER + i || g_dragging == i;
        if (active) fill(g, box(r.X - 6.f, r.Y + 1.f, r.Width + 12.f, r.Height - 2.f), col(C_HOVER));
        int v = g_values.sliders[i];
        bool capped = v > g_values.sliders[0];  // BUFFER is lower, so it wins
        RectF head = box(r.X, r.Y + 6.f, r.Width, 18.f);
        spaced(g, SLIDER_NAMES[i], Face::Ui, 10.f, col(active ? C_TEXT : C_MUTED), head);
        auto value = v > 0 ? std::to_wstring(v) : std::wstring(L"off");
        text(g, value, Face::Semi, 12.f, col(capped ? C_FAINT : C_TEXT), head, RIGHT);
        float y = trackY(i);
        float x0 = trackX0(i);
        float x1 = trackX1(i);
        float hx = thumbX(i, v);
        fill(g, box(x0, y - 1.f, x1 - x0, 2.f), col(C_TRACK));
        if (hx > x0) fill(g, box(x0, y - 1.f, hx - x0, 2.f), col(capped ? C_FAINT : C_TEXT));
        circle(g, hx, y, 5.f, col(active ? 0xffffff : capped ? C_MUTED : C_TEXT));
    }
    hline(g, TOGGLE_Y - 4.f);
}

void paintSliders(gp::Graphics& g) {
    for (int i = 0; i < MAIN_SLIDERS; ++i) {
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
            auto unit = fmtw(L" tick%ls  ·  %.1f ms", plural(v), v * 1000.0 / 240.0);
            runs(g, { { big, Face::Light, 20.f, col(C_TEXT) }, { unit, Face::Ui, 12.f, col(C_MUTED) } }, head, RIGHT);
        }
        else {
            auto big = fmtw(L"+%d", v);
            auto unit = fmtw(L" tick%ls", plural(v));
            runs(g, { { big, Face::Light, 20.f, col(C_TEXT) }, { unit, Face::Ui, 12.f, col(C_MUTED) } }, head, RIGHT);
        }

        float y = trackY(i);
        float hx = thumbX(i, v);
        fill(g, box(TRACK_X0, y - 1.f, TRACK_X1 - TRACK_X0, 2.f), col(C_TRACK));
        if (hx > TRACK_X0) fill(g, box(TRACK_X0, y - 1.f, hx - TRACK_X0, 2.f), col(C_TEXT));
        circle(g, hx, y, 7.f, col(active ? 0xffffff : C_TEXT));
        hline(g, row.Y + row.Height);
    }
}

// Two groups of switches, each a grid of two columns.
void paintToggles(gp::Graphics& g) {
    for (int group = 0; group < TOGGLE_GROUPS; ++group) {
        spaced(g, GROUP_NAMES[group], Face::Ui, 11.f, col(C_MUTED), box(PAD, toggleGroupY(group) + 6.f, W - 2 * PAD, 18.f));
    }
    for (int i = 0; i < TOGGLES; ++i) {
        RectF r = toggleRow(i);
        bool on = g_values.toggles[i];
        bool hover = g_hover == HIT_TOGGLE + i;
        if (hover) fill(g, box(r.X - 6.f, r.Y + 1.f, r.Width + 12.f, r.Height - 2.f), col(C_HOVER));
        text(g, TOGGLE_NAMES[i], Face::Ui, 13.f, col(on || hover ? C_TEXT : C_MUTED), box(r.X, r.Y, r.Width - 40.f, r.Height));
        // a switch: white with the knob on the right when on
        RectF sw = box(r.X + r.Width - 30.f, r.Y + (r.Height - 16.f) / 2.f, 30.f, 16.f);
        pill(g, sw, col(on ? C_TEXT : C_TRACK));
        circle(g, on ? sw.X + sw.Width - 8.f : sw.X + 8.f, sw.Y + 8.f, 5.f, col(on ? C_BG : C_MUTED));
        bool lastRow = (i % TOGGLE_GROUP) / 2 == TOGGLE_GROUP / 2 - 1;
        hseg(g, r.X, r.X + r.Width, r.Y + r.Height, lastRow ? C_LINE : C_DIVIDER);
    }
}

void paintItem(gp::Graphics& g, Item const& it, int age, RectF r) {
    int alpha = ageAlpha(age);
    // a mark in the line's colour: white = held back, blue / orange = the coach, red = bad news
    bool saved = it.note.empty();
    if (saved || it.tone != Plain) {
        fill(g, box(r.X, r.Y + 4.f, 2.f, r.Height - 8.f), col(saved ? C_TEXT : toneColor(it.tone), alpha));
    }
    float x = r.X + 9.f;
    text(g, it.time, Face::Mono, 12.f, col(C_FAINT, alpha), box(x, r.Y, 60.f, r.Height));
    x += 62.f;
    if (!saved) {
        text(g, it.note, Face::Mono, 12.f, col(toneColor(it.tone), alpha), box(x, r.Y, r.X + r.Width - x, r.Height));
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

// Rows the log has in all (an attempt's header and each of its lines).
int logRows() {
    int rows = 0;
    for (size_t ai = 0; ai < g_attempts.size(); ++ai) {
        auto const& a = g_attempts[ai];
        if (a.number > 0 && ai > 0) rows++;
        rows += static_cast<int>(a.items.size());
    }
    return rows;
}

// Newest attempt first, its newest line first. The mouse wheel scrolls it.
void paintLog(gp::Graphics& g) {
    RectF area = logRect();
    constexpr float LH = 18.f;
    bool current = !g_attempts.empty() && g_attempts.front().number > 0;
    std::wstring title = current ? fmtw(L"LOG · ATTEMPT %d", g_attempts.front().number) : std::wstring(L"LOG");
    spaced(g, title, Face::Ui, 11.f, col(C_MUTED), box(area.X, area.Y, area.Width, 20.f));
    // how often the ghost's "you die / you're fine" came true
    if (g_coach.calls > 0) {
        text(g, fmtw(L"ghost right %d of %d", g_coach.callHits, g_coach.calls), Face::Ui, 11.f, col(C_FAINT),
            box(area.X, area.Y, area.Width, 20.f), RIGHT);
    }

    float top = area.Y + 26.f;
    float bottom = area.Y + area.Height;
    int total = logRows();
    int fits = std::max(1, static_cast<int>((bottom - top) / LH));
    g_logScroll = std::clamp(g_logScroll, 0, std::max(0, total - fits / 2));

    float y = top;
    int row = 0; // rows passed so far
    bool any = false;
    bool full = false;
    for (size_t ai = 0; ai < g_attempts.size() && !full; ++ai) {
        auto const& a = g_attempts[ai];
        int age = static_cast<int>(ai);
        if (a.number > 0 && ai > 0) {
            if (row++ >= g_logScroll) {
                float gap = any ? 6.f : 0.f;
                if (y + gap + LH > bottom) break;
                y += gap;
                RectF hr = box(area.X, y, area.Width - 8.f, LH);
                spaced(g, fmtw(L"ATTEMPT %d", a.number), Face::Ui, 11.f, col(C_FAINT), hr);
                if (a.death >= 0) text(g, fmtw(L"died at %d%%", a.death), Face::Ui, 11.f, col(C_DANGER, 200), hr, RIGHT);
                y += LH;
                any = true;
            }
        }
        for (auto it = a.items.rbegin(); it != a.items.rend(); ++it) {
            if (row++ < g_logScroll) continue;
            if (y + LH > bottom) {
                full = true;
                break;
            }
            paintItem(g, *it, g_logScroll > 0 ? 0 : age, box(area.X, y, area.Width - 8.f, LH));
            y += LH;
            any = true;
        }
    }
    if (total == 0) {
        text(g, L"Nothing yet. Held-back clicks and what the coach finds show up here.", Face::Ui, 12.f, col(C_FAINT),
            box(area.X, top, area.Width, LH));
    }
    // a scroll bar once there's more than fits
    if (total > fits) {
        float trackH = bottom - top;
        float thumbH = std::max(24.f, trackH * fits / static_cast<float>(total));
        float t = static_cast<float>(g_logScroll) / static_cast<float>(std::max(1, total - fits / 2));
        float ty = top + (trackH - thumbH) * std::clamp(t, 0.f, 1.f);
        fill(g, box(area.X + area.Width - 2.f, top, 2.f, trackH), col(C_DIVIDER));
        fill(g, box(area.X + area.Width - 2.f, ty, 2.f, thumbH), col(g_logScroll > 0 || g_hover == HIT_LOG ? C_MUTED : C_TRACK));
    }
}

void paintFooter(gp::Graphics& g) {
    RectF r = footerRect();
    hline(g, r.Y - 9.f);
    auto h = help();
    gp::SolidBrush brush(col(h.empty() ? C_FAINT : C_TEXT));
    std::wstring_view s = h.empty() ? std::wstring_view(g_tab == TAB_LIVE ? LIVE_HELP : SETTINGS_HELP) : std::wstring_view(h);
    g.DrawString(s.data(), static_cast<INT>(s.size()), font(Face::Ui, h.empty() ? 11.5f : 12.f), r, g_wrap, &brush);
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
    paintTabs(g);
    if (g_tab == TAB_LIVE) {
        paintStats(g);
        paintTiming(g);
        paintDeaths(g);
        paintLog(g);
    }
    else {
        paintSliders(g);
        paintModeLimits(g);
        paintToggles(g);
    }
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
    if (g_logScroll > 0) g_logScroll++; // keep what you scrolled to in place
}

void handleEvent(Event& e) {
    switch (e.type) {
        case Event::NewAttempt:
            if (e.newLevel) {
                g_levelSaves = 0;
                g_levelMs = 0.0;
                g_coach = {};
                g_timing.clear();
                for (auto& d : g_deaths) d = 0;
                g_deathCount = 0;
                g_lastDeath = -1;
                g_logScroll = 0;
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
                if (g_logScroll > 0) g_logScroll++;
            }
            break;
        case Event::Death:
            if (!g_attempts.empty() && g_attempts.front().number > 0 && g_attempts.front().death < 0) {
                g_attempts.front().death = e.number;
            }
            if (e.number >= 0 && e.number <= 100) {
                g_deaths[e.number]++;
                g_deathCount++;
                g_lastDeath = e.number;
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
        case Event::Accuracy:
            g_checks = e.number;
            g_hits = static_cast<int>(e.item.save.ticks);
            break;
        case Event::CoachStats:
            g_coach = e.coach;
            break;
        case Event::Timing:
            g_timing.push_back(e.ms);
            if (g_timing.size() > TIMING_MAX) g_timing.erase(g_timing.begin());
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
        // keep the bottom on its screen
        int y = v.y;
        POINT corner { v.x, v.y };
        MONITORINFO info {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(MonitorFromPoint(corner, MONITOR_DEFAULTTONEAREST), &info) && y + h > info.rcWork.bottom) {
            y = std::max<int>(info.rcWork.top, info.rcWork.bottom - h);
        }
        SetWindowPos(wnd, nullptr, v.x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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
    if (!g_placed) g_tab = std::clamp(g_values.tab, 0, TABS - 1); // the tab you left it on
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
            // any time it's open (even minimized or behind the game) the key hides it completely,
            // taskbar button included; pressing again brings it back on top
            if (IsWindowVisible(wnd)) {
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
            if (LOWORD(lParam) == HTCLIENT && clickable(g_hover)) {
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
                setSlider(wnd, g_dragging, valueAt(g_dragging, x));
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
            else if (within(hit, HIT_TAB, TABS)) {
                if (g_tab != hit - HIT_TAB) {
                    g_tab = hit - HIT_TAB;
                    saveTab();
                    InvalidateRect(wnd, nullptr, FALSE);
                }
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
                float reach = isMode(i) ? 12.f : 16.f;
                if (std::abs(y - trackY(i)) <= reach && x >= trackX0(i) - 10.f && x <= trackX1(i) + 10.f) {
                    g_dragging = i;
                    SetCapture(wnd);
                    setSlider(wnd, i, valueAt(i, x));
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
            if (hit == HIT_LOG) {
                // wheel up = towards the newest line
                g_logScroll = std::max(0, g_logScroll - steps * 3);
                InvalidateRect(wnd, nullptr, FALSE);
            }
            else if (within(hit, HIT_SLIDER, SLIDERS)) {
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
                    if (g_tab != TAB_SETTINGS) return 0;
                    setSlider(wnd, g_focus, g_values.sliders[g_focus] - 1);
                    save();
                    return 0;
                case VK_RIGHT:
                case VK_UP:
                    if (g_tab != TAB_SETTINGS) return 0;
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

    // Hidden from screen capture, same as v1.x (WDA_EXCLUDEFROMCAPTURE, spelled
    // out for older SDK headers). Windows older than 10 (2004) shows a black box instead.
    constexpr DWORD EXCLUDE_FROM_CAPTURE = 0x00000011;
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

void note(std::string const& line, int tone) {
    if (!g_window.load()) return;
    auto e = new Event;
    e->type = Event::Note;
    e->item.time = timestamp();
    e->item.note = widen(line);
    e->item.tone = tone;
    post(e);
}

void coach(Coach const& stats) {
    if (!g_window.load()) return;
    auto e = new Event;
    e->type = Event::CoachStats;
    e->coach = stats;
    post(e);
}

void timing(double ms) {
    if (!g_window.load()) return;
    auto e = new Event;
    e->type = Event::Timing;
    e->ms = static_cast<float>(ms);
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

void accuracy(int checks, int hits) {
    if (!g_window.load()) return;
    auto e = new Event;
    e->type = Event::Accuracy;
    e->number = checks;
    e->item.save.ticks = hits;
    post(e);
}

} // namespace pb::window

#else

namespace pb::window {

void toggle() { openSettingsPopup(Mod::get(), false); }
void open() {}
void prepare() {}
void sync() {}
void note(std::string const&, int) {}
void coach(Coach const&) {}
void timing(double) {}
void saved(Save const&) {}
void attempt(int, bool) {}
void death(int) {}
void accuracy(int, int) {}

} // namespace pb::window

#endif
