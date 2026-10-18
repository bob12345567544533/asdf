#include "Menu.hpp"
#include "Overlay.hpp"

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
#include <ctime>
#include <deque>
#include <string>
#include <thread>

// gdiplus.h wants min/max, which NOMINMAX removes.
namespace Gdiplus {
    using std::max;
    using std::min;
}
#include <gdiplus.h>

namespace pb::menu {
namespace {

namespace gp = Gdiplus;

// ---------------------------------------------------------------------------
// Look (colors taken from the GD Clean texture pack)
// ---------------------------------------------------------------------------

const gp::Color COL_BG(255, 0x1b, 0x1b, 0x1b);
const gp::Color COL_CARD(255, 0x22, 0x22, 0x22);
const gp::Color COL_CARD_HOVER(255, 0x2a, 0x2a, 0x2a);
const gp::Color COL_BORDER(255, 0x00, 0x00, 0x00);
const gp::Color COL_TEXT(255, 0xf0, 0xf0, 0xf0);
const gp::Color COL_MUTED(255, 0x9a, 0x9a, 0x9a);
const gp::Color COL_TRACK(255, 0x3a, 0x3a, 0x3a);
const gp::Color COL_ACCENT(255, 0x7a, 0xde, 0x2d);
const gp::Color COL_CLOSE_HOVER(255, 0xc4, 0x2b, 0x1c);

constexpr int MAX_TICKS = 24;
constexpr int TOGGLE_COUNT = 4;

// Layout in logical pixels (scaled to the monitor when drawn). One column:
// slider, toggles, log.
constexpr float TITLE_H = 40.f;
constexpr float PAD = 12.f;
constexpr float GAP = 8.f;
constexpr float COL_W = 330.f;
constexpr float W = PAD + COL_W + PAD;
constexpr float LEFT_X = PAD;
constexpr float RIGHT_X = PAD;

enum class Unit { Ticks, Percent, Pixels, Slower };

constexpr int SLIDER_COUNT = 1;
constexpr int SLIDER_MAX[SLIDER_COUNT] { MAX_TICKS };
constexpr Unit SLIDER_UNIT[SLIDER_COUNT] { Unit::Ticks };
constexpr char const* SLIDER_KEYS[SLIDER_COUNT] { "buffer-ticks" };
constexpr wchar_t const* SLIDER_DESCS[SLIDER_COUNT] {
    L"Holds clicks that are a little too early and would kill you, then fires them on the step where they survive.",
};
constexpr wchar_t const* SLIDER_NAMES[SLIDER_COUNT] { L"Buffer" };

constexpr float SROW_H = 50.f;
constexpr float SLIDER_CARD_Y = TITLE_H + GAP;
constexpr float SLIDER_CARD_H = 10.f + SROW_H * SLIDER_COUNT;
constexpr float SLIDER_X0 = LEFT_X + 16.f;
constexpr float SLIDER_X1 = LEFT_X + COL_W - 16.f;
constexpr float sliderRowY(int i) { return SLIDER_CARD_Y + 5.f + i * SROW_H; }
constexpr float sliderY(int i) { return sliderRowY(i) + 35.f; }

constexpr float ROW_H = 38.f;
constexpr float TOGGLE_CARD_Y = SLIDER_CARD_Y + SLIDER_CARD_H + GAP;
constexpr float TOGGLE_CARD_H = ROW_H * TOGGLE_COUNT + 8.f;

constexpr float LOG_LINE_H = 18.f;
constexpr float LOG_CARD_Y = TOGGLE_CARD_Y + TOGGLE_CARD_H + GAP;
constexpr size_t LOG_LINES = 8;
constexpr float LOG_CARD_H = 42.f + LOG_LINE_H * LOG_LINES;

constexpr float FOOTER_H = 52.f;
constexpr float H = LOG_CARD_Y + LOG_CARD_H + FOOTER_H;

constexpr UINT WM_PB_SHOW = WM_APP + 1;
constexpr UINT WM_PB_HIDE = WM_APP + 2;
constexpr UINT WM_PB_LOG = WM_APP + 3;
constexpr UINT WM_PB_OPEN = WM_APP + 4;
constexpr UINT WM_PB_SYNC = WM_APP + 5;

// WDA_EXCLUDEFROMCAPTURE, spelled out for older SDK headers.
constexpr DWORD EXCLUDE_FROM_CAPTURE = 0x00000011;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Values {
    int sliders[SLIDER_COUNT] { 2 };
    bool toggles[TOGGLE_COUNT] { true, true, false, true };
    bool hasPos = false;
    int x = 0;
    int y = 0;
    int sizePercent = 100;
};

constexpr char const* TOGGLE_KEYS[TOGGLE_COUNT] {
    "enabled", "buffer-releases", "buffer-ship", "show-feedback"
};
constexpr wchar_t const* TOGGLE_DESCS[TOGGLE_COUNT] {
    L"Turns the buffer on or off. Same as the toggle key (F6).",
    L"Also buffers early releases for wave, ship and robot, where letting go too early kills you.",
    L"Lets the buffer work in ship mode. Off by default since ship predictions are less reliable.",
    L"Shows in-game how early each buffered click was.",
};
constexpr wchar_t const* TOGGLE_NAMES[TOGGLE_COUNT] {
    L"Enabled", L"Releases (wave, ship, robot)", L"Ship", L"Timing popup"
};

std::atomic<HWND> g_window = nullptr;
std::atomic<HWND> g_game = nullptr;
std::atomic<bool> g_started = false;

// Only touched on the window thread.
Values g_values;
std::deque<std::wstring> g_log;
int g_hover = -1; // see hitTest()
int g_dragging = -1;    // slider being dragged, -1 if none
int g_focusSlider = 0;  // slider the arrow keys move
bool g_placed = false;
float g_scale = 1.f;

// Settings are only read and written on the main thread.
Values readSettings() {
    auto mod = Mod::get();
    Values v;
    for (int i = 0; i < SLIDER_COUNT; ++i) v.sliders[i] = static_cast<int>(mod->getSettingValue<int64_t>(SLIDER_KEYS[i]));
    for (int i = 0; i < TOGGLE_COUNT; ++i) v.toggles[i] = mod->getSettingValue<bool>(TOGGLE_KEYS[i]);
    v.hasPos = mod->getSavedValue<bool>("window-has-pos", false);
    v.x = static_cast<int>(mod->getSavedValue<int64_t>("window-x", 0));
    v.y = static_cast<int>(mod->getSavedValue<int64_t>("window-y", 0));
    v.sizePercent = static_cast<int>(mod->getSettingValue<int64_t>("window-size"));
    return v;
}

void writeSettings(Values const& v) {
    auto mod = Mod::get();
    for (int i = 0; i < SLIDER_COUNT; ++i) mod->setSettingValue<int64_t>(SLIDER_KEYS[i], v.sliders[i]);
    for (int i = 0; i < TOGGLE_COUNT; ++i) mod->setSettingValue<bool>(TOGGLE_KEYS[i], v.toggles[i]);
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

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

gp::RectF closeRect() { return { W - 40.f, 8.f, 28.f, 24.f }; }
gp::RectF minimizeRect() { return { W - 72.f, 8.f, 28.f, 24.f }; }
gp::RectF sliderHitRect(int i) { return { LEFT_X, sliderRowY(i), COL_W, SROW_H }; }
gp::RectF toggleRowRect(int i) { return { RIGHT_X, TOGGLE_CARD_Y + 4.f + i * ROW_H, COL_W, ROW_H }; }

std::wstring valueLabel(int slider, int value) {
    if (value == 0) return L"off";
    wchar_t buf[48];
    switch (SLIDER_UNIT[slider]) {
        case Unit::Ticks:
            std::swprintf(buf, 48, L"%d tick%ls \u00b7 %.1f ms", value, value == 1 ? L"" : L"s", value * 1000.0 / 240.0);
            break;
        case Unit::Percent: std::swprintf(buf, 48, L"%d%%", value); break;
        case Unit::Pixels: std::swprintf(buf, 48, L"%d px", value); break;
        case Unit::Slower: std::swprintf(buf, 48, L"%d%% slower", value); break;
    }
    return buf;
}

float thumbX(int slider, int value) {
    return SLIDER_X0 + (SLIDER_X1 - SLIDER_X0) * (static_cast<float>(value) / SLIDER_MAX[slider]);
}

int valueAt(int slider, float x) {
    float t = (x - SLIDER_X0) / (SLIDER_X1 - SLIDER_X0);
    int max = SLIDER_MAX[slider];
    return std::clamp(static_cast<int>(std::lround(t * max)), 0, max);
}

// Hit targets: 0 = close, 2..6 = toggles, 7 = minimize, 10+ = sliders.
constexpr int HIT_CLOSE = 0;
constexpr int HIT_TOGGLE = 2;
constexpr int HIT_MINIMIZE = 7;
constexpr int HIT_SLIDER = 10;

bool isSliderHit(int hit) { return hit >= HIT_SLIDER && hit < HIT_SLIDER + SLIDER_COUNT; }

int hitTest(float x, float y) {
    if (closeRect().Contains(x, y)) return HIT_CLOSE;
    if (minimizeRect().Contains(x, y)) return HIT_MINIMIZE;
    for (int i = 0; i < SLIDER_COUNT; ++i) {
        if (sliderHitRect(i).Contains(x, y)) return HIT_SLIDER + i;
    }
    for (int i = 0; i < TOGGLE_COUNT; ++i) {
        if (toggleRowRect(i).Contains(x, y)) return HIT_TOGGLE + i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void roundRect(gp::GraphicsPath& path, gp::RectF r, float radius) {
    float d = std::min(radius * 2.f, std::min(r.Width, r.Height));
    path.AddArc(r.X, r.Y, d, d, 180.f, 90.f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.f, 90.f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.f, 90.f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.f, 90.f);
    path.CloseFigure();
}

void fillRound(gp::Graphics& g, gp::Color color, gp::RectF r, float radius) {
    gp::GraphicsPath path;
    roundRect(path, r, radius);
    gp::SolidBrush brush(color);
    g.FillPath(&brush, &path);
}

void strokeRound(gp::Graphics& g, gp::Color color, gp::RectF r, float radius) {
    gp::GraphicsPath path;
    roundRect(path, r, radius);
    gp::Pen pen(color, 1.f);
    g.DrawPath(&pen, &path);
}

void card(gp::Graphics& g, gp::RectF r, bool hover = false) {
    fillRound(g, hover ? COL_CARD_HOVER : COL_CARD, r, 6.f);
    strokeRound(g, COL_BORDER, r, 6.f);
}

void text(gp::Graphics& g, std::wstring const& s, float size, gp::Color color, gp::RectF r,
          gp::StringAlignment align = gp::StringAlignmentNear, bool bold = false,
          wchar_t const* family = L"Segoe UI") {
    gp::FontFamily fam(family);
    gp::Font font(&fam, size, bold ? gp::FontStyleBold : gp::FontStyleRegular, gp::UnitPixel);
    gp::SolidBrush brush(color);
    gp::StringFormat fmt;
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(gp::StringAlignmentCenter);
    fmt.SetFormatFlags(gp::StringFormatFlagsNoWrap);
    fmt.SetTrimming(gp::StringTrimmingEllipsisCharacter);
    g.DrawString(s.c_str(), -1, &font, r, &fmt, &brush);
}

void wrappedText(gp::Graphics& g, std::wstring const& s, float size, gp::Color color, gp::RectF r) {
    gp::FontFamily fam(L"Segoe UI");
    gp::Font font(&fam, size, gp::FontStyleRegular, gp::UnitPixel);
    gp::SolidBrush brush(color);
    gp::StringFormat fmt;
    fmt.SetAlignment(gp::StringAlignmentCenter);
    fmt.SetLineAlignment(gp::StringAlignmentCenter);
    fmt.SetTrimming(gp::StringTrimmingEllipsisWord);
    g.DrawString(s.c_str(), -1, &font, r, &fmt, &brush);
}

void drawToggle(gp::Graphics& g, float x, float y, bool on) {
    fillRound(g, on ? COL_ACCENT : COL_TRACK, { x, y, 38.f, 20.f }, 10.f);
    float knobX = on ? x + 38.f - 17.f : x + 3.f;
    gp::SolidBrush knob(on ? gp::Color(255, 0xff, 0xff, 0xff) : COL_MUTED);
    g.FillEllipse(&knob, knobX, y + 3.f, 14.f, 14.f);
}

void paint(HWND wnd, HDC target) {
    RECT client;
    GetClientRect(wnd, &client);
    int pw = client.right - client.left;
    int ph = client.bottom - client.top;

    // Draw into a back buffer so nothing flickers.
    gp::Bitmap buffer(pw, ph, PixelFormat32bppARGB);
    gp::Graphics g(&buffer);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
    g.ScaleTransform(g_scale, g_scale);

    g.Clear(COL_BG);
    gp::Pen outline(COL_BORDER, 1.f);
    g.DrawRectangle(&outline, 0.f, 0.f, W - 1.f, H - 1.f);

    // Title bar: name, minimize, close
    text(g, L"Input Buffer Lite", 15.f, COL_TEXT, { PAD + 4.f, 0.f, 200.f, TITLE_H }, gp::StringAlignmentNear, true);

    auto mini = minimizeRect();
    if (g_hover == HIT_MINIMIZE) fillRound(g, COL_CARD_HOVER, mini, 4.f);
    {
        gp::Pen pen(g_hover == HIT_MINIMIZE ? COL_TEXT : COL_MUTED, 1.5f);
        float cy = mini.Y + mini.Height / 2.f;
        float cx = mini.X + mini.Width / 2.f;
        g.DrawLine(&pen, cx - 5.f, cy, cx + 5.f, cy);
    }

    auto close = closeRect();
    if (g_hover == HIT_CLOSE) fillRound(g, COL_CLOSE_HOVER, close, 4.f);
    {
        gp::Pen pen(g_hover == HIT_CLOSE ? COL_TEXT : COL_MUTED, 1.5f);
        float cx = close.X + close.Width / 2.f;
        float cy = close.Y + close.Height / 2.f;
        g.DrawLine(&pen, cx - 5.f, cy - 5.f, cx + 5.f, cy + 5.f);
        g.DrawLine(&pen, cx + 5.f, cy - 5.f, cx - 5.f, cy + 5.f);
    }

    // Slider card (left column)
    card(g, { LEFT_X, SLIDER_CARD_Y, COL_W, SLIDER_CARD_H });
    for (int i = 0; i < SLIDER_COUNT; ++i) {
        float rowY = sliderRowY(i);
        float y = sliderY(i);
        int value = g_values.sliders[i];
        bool active = g_hover == HIT_SLIDER + i || g_dragging == i;

        if (active) fillRound(g, COL_CARD_HOVER, { LEFT_X + 4.f, rowY + 1.f, COL_W - 8.f, SROW_H - 2.f }, 4.f);
        if (i > 0) {
            gp::Pen sep(COL_BG, 1.f);
            g.DrawLine(&sep, LEFT_X + 14.f, rowY, LEFT_X + COL_W - 14.f, rowY);
        }

        text(g, SLIDER_NAMES[i], 13.f, value > 0 ? COL_TEXT : COL_MUTED, { SLIDER_X0, rowY + 5.f, 190.f, 22.f });
        text(g, valueLabel(i, value), 12.f, value > 0 ? COL_TEXT : COL_MUTED,
             { SLIDER_X0, rowY + 5.f, SLIDER_X1 - SLIDER_X0, 22.f }, gp::StringAlignmentFar);

        float tx = thumbX(i, value);
        fillRound(g, COL_TRACK, { SLIDER_X0, y - 2.f, SLIDER_X1 - SLIDER_X0, 4.f }, 2.f);
        if (tx > SLIDER_X0) fillRound(g, COL_TEXT, { SLIDER_X0, y - 2.f, tx - SLIDER_X0, 4.f }, 2.f);
        fillRound(g, gp::Color(255, 0xff, 0xff, 0xff), { tx - 4.f, y - 9.f, 8.f, 18.f }, 4.f);
    }

    // Toggle card (right column)
    card(g, { RIGHT_X, TOGGLE_CARD_Y, COL_W, TOGGLE_CARD_H });
    for (int i = 0; i < TOGGLE_COUNT; ++i) {
        auto row = toggleRowRect(i);
        if (g_hover == HIT_TOGGLE + i) {
            fillRound(g, COL_CARD_HOVER, { row.X + 4.f, row.Y + 2.f, row.Width - 8.f, row.Height - 4.f }, 4.f);
        }
        if (i > 0) {
            gp::Pen sep(COL_BG, 1.f);
            g.DrawLine(&sep, row.X + 14.f, row.Y, row.X + row.Width - 14.f, row.Y);
        }
        text(g, TOGGLE_NAMES[i], 13.f, COL_TEXT, { row.X + 16.f, row.Y, 220.f, row.Height });
        drawToggle(g, row.X + row.Width - 16.f - 38.f, row.Y + (row.Height - 20.f) / 2.f, g_values.toggles[i]);
    }

    // Live log (right column), newest on top
    float logX = RIGHT_X + 16.f;
    float logW = COL_W - 32.f;
    card(g, { RIGHT_X, LOG_CARD_Y, COL_W, LOG_CARD_H });
    text(g, L"Log", 14.f, COL_TEXT, { logX, LOG_CARD_Y + 6.f, 120.f, 24.f }, gp::StringAlignmentNear, true);
    if (g_log.empty()) {
        text(g, L"Nothing yet", 12.f, COL_MUTED, { logX, LOG_CARD_Y + 34.f, logW, LOG_LINE_H });
    }
    for (size_t i = 0; i < g_log.size(); ++i) {
        // Older lines fade out a bit.
        BYTE fade = static_cast<BYTE>(std::max(90, 240 - static_cast<int>(i) * 14));
        text(g, g_log[i], 12.f, gp::Color(255, fade, fade, fade),
             { logX, LOG_CARD_Y + 34.f + i * LOG_LINE_H, logW, LOG_LINE_H },
             gp::StringAlignmentNear, false, L"Consolas");
    }

    // Footer: description of whatever is under the mouse, or the key hints.
    gp::RectF footer(PAD, H - FOOTER_H + 4.f, W - PAD * 2, FOOTER_H - 10.f);
    if (isSliderHit(g_hover) || g_dragging >= 0) {
        int i = g_dragging >= 0 ? g_dragging : g_hover - HIT_SLIDER;
        wrappedText(g, SLIDER_DESCS[i], 13.f, COL_TEXT, footer);
    }
    else if (g_hover >= HIT_TOGGLE && g_hover < HIT_TOGGLE + TOGGLE_COUNT) {
        wrappedText(g, TOGGLE_DESCS[g_hover - HIT_TOGGLE], 13.f, COL_TEXT, footer);
    }
    else {
        wrappedText(g, L"Hover a setting to see what it does  \u00b7  F7 to show / hide  \u00b7  scroll or arrows to adjust", 12.f, COL_MUTED, footer);
    }

    gp::Graphics screen(target);
    screen.DrawImage(&buffer, 0, 0, pw, ph);
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

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

// Size the window for the monitor it's on: 1x on 1080p, 1.33x on 1440p,
// 2x on 4K, times the Window Size setting.
void applyScale(HWND wnd) {
    float monitorScale = 1.f;
    MONITORINFO info { sizeof(info) };
    if (GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &info)) {
        float height = static_cast<float>(info.rcMonitor.bottom - info.rcMonitor.top);
        monitorScale = std::clamp(height / 1080.f, 0.75f, 3.f);
    }
    float user = std::clamp(g_values.sizePercent / 100.f, 0.5f, 2.f);
    float scale = std::clamp(monitorScale * user, 0.5f, 4.f);
    if (std::abs(scale - g_scale) < 0.01f) return;

    g_scale = scale;
    SetWindowPos(
        wnd, nullptr, 0, 0,
        static_cast<int>(std::lround(W * g_scale)), static_cast<int>(std::lround(H * g_scale)),
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE
    );
    InvalidateRect(wnd, nullptr, FALSE);
}

void showWindow(HWND wnd, Values* values) {
    g_values = *values;
    if (!g_placed) {
        place(wnd, *values);
        g_placed = true;
    }
    applyScale(wnd);
    delete values;
    g_hover = -1;
    // Don't steal focus from the game.
    ShowWindow(wnd, SW_SHOWNOACTIVATE);
    InvalidateRect(wnd, nullptr, FALSE);
}

void hide(HWND wnd) {
    g_dragging = -1;
    ReleaseCapture();
    bool hadFocus = GetForegroundWindow() == wnd;
    ShowWindow(wnd, SW_HIDE);
    if (hadFocus) {
        if (HWND game = g_game.load()) SetForegroundWindow(game);
    }
}

void setSlider(HWND wnd, int slider, int value, bool commit) {
    value = std::clamp(value, 0, SLIDER_MAX[slider]);
    if (value != g_values.sliders[slider]) {
        g_values.sliders[slider] = value;
        InvalidateRect(wnd, nullptr, FALSE);
    }
    if (commit) save();
}

float logicalX(LPARAM lParam) { return GET_X_LPARAM(lParam) / g_scale; }
float logicalY(LPARAM lParam) { return GET_Y_LPARAM(lParam) / g_scale; }

LRESULT CALLBACK windowProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PB_SHOW:
            showWindow(wnd, reinterpret_cast<Values*>(lParam));
            return 0;

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
            if (g_dragging < 0) {
                for (int i = 0; i < SLIDER_COUNT; ++i) g_values.sliders[i] = values->sliders[i];
                for (int i = 0; i < TOGGLE_COUNT; ++i) g_values.toggles[i] = values->toggles[i];
                g_values.sizePercent = values->sizePercent;
                applyScale(wnd);
                InvalidateRect(wnd, nullptr, FALSE);
            }
            delete values;
            return 0;
        }

        case WM_PB_LOG: {
            auto line = reinterpret_cast<std::wstring*>(lParam);
            g_log.push_front(std::move(*line));
            delete line;
            while (g_log.size() > LOG_LINES) g_log.pop_back();
            if (IsWindowVisible(wnd)) InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        }

        case WM_NCHITTEST: {
            // Drag the window by its title bar.
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(wnd, &pt);
            float x = pt.x / g_scale;
            float y = pt.y / g_scale;
            if (y < TITLE_H && !closeRect().Contains(x, y) && !minimizeRect().Contains(x, y)) return HTCAPTION;
            return HTCLIENT;
        }

        case WM_EXITSIZEMOVE:
            applyScale(wnd); // moved to a different monitor?
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
                setSlider(wnd, g_dragging, valueAt(g_dragging, x), false);
                return 0;
            }
            int hover = hitTest(x, y);
            if (hover != g_hover) {
                g_hover = hover;
                InvalidateRect(wnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme { sizeof(tme), TME_LEAVE, wnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            if (g_hover != -1) {
                g_hover = -1;
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
                ShowWindow(wnd, SW_MINIMIZE);
            }
            else if (isSliderHit(hit)) {
                int i = hit - HIT_SLIDER;
                g_focusSlider = i;
                // Only grab when clicking near the bar itself.
                if (std::abs(y - sliderY(i)) <= 16.f && x >= SLIDER_X0 - 12.f && x <= SLIDER_X1 + 12.f) {
                    g_dragging = i;
                    SetCapture(wnd);
                    setSlider(wnd, i, valueAt(i, x), false);
                }
            }
            else if (hit >= HIT_TOGGLE && hit < HIT_TOGGLE + TOGGLE_COUNT) {
                g_values.toggles[hit - HIT_TOGGLE] = !g_values.toggles[hit - HIT_TOGGLE];
                InvalidateRect(wnd, nullptr, FALSE);
                save();
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (g_dragging >= 0) {
                g_dragging = -1;
                ReleaseCapture();
                save();
                InvalidateRect(wnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL: {
            int steps = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            // Scroll moves whichever slider the mouse is over.
            int slider = isSliderHit(g_hover) ? g_hover - HIT_SLIDER : g_focusSlider;
            g_focusSlider = slider;
            setSlider(wnd, slider, g_values.sliders[slider] + steps, true);
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
                    setSlider(wnd, g_focusSlider, g_values.sliders[g_focusSlider] - 1, true);
                    return 0;
                case VK_RIGHT:
                case VK_UP:
                    setSlider(wnd, g_focusSlider, g_values.sliders[g_focusSlider] + 1, true);
                    return 0;
            }
            break;

        case WM_CLOSE:
            hide(wnd); // keep the window around, just hide it
            return 0;
    }
    return DefWindowProcW(wnd, msg, wParam, lParam);
}

void roundCorners(HWND wnd) {
    // Windows 11 rounded corners + dark frame, loaded at runtime so nothing extra is linked.
    if (auto dwm = LoadLibraryW(L"dwmapi.dll")) {
        using Fn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        if (auto set = reinterpret_cast<Fn>(GetProcAddress(dwm, "DwmSetWindowAttribute"))) {
            int round = 2;  // DWMWCP_ROUND
            set(wnd, 33, &round, sizeof(round)); // DWMWA_WINDOW_CORNER_PREFERENCE
            BOOL dark = TRUE;
            set(wnd, 20, &dark, sizeof(dark));   // DWMWA_USE_IMMERSIVE_DARK_MODE
        }
    }
}

void windowThread() {
    gp::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    gp::GdiplusStartup(&token, &input, nullptr);

    auto instance = GetModuleHandleW(nullptr);

    HDC screen = GetDC(nullptr);
    g_scale = GetDeviceCaps(screen, LOGPIXELSX) / 96.f; // first guess, applyScale fixes it on show
    ReleaseDC(nullptr, screen);
    if (!(g_scale >= 1.f && g_scale <= 4.f)) g_scale = 1.f;

    WNDCLASSW cls {};
    cls.style = CS_DROPSHADOW;
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    cls.lpszClassName = L"InputBufferLiteWindow";
    RegisterClassW(&cls);

    int pw = static_cast<int>(std::lround(W * g_scale));
    int ph = static_cast<int>(std::lround(H * g_scale));

    // A normal app window (own taskbar button) so it can live on another monitor.
    HWND wnd = CreateWindowExW(
        WS_EX_APPWINDOW, cls.lpszClassName, L"Input Buffer Lite", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, pw, ph, nullptr, nullptr, instance, nullptr
    );
    if (!wnd) {
        g_started = false;
        return;
    }
    roundCorners(wnd);

    // Hide from capture. Falls back to a black box on Windows older than 10 (2004).
    if (!SetWindowDisplayAffinity(wnd, EXCLUDE_FROM_CAPTURE)) {
        SetWindowDisplayAffinity(wnd, WDA_MONITOR);
    }

    g_window = wnd;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

HWND ensureWindow() {
    if (!g_started.exchange(true)) {
        std::thread(windowThread).detach();
        // First time only: give the window a moment to be created.
        for (int i = 0; i < 100 && !g_window.load() && g_started.load(); ++i) Sleep(5);
    }
    return g_window.load();
}

void rememberGameWindow(HWND wnd) {
    HWND fg = GetForegroundWindow();
    if (fg && fg != wnd) g_game = fg;
}

} // namespace

void toggle() {
    HWND wnd = ensureWindow();
    if (!wnd) return;

    if (IsWindowVisible(wnd) && !IsIconic(wnd)) {
        PostMessageW(wnd, WM_PB_HIDE, 0, 0);
        return;
    }
    if (IsIconic(wnd)) ShowWindowAsync(wnd, SW_RESTORE);

    rememberGameWindow(wnd);
    PostMessageW(wnd, WM_PB_SHOW, 0, reinterpret_cast<LPARAM>(new Values(readSettings())));
}

void open() {
    HWND wnd = ensureWindow();
    if (!wnd) return;
    rememberGameWindow(wnd);
    PostMessageW(wnd, WM_PB_OPEN, 0, reinterpret_cast<LPARAM>(new Values(readSettings())));
}

void sync() {
    HWND wnd = g_window.load();
    if (!wnd) return;
    auto values = new Values(readSettings());
    if (!PostMessageW(wnd, WM_PB_SYNC, 0, reinterpret_cast<LPARAM>(values))) delete values;
}

void log(std::string const& line) {
    HWND wnd = g_window.load();
    if (!wnd) return;

    // Prefix with the time, like the old console did.
    std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_s(&local, &now);
    char stamp[16];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);

    std::string full = std::string(stamp) + "  " + line;
    int len = MultiByteToWideChar(CP_UTF8, 0, full.c_str(), static_cast<int>(full.size()), nullptr, 0);
    auto wide = new std::wstring(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, full.c_str(), static_cast<int>(full.size()), wide->data(), len);

    if (!PostMessageW(wnd, WM_PB_LOG, 0, reinterpret_cast<LPARAM>(wide))) delete wide;
}

} // namespace pb::menu

#else

namespace pb::menu {

void toggle() {
    openSettingsPopup(Mod::get(), false);
}

void open() {}

void sync() {}

void log(std::string const&) {}

} // namespace pb::menu

#endif
