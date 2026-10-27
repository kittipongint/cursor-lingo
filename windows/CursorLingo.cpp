// CursorLingo for Windows — shows the current keyboard input language next to the mouse pointer.
//
// Plain Win32 + GDI, no runtime dependencies. Footprint goals:
//   * The badge follows the mouse via a low-level mouse hook (event-driven, coalesced).
//   * Rendering happens only when the language, style, size or monitor DPI changes.
//   * The badge is a per-pixel-alpha layered window, so it is anti-aliased and click-through.
//   * The active layout is read with GetKeyboardLayout(). A keyboard hook exists only while the optional
//     "Fix wrong language" feature is switched on (off by default); it keeps nothing but the current
//     and last word in memory (plus the personal word list the user teaches it), and never touches the network.
//   * Runs as a normal user: no administrator rights, no installer, no services, no registry. Settings are
//     plain files in %APPDATA%\CursorLingo; "Start with Windows" is a shortcut in the user's Startup folder.

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
#include <wtsapi32.h>
#include <shlobj.h>
#include <shobjidl.h>
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

// Menu text in English [0] and Thai [1]. Thai literals need the compiler to read this file as UTF-8 (/utf-8).
static_assert(sizeof(L"ไทย") / sizeof(wchar_t) == 4, "compile with /utf-8 (MSVC) so Thai text is read correctly");
static const wchar_t* kStyleNames[2][4] = {{L"Color", L"Light", L"Dark", L"Dot"}, {L"สีสด", L"โทนสว่าง", L"โทนมืด", L"จุดเล็ก"}};
static const wchar_t* kModeNames[2][2] = {{L"Always", L"Only when switching"}, {L"ตลอดเวลา", L"เฉพาะตอนเปลี่ยนภาษา"}};
static const wchar_t* kSizeNames[2][3] = {{L"Small", L"Medium", L"Large"}, {L"เล็ก", L"กลาง", L"ใหญ่"}};
static const wchar_t* kFixNames[2][3] = {{L"Off", L"Double Shift only", L"Automatic"},
                                         {L"ปิด", L"กด Shift 2 ครั้งเพื่อแก้", L"แก้ให้อัตโนมัติ"}};
static const wchar_t* kUiLangNames[2][3] = {{L"System", L"English", L"ไทย"}, {L"ตามภาษาของระบบ", L"English", L"ไทย"}};
static const float kSizeScale[] = {0.85f, 1.0f, 1.3f};

static const wchar_t kAppName[] = L"CursorLingo";

struct Settings {
    DWORD style = STYLE_COLOR;
    DWORD mode = MODE_ALWAYS;
    DWORD size = SIZE_MEDIUM;
    DWORD autoFix = 0;          // re-type words typed in the wrong layout (EN <-> TH)
    DWORD fixAsYouType = 1;     // with autoFix: fix by itself (1), or only on double Shift (0)
    DWORD codeEditorsManual = 1;  // code editors: double Shift only
    DWORD perAppLanguage = 0;   // switch to the layout each app was last used with
    DWORD uiLang = 0;           // menu language: 0 = Windows display language, 1 = English, 2 = Thai
    wchar_t hidden[256] = L"";  // comma separated codes, e.g. ",EN,JA,"
} g_set;

// ---------------------------------------------------------------------------
// Settings storage: plain files in the user's profile, never the registry
// ---------------------------------------------------------------------------
//
// CursorLingo.ini holds the settings and the per-app layouts, CursorLingo.words the personal word list. They live in
// %APPDATA%\CursorLingo, or next to the exe when a CursorLingo.ini is already there (portable use). Nothing here
// needs administrator rights, and deleting the folder resets everything.

static wchar_t g_ini[MAX_PATH], g_wordsFile[MAX_PATH];

struct SettingField {
    const wchar_t* name;
    DWORD* value;
    DWORD def, max;
};
static const SettingField kFields[] = {
    {L"Style", &g_set.style, STYLE_COLOR, STYLE_COUNT},
    {L"Mode", &g_set.mode, MODE_ALWAYS, MODE_COUNT},
    {L"Size", &g_set.size, SIZE_MEDIUM, SIZE_COUNT},
    {L"AutoFix", &g_set.autoFix, 0, 2},
    {L"FixAsYouType", &g_set.fixAsYouType, 1, 2},
    {L"CodeEditorsManual", &g_set.codeEditorsManual, 1, 2},
    {L"PerAppLanguage", &g_set.perAppLanguage, 0, 2},
    {L"UILanguage", &g_set.uiLang, 0, 3},
};

static void FindSettingsFiles() {
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) *slash = 0;
    swprintf(g_ini, MAX_PATH, L"%ls\\CursorLingo.ini", dir);
    if (GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES) {
        wchar_t* appData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
            swprintf(dir, MAX_PATH, L"%ls\\CursorLingo", appData);
            CreateDirectoryW(dir, nullptr);
        }
        CoTaskMemFree(appData);
        swprintf(g_ini, MAX_PATH, L"%ls\\CursorLingo.ini", dir);
    }
    swprintf(g_wordsFile, MAX_PATH, L"%ls\\CursorLingo.words", dir);
}

// The profile functions write UTF-16 only into a file that already starts with a UTF-16 byte order mark.
static void EnsureIni() {
    HANDLE f = CreateFileW(g_ini, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;  // already there
    const WORD bom = 0xFEFF;
    DWORD written;
    WriteFile(f, &bom, sizeof(bom), &written, nullptr);
    CloseHandle(f);
}

static void LoadSettings() {
    for (const SettingField& f : kFields) {
        UINT v = GetPrivateProfileIntW(L"Settings", f.name, (INT)f.def, g_ini);
        *f.value = v < f.max ? v : f.def;
    }
    GetPrivateProfileStringW(L"Settings", L"Hidden", L"", g_set.hidden, 256, g_ini);
}

static void SaveSettings() {
    EnsureIni();
    wchar_t v[16];
    for (const SettingField& f : kFields) {
        swprintf(v, 16, L"%lu", (unsigned long)*f.value);
        WritePrivateProfileStringW(L"Settings", f.name, v, g_ini);
    }
    WritePrivateProfileStringW(L"Settings", L"Hidden", g_set.hidden, g_ini);
}

// 1 when the menu speaks Thai, 0 for English.
static int UiThai() {
    if (g_set.uiLang) return g_set.uiLang == 2;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_THAI;
}
#define TR(en, th) (UiThai() ? (th) : (en))

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

// "Start with Windows" is a shortcut in the user's Startup folder: no registry entry, and it shows up (and can be
// switched off) in Task Manager's Startup apps like any other.
static bool StartupLinkPath(wchar_t* out) {
    wchar_t* folder = nullptr;
    bool ok = SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &folder));
    if (ok) swprintf(out, MAX_PATH, L"%ls\\CursorLingo.lnk", folder);
    CoTaskMemFree(folder);
    return ok;
}

static bool StartupEnabled() {
    wchar_t link[MAX_PATH];
    return StartupLinkPath(link) && GetFileAttributesW(link) != INVALID_FILE_ATTRIBUTES;
}

static void SetStartup(bool on) {
    wchar_t link[MAX_PATH], exe[MAX_PATH], dir[MAX_PATH];
    if (!StartupLinkPath(link)) return;
    if (!on) { DeleteFileW(link); return; }
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wcscpy(dir, exe);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) *slash = 0;
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    sl->SetPath(exe);
    sl->SetWorkingDirectory(dir);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        pf->Save(link, TRUE);
        pf->Release();
    }
    sl->Release();
}

// Versions up to 1.1.4 kept their settings under HKCU\Software\CursorLingo and started from the Run key. Carry them
// over into the files once, then remove what they left in the registry.
static void MigrateFromRegistry() {
    HKEY k;
    if (GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES &&
        RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\CursorLingo", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        for (const SettingField& f : kFields) {
            DWORD v = 0, cb = sizeof(v);
            if (RegQueryValueExW(k, f.name, nullptr, nullptr, (BYTE*)&v, &cb) == ERROR_SUCCESS && v < f.max) *f.value = v;
        }
        DWORD cb = sizeof(g_set.hidden) - sizeof(wchar_t);
        if (RegQueryValueExW(k, L"Hidden", nullptr, nullptr, (BYTE*)g_set.hidden, &cb) != ERROR_SUCCESS) g_set.hidden[0] = 0;
        g_set.hidden[255] = 0;
        RegCloseKey(k);
        SaveSettings();
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\CursorLingo");
    }
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ | KEY_WRITE, &k) ==
        ERROR_SUCCESS) {
        if (RegQueryValueExW(k, kAppName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            SetStartup(true);
            RegDeleteValueW(k, kAppName);
        }
        RegCloseKey(k);
    }
}

// ---------------------------------------------------------------------------
// Input language
// ---------------------------------------------------------------------------

static HKL g_lastHkl = nullptr;
static wchar_t g_code[4] = L"??";
static int g_fixLang = -1;     // layout the auto-fixer understands: 0 = US English, 1 = Thai (Kedmanee), -1 = other
static int g_expectLang = -1;  // a layout switch the auto-fixer made itself: it must not end the word

// A word on screen (see the auto-fix section): its physical keys, the layout it is shown in, the layout it was
// typed in, and how many spaces follow it. `decided` stops the detector from touching it again.
struct FixWord {
    uint8_t keys[lf::kMaxWord];
    int n, shown, typed, trailing;
    bool decided;
};
static FixWord g_word, g_last;  // being typed; finished with nothing but spaces after it (n == 0: none)
static void ResetWords() { g_word.n = 0; g_last.n = 0; }

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
    if (g_expectLang >= 0 && g_fixLang == g_expectLang) g_expectLang = -1;  // our own switch
    else if (!(g_word.n && g_word.shown == g_fixLang)) ResetWords();         // a word never spans the user's

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
#define WM_APP_SELECTION (WM_APP + 4)
#define WM_APP_SAVE_WORDS (WM_APP + 5)
enum { TIMER_POLL = 1, TIMER_HIDE = 2, TIMER_FADE = 3, TIMER_COPY = 4, TIMER_RESTORE = 5 };
enum {
    ID_HIDE_CURRENT = 100, ID_SHOW_ALL, ID_STARTUP, ID_QUIT, ID_CODE_EDITORS, ID_REPORT, ID_FORGET_WORDS, ID_PER_APP,
    ID_FIX_BASE = 150, ID_UILANG_BASE = 160,
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
    swprintf(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"CursorLingo \u2014 %ls", g_code);
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
//
// Double-tapping Shift converts the word being typed, or else the last word, by hand. Flipping a word back
// to the layout it was typed in undoes a fix, and that word is left alone for the rest of the session.
// With no word to work on, it converts the selected text instead (read with Ctrl+C; the clipboard is put back).

static FixWord g_jobs[4];  // hand-off to the message loop: what is on screen, to be flipped
static int g_jobHead = 0, g_jobTail = 0;
// The personal word list: words the user flipped with double Shift. In layout `shown`, always convert the word
// (`fix`), or never. Saved in CursorLingo.words so it survives restarts; "Forget learned words" clears it.
struct Rule {
    uint8_t n, shown, fix;
    uint8_t keys[lf::kMaxWord];
};
static Rule g_rules[500];
static int g_ruleCount = 0;
static bool g_shiftDown = false, g_shiftClean = false;
static DWORD g_shiftDownAt = 0, g_lastShiftTap = 0;

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

static wchar_t g_fgExe[64];  // file name of the foreground app's exe, lower case ("" if unknown)

static void ReadForegroundExe() {
    g_fgExe[0] = 0;
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg || !GetWindowThreadProcessId(fg, &pid)) return;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return;
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(p, 0, path, &n) != 0;
    CloseHandle(p);
    if (!ok) return;
    const wchar_t* name = wcsrchr(path, L'\\');
    wcsncpy(g_fgExe, name ? name + 1 : path, 63);
    g_fgExe[63] = 0;
    for (wchar_t* c = g_fgExe; *c; c++) *c = (wchar_t)towlower(*c);
}

// Identifiers often look like gibberish, so code editors are left to double Shift (when that option is on).
static bool ForegroundIsCodeEditor() {
    static const wchar_t* kEditors[] = {
        L"code.exe", L"code - insiders.exe", L"vscodium.exe", L"cursor.exe", L"windsurf.exe", L"zed.exe",
        L"devenv.exe", L"sublime_text.exe", L"notepad++.exe", L"studio64.exe", L"idea64.exe", L"pycharm64.exe",
        L"webstorm64.exe", L"phpstorm64.exe", L"clion64.exe", L"goland64.exe", L"rider64.exe", L"rubymine64.exe",
        L"datagrip64.exe", L"rustrover64.exe", L"fleet.exe", L"gvim.exe"};
    for (const wchar_t* e : kEditors)
        if (wcscmp(g_fgExe, e) == 0) return true;
    return false;
}

static bool g_inTerminal = false, g_inCodeEditor = false;  // the foreground app, refreshed when it changes

static void ClassifyForeground() {
    ReadForegroundExe();
    g_inTerminal = ForegroundIsTerminal();
    g_inCodeEditor = ForegroundIsCodeEditor();
}

// May this word be fixed without being asked to (double Shift always works)?
static bool AutoAllowed() {
    return g_set.fixAsYouType && !g_inTerminal && !(g_set.codeEditorsManual && g_inCodeEditor);
}

// The rule for this word in layout `shown` (or, with `prefix`, a keep rule for a word starting like it).
static const Rule* FindRule(const FixWord& w, bool prefix) {
    for (int i = 0; i < g_ruleCount; i++) {
        const Rule& r = g_rules[i];
        if (r.shown != w.shown || (prefix ? r.fix || r.n < w.n : r.n != w.n)) continue;
        if (memcmp(r.keys, w.keys, w.n) == 0) return &r;
    }
    return nullptr;
}
static bool IsKept(const FixWord& w, bool prefix) {
    const Rule* r = FindRule(w, prefix);
    return r && !r->fix;
}

// A word the user just flipped with double Shift. Back to the layout it was typed in: they want it as typed, so
// it is never fixed again. Into the other layout: fix it by itself from now on.
static void Learn(const FixWord& w) {
    bool fix = w.shown != w.typed;
    if (w.n <= 0 || (fix && w.n < 2)) return;
    FixWord typed = w;
    typed.shown = w.typed;
    for (int i = 0; i < g_ruleCount; i++)
        if (g_rules[i].shown == typed.shown && g_rules[i].n == w.n && memcmp(g_rules[i].keys, w.keys, w.n) == 0) {
            memmove(g_rules + i, g_rules + i + 1, sizeof(Rule) * (g_ruleCount - i - 1));
            g_ruleCount--;
            break;
        }
    if (g_ruleCount == 500) { memmove(g_rules, g_rules + 1, sizeof(Rule) * 499); g_ruleCount--; }
    Rule& r = g_rules[g_ruleCount++];
    r.n = (uint8_t)w.n;
    r.shown = (uint8_t)typed.shown;
    r.fix = fix;
    memcpy(r.keys, w.keys, w.n);
    PostMessageW(g_host, WM_APP_SAVE_WORDS, 0, 0);  // not from inside the hook
}

// Stored as a small binary file: per word, its length, (layout | fix << 1), then the keys.
static void SaveRules() {
    static uint8_t buf[500 * (lf::kMaxWord + 2)];
    DWORD len = 0;
    for (int i = 0; i < g_ruleCount; i++) {
        buf[len++] = g_rules[i].n;
        buf[len++] = (uint8_t)(g_rules[i].shown | (g_rules[i].fix << 1));
        memcpy(buf + len, g_rules[i].keys, g_rules[i].n);
        len += g_rules[i].n;
    }
    if (!len) { DeleteFileW(g_wordsFile); return; }
    HANDLE f = CreateFileW(g_wordsFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(f, buf, len, &written, nullptr);
    CloseHandle(f);
}

static void LoadRules() {
    static uint8_t buf[500 * (lf::kMaxWord + 2)];
    DWORD len = 0;
    g_ruleCount = 0;
    HANDLE f = CreateFileW(g_wordsFile, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    bool ok = ReadFile(f, buf, sizeof(buf), &len, nullptr) != 0;
    CloseHandle(f);
    if (!ok) return;
    for (DWORD i = 0; i + 2 <= len && g_ruleCount < 500;) {
        uint8_t n = buf[i], flags = buf[i + 1];
        if (n == 0 || n > lf::kMaxWord || i + 2 + n > len) break;
        Rule& r = g_rules[g_ruleCount++];
        r.n = n;
        r.shown = flags & 1;
        r.fix = (flags >> 1) & 1;
        memcpy(r.keys, buf + i + 2, n);
        i += 2 + n;
    }
}

// Queues `w` (as it is on screen now) to be flipped to the other layout, and records the flip.
static void QueueFlip(FixWord& w) {
    int next = (g_jobTail + 1) % 4;
    if (next == g_jobHead) return;  // the user is faster than the message loop: drop it
    g_jobs[g_jobTail] = w;
    g_jobTail = next;
    w.shown = 1 - w.shown;
    w.decided = true;
    if (g_fixLang != w.shown) g_expectLang = w.shown;
    PostMessageW(g_host, WM_APP_FIX, 0, 0);
}

static void ToggleWord() {
    FixWord* w = g_word.n ? &g_word : (g_last.n ? &g_last : nullptr);
    if (!w) { PostMessageW(g_host, WM_APP_SELECTION, 0, 0); return; }  // nothing typed: convert the selection
    if (g_fixLang < 0) return;
    QueueFlip(*w);
    Learn(*w);
}

static void EndOfWord() {
    FixWord w = g_word;
    w.trailing = 1;
    g_word.n = 0;
    if (!w.decided && w.n >= 2 && w.shown >= 0 && AutoAllowed() && !IsKept(w, false) &&
        (FindRule(w, false) || lf::shouldConvert(w.keys, w.n, (lf::Lang)w.shown, true)))
        QueueFlip(w);
    g_last = w;
}

static LRESULT CALLBACK KeyboardHook(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
    const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lp;
    if (k->flags & LLKHF_INJECTED) return CallNextHookEx(nullptr, code, wp, lp);  // ours or another tool's
    const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;

    if (k->vkCode == VK_LSHIFT || k->vkCode == VK_RSHIFT || k->vkCode == VK_SHIFT) {
        if (down) {
            if (!g_shiftDown) { g_shiftDown = true; g_shiftClean = true; g_shiftDownAt = k->time; }  // not repeats
        } else {
            g_shiftDown = false;
            if (g_shiftClean && k->time - g_shiftDownAt < 350) {  // a tap, not a hold
                if (g_lastShiftTap && k->time - g_lastShiftTap < 450) { g_lastShiftTap = 0; ToggleWord(); }
                else g_lastShiftTap = k->time;
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    if (!down) return CallNextHookEx(nullptr, code, wp, lp);
    g_shiftClean = false;
    g_lastShiftTap = 0;

    if (wp == WM_SYSKEYDOWN || (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) ||
        (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
        ResetWords();  // shortcut
    } else if (k->vkCode == VK_BACK) {
        if (g_word.n) g_word.n--;
        else g_last.n = 0;
    } else if (k->vkCode == VK_SPACE) {
        if (g_word.n) EndOfWord();
        else if (g_last.n && ++g_last.trailing > 8) g_last.n = 0;
    } else {
        int idx = (k->flags & LLKHF_EXTENDED) ? -1 : lf::keyIndexFrom(lf::kWinScan, (int)k->scanCode);
        if (idx < 0 || g_word.n >= lf::kMaxWord) {
            ResetWords();
        } else {
            if (g_word.n == 0) {
                g_last.n = 0;
                g_fixLang = FixLangOf(CurrentHkl());  // fresh layout at the start of each word
                if (g_fixLang < 0 || FocusIsPassword()) return CallNextHookEx(nullptr, code, wp, lp);
                g_word = FixWord{};
                g_word.shown = g_word.typed = g_fixLang;
            }
            g_word.keys[g_word.n++] = (uint8_t)(idx | ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? lf::kShiftBit : 0));
            // A long run without spaces (typical for Thai) is checked as it grows, so it is fixed early.
            if (!g_word.decided && g_word.shown == 0 && g_word.n >= 6 && AutoAllowed() && !IsKept(g_word, true) &&
                lf::shouldConvert(g_word.keys, g_word.n, lf::LANG_EN, false))
                QueueFlip(g_word);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void SetAutoFix(bool on) {
    ResetWords();
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
static void ApplyFlip(const FixWord& w) {
    if (w.n <= 0 || w.shown < 0) return;
    const int target = 1 - w.shown;
    char16_t text[lf::kMaxWord + 10];
    lf::render(w.keys, w.n, target ? lf::LANG_TH : lf::LANG_EN, text);
    int len = w.n;
    for (int i = 0; i < w.trailing && len < lf::kMaxWord + 9; i++) text[len++] = u' ';
    const int erase = len;

    // Switch first so keys typed in the meantime already land in the right layout.
    if (HKL hkl = FindLayout(target)) {
        HWND fg = GetForegroundWindow();
        if (fg) PostMessageW(fg, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)hkl);
    }

    INPUT in[(lf::kMaxWord + 10) * 4] = {};
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
}

static void ApplyFlips() {
    while (g_jobHead != g_jobTail) {
        ApplyFlip(g_jobs[g_jobHead]);
        g_jobHead = (g_jobHead + 1) % 4;
    }
}

// ---------------------------------------------------------------------------
// Converting a selection (double Shift with no word to work on)
// ---------------------------------------------------------------------------
//
// Win32 has no common way to read another app's selection, so it is copied with Ctrl+C, converted by
// core/layoutfix.h, and pasted back with Ctrl+V. The user's clipboard is saved first and restored after.

static struct {
    UINT format[32];
    HGLOBAL data[32];
    int count;
    DWORD seq;
    int tries;
    bool saved;
} g_clip;

static void SendChord(WORD vk) {  // Ctrl+<vk>
    INPUT in[4] = {};
    for (int i = 0; i < 4; i++) in[i].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].ki.wVk = vk;
    in[2].ki.wVk = vk;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

static bool OpenClipboardRetry() {
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(g_host)) return true;
        Sleep(10);
    }
    return false;
}

static void FreeSavedClipboard() {
    for (int i = 0; i < g_clip.count; i++) GlobalFree(g_clip.data[i]);
    g_clip.count = 0;
    g_clip.saved = false;
}

// Copies every memory-based clipboard format (text, HTML, files, images as DIB, ...).
static void SaveClipboard() {
    FreeSavedClipboard();
    if (!OpenClipboardRetry()) return;
    for (UINT f = EnumClipboardFormats(0); f && g_clip.count < 32; f = EnumClipboardFormats(f)) {
        if (f == CF_BITMAP || f == CF_PALETTE || f == CF_METAFILEPICT || f == CF_ENHMETAFILE || f == CF_OWNERDISPLAY ||
            f == CF_DSPBITMAP || f == CF_DSPMETAFILEPICT || f == CF_DSPENHMETAFILE)
            continue;  // GDI handles, synthesized from the formats we do keep
        HANDLE h = GetClipboardData(f);
        SIZE_T size = h ? GlobalSize(h) : 0;
        if (!size || size > (16u << 20)) continue;
        void* src = GlobalLock(h);
        HGLOBAL copy = src ? GlobalAlloc(GMEM_MOVEABLE, size) : nullptr;
        if (copy) {
            memcpy(GlobalLock(copy), src, size);
            GlobalUnlock(copy);
            g_clip.format[g_clip.count] = f;
            g_clip.data[g_clip.count++] = copy;
        }
        if (src) GlobalUnlock(h);
    }
    CloseClipboard();
    g_clip.saved = true;
}

static void RestoreClipboard() {
    if (!g_clip.saved || !OpenClipboardRetry()) return;
    EmptyClipboard();
    for (int i = 0; i < g_clip.count; i++) SetClipboardData(g_clip.format[i], g_clip.data[i]);  // ownership passes on
    CloseClipboard();
    g_clip.count = 0;
    g_clip.saved = false;
}

static void BeginSelectionCopy() {
    if (ForegroundIsTerminal() || g_clip.saved) return;  // Ctrl+C is "interrupt" there; or a copy is in flight
    SaveClipboard();
    g_clip.seq = GetClipboardSequenceNumber();
    g_clip.tries = 0;
    SendChord('C');
    SetTimer(g_host, TIMER_COPY, 40, nullptr);
}

// Polled until the copy lands (or gives up when nothing was selected).
static void CheckSelectionCopy() {
    if (GetClipboardSequenceNumber() == g_clip.seq) {
        if (++g_clip.tries < 8) return;
        KillTimer(g_host, TIMER_COPY);
        FreeSavedClipboard();  // nothing selected: the clipboard was never touched
        return;
    }
    KillTimer(g_host, TIMER_COPY);

    static char16_t in[2001], out[2001];
    int n = 0;
    if (OpenClipboardRetry()) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (const char16_t* p = (const char16_t*)GlobalLock(h)) {
                while (p[n] && n < 2001) { in[n] = p[n]; n++; }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    // Too long, or an editor that copies the whole line ("...\r\n") when nothing is selected: leave it.
    int target = (n == 0 || n > 2000 || in[n - 1] == u'\n') ? -1 : lf::convertText(in, n, out);
    if (target < 0) { RestoreClipboard(); return; }

    if (HKL hkl = FindLayout(target)) {
        HWND fg = GetForegroundWindow();
        if (fg) PostMessageW(fg, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)hkl);
    }
    if (OpenClipboardRetry()) {
        EmptyClipboard();
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(char16_t))) {
            char16_t* d = (char16_t*)GlobalLock(g);
            memcpy(d, out, n * sizeof(char16_t));
            d[n] = 0;
            GlobalUnlock(g);
            SetClipboardData(CF_UNICODETEXT, g);
        }
        CloseClipboard();
    }
    SendChord('V');
    SetTimer(g_host, TIMER_RESTORE, 300, nullptr);  // after the app has read it

    // The pasted text is now "the last word" if it is one, so double Shift flips it straight back.
    FixWord w = {};
    w.shown = target;
    w.typed = 1 - target;
    w.decided = true;
    for (int i = 0; i < n && w.n >= 0; i++) {
        int k = lf::keyFor(out[i], (lf::Lang)target);
        if (k < 0 || i >= lf::kMaxWord) w.n = -1;
        else w.keys[w.n++] = (uint8_t)k;
    }
    g_word.n = 0;
    g_last = w.n > 0 ? w : FixWord{};
    if (g_fixLang != target) g_expectLang = target;
}

static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) ResetWords();  // caret moved
    if (code == HC_ACTION && wp == WM_MOUSEMOVE && g_visible) {
        g_pt = ((MSLLHOOKSTRUCT*)lp)->pt;
        // Coalesce: at most one pending move message, so fast mice never flood the queue.
        if (!InterlockedExchange(&g_movePending, 1)) PostMessageW(g_badge, WM_APP_MOVE, 0, 0);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// ---------------------------------------------------------------------------
// Language per app
// ---------------------------------------------------------------------------
//
// Remembers the layout each app (by exe name) was last used with, and switches back to it when that app comes to
// the front. Saved in the [AppLanguages] section of CursorLingo.ini.

static DWORD g_appSwitchUntil = 0;  // a switch we requested is still landing: don't record the old layout
static HKL g_appSwitchTo = nullptr;

static void RememberAppLanguage(HKL hkl) {
    if (!g_set.perAppLanguage || !g_fgExe[0] || !hkl) return;
    if (GetTickCount() < g_appSwitchUntil && hkl != g_appSwitchTo) return;
    wchar_t v[24];
    swprintf(v, 24, L"%llx", (unsigned long long)(UINT_PTR)hkl);
    EnsureIni();
    WritePrivateProfileStringW(L"AppLanguages", g_fgExe, v, g_ini);
}

// The foreground app just changed: give it back its layout, or remember the one it has.
static void ApplyAppLanguage() {
    if (!g_set.perAppLanguage || !g_fgExe[0]) return;
    wchar_t v[24];
    HKL now = CurrentHkl();
    if (!GetPrivateProfileStringW(L"AppLanguages", g_fgExe, L"", v, 24, g_ini)) {
        RememberAppLanguage(now);
        return;
    }
    HKL want = (HKL)(UINT_PTR)wcstoull(v, nullptr, 16);
    HKL list[64];
    int n = GetKeyboardLayoutList(64, list), i = 0;
    while (i < n && list[i] != want) i++;
    if (want == now || i == n) return;  // already there, or that layout was removed
    g_appSwitchTo = want;
    g_appSwitchUntil = GetTickCount() + 1000;
    if (HWND fg = GetForegroundWindow()) PostMessageW(fg, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)want);
}

static void CALLBACK ForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    g_fullscreen = ForegroundIsFullscreen();
    ClassifyForeground();
    ApplyAppLanguage();
    ResetWords();
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

// After sleep or a screen lock Windows may have dropped our low-level hooks (it removes hooks that time out,
// e.g. while the machine is busy waking up), and the word in memory no longer matches the screen.
// Hooking again is cheap, so do it unconditionally.
static void ReinstallHooks() {
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, g_inst, 0);
    if (g_kbHook) { SetAutoFix(false); SetAutoFix(true); }
    g_shiftDown = g_shiftClean = false;
    g_lastShiftTap = 0;
    ClassifyForeground();
    if (UpdateLanguage()) Refresh(true);
}

static void ShowMenu() {
    HMENU m = CreatePopupMenu(), style = CreatePopupMenu(), mode = CreatePopupMenu(), size = CreatePopupMenu(),
          fix = CreatePopupMenu(), lang = CreatePopupMenu();
    const int th = UiThai();
    wchar_t buf[64];
    swprintf(buf, 64, TR(L"Current input: %ls", L"ภาษาที่ใช้อยู่: %ls"), g_code);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, buf);
    swprintf(buf, 64, TR(L"Hide badge for %ls", L"ซ่อนป้ายเมื่อเป็น %ls"), g_code);
    AppendMenuW(m, MF_STRING | (IsHidden(g_code) ? MF_CHECKED : 0), ID_HIDE_CURRENT, buf);
    if (g_set.hidden[0]) AppendMenuW(m, MF_STRING, ID_SHOW_ALL, TR(L"Show for all languages", L"แสดงป้ายทุกภาษา"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < MODE_COUNT; i++) AppendMenuW(mode, MF_STRING | (g_set.mode == (DWORD)i ? MF_CHECKED : 0), ID_MODE_BASE + i, kModeNames[th][i]);
    for (int i = 0; i < STYLE_COUNT; i++) AppendMenuW(style, MF_STRING | (g_set.style == (DWORD)i ? MF_CHECKED : 0), ID_STYLE_BASE + i, kStyleNames[th][i]);
    for (int i = 0; i < SIZE_COUNT; i++) AppendMenuW(size, MF_STRING | (g_set.size == (DWORD)i ? MF_CHECKED : 0), ID_SIZE_BASE + i, kSizeNames[th][i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mode, TR(L"Show", L"แสดงป้าย"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)style, TR(L"Style", L"รูปแบบ"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)size, TR(L"Size", L"ขนาด"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // One menu for the whole feature: Off, Double Shift only (fix on request) or Automatic.
    const int fixMode = !g_set.autoFix ? 0 : g_set.fixAsYouType ? 2 : 1;
    for (int i = 0; i < 3; i++) AppendMenuW(fix, MF_STRING | (fixMode == i ? MF_CHECKED : 0), ID_FIX_BASE + i, kFixNames[th][i]);
    AppendMenuW(fix, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fix, MF_STRING | (g_set.codeEditorsManual ? MF_CHECKED : 0) | (fixMode == 2 ? 0 : MF_GRAYED),
                ID_CODE_EDITORS, TR(L"In code editors: double Shift only", L"ใน code editor แก้เฉพาะตอนกด Shift 2 ครั้ง"));
    wchar_t learned[64];
    if (!g_ruleCount) wcscpy(learned, TR(L"Learned words: none yet", L"ยังไม่มีคำที่จำไว้"));
    else if (th) swprintf(learned, 64, L"ล้างคำที่จำไว้ (%d คำ)", g_ruleCount);
    else swprintf(learned, 64, L"Forget %d learned word%ls", g_ruleCount, g_ruleCount == 1 ? L"" : L"s");
    AppendMenuW(fix, MF_STRING | (g_ruleCount ? 0 : MF_GRAYED), ID_FORGET_WORDS, learned);
    wchar_t fixTitle[96];
    swprintf(fixTitle, 96, TR(L"Fix wrong language (EN \u21C4 TH): %ls", L"แก้คำที่พิมพ์ผิดภาษา (EN \u21C4 TH): %ls"), kFixNames[th][fixMode]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)fix, fixTitle);
    AppendMenuW(m, MF_STRING | (g_set.perAppLanguage ? MF_CHECKED : 0), ID_PER_APP, TR(L"Remember language per app", L"จำภาษาของแต่ละแอป"));
    AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), ID_STARTUP, TR(L"Start with Windows", L"เปิดพร้อม Windows"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < 3; i++) AppendMenuW(lang, MF_STRING | (g_set.uiLang == (DWORD)i ? MF_CHECKED : 0), ID_UILANG_BASE + i, kUiLangNames[th][i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)lang, L"Language / ภาษา");  // bilingual, so it can always be found
    AppendMenuW(m, MF_STRING, ID_REPORT, TR(L"Report a wrong fix or bug\u2026", L"แจ้งปัญหาหรือคำที่แก้ผิด\u2026"));
    AppendMenuW(m, MF_STRING, ID_QUIT, TR(L"Quit CursorLingo", L"ปิด CursorLingo"));

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_host);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_host, nullptr);
    DestroyMenu(m);  // destroys submenus too

    if (!cmd) return;
    if (cmd == ID_QUIT) { DestroyWindow(g_host); return; }
    if (cmd == ID_STARTUP) { SetStartup(!StartupEnabled()); return; }
    if (cmd == ID_REPORT) {  // only opens the issue form; nothing typed is ever attached
        ShellExecuteW(nullptr, L"open", L"https://github.com/kittipongint/cursor-lingo/issues/new/choose", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (cmd == ID_FORGET_WORDS) { g_ruleCount = 0; SaveRules(); return; }
    if (cmd == ID_PER_APP) {
        g_set.perAppLanguage = !g_set.perAppLanguage;
        SaveSettings();
        RememberAppLanguage(CurrentHkl());
        return;
    }
    if (cmd >= ID_UILANG_BASE && cmd < ID_UILANG_BASE + 3) {
        g_set.uiLang = cmd - ID_UILANG_BASE;
        SaveSettings();
        return;
    }
    if (cmd == ID_CODE_EDITORS) {
        g_set.codeEditorsManual = !g_set.codeEditorsManual;
        SaveSettings();
        return;
    }
    if (cmd >= ID_FIX_BASE && cmd < ID_FIX_BASE + 3) {
        int pick = cmd - ID_FIX_BASE;  // stored as AutoFix (on/off) + FixAsYouType, so older settings carry over
        g_set.autoFix = pick != 0;
        if (pick) g_set.fixAsYouType = pick == 2;
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
                HKL before = g_lastHkl;
                if (UpdateLanguage()) Refresh(true);
                if (g_lastHkl != before) RememberAppLanguage(g_lastHkl);
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
            ApplyFlips();
            return 0;
        case WM_APP_SELECTION:
            BeginSelectionCopy();
            return 0;
        case WM_APP_SAVE_WORDS:
            SaveRules();
            return 0;
        case WM_TIMER:
            if (wp == TIMER_COPY) CheckSelectionCopy();
            if (wp == TIMER_RESTORE) { KillTimer(h, TIMER_RESTORE); RestoreClipboard(); }
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) ReinstallHooks();
            return TRUE;
        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_UNLOCK || wp == WTS_CONSOLE_CONNECT || wp == WTS_REMOTE_CONNECT) ReinstallHooks();
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
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // for the Startup-folder shortcut
    FindSettingsFiles();
    MigrateFromRegistry();
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
    ClassifyForeground();
    LoadRules();
    if (g_set.autoFix) SetAutoFix(true);
    WTSRegisterSessionNotification(g_host, NOTIFY_FOR_THIS_SESSION);  // screen lock / unlock
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundChanged, 0, 0,
                               WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // Coalescable timer lets Windows batch our wake-ups with others to save power.
    SetCoalescableTimer(g_badge, TIMER_POLL, 200, nullptr, 100);

    // Release start-up pages we no longer need.
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);

    WTSUnRegisterSessionNotification(g_host);
    UnhookWindowsHookEx(g_mouseHook);
    SetAutoFix(false);
    UnhookWinEvent(g_fgHook);
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    CoUninitialize();
    ReleaseMutex(mutex);
    return 0;
}
