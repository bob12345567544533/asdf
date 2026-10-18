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
constexpr int TOGGLE_COUNT = 5;
constexpr size_t LOG_LINES = 10;

// Layout in logical pixels (scaled by DPI when drawn).
constexpr float W = 340.f;
constexpr float TITLE_H = 40.f;
constexpr float PAD = 12.f;
constexpr float GAP = 8.f;

constexpr float SLIDER_CARD_Y = TITLE_H + GAP;
constexpr float SLIDER_CARD_H = 92.f;
constexpr float SLIDER_X0 = PAD + 18.f;
constexpr float SLIDER_X1 = W - PAD - 64.f;
constexpr float SLIDER_Y = SLIDER_CARD_Y + 64.f;

constexpr float ROW_H = 38.f;
constexpr float TOGGLE_CARD_Y = SLIDER_CARD_Y + SLIDER_CARD_H + GAP;
constexpr float TOGGLE_CARD_H = ROW_H * TOGGLE_COUNT + 8.f;

constexpr float LOG_LINE_H = 18.f;
constexpr float LOG_CARD_Y = TOGGLE_CARD_Y + TOGGLE_CARD_H + GAP;
constexpr float LOG_CARD_H = 34.f + LOG_LINE_H * LOG_LINES + 8.f;

constexpr float H = LOG_CARD_Y + LOG_CARD_H + 34.f;

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
    int ticks = 2;
    bool toggles[TOGGLE_COUNT] { true, true, false, true, false };
    bool hasPos = false;
    int x = 0;
    int y = 0;
};

constexpr char const* TOGGLE_KEYS[TOGGLE_COUNT] {
    "enabled", "buffer-releases", "buffer-ship", "show-feedback", "show-stats"
};
constexpr wchar_t const* TOGGLE_NAMES[TOGGLE_COUNT] {
    L"Enabled", L"Releases (wave, ship, robot)", L"Ship", L"Timing popup", L"Counter"
};

std::atomic<HWND> g_window = nullptr;
std::atomic<HWND> g_game = nullptr;
std::atomic<bool> g_started = false;

// Only touched on the window thread.
Values g_values;
std::deque<std::wstring> g_log;
int g_hover = -1; // see hitTest()
bool g_dragging = false;
bool g_placed = false;
float g_scale = 1.f;

// Settings are only read and written on the main thread.
Values readSettings() {
    auto mod = Mod::get();
    Values v;
    v.ticks = static_cast<int>(mod->getSettingValue<int64_t>("buffer-ticks"));
    for (int i = 0; i < TOGGLE_COUNT; ++i) v.toggles[i] = mod->getSettingValue<bool>(TOGGLE_KEYS[i]);
    v.hasPos = mod->getSavedValue<bool>("window-has-pos", false);
    v.x = static_cast<int>(mod->getSavedValue<int64_t>("window-x", 0));
    v.y = static_cast<int>(mod->getSavedValue<int64_t>("window-y", 0));
    return v;
}

void writeSettings(Values const& v) {
    auto mod = Mod::get();
    mod->setSettingValue<int64_t>("buffer-ticks", v.ticks);
    for (int i = 0; i < TOGGLE_COUNT; ++i) mod->setSettingValue<bool>(TOGGLE_KEYS[i], v.toggles[i]);
    if (auto overlay = BufferOverlay::get(PlayLayer::get())) overlay->refresh();
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
gp::RectF sliderHitRect() { return { SLIDER_X0 - 10.f, SLIDER_Y - 14.f, SLIDER_X1 - SLIDER_X0 + 20.f, 28.f }; }
gp::RectF toggleRowRect(int i) { return { PAD, TOGGLE_CARD_Y + 4.f + i * ROW_H, W - PAD * 2, ROW_H }; }

float thumbX(int ticks) {
    return SLIDER_X0 + (SLIDER_X1 - SLIDER_X0) * (static_cast<float>(ticks) / MAX_TICKS);
}

int ticksAt(float x) {
    float t = (x - SLIDER_X0) / (SLIDER_X1 - SLIDER_X0);
    return std::clamp(static_cast<int>(std::lround(t * MAX_TICKS)), 0, MAX_TICKS);
}

// Hit targets: 0 = close, 1 = slider, 2..6 = toggles, 7 = minimize.
constexpr int HIT_CLOSE = 0;
constexpr int HIT_SLIDER = 1;
constexpr int HIT_TOGGLE = 2;
constexpr int HIT_MINIMIZE = 7;

int hitTest(float x, float y) {
    if (closeRect().Contains(x, y)) return HIT_CLOSE;
    if (minimizeRect().Contains(x, y)) return HIT_MINIMIZE;
    if (sliderHitRect().Contains(x, y)) return HIT_SLIDER;
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
    text(g, L"Input Buffer", 15.f, COL_TEXT, { PAD + 4.f, 0.f, 200.f, TITLE_H }, gp::StringAlignmentNear, true);

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

    // Slider card
    card(g, { PAD, SLIDER_CARD_Y, W - PAD * 2, SLIDER_CARD_H }, g_hover == HIT_SLIDER || g_dragging);
    text(g, L"Buffer", 14.f, COL_TEXT, { SLIDER_X0, SLIDER_CARD_Y + 10.f, 120.f, 24.f }, gp::StringAlignmentNear, true);
    wchar_t ms[48];
    std::swprintf(ms, 48, L"%.1f ms", g_values.ticks * 1000.0 / 240.0);
    text(g, ms, 12.f, COL_MUTED, { SLIDER_X0, SLIDER_CARD_Y + 10.f, W - PAD - 18.f - SLIDER_X0, 24.f }, gp::StringAlignmentFar);

    float tx = thumbX(g_values.ticks);
    fillRound(g, COL_TRACK, { SLIDER_X0, SLIDER_Y - 2.f, SLIDER_X1 - SLIDER_X0, 4.f }, 2.f);
    if (tx > SLIDER_X0) fillRound(g, COL_TEXT, { SLIDER_X0, SLIDER_Y - 2.f, tx - SLIDER_X0, 4.f }, 2.f);
    fillRound(g, gp::Color(255, 0xff, 0xff, 0xff), { tx - 4.f, SLIDER_Y - 11.f, 8.f, 22.f }, 4.f);
    text(g, std::to_wstring(g_values.ticks), 22.f, COL_TEXT, { SLIDER_X1 + 8.f, SLIDER_Y - 16.f, 46.f, 32.f }, gp::StringAlignmentFar);

    // Toggle card
    card(g, { PAD, TOGGLE_CARD_Y, W - PAD * 2, TOGGLE_CARD_H });
    for (int i = 0; i < TOGGLE_COUNT; ++i) {
        auto row = toggleRowRect(i);
        if (g_hover == HIT_TOGGLE + i) {
            fillRound(g, COL_CARD_HOVER, { row.X + 4.f, row.Y + 2.f, row.Width - 8.f, row.Height - 4.f }, 4.f);
        }
        if (i > 0) {
            gp::Pen sep(COL_BG, 1.f);
            g.DrawLine(&sep, row.X + 14.f, row.Y, row.X + row.Width - 14.f, row.Y);
        }
        text(g, TOGGLE_NAMES[i], 13.f, COL_TEXT, { row.X + 18.f, row.Y, 220.f, row.Height });
        drawToggle(g, row.X + row.Width - 18.f - 38.f, row.Y + (row.Height - 20.f) / 2.f, g_values.toggles[i]);
    }

    // Live log, newest on top
    card(g, { PAD, LOG_CARD_Y, W - PAD * 2, LOG_CARD_H });
    text(g, L"Log", 14.f, COL_TEXT, { SLIDER_X0, LOG_CARD_Y + 6.f, 120.f, 24.f }, gp::StringAlignmentNear, true);
    if (g_log.empty()) {
        text(g, L"Nothing yet", 12.f, COL_MUTED, { SLIDER_X0, LOG_CARD_Y + 34.f, W - PAD * 2 - 36.f, LOG_LINE_H });
    }
    for (size_t i = 0; i < g_log.size(); ++i) {
        // Older lines fade out a bit.
        BYTE fade = static_cast<BYTE>(std::max(90, 240 - static_cast<int>(i) * 16));
        text(g, g_log[i], 12.f, gp::Color(255, fade, fade, fade),
             { SLIDER_X0, LOG_CARD_Y + 34.f + i * LOG_LINE_H, W - PAD * 2 - 36.f, LOG_LINE_H },
             gp::StringAlignmentNear, false, L"Consolas");
    }

    // Footer
    text(g, L"F7 to show / hide  ·  scroll or arrows to adjust", 11.f, COL_MUTED,
         { PAD, H - 30.f, W - PAD * 2, 22.f }, gp::StringAlignmentCenter);

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

void showWindow(HWND wnd, Values* values) {
    g_values = *values;
    if (!g_placed) {
        place(wnd, *values);
        g_placed = true;
    }
    delete values;
    g_hover = -1;
    // Don't steal focus from the game.
    ShowWindow(wnd, SW_SHOWNOACTIVATE);
    InvalidateRect(wnd, nullptr, FALSE);
}

void hide(HWND wnd) {
    g_dragging = false;
    ReleaseCapture();
    bool hadFocus = GetForegroundWindow() == wnd;
    ShowWindow(wnd, SW_HIDE);
    if (hadFocus) {
        if (HWND game = g_game.load()) SetForegroundWindow(game);
    }
}

void setTicks(HWND wnd, int ticks, bool commit) {
    ticks = std::clamp(ticks, 0, MAX_TICKS);
    if (ticks != g_values.ticks) {
        g_values.ticks = ticks;
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
            if (!g_dragging) {
                g_values.ticks = values->ticks;
                for (int i = 0; i < TOGGLE_COUNT; ++i) g_values.toggles[i] = values->toggles[i];
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
            if (g_dragging) {
                setTicks(wnd, ticksAt(x), false);
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
            else if (hit == HIT_SLIDER) {
                g_dragging = true;
                SetCapture(wnd);
                setTicks(wnd, ticksAt(x), false);
            }
            else if (hit >= HIT_TOGGLE && hit < HIT_TOGGLE + TOGGLE_COUNT) {
                g_values.toggles[hit - HIT_TOGGLE] = !g_values.toggles[hit - HIT_TOGGLE];
                InvalidateRect(wnd, nullptr, FALSE);
                save();
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (g_dragging) {
                g_dragging = false;
                ReleaseCapture();
                save();
                InvalidateRect(wnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL: {
            int steps = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            setTicks(wnd, g_values.ticks + steps, true);
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
                    setTicks(wnd, g_values.ticks - 1, true);
                    return 0;
                case VK_RIGHT:
                case VK_UP:
                    setTicks(wnd, g_values.ticks + 1, true);
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
    g_scale = GetDeviceCaps(screen, LOGPIXELSX) / 96.f;
    ReleaseDC(nullptr, screen);
    if (!(g_scale >= 1.f && g_scale <= 4.f)) g_scale = 1.f;

    WNDCLASSW cls {};
    cls.style = CS_DROPSHADOW;
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    cls.lpszClassName = L"InputBufferWindow";
    RegisterClassW(&cls);

    int pw = static_cast<int>(std::lround(W * g_scale));
    int ph = static_cast<int>(std::lround(H * g_scale));

    // A normal app window (own taskbar button) so it can live on another monitor.
    HWND wnd = CreateWindowExW(
        WS_EX_APPWINDOW, cls.lpszClassName, L"Input Buffer", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
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
