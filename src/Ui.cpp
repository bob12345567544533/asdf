#include "Ui.hpp"

#include <Geode/Geode.hpp>

#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

namespace {

constexpr double MS_PER_TICK = 1000.0 / 240.0;

// ---------------------------------------------------------------------------
// In-game popup ("Click 12.5ms early (3 ticks)"), bottom left, fades out.
// ---------------------------------------------------------------------------

constexpr char const* POPUP_ID = "predictive-buffer-popup";

void showPopup(std::string const& text, ccColor3B color) {
    auto pl = PlayLayer::get();
    if (!pl) return;
    auto holder = pl->getChildByID(POPUP_ID);
    if (!holder) return;
    holder->removeAllChildren();

    auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    label->setScale(0.4f);
    label->setAnchorPoint({ 0.f, 0.f });
    label->setPosition({ 6.f, 6.f });
    label->setColor(color);
    label->setOpacity(200);
    holder->addChild(label);
    label->runAction(CCSequence::create(CCDelayTime::create(1.0f), CCFadeOut::create(0.4f), nullptr));
}

#ifdef GEODE_IS_WINDOWS

// ---------------------------------------------------------------------------
// Minimal window: own thread, never takes focus from the game, hidden from
// screen capture. F7 shows / hides it.
// ---------------------------------------------------------------------------

struct Entry {
    std::wstring time;
    int ticks;
    bool press;
    int percent;
};

std::mutex g_mutex;
std::deque<Entry> g_log;            // newest first
int g_saves = 0;
double g_totalMs = 0.0;
std::wstring g_stageText = L"Stage 1  ·  2 / 2 ticks";
bool g_off = false;
std::atomic<HWND> g_hwnd { nullptr };
int g_scroll = 0;                   // rows scrolled (window thread only)

constexpr UINT WM_REFRESH = WM_APP + 1;

COLORREF rgb(unsigned hex) {
    return RGB((hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);
}

// Minimal palette (same as the Lite "Minimal" theme).
const COLORREF BG = rgb(0x121212);
const COLORREF TEXT = rgb(0xececec);
const COLORREF DIM = rgb(0x9a9a9a);
const COLORREF FAINT = rgb(0x6a6a6a);
const COLORREF LINE = rgb(0x2a2a2a);
const COLORREF RED = rgb(0xff6b6b);

float scaleOf(HWND hwnd) {
    UINT dpi = 96;
    if (auto user32 = GetModuleHandleW(L"user32.dll")) {
        using Fn = UINT(WINAPI*)(HWND);
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(user32, "GetDpiForWindow"))) dpi = fn(hwnd);
    }
    return dpi / 96.f;
}

HFONT makeFont(float s, int px, int weight, wchar_t const* face) {
    return CreateFontW(-static_cast<int>(px * s + 0.5f), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

void text(HDC dc, HFONT font, COLORREF color, int x, int y, std::wstring const& str, UINT align = TA_LEFT) {
    SelectObject(dc, font);
    SetTextColor(dc, color);
    SetTextAlign(dc, align | TA_TOP);
    TextOutW(dc, x, y, str.c_str(), static_cast<int>(str.size()));
}

void hline(HDC dc, int x0, int x1, int y) {
    RECT r { x0, y, x1, y + 1 };
    auto brush = CreateSolidBrush(LINE);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}

std::wstring spaced(std::wstring const& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i) {
        out += s[i];
        if (i + 1 < s.size()) out += L' ';
    }
    return out;
}

std::wstring fmtMs(double ms) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f ms", ms);
    return buf;
}

int rowHeight(float s) { return static_cast<int>(22 * s); }
int logTop(float s) { return static_cast<int>(150 * s); }

void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    float s = scaleOf(hwnd);

    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    auto oldBmp = SelectObject(dc, bmp);
    auto bg = CreateSolidBrush(BG);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    HFONT title = makeFont(s, 13, FW_SEMIBOLD, L"Segoe UI");
    HFONT body = makeFont(s, 14, FW_NORMAL, L"Segoe UI");
    HFONT smallF = makeFont(s, 11, FW_SEMIBOLD, L"Segoe UI");
    HFONT mono = makeFont(s, 13, FW_NORMAL, L"Consolas");
    HFONT big = makeFont(s, 22, FW_SEMIBOLD, L"Segoe UI");

    int pad = static_cast<int>(22 * s);
    std::wstring stageText;
    bool off;
    int saves;
    double total;
    std::deque<Entry> log;
    {
        std::lock_guard lock(g_mutex);
        stageText = g_stageText;
        off = g_off;
        saves = g_saves;
        total = g_totalMs;
        log = g_log;
    }

    // Header (the top strip drags the window, the x hides it).
    text(dc, title, TEXT, pad, static_cast<int>(18 * s), spaced(L"INPUT BUFFER"));
    text(dc, body, DIM, w - pad, static_cast<int>(16 * s), L"×", TA_RIGHT);
    text(dc, body, off ? RED : DIM, pad, static_cast<int>(44 * s), stageText);
    hline(dc, pad, w - pad, static_cast<int>(74 * s));

    // Stats
    text(dc, smallF, FAINT, pad, static_cast<int>(86 * s), spaced(L"SAVES"));
    text(dc, smallF, FAINT, pad + static_cast<int>(130 * s), static_cast<int>(86 * s), spaced(L"AVG EARLY"));
    text(dc, big, TEXT, pad, static_cast<int>(102 * s), std::to_wstring(saves));
    text(dc, big, TEXT, pad + static_cast<int>(130 * s), static_cast<int>(102 * s),
        saves ? fmtMs(total / saves) : L"—");

    // Log
    int top = logTop(s);
    hline(dc, pad, w - pad, top - static_cast<int>(6 * s));
    text(dc, smallF, FAINT, pad, top + static_cast<int>(4 * s), spaced(L"LOG"));
    int rowsTop = top + static_cast<int>(26 * s);
    int footer = h - static_cast<int>(34 * s);
    int rh = rowHeight(s);
    int visible = std::max(0, (footer - rowsTop) / rh);
    int maxScroll = std::max(0, static_cast<int>(log.size()) - visible);
    g_scroll = std::clamp(g_scroll, 0, maxScroll);

    if (log.empty()) {
        text(dc, body, FAINT, pad, rowsTop, L"Buffered inputs show up here.");
    }
    for (int i = 0; i < visible && g_scroll + i < static_cast<int>(log.size()); ++i) {
        auto const& e = log[g_scroll + i];
        int y = rowsTop + i * rh;
        text(dc, mono, FAINT, pad, y, e.time);
        text(dc, body, TEXT, pad + static_cast<int>(78 * s), y, L"+" + std::to_wstring(e.ticks));
        text(dc, body, TEXT, pad + static_cast<int>(118 * s), y, fmtMs(e.ticks * MS_PER_TICK));
        text(dc, body, DIM, pad + static_cast<int>(196 * s), y, e.press ? L"press" : L"release");
        if (e.percent >= 0) text(dc, body, DIM, w - pad, y, std::to_wstring(e.percent) + L"%", TA_RIGHT);
    }

    hline(dc, pad, w - pad, footer);
    text(dc, smallF, FAINT, w / 2, footer + static_cast<int>(10 * s), L"F6 stage  ·  F7 show / hide", TA_CENTER);

    BitBlt(wdc, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    for (auto f : { title, body, smallF, mono, big }) DeleteObject(f);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_REFRESH:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST: {
            POINT p { static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)) };
            ScreenToClient(hwnd, &p);
            RECT rc;
            GetClientRect(hwnd, &rc);
            float s = scaleOf(hwnd);
            if (p.y < 70 * s && p.x < rc.right - 44 * s) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_LBUTTONUP: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            float s = scaleOf(hwnd);
            int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
            if (y < 50 * s && x >= rc.right - 44 * s) ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        case WM_MOUSEWHEEL:
            g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 3;
            if (g_scroll < 0) g_scroll = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            auto r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool gameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void windowThread() {
    if (auto user32 = GetModuleHandleW(L"user32.dll")) {
        using Fn = HANDLE(WINAPI*)(HANDLE);
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(user32, "SetThreadDpiAwarenessContext"))) {
            fn(reinterpret_cast<HANDLE>(-4)); // per-monitor v2
        }
    }

    WNDCLASSW wc {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PredictiveBufferMinimal";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        wc.lpszClassName, L"Input Buffer", WS_POPUP,
        60, 60, 360, 480, nullptr, nullptr, wc.hInstance, nullptr
    );
    if (!hwnd) return;
    float s = scaleOf(hwnd);
    SetWindowPos(hwnd, nullptr, 0, 0, static_cast<int>(360 * s), static_cast<int>(480 * s),
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowDisplayAffinity(hwnd, 0x11); // WDA_EXCLUDEFROMCAPTURE: hidden from OBS / screenshots
    g_hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    SetTimer(hwnd, 1, 30, [](HWND h, UINT, UINT_PTR, DWORD) {
        static bool wasDown = false;
        bool down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (down && !wasDown && gameFocused()) {
            ShowWindow(h, IsWindowVisible(h) ? SW_HIDE : SW_SHOWNOACTIVATE);
        }
        wasDown = down;
    });

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void refresh() {
    if (HWND h = g_hwnd.load()) PostMessageW(h, WM_REFRESH, 0, 0);
}

std::wstring nowText() {
    std::time_t t = std::time(nullptr);
    std::tm tm {};
    localtime_s(&tm, &t);
    wchar_t buf[16];
    wcsftime(buf, 16, L"%H:%M:%S", &tm);
    return buf;
}

#endif

} // namespace

namespace ui {

void init() {
#ifdef GEODE_IS_WINDOWS
    std::thread(windowThread).detach();
#endif
}

void attach(PlayLayer* layer) {
    if (!layer || layer->getChildByID(POPUP_ID)) return;
    auto holder = CCNode::create();
    holder->setID(POPUP_ID);
    layer->addChild(holder, 10000);
}

void saved(int ticks, bool press, int limit) {
    (void)limit;
    double ms = ticks * MS_PER_TICK;
    showPopup(fmt::format("{} {:.1f}ms early ({} tick{})", press ? "Click" : "Release", ms, ticks, ticks == 1 ? "" : "s"),
        { 120, 255, 140 });

#ifdef GEODE_IS_WINDOWS
    int percent = -1;
    if (auto pl = PlayLayer::get()) {
        if (pl->m_player1 && pl->m_levelLength > 0.f) {
            percent = static_cast<int>(std::clamp(pl->m_player1->getPositionX() / pl->m_levelLength * 100.f, 0.f, 100.f));
        }
    }
    {
        std::lock_guard lock(g_mutex);
        g_log.push_front({ nowText(), ticks, press, percent });
        if (g_log.size() > 300) g_log.pop_back();
        g_saves++;
        g_totalMs += ms;
    }
    refresh();
#endif
}

void stage(int index, int all, int wave) {
    bool off = index >= 4;
    showPopup(off ? "Buffer OFF" : fmt::format("Stage {}: {} / {} ticks", index + 1, all, wave),
        off ? ccColor3B { 255, 120, 120 } : ccColor3B { 120, 255, 140 });
#ifdef GEODE_IS_WINDOWS
    {
        std::lock_guard lock(g_mutex);
        g_off = off;
        if (off) g_stageText = L"Off  ·  raw input";
        else g_stageText = L"Stage " + std::to_wstring(index + 1) + L"  ·  " + std::to_wstring(all) + L" / "
            + std::to_wstring(wave) + L" ticks";
    }
    refresh();
#endif
}

} // namespace ui
