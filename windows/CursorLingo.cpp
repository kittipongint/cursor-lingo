// CursorLingo for Windows — shows the current keyboard input language next to the mouse pointer.
//
// Plain Win32 + GDI, no runtime dependencies. Footprint goals:
//   * The badge follows the mouse via a low-level mouse hook (event-driven, coalesced).
//   * Rendering happens only when the language, style, size or monitor DPI changes.
//   * The badge is a per-pixel-alpha layered window, so it is anti-aliased and click-through.
//   * The active layout is read with GetKeyboardLayout(). A keyboard hook exists only while the optional
//     "Auto-fix wrong language" feature is switched on (off by default); it keeps nothing but the word
//     being typed, in memory, and never touches the network or disk.
//   * Runs as a normal user: no administrator rights, no installer, no services. The registry is only
//     touched under HKCU.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

#include "resource.h"
#include "../core/layoutfix.h"

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

enum Style { STYLE_COLOR, STYLE_LIGHT, STYLE_DARK, STYLE_DOT, STYLE_COUNT };
enum Mode { MODE_ALWAYS, MODE_ON_SWITCH, MODE_COUNT };
enum Size { SIZE_SMALL, SIZE_MEDIUM, SIZE_LARGE, SIZE_COUNT };

static const wchar_t* kStyleNames[] = {L"Color", L"Light", L"Dark", L"Dot"};
static const wchar_t* kModeNames[] = {L"Always", L"Only when switching"};
static const wchar_t* kSizeNames[] = {L"Small", L"Medium", L"Large"};
static const float kSizeScale[] = {0.85f, 1.0f, 1.3f};

static const wchar_t kRegKey[] = L"Software\\CursorLingo";
static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kAppName[] = L"CursorLingo";

struct Settings {
    DWORD style = STYLE_COLOR;
    DWORD mode = MODE_ALWAYS;
    DWORD size = SIZE_MEDIUM;
    DWORD autoFix = 0;          // re-type words typed in the wrong layout (EN <-> TH)
    wchar_t hidden[256] = L"";  // comma separated codes, e.g. ",EN,JA,"
} g_set;

static DWORD RegReadDword(HKEY k, const wchar_t* name, DWORD def, DWORD max) {
    DWORD v = 0, cb = sizeof(v);
    if (RegQueryValueExW(k, name, nullptr, nullptr, (BYTE*)&v, &cb) != ERROR_SUCCESS || v >= max) return def;
    return v;
}

static void LoadSettings() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    g_set.style = RegReadDword(k, L"Style", STYLE_COLOR, STYLE_COUNT);
    g_set.mode = RegReadDword(k, L"Mode", MODE_ALWAYS, MODE_COUNT);
    g_set.size = RegReadDword(k, L"Size", SIZE_MEDIUM, SIZE_COUNT);
    g_set.autoFix = RegReadDword(k, L"AutoFix", 0, 2);
    DWORD cb = sizeof(g_set.hidden) - sizeof(wchar_t);
    if (RegQueryValueExW(k, L"Hidden", nullptr, nullptr, (BYTE*)g_set.hidden, &cb) != ERROR_SUCCESS) g_set.hidden[0] = 0;
    g_set.hidden[255] = 0;
    RegCloseKey(k);
}

static void SaveSettings() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, L"Style", 0, REG_DWORD, (BYTE*)&g_set.style, sizeof(DWORD));
    RegSetValueExW(k, L"Mode", 0, REG_DWORD, (BYTE*)&g_set.mode, sizeof(DWORD));
    RegSetValueExW(k, L"Size", 0, REG_DWORD, (BYTE*)&g_set.size, sizeof(DWORD));
    RegSetValueExW(k, L"AutoFix", 0, REG_DWORD, (BYTE*)&g_set.autoFix, sizeof(DWORD));
    RegSetValueExW(k, L"Hidden", 0, REG_SZ, (BYTE*)g_set.hidden, (DWORD)((wcslen(g_set.hidden) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

static bool IsHidden(const wchar_t* code) {
    wchar_t needle[8];
    swprintf(needle, 8, L",%ls,", code);
    return wcsstr(g_set.hidden, needle) != nullptr;
}

static void ToggleHidden(const wchar_t* code) {
    wchar_t needle[8];
    swprintf(needle, 8, L",%ls,", code);
    if (wchar_t* p = wcsstr(g_set.hidden, needle)) {
        wmemmove(p, p + wcslen(needle) - 1, wcslen(p + wcslen(needle) - 1) + 1);  // keep one comma
        if (wcscmp(g_set.hidden, L",") == 0) g_set.hidden[0] = 0;
    } else if (wcslen(g_set.hidden) + 6 < 250) {
        if (!g_set.hidden[0]) wcscpy(g_set.hidden, L",");
        wcscat(g_set.hidden, code);
        wcscat(g_set.hidden, L",");
    }
}

static bool StartupEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(k, kAppName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

static void SetStartup(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH], cmd[MAX_PATH + 4];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        swprintf(cmd, MAX_PATH + 4, L"\"%ls\"", path);
        RegSetValueExW(k, kAppName, 0, REG_SZ, (BYTE*)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kAppName);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------
// Input language
// ---------------------------------------------------------------------------

static HKL g_lastHkl = nullptr;
static wchar_t g_code[4] = L"??";
static int g_fixLang = -1;  // layout the auto-fixer understands: 0 = US English, 1 = Thai (Kedmanee), -1 = other
static int g_fixLen = 0;    // keys of the word being typed (see the auto-fix section)

// Only the stock layouts: remapped ones (Dvorak, Pattachote, ...) have a different key->letter table.
static int FixLangOf(HKL hkl) {
    UINT_PTR v = (UINT_PTR)hkl;
    if (HIWORD(v) != LOWORD(v)) return -1;
    if (LOWORD(v) == 0x0409) return 0;
    if (LOWORD(v) == 0x041E) return 1;
    return -1;
}

// Keyboard layout of the window that receives typing.
static HKL CurrentHkl() {
    HWND fg = GetForegroundWindow();
    GUITHREADINFO gti = {sizeof(gti)};
    DWORD tid = 0;
    if (fg) {
        tid = GetWindowThreadProcessId(fg, nullptr);
        // UWP apps host their input on a different thread than the frame window.
        if (GetGUIThreadInfo(tid, &gti) && gti.hwndFocus) tid = GetWindowThreadProcessId(gti.hwndFocus, nullptr);
    }
    return GetKeyboardLayout(tid);
}

// Returns true when the language code changed.
static bool UpdateLanguage() {
    HKL hkl = CurrentHkl();
    if (!hkl || hkl == g_lastHkl) return false;
    g_lastHkl = hkl;
    g_fixLang = FixLangOf(hkl);
    g_fixLen = 0;  // a word never spans a layout change

    wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"";
    wchar_t code[4] = L"??";
    if (LCIDToLocaleName(MAKELCID(LOWORD((UINT_PTR)hkl), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) > 0) {
        int i = 0;
        for (; i < 3 && name[i] && name[i] != L'-'; i++) code[i] = (wchar_t)towupper(name[i]);
        code[i] = 0;
        if (i == 0) wcscpy(code, L"??");
    }
    if (wcscmp(code, g_code) == 0) return false;
    wcscpy(g_code, code);
    return true;
}

// ---------------------------------------------------------------------------
// Rendering (premultiplied BGRA, analytic anti-aliasing — no GDI+ / Direct2D)
// ---------------------------------------------------------------------------

struct Rgb { float r, g, b; };

static Rgb AccentFor(const wchar_t* code) {
    struct { const wchar_t* c; Rgb rgb; } known[] = {
        {L"EN", {0.20f, 0.68f, 0.40f}}, {L"TH", {0.17f, 0.45f, 0.95f}}, {L"JA", {0.90f, 0.25f, 0.30f}},
        {L"ZH", {0.95f, 0.55f, 0.10f}}, {L"KO", {0.58f, 0.35f, 0.90f}}, {L"RU", {0.10f, 0.65f, 0.75f}},
        {L"VI", {0.85f, 0.35f, 0.65f}}, {L"??", {0.55f, 0.55f, 0.58f}},
    };
    for (auto& k : known) if (wcscmp(k.c, code) == 0) return k.rgb;
    unsigned h = 5381;
    for (const wchar_t* p = code; *p; p++) h = h * 33 + *p;
    float hue = (h % 360) / 60.0f, s = 0.6f, v = 0.85f;
    float c = v * s, x = c * (1 - std::fabs(std::fmod(hue, 2.0f) - 1)), m = v - c;
    Rgb r[] = {{c, x, 0}, {x, c, 0}, {0, c, x}, {0, x, c}, {x, 0, c}, {c, 0, x}};
    Rgb o = r[(int)hue % 6];
    return {o.r + m, o.g + m, o.b + m};
}

static inline float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Signed distance from (px,py) to a rounded rectangle centred at (cx,cy).
static inline float SdRoundRect(float px, float py, float cx, float cy, float hw, float hh, float r) {
    float qx = std::fabs(px - cx) - (hw - r), qy = std::fabs(py - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return std::sqrt(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

struct Bitmap {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr, old = nullptr;
    DWORD* px = nullptr;
    int w = 0, h = 0;

    bool Create(int cw, int ch) {
        Destroy();
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cw;
        bi.bmiHeader.biHeight = -ch;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        dc = CreateCompatibleDC(nullptr);
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
        if (!bmp) { Destroy(); return false; }
        old = (HBITMAP)SelectObject(dc, bmp);
        w = cw; h = ch;
        return true;
    }
    void Destroy() {
        if (dc) { if (old) SelectObject(dc, old); DeleteDC(dc); }
        if (bmp) DeleteObject(bmp);
        dc = nullptr; bmp = old = nullptr; px = nullptr; w = h = 0;
    }
    ~Bitmap() { Destroy(); }
};

// Draws a badge for `code` into `out` (premultiplied BGRA).
// The badge occupies a bw×bh box centred in the bitmap; the rest is room for the shadow.
static void RenderBadge(Bitmap& out, const wchar_t* code, int style, int bw, int bh, float fontPx, bool shadow) {
    const Rgb accent = AccentFor(code);
    Rgb fill, text, border;
    float borderW = 0.75f, borderA = 0.22f;
    switch (style) {
        case STYLE_LIGHT: fill = {1, 1, 1}; text = {accent.r * 0.75f, accent.g * 0.75f, accent.b * 0.75f}; border = {0, 0, 0}; break;
        case STYLE_DARK: fill = {0.1f, 0.1f, 0.1f}; text = {1, 1, 1}; border = accent; borderW = 1.25f; borderA = 1; break;
        default: fill = accent; text = {1, 1, 1}; border = {0, 0, 0}; break;
    }
    const float fillA = style == STYLE_DARK ? 0.93f : (style == STYLE_LIGHT ? 0.96f : 1.0f);
    const float cx = out.w / 2.0f, cy = out.h / 2.0f, hw = bw / 2.0f, hh = bh / 2.0f;
    const bool dot = style == STYLE_DOT;
    const float radius = dot ? hw : hh;

    // Text mask: white text on black, grayscale anti-aliased.
    Bitmap mask;
    if (!dot && mask.Create(out.w, out.h)) {
        HFONT font = CreateFontW(-(int)std::lround(fontPx), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                 ANTIALIASED_QUALITY, 0, L"Segoe UI");
        HGDIOBJ of = SelectObject(mask.dc, font);
        SetBkMode(mask.dc, TRANSPARENT);
        SetTextColor(mask.dc, RGB(255, 255, 255));
        RECT r = {0, 0, out.w, out.h};
        DrawTextW(mask.dc, code, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        SelectObject(mask.dc, of);
        DeleteObject(font);
    }

    for (int y = 0; y < out.h; y++) {
        for (int x = 0; x < out.w; x++) {
            const float px = x + 0.5f, py = y + 0.5f;
            float r = 0, g = 0, b = 0, a = 0;  // premultiplied accumulator

            if (shadow) {
                float ds = SdRoundRect(px, py - 1.0f, cx, cy, hw, hh, radius);
                float sa = 0.28f * Clamp01(1.0f - (ds + 0.5f) / 2.5f);
                a = sa;  // black shadow
            }
            float d = SdRoundRect(px, py, cx, cy, hw, hh, radius);
            float cov = Clamp01(0.5f - d);
            if (cov > 0) {
                Rgb c;
                float ca;
                if (dot) {
                    // white ring + coloured core
                    float core = Clamp01(0.5f - (d + 1.5f));
                    c = {fill.r * core + (1 - core), fill.g * core + (1 - core), fill.b * core + (1 - core)};
                    ca = 1;
                } else {
                    float inner = Clamp01(0.5f - (d + borderW));
                    float bmix = (1 - inner) * borderA;  // how much border colour shows at this pixel
                    c = {fill.r * (1 - bmix) + border.r * bmix, fill.g * (1 - bmix) + border.g * bmix,
                         fill.b * (1 - bmix) + border.b * bmix};
                    ca = fillA + (1 - fillA) * (1 - inner);
                    if (mask.px) {
                        float t = (mask.px[y * out.w + x] & 0xFF) / 255.0f * inner;
                        c = {c.r * (1 - t) + text.r * t, c.g * (1 - t) + text.g * t, c.b * (1 - t) + text.b * t};
                        ca = ca * (1 - t) + t;
                    }
                }
                float sa = cov * ca;  // source-over onto shadow
                r = c.r * sa + r * (1 - sa);
                g = c.g * sa + g * (1 - sa);
                b = c.b * sa + b * (1 - sa);
                a = sa + a * (1 - sa);
            }
            out.px[y * out.w + x] = ((DWORD)std::lround(a * 255) << 24) | ((DWORD)std::lround(r * 255) << 16) |
                                    ((DWORD)std::lround(g * 255) << 8) | (DWORD)std::lround(b * 255);
        }
    }
}

// ---------------------------------------------------------------------------
// Badge window
// ---------------------------------------------------------------------------

#define WM_APP_MOVE (WM_APP + 1)
#define WM_APP_TRAY (WM_APP + 2)
#define WM_APP_FIX (WM_APP + 3)
enum { TIMER_POLL = 1, TIMER_HIDE = 2, TIMER_FADE = 3 };
enum {
    ID_HIDE_CURRENT = 100, ID_SHOW_ALL, ID_STARTUP, ID_AUTOFIX, ID_QUIT,
    ID_STYLE_BASE = 200, ID_MODE_BASE = 300, ID_SIZE_BASE = 400,
};

static HINSTANCE g_inst;
static HWND g_badge, g_host;
static HHOOK g_mouseHook, g_kbHook;
static HWINEVENTHOOK g_fgHook;
static UINT g_taskbarCreated;
static Bitmap g_img;
static UINT g_dpi = 96;
static int g_pad = 0;  // shadow margin in px
static POINT g_pt;
static volatile LONG g_movePending = 0;
static bool g_visible = false, g_wanted = false, g_fullscreen = false;
static BYTE g_alpha = 255;
static RECT g_monRect = {1, 1, 0, 0}, g_workRect;
static NOTIFYICONDATAW g_nid;
static HICON g_trayIcon;

static float Scale() { return kSizeScale[g_set.size] * g_dpi / 96.0f; }

static void Present(bool includePos) {
    if (!g_img.px) return;
    POINT src = {0, 0};
    SIZE sz = {g_img.w, g_img.h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, g_alpha, AC_SRC_ALPHA};
    POINT dst;
    if (includePos) {
        float s = Scale();
        int bw = g_img.w - 2 * g_pad, bh = g_img.h - 2 * g_pad;
        int x = g_pt.x + (int)(14 * s), y = g_pt.y + (int)(20 * s);
        if (x + bw > g_workRect.right) x = g_pt.x - (int)(6 * s) - bw;  // flip left
        if (y + bh > g_workRect.bottom) y = g_pt.y - (int)(8 * s) - bh;  // flip up
        dst = {x - g_pad, y - g_pad};
    }
    UpdateLayeredWindow(g_badge, nullptr, includePos ? &dst : nullptr, &sz, g_img.dc, &src, 0, &bf, ULW_ALPHA);
}

static void Rebuild() {
    float s = Scale();
    bool dot = g_set.style == STYLE_DOT;
    int bw = (int)std::lround((dot ? 12 : (wcslen(g_code) > 2 ? 30 : 24)) * s);
    int bh = (int)std::lround((dot ? 12 : 17) * s);
    g_pad = (int)std::ceil(3 * g_dpi / 96.0f);
    if (g_img.Create(bw + 2 * g_pad, bh + 2 * g_pad))
        RenderBadge(g_img, g_code, g_set.style, bw, bh, 11.5f * s, true);
}

// Tray icon showing the current language.
static void UpdateTrayIcon() {
    int size = GetSystemMetrics(SM_CXSMICON);
    Bitmap bmp;
    if (!bmp.Create(size, size)) return;
    RenderBadge(bmp, g_code, g_set.style == STYLE_DOT ? (int)STYLE_COLOR : (int)g_set.style, size, (int)(size * 0.8f),
                size * (wcslen(g_code) > 2 ? 0.48f : 0.62f), false);
    // Icons want straight (non-premultiplied) alpha.
    for (int i = 0; i < size * size; i++) {
        DWORD p = bmp.px[i];
        BYTE a = p >> 24;
        if (a && a < 255) {
            auto un = [a](DWORD c) { return (DWORD)std::min(255, (int)(c * 255 / a)); };
            p = ((DWORD)a << 24) | (un((p >> 16) & 0xFF) << 16) | (un((p >> 8) & 0xFF) << 8) | un(p & 0xFF);
            bmp.px[i] = p;
        }
    }
    HBITMAP maskBmp = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {TRUE, 0, 0, maskBmp, bmp.bmp};
    SelectObject(bmp.dc, bmp.old);  // a DIB must not be selected while used for an icon
    bmp.old = nullptr;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(maskBmp);
    if (!icon) return;

    g_nid.hIcon = icon;
    swprintf(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"CursorLingo — %ls", g_code);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    g_trayIcon = icon;
}

static void UpdateMonitor(POINT pt) {
    if (PtInRect(&g_monRect, pt)) return;
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    g_monRect = mi.rcMonitor;
    g_workRect = mi.rcWork;
    UINT dx = 96, dy = 96;
    if (SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) && dx != g_dpi) {
        g_dpi = dx;
        Rebuild();
    }
}

static void SetVisible(bool on, bool fade) {
    KillTimer(g_badge, TIMER_FADE);
    if (on) {
        g_alpha = 255;
        if (!g_visible) {
            g_visible = true;
            GetCursorPos(&g_pt);
            UpdateMonitor(g_pt);
            Present(true);
            ShowWindow(g_badge, SW_SHOWNOACTIVATE);
        } else {
            Present(false);
        }
    } else if (g_visible) {
        if (fade) { SetTimer(g_badge, TIMER_FADE, 16, nullptr); return; }
        g_visible = false;
        ShowWindow(g_badge, SW_HIDE);
    }
}

static bool ForegroundIsFullscreen() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == GetDesktopWindow() || fg == GetShellWindow()) return false;
    wchar_t cls[32];
    if (GetClassNameW(fg, cls, 32) && (wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Progman") == 0)) return false;
    RECT wr;
    GetWindowRect(fg, &wr);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi);
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
           wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

// Re-evaluates language + visibility. `switched` means the user just changed language.
static void Refresh(bool switched) {
    if (switched) {
        Rebuild();
        UpdateTrayIcon();
    }
    KillTimer(g_badge, TIMER_HIDE);
    g_wanted = !IsHidden(g_code) && !g_fullscreen;
    if (!g_wanted) { SetVisible(false, switched); return; }
    if (g_set.mode == MODE_ALWAYS) {
        SetVisible(true, false);
    } else if (switched) {
        SetVisible(true, false);
        SetTimer(g_badge, TIMER_HIDE, 1200, nullptr);
    } else {
        SetVisible(false, false);
    }
}

// ---------------------------------------------------------------------------
// Auto-fix wrong language
// ---------------------------------------------------------------------------
//
// A low-level keyboard hook records the *physical keys* of the current word (scan codes, so the
// layout does not matter). At a space the word is judged by core/layoutfix.h; when it was typed in the
// wrong layout, the word and the space are erased and typed again from the other layout's reading of
// the same keys, and the layout is switched so typing continues correctly.

static uint8_t g_fixKeys[lf::kMaxWord];
static uint8_t g_fixWord[lf::kMaxWord];  // hand-off to the message loop
static int g_fixWordLen = 0;

static bool FocusIsPassword() {
    HWND fg = GetForegroundWindow();
    GUITHREADINFO gti = {sizeof(gti)};
    if (!fg || !GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gti) || !gti.hwndFocus) return false;
    wchar_t cls[16];
    if (!GetClassNameW(gti.hwndFocus, cls, 16) || _wcsicmp(cls, L"Edit") != 0) return false;
    return (GetWindowLongPtrW(gti.hwndFocus, GWL_STYLE) & ES_PASSWORD) != 0;
}

static bool ForegroundIsTerminal() {
    wchar_t cls[48];
    HWND fg = GetForegroundWindow();
    if (!fg || !GetClassNameW(fg, cls, 48)) return false;
    return wcscmp(cls, L"ConsoleWindowClass") == 0 || wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0 ||
           wcscmp(cls, L"mintty") == 0;
}

static LRESULT CALLBACK KeyboardHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lp;
        if (k->flags & LLKHF_INJECTED) return CallNextHookEx(nullptr, code, wp, lp);  // ours or another tool's

        if (wp == WM_SYSKEYDOWN || (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) ||
            (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
            g_fixLen = 0;  // shortcut
        } else if (k->vkCode == VK_BACK) {
            if (g_fixLen > 0) g_fixLen--;
        } else if (k->vkCode == VK_SPACE) {
            int n = g_fixLen;
            g_fixLen = 0;
            if (n >= 3 && g_fixLang >= 0 && !ForegroundIsTerminal() &&
                lf::shouldConvert(g_fixKeys, n, (lf::Lang)g_fixLang, true)) {
                memcpy(g_fixWord, g_fixKeys, n);
                g_fixWordLen = n;
                PostMessageW(g_host, WM_APP_FIX, 1, 0);  // 1 = also re-type the space
            }
        } else {
            int idx = (k->flags & LLKHF_EXTENDED) ? -1 : lf::keyIndexFrom(lf::kWinScan, (int)k->scanCode);
            if (idx < 0 || g_fixLen >= lf::kMaxWord) {
                g_fixLen = 0;
            } else {
                if (g_fixLen == 0) {
                    g_fixLang = FixLangOf(CurrentHkl());  // fresh layout at the start of each word
                    if (g_fixLang < 0 || FocusIsPassword()) return CallNextHookEx(nullptr, code, wp, lp);
                }
                g_fixKeys[g_fixLen++] = (uint8_t)(idx | ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? lf::kShiftBit : 0));
                // A long run without spaces (typical for Thai) is checked as it grows, so it is fixed early.
                if (g_fixLang == 0 && g_fixLen >= 6 && lf::shouldConvert(g_fixKeys, g_fixLen, lf::LANG_EN, false)) {
                    memcpy(g_fixWord, g_fixKeys, g_fixLen);
                    g_fixWordLen = g_fixLen;
                    g_fixLen = 0;
                    PostMessageW(g_host, WM_APP_FIX, 0, 0);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void SetAutoFix(bool on) {
    g_fixLen = 0;
    if (on && !g_kbHook) g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, g_inst, 0);
    if (!on && g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
}

static HKL FindLayout(int lang) {
    HKL list[64];
    int n = GetKeyboardLayoutList(64, list);
    WORD want = lang == 0 ? 0x0409 : 0x041E;
    for (int i = 0; i < n; i++)
        if (FixLangOf(list[i]) == lang && LOWORD((UINT_PTR)list[i]) == want) return list[i];
    return nullptr;
}

// Runs from the message loop (never inside the hook): erase the word, type the other reading, switch layout.
static void ApplyFix(bool trailingSpace) {
    const int n = g_fixWordLen;
    if (n <= 0 || g_fixLang < 0) return;
    const int target = 1 - g_fixLang;
    char16_t text[lf::kMaxWord + 2];
    lf::render(g_fixWord, n, target ? lf::LANG_TH : lf::LANG_EN, text);
    int len = n;
    if (trailingSpace) text[len++] = u' ';
    const int erase = n + (trailingSpace ? 1 : 0);

    // Switch first so keys typed in the meantime already land in the right layout.
    if (HKL hkl = FindLayout(target)) {
        HWND fg = GetForegroundWindow();
        if (fg) PostMessageW(fg, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)hkl);
    }

    INPUT in[(lf::kMaxWord + 2) * 4] = {};
    int c = 0;
    for (int i = 0; i < erase; i++)
        for (int up = 0; up < 2; up++) {
            in[c].type = INPUT_KEYBOARD;
            in[c].ki.wVk = VK_BACK;
            in[c].ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
            c++;
        }
    for (int i = 0; i < len; i++)
        for (int up = 0; up < 2; up++) {
            in[c].type = INPUT_KEYBOARD;
            in[c].ki.wScan = (WORD)text[i];
            in[c].ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
            c++;
        }
    SendInput((UINT)c, in, sizeof(INPUT));
    g_fixWordLen = 0;
}

static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) g_fixLen = 0;  // caret moved
    if (code == HC_ACTION && wp == WM_MOUSEMOVE && g_visible) {
        g_pt = ((MSLLHOOKSTRUCT*)lp)->pt;
        // Coalesce: at most one pending move message, so fast mice never flood the queue.
        if (!InterlockedExchange(&g_movePending, 1)) PostMessageW(g_badge, WM_APP_MOVE, 0, 0);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void CALLBACK ForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    g_fullscreen = ForegroundIsFullscreen();
    g_fixLen = 0;
    bool changed = UpdateLanguage();
    Refresh(changed);
    if (g_visible) SetWindowPos(g_badge, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void AddTrayIcon() {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_host;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP));
    wcscpy(g_nid.szTip, L"CursorLingo");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    UpdateTrayIcon();
}

static void ShowMenu() {
    HMENU m = CreatePopupMenu(), style = CreatePopupMenu(), mode = CreatePopupMenu(), size = CreatePopupMenu();
    wchar_t buf[64];
    swprintf(buf, 64, L"Current input: %ls", g_code);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, buf);
    swprintf(buf, 64, L"Hide badge for %ls", g_code);
    AppendMenuW(m, MF_STRING | (IsHidden(g_code) ? MF_CHECKED : 0), ID_HIDE_CURRENT, buf);
    if (g_set.hidden[0]) AppendMenuW(m, MF_STRING, ID_SHOW_ALL, L"Show for all languages");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < MODE_COUNT; i++) AppendMenuW(mode, MF_STRING | (g_set.mode == (DWORD)i ? MF_CHECKED : 0), ID_MODE_BASE + i, kModeNames[i]);
    for (int i = 0; i < STYLE_COUNT; i++) AppendMenuW(style, MF_STRING | (g_set.style == (DWORD)i ? MF_CHECKED : 0), ID_STYLE_BASE + i, kStyleNames[i]);
    for (int i = 0; i < SIZE_COUNT; i++) AppendMenuW(size, MF_STRING | (g_set.size == (DWORD)i ? MF_CHECKED : 0), ID_SIZE_BASE + i, kSizeNames[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mode, L"Show");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)style, L"Style");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)size, L"Size");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_set.autoFix ? MF_CHECKED : 0), ID_AUTOFIX, L"Auto-fix wrong language (EN \u21C4 TH)");
    AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit CursorLingo");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_host);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_host, nullptr);
    DestroyMenu(m);  // destroys submenus too

    if (!cmd) return;
    if (cmd == ID_QUIT) { DestroyWindow(g_host); return; }
    if (cmd == ID_STARTUP) { SetStartup(!StartupEnabled()); return; }
    if (cmd == ID_AUTOFIX) {
        g_set.autoFix = !g_set.autoFix;
        SetAutoFix(g_set.autoFix != 0);
        SaveSettings();
        return;
    }
    if (cmd == ID_HIDE_CURRENT) ToggleHidden(g_code);
    else if (cmd == ID_SHOW_ALL) g_set.hidden[0] = 0;
    else if (cmd >= ID_STYLE_BASE && cmd < ID_STYLE_BASE + STYLE_COUNT) g_set.style = cmd - ID_STYLE_BASE;
    else if (cmd >= ID_MODE_BASE && cmd < ID_MODE_BASE + MODE_COUNT) g_set.mode = cmd - ID_MODE_BASE;
    else if (cmd >= ID_SIZE_BASE && cmd < ID_SIZE_BASE + SIZE_COUNT) g_set.size = cmd - ID_SIZE_BASE;
    SaveSettings();
    Refresh(true);
}

static LRESULT CALLBACK BadgeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_MOVE:
            InterlockedExchange(&g_movePending, 0);
            if (g_visible) {
                UpdateMonitor(g_pt);
                Present(true);
            }
            return 0;
        case WM_TIMER:
            if (wp == TIMER_POLL) {
                // There is no system-wide "layout changed" event, so poll this one cheap call.
                if (UpdateLanguage()) Refresh(true);
            } else if (wp == TIMER_HIDE) {
                KillTimer(h, TIMER_HIDE);
                SetVisible(false, true);
            } else if (wp == TIMER_FADE) {
                g_alpha = g_alpha > 40 ? g_alpha - 40 : 0;
                if (g_alpha == 0) {
                    KillTimer(h, TIMER_FADE);
                    g_visible = false;
                    ShowWindow(h, SW_HIDE);
                    g_alpha = 255;
                } else {
                    Present(false);
                }
            }
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK HostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated) { AddTrayIcon(); return 0; }  // Explorer restarted
    switch (msg) {
        case WM_APP_TRAY:
            if (LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT) ShowMenu();
            return 0;
        case WM_APP_FIX:
            ApplyFix(wp != 0);
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            g_monRect = {1, 1, 0, 0};  // force monitor re-query
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\CursorLingo.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_inst = inst;
    LoadSettings();

    WNDCLASSW wc = {};
    wc.hInstance = inst;
    wc.lpfnWndProc = BadgeProc;
    wc.lpszClassName = L"CursorLingoBadge";
    RegisterClassW(&wc);
    wc.lpfnWndProc = HostProc;
    wc.lpszClassName = L"CursorLingoHost";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);

    g_host = CreateWindowExW(0, L"CursorLingoHost", kAppName, 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_badge = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              L"CursorLingoBadge", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    GetCursorPos(&g_pt);
    UpdateMonitor(g_pt);
    UpdateLanguage();
    Rebuild();
    AddTrayIcon();
    Refresh(false);

    // Portable use: if the exe was moved, keep "Start with Windows" pointing at its current location.
    if (StartupEnabled()) SetStartup(true);

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, inst, 0);
    if (g_set.autoFix) SetAutoFix(true);
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundChanged, 0, 0,
                               WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // Coalescable timer lets Windows batch our wake-ups with others to save power.
    SetCoalescableTimer(g_badge, TIMER_POLL, 200, nullptr, 100);

    // Release start-up pages we no longer need.
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);

    UnhookWindowsHookEx(g_mouseHook);
    SetAutoFix(false);
    UnhookWinEvent(g_fgHook);
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    ReleaseMutex(mutex);
    return 0;
}
