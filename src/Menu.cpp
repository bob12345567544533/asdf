#include "Menu.hpp"
#include "Overlay.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>

using namespace geode::prelude;

#ifdef GEODE_IS_WINDOWS

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <thread>

namespace pb::menu {
namespace {

constexpr int MODE_COUNT = 8;
constexpr std::array<char const*, MODE_COUNT> MODE_KEYS {
    "cube-ms", "ship-ms", "ball-ms", "ufo-ms", "wave-ms", "robot-ms", "spider-ms", "swing-ms"
};
constexpr std::array<wchar_t const*, MODE_COUNT> MODE_NAMES {
    L"Cube", L"Ship", L"Ball", L"UFO", L"Wave", L"Robot", L"Spider", L"Swing"
};
constexpr int SHIP_INDEX = 1;

// The old F6 stages, in ms (2, 4, 8 and 16 steps at 240 TPS).
constexpr std::array<double, 4> PRESETS { 8.5, 17.0, 33.5, 67.0 };
constexpr std::array<wchar_t const*, 4> PRESET_LABELS { L"2 ticks", L"4 ticks", L"8 ticks", L"16 ticks" };

// Control IDs. IDOK / IDCANCEL make Enter apply and Esc close.
constexpr int ID_APPLY = IDOK;
constexpr int ID_CLOSE = IDCANCEL;
constexpr int ID_ENABLED = 100;
constexpr int ID_RELEASES = 101;
constexpr int ID_FEEDBACK = 102;
constexpr int ID_STATS = 103;
constexpr int ID_MS_BASE = 200;
constexpr int ID_PRESET_BASE = 300;

constexpr UINT WM_PB_SHOW = WM_APP + 1;
constexpr UINT WM_PB_HIDE = WM_APP + 2;

// WDA_EXCLUDEFROMCAPTURE, spelled out for older SDK headers.
constexpr DWORD EXCLUDE_FROM_CAPTURE = 0x00000011;

struct Values {
    bool enabled = true;
    bool releases = true;
    bool feedback = true;
    bool stats = false;
    std::array<double, MODE_COUNT> ms {};
};

std::atomic<HWND> g_window = nullptr;
std::atomic<HWND> g_game = nullptr;
std::atomic<bool> g_started = false;
HFONT g_font = nullptr;

// Settings are only read and written on the main thread.
Values readSettings() {
    auto mod = Mod::get();
    Values v;
    v.enabled = mod->getSettingValue<bool>("enabled");
    v.releases = mod->getSettingValue<bool>("buffer-releases");
    v.feedback = mod->getSettingValue<bool>("show-feedback");
    v.stats = mod->getSettingValue<bool>("show-stats");
    for (int i = 0; i < MODE_COUNT; ++i) v.ms[i] = mod->getSettingValue<double>(MODE_KEYS[i]);
    return v;
}

void writeSettings(Values const& v) {
    auto mod = Mod::get();
    mod->setSettingValue<bool>("enabled", v.enabled);
    mod->setSettingValue<bool>("buffer-releases", v.releases);
    mod->setSettingValue<bool>("show-feedback", v.feedback);
    mod->setSettingValue<bool>("show-stats", v.stats);
    for (int i = 0; i < MODE_COUNT; ++i) mod->setSettingValue<double>(MODE_KEYS[i], v.ms[i]);
    if (auto overlay = BufferOverlay::get(PlayLayer::get())) overlay->refresh();
}

void setCheck(HWND wnd, int id, bool on) {
    SendDlgItemMessageW(wnd, id, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool getCheck(HWND wnd, int id) {
    return SendDlgItemMessageW(wnd, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void setNumber(HWND wnd, int id, double value) {
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%g", value);
    SetDlgItemTextW(wnd, id, buf);
}

double getNumber(HWND wnd, int id) {
    wchar_t buf[32] {};
    GetDlgItemTextW(wnd, id, buf, 32);
    double value = std::wcstod(buf, nullptr);
    if (!(value >= 0.0)) value = 0.0; // also catches NaN
    return std::min(value, 100.0);
}

void fill(HWND wnd, Values const& v) {
    setCheck(wnd, ID_ENABLED, v.enabled);
    setCheck(wnd, ID_RELEASES, v.releases);
    setCheck(wnd, ID_FEEDBACK, v.feedback);
    setCheck(wnd, ID_STATS, v.stats);
    for (int i = 0; i < MODE_COUNT; ++i) setNumber(wnd, ID_MS_BASE + i, v.ms[i]);
}

Values collect(HWND wnd) {
    Values v;
    v.enabled = getCheck(wnd, ID_ENABLED);
    v.releases = getCheck(wnd, ID_RELEASES);
    v.feedback = getCheck(wnd, ID_FEEDBACK);
    v.stats = getCheck(wnd, ID_STATS);
    for (int i = 0; i < MODE_COUNT; ++i) v.ms[i] = getNumber(wnd, ID_MS_BASE + i);
    return v;
}

void hide(HWND wnd) {
    ShowWindow(wnd, SW_HIDE);
    if (HWND game = g_game.load()) SetForegroundWindow(game);
}

HWND add(HWND parent, wchar_t const* cls, wchar_t const* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND ctl = CreateWindowExW(
        0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(ctl, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return ctl;
}

void build(HWND wnd) {
    int y = 10;
    add(wnd, L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 130, 22, ID_ENABLED);
    add(wnd, L"BUTTON", L"Buffer releases", BS_AUTOCHECKBOX | WS_TABSTOP, 150, y, 140, 22, ID_RELEASES);
    y += 24;
    add(wnd, L"BUTTON", L"Timing feedback", BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 130, 22, ID_FEEDBACK);
    add(wnd, L"BUTTON", L"Show counter", BS_AUTOCHECKBOX | WS_TABSTOP, 150, y, 140, 22, ID_STATS);
    y += 32;

    add(wnd, L"STATIC", L"Max delay per gamemode (ms, 0 = off)", 0, 12, y, 280, 18, -1);
    y += 22;
    for (int i = 0; i < MODE_COUNT; ++i) {
        int col = i % 2;
        int row = i / 2;
        int x = 12 + col * 138;
        int rowY = y + row * 28;
        add(wnd, L"STATIC", MODE_NAMES[i], SS_CENTERIMAGE, x, rowY, 52, 22, -1);
        add(wnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, x + 54, rowY, 70, 22, ID_MS_BASE + i);
    }
    y += 4 * 28 + 6;

    add(wnd, L"STATIC", L"Set all (except ship):", 0, 12, y, 280, 18, -1);
    y += 20;
    for (int i = 0; i < 4; ++i) {
        add(wnd, L"BUTTON", PRESET_LABELS[i], BS_PUSHBUTTON | WS_TABSTOP, 12 + i * 69, y, 64, 24, ID_PRESET_BASE + i);
    }
    y += 34;

    add(wnd, L"BUTTON", L"Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, 12, y, 133, 28, ID_APPLY);
    add(wnd, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 151, y, 133, 28, ID_CLOSE);
    y += 34;

    add(wnd, L"STATIC", L"Hidden from OBS / Discord capture. Esc closes.", 0, 12, y, 280, 18, -1);
}

void showNear(HWND wnd) {
    RECT game {};
    HWND gameWnd = g_game.load();
    if (gameWnd && GetWindowRect(gameWnd, &game)) {
        SetWindowPos(wnd, HWND_TOPMOST, game.left + 40, game.top + 60, 0, 0, SWP_NOSIZE);
    }
    ShowWindow(wnd, SW_SHOW);
    SetForegroundWindow(wnd);
    SetFocus(GetDlgItem(wnd, ID_MS_BASE));
}

LRESULT CALLBACK windowProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PB_SHOW: {
            auto values = reinterpret_cast<Values*>(lParam);
            fill(wnd, *values);
            delete values;
            showNear(wnd);
            return 0;
        }
        case WM_PB_HIDE:
            hide(wnd);
            return 0;

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == ID_APPLY) {
                auto values = collect(wnd);
                fill(wnd, values); // show the clamped numbers
                queueInMainThread([values] { writeSettings(values); });
                return 0;
            }
            if (id == ID_CLOSE) {
                hide(wnd);
                return 0;
            }
            if (id >= ID_PRESET_BASE && id < ID_PRESET_BASE + 4) {
                double ms = PRESETS[id - ID_PRESET_BASE];
                for (int i = 0; i < MODE_COUNT; ++i) {
                    if (i != SHIP_INDEX) setNumber(wnd, ID_MS_BASE + i, ms);
                }
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            hide(wnd); // keep the window around, just hide it
            return 0;
    }
    return DefWindowProcW(wnd, msg, wParam, lParam);
}

void windowThread() {
    auto instance = GetModuleHandleW(nullptr);

    NONCLIENTMETRICSW metrics { sizeof(metrics) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    g_font = CreateFontIndirectW(&metrics.lfMessageFont);

    WNDCLASSW cls {};
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    cls.lpszClassName = L"PredictiveBufferMenu";
    RegisterClassW(&cls);

    // Size the outer window so the client area fits the controls.
    RECT rect { 0, 0, 296, 380 };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    AdjustWindowRectEx(&rect, style, FALSE, exStyle);

    HWND wnd = CreateWindowExW(
        exStyle, cls.lpszClassName, L"Predictive Buffer", style,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, nullptr
    );
    if (!wnd) {
        g_started = false;
        return;
    }
    build(wnd);

    // Hide from capture. Falls back to showing as a black box on Windows
    // versions older than 10 (2004).
    if (!SetWindowDisplayAffinity(wnd, EXCLUDE_FROM_CAPTURE)) {
        SetWindowDisplayAffinity(wnd, WDA_MONITOR);
    }

    g_window = wnd;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

} // namespace

void toggle() {
    if (!g_started.exchange(true)) {
        std::thread(windowThread).detach();
        // First press only: give the window a moment to be created.
        for (int i = 0; i < 100 && !g_window.load() && g_started.load(); ++i) Sleep(5);
    }

    HWND wnd = g_window.load();
    if (!wnd) return;

    if (IsWindowVisible(wnd)) {
        PostMessageW(wnd, WM_PB_HIDE, 0, 0);
        return;
    }

    HWND fg = GetForegroundWindow();
    if (fg && fg != wnd) g_game = fg;
    PostMessageW(wnd, WM_PB_SHOW, 0, reinterpret_cast<LPARAM>(new Values(readSettings())));
}

} // namespace pb::menu

#else

namespace pb::menu {

void toggle() {
    openSettingsPopup(Mod::get(), false);
}

} // namespace pb::menu

#endif
