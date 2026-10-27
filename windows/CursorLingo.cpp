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
#include <uiautomation.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <vector>

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
    DWORD presentation = 0;     // spotlight around the pointer + click ripples
    DWORD spotRadius = 100;     // spotlight radius in points (40..300)
    DWORD zoomLevel = 20;       // zoom lens magnification, in tenths (15..80)
    DWORD keySpotlight = 0x648, keyZoom = 0x65A, keyDraw = 0x644;  // tool shortcuts (see RegisterToolKeys), Ctrl+Alt+H/Z/D
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
    {L"Presentation", &g_set.presentation, 0, 2},
    {L"SpotlightRadius", &g_set.spotRadius, 100, 301},
    {L"ZoomLevel", &g_set.zoomLevel, 20, 81},
    {L"ShortcutSpotlight", &g_set.keySpotlight, 0x648, 0x10000},
    {L"ShortcutZoom", &g_set.keyZoom, 0x65A, 0x10000},
    {L"ShortcutDraw", &g_set.keyDraw, 0x644, 0x10000},
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

// Installed from the Microsoft Store (an MSIX package)? Then Windows owns "start with Windows": the package declares
// a StartupTask, which the user switches on in Settings > Apps > Startup. Writes to %APPDATA% go to the package's
// own storage, so the settings files below need no change.
static bool IsPackaged() {
    static int packaged = -1;
    if (packaged < 0) {
        typedef LONG(WINAPI * GetPackageNameFn)(UINT32*, PWSTR);
        auto fn = (GetPackageNameFn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentPackageFullName");
        UINT32 len = 0;
        packaged = fn && fn(&len, nullptr) == ERROR_INSUFFICIENT_BUFFER;
    }
    return packaged == 1;
}

// Otherwise "Start with Windows" is a shortcut in the user's Startup folder: no registry entry, and it shows up (and can be
// switched off) in Task Manager's Startup apps like any other.
static bool StartupLinkPath(wchar_t* out) {
    wchar_t* folder = nullptr;
    bool ok = SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &folder));
    if (ok) swprintf(out, MAX_PATH, L"%ls\\CursorLingo.lnk", folder);
    CoTaskMemFree(folder);
    return ok;
}

static bool StartupEnabled() {
    if (IsPackaged()) return false;
    wchar_t link[MAX_PATH];
    return StartupLinkPath(link) && GetFileAttributesW(link) != INVALID_FILE_ATTRIBUTES;
}

static void SetStartup(bool on) {
    wchar_t link[MAX_PATH], exe[MAX_PATH], dir[MAX_PATH];
    if (IsPackaged() || !StartupLinkPath(link)) return;
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
        if (GetFileAttributesW(g_ini) != INVALID_FILE_ATTRIBUTES)  // only once the settings are safely in the file
            RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\CursorLingo");
    }
    // The Store package starts through its StartupTask instead; leave an older install's entry alone there.
    if (!IsPackaged() &&
        RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ | KEY_WRITE, &k) ==
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
#define WM_APP_LEARN (WM_APP + 5)
#define WM_APP_RIPPLE (WM_APP + 6)
#define WM_APP_SPOT_SIZE (WM_APP + 7)
enum { TIMER_POLL = 1, TIMER_HIDE = 2, TIMER_FADE = 3, TIMER_COPY = 4, TIMER_RESTORE = 5, TIMER_RIPPLE = 6, TIMER_SPIN = 7, TIMER_SAVE = 8,
       TIMER_LENS = 9 };
enum { HOTKEY_PRESENTATION = 1, HOTKEY_ESCAPE, HOTKEY_ZOOM, HOTKEY_DRAW, HOTKEY_ZOOM_IN, HOTKEY_ZOOM_OUT, HOTKEY_UNDO };
enum {
    ID_HIDE_CURRENT = 100, ID_SHOW_ALL, ID_STARTUP, ID_QUIT, ID_CODE_EDITORS, ID_REPORT, ID_FORGET_WORDS, ID_PER_APP,
    ID_PRESENTATION, ID_ZOOM, ID_DRAW, ID_SHORTCUTS,
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

static Bitmap g_haloImg;  // the presentation ring (see below)
static void RebuildHalo();
static void PaintHalo();
static void MoveHalo();

static void Rebuild() {
    float s = Scale();
    bool dot = g_set.style == STYLE_DOT;
    int bw = (int)std::lround((dot ? 12 : (wcslen(g_code) > 2 ? 30 : 24)) * s);
    int bh = (int)std::lround((dot ? 12 : 17) * s);
    g_pad = (int)std::ceil(3 * g_dpi / 96.0f);
    if (g_img.Create(bw + 2 * g_pad, bh + 2 * g_pad))
        RenderBadge(g_img, g_code, g_set.style, bw, bh, 11.5f * s, true);
    RebuildHalo();  // its size follows the monitor's DPI
    PaintHalo();
    MoveHalo();
}

// ---------------------------------------------------------------------------
// Presentation tools, each usable alone or together:
//   * Spotlight, after Hazyzh/cursor-highlight: the screen is dimmed except for a circle around the pointer, ringed
//     by a slowly turning rainbow. It stays click-through, so you keep working underneath.
//   * Zoom: the circle shows what is under the pointer, magnified (Windows' Magnification API: no special rights).
//   * Draw: pen strokes over everything until you leave; while drawing, clicks go to the pen.
// Clicks send out a ripple in the language's colour. Ctrl+Alt + wheel resizes the circle, Ctrl+Alt+= / - zoom in
// and out, Ctrl+Z takes back the last stroke, Esc leaves drawing first, then everything else. Mouse only.
// ---------------------------------------------------------------------------

static HWND g_dim, g_halo;          // the dimming layer over all monitors, and the rainbow ring
static HWND g_lens, g_magnifier;    // zoom: a round window with the system's magnifier control inside
static HWND g_draw;                 // the drawing layer over all monitors
static bool g_zoom = false, g_drawing = false;
static float* g_ringCov = nullptr;  // per ring pixel: coverage 0..1
static float* g_ringTurn = nullptr; // ... and its angle, in turns (0..1)
static float g_spin = 0;            // ring rotation, in turns
static RECT g_virtual;              // the whole virtual screen (all monitors)
struct Ripple {
    HWND wnd;
    Bitmap img;
    POINT at;
    DWORD start;
    bool live;
};
static Ripple g_ripples[3];
static int g_nextRipple = 0;
static const DWORD kRippleMs = 400;
static const Rgb kRainbow[] = {{0, 1, 1}, {0.70f, 1, 0.53f}, {0.91f, 1, 0.31f}, {1, 0.31f, 0.31f},
                               {0.04f, 0.96f, 0.48f}, {1, 1, 1}, {0, 1, 1}};

// The ring (and the circle inside it) is up for the spotlight and for zoom.
static bool CircleOn() { return g_set.presentation || g_zoom; }

// A premultiplied, anti-aliased ring in one colour, centred in `out` (the click ripples).
static void RenderRing(Bitmap& out, float radius, float width, Rgb c, float ringA) {
    const float cx = out.w / 2.0f, cy = out.h / 2.0f;
    for (int y = 0; y < out.h; y++)
        for (int x = 0; x < out.w; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = std::sqrt(dx * dx + dy * dy);
            float a = Clamp01(width / 2 - std::fabs(d - radius) + 0.5f) * ringA;
            auto ch = [a](float v) { return (DWORD)std::lround(v * a * 255); };
            out.px[y * out.w + x] = ((DWORD)std::lround(a * 255) << 24) | (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
        }
}

static float DpiScale() { return g_dpi / 96.0f; }
static float SpotRadius() { return std::min(std::max((float)g_set.spotRadius, 40.0f), 300.0f) * DpiScale(); }
static float ZoomFactor() { return std::min(std::max((float)g_set.zoomLevel, 15.0f), 80.0f) / 10; }

// Geometry of the rainbow ring, worked out once per size; each animation frame then only looks up colours.
static void RebuildHalo() {
    if (!CircleOn()) return;
    float r = SpotRadius(), w = std::max(r / 30, 3 * DpiScale()), ringR = r + w / 2 - 1;  // overlaps the hole by 1px
    int side = (int)std::ceil(2 * (ringR + w)) + 4;
    if (!g_haloImg.Create(side, side)) return;
    delete[] g_ringCov;
    delete[] g_ringTurn;
    g_ringCov = new float[side * side];
    g_ringTurn = new float[side * side];
    const float c = side / 2.0f;
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++) {
            float dx = x + 0.5f - c, dy = y + 0.5f - c, d = std::sqrt(dx * dx + dy * dy);
            g_ringCov[y * side + x] = Clamp01(w / 2 - std::fabs(d - ringR) + 0.5f);
            g_ringTurn[y * side + x] = std::atan2(dy, dx) / 6.2831853f + 0.5f;
        }
}

static void PaintHalo() {
    if (!g_haloImg.px || !g_ringCov) return;
    const int n = g_haloImg.w * g_haloImg.h;
    for (int i = 0; i < n; i++) {
        float a = g_ringCov[i] * 0.85f;
        if (a <= 0) { g_haloImg.px[i] = 0; continue; }
        float t = g_ringTurn[i] + g_spin;
        t = (t - std::floor(t)) * 6;
        int k = std::min((int)t, 5);
        float f = t - k;
        Rgb c0 = kRainbow[k], c1 = kRainbow[k + 1];
        Rgb c = {c0.r + (c1.r - c0.r) * f, c0.g + (c1.g - c0.g) * f, c0.b + (c1.b - c0.b) * f};
        auto ch = [a](float v) { return (DWORD)std::lround(v * a * 255); };
        g_haloImg.px[i] = ((DWORD)std::lround(a * 255) << 24) | (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
    }
}

static void ShowLayered(HWND wnd, Bitmap& img, POINT centre) {
    POINT src = {0, 0}, dst = {centre.x - img.w / 2, centre.y - img.h / 2};
    SIZE sz = {img.w, img.h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd, nullptr, &dst, &sz, img.dc, &src, 0, &bf, ULW_ALPHA);
}

// Magnification.dll, loaded on first use of zoom.
struct MagTransform { float v[3][3]; };
static BOOL(WINAPI* g_magSetSource)(HWND, RECT);
static BOOL(WINAPI* g_magSetTransform)(HWND, MagTransform*);
static BOOL(WINAPI* g_magSetFilter)(HWND, DWORD, int, HWND*);
static int g_lensSide = 0;

template <typename F> static void LoadProc(HMODULE dll, const char* name, F& fn) {
    fn = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
}

static bool MakeLens() {
    if (g_magnifier) return true;
    HMODULE dll = LoadLibraryExW(L"Magnification.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return false;
    BOOL(WINAPI* init)() = nullptr;
    LoadProc(dll, "MagInitialize", init);
    LoadProc(dll, "MagSetWindowSource", g_magSetSource);
    LoadProc(dll, "MagSetWindowTransform", g_magSetTransform);
    LoadProc(dll, "MagSetWindowFilterList", g_magSetFilter);
    if (!init || !g_magSetSource || !g_magSetTransform || !g_magSetFilter || !init()) return false;
    g_lens = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             L"CursorLingoOverlay", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_inst, nullptr);
    SetLayeredWindowAttributes(g_lens, 0, 255, LWA_ALPHA);
    g_magnifier = CreateWindowExW(0, L"Magnifier", L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, g_lens, nullptr, g_inst, nullptr);
    if (!g_magnifier) {
        DestroyWindow(g_lens);
        g_lens = nullptr;
        return false;
    }
    // Never magnify our own layers, the lens included.
    HWND mine[] = {g_lens, g_dim, g_halo, g_badge, g_draw, g_ripples[0].wnd, g_ripples[1].wnd, g_ripples[2].wnd};
    g_magSetFilter(g_magnifier, 0 /* MW_FILTERMODE_EXCLUDE */, 8, mine);
    return true;
}

static void ApplyZoomLevel() {
    if (!g_magnifier) return;
    float z = ZoomFactor();
    MagTransform t = {{{z, 0, 0}, {0, z, 0}, {0, 0, 1}}};
    g_magSetTransform(g_magnifier, &t);
}

// Puts the lens over the pointer and aims it at the circle around it, ZoomFactor() times smaller than the lens.
static void MoveLens() {
    if (!g_zoom || !g_magnifier) return;
    int r = (int)std::lround(SpotRadius()), side = 2 * r;
    if (side != g_lensSide) {
        g_lensSide = side;
        SetWindowPos(g_magnifier, nullptr, 0, 0, side, side, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowRgn(g_lens, CreateEllipticRgn(0, 0, side + 1, side + 1), TRUE);
    }
    SetWindowPos(g_lens, nullptr, g_pt.x - r, g_pt.y - r, side, side, SWP_NOZORDER | SWP_NOACTIVATE);
    int half = (int)std::lround(r / ZoomFactor());
    int x = std::min(std::max((int)g_pt.x, (int)g_virtual.left + half), (int)g_virtual.right - half);  // stay on screen
    int y = std::min(std::max((int)g_pt.y, (int)g_virtual.top + half), (int)g_virtual.bottom - half);
    g_magSetSource(g_magnifier, RECT{x - half, y - half, x + half, y + half});
    InvalidateRect(g_magnifier, nullptr, FALSE);
}

// Moves the hole in the dimming layer, the ring and the lens with the pointer.
static void MoveHalo() {
    if (!CircleOn() || !g_haloImg.px) return;
    if (g_set.presentation) {
        int r = (int)std::lround(SpotRadius()), x = g_pt.x - g_virtual.left, y = g_pt.y - g_virtual.top;
        HRGN region = CreateRectRgn(0, 0, g_virtual.right - g_virtual.left, g_virtual.bottom - g_virtual.top);
        HRGN hole = CreateEllipticRgn(x - r, y - r, x + r + 1, y + r + 1);
        CombineRgn(region, region, hole, RGN_DIFF);
        DeleteObject(hole);
        SetWindowRgn(g_dim, region, TRUE);  // the window owns the region now
    }
    ShowLayered(g_halo, g_haloImg, g_pt);
    MoveLens();
}

static void PlaceDim() {
    g_virtual = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    g_virtual.right = g_virtual.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_virtual.bottom = g_virtual.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    SetWindowPos(g_dim, HWND_TOPMOST, g_virtual.left, g_virtual.top, g_virtual.right - g_virtual.left,
                 g_virtual.bottom - g_virtual.top, SWP_NOACTIVATE);
}

// Keeps the stacking order: dimming layer, drawing, lens, ring, then the badge on top.
static void RaiseOverlays() {
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    if (g_set.presentation) SetWindowPos(g_dim, HWND_TOPMOST, 0, 0, 0, 0, flags);
    if (g_drawing) SetWindowPos(g_draw, HWND_TOPMOST, 0, 0, 0, 0, flags);
    if (g_zoom) SetWindowPos(g_lens, HWND_TOPMOST, 0, 0, 0, 0, flags);
    if (CircleOn()) SetWindowPos(g_halo, HWND_TOPMOST, 0, 0, 0, 0, flags);
    if (g_visible) SetWindowPos(g_badge, HWND_TOPMOST, 0, 0, 0, 0, flags);
}

// Shortcuts that only make sense while a tool is on, so they are taken from other apps only then.
static void UpdatePresentKeys() {
    for (int id : {HOTKEY_ESCAPE, HOTKEY_ZOOM_IN, HOTKEY_ZOOM_OUT, HOTKEY_UNDO}) UnregisterHotKey(g_host, id);
    if (CircleOn() || g_drawing) RegisterHotKey(g_host, HOTKEY_ESCAPE, MOD_NOREPEAT, VK_ESCAPE);
    if (g_zoom) {
        RegisterHotKey(g_host, HOTKEY_ZOOM_IN, MOD_CONTROL | MOD_ALT, VK_OEM_PLUS);
        RegisterHotKey(g_host, HOTKEY_ZOOM_OUT, MOD_CONTROL | MOD_ALT, VK_OEM_MINUS);
    }
    if (g_drawing) RegisterHotKey(g_host, HOTKEY_UNDO, MOD_CONTROL, 'Z');
}

// Shows the dimming layer, the lens and the ring to match what is on.
static void UpdateCircle() {
    PlaceDim();
    GetCursorPos(&g_pt);
    if (CircleOn()) {
        RebuildHalo();
        PaintHalo();
        g_lensSide = 0;
        ApplyZoomLevel();
        MoveHalo();
        ShowWindow(g_dim, g_set.presentation ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (g_lens) ShowWindow(g_lens, g_zoom ? SW_SHOWNOACTIVATE : SW_HIDE);
        ShowWindow(g_halo, SW_SHOWNOACTIVATE);
        RaiseOverlays();
        SetTimer(g_host, TIMER_SPIN, 33, nullptr);
    } else {
        KillTimer(g_host, TIMER_SPIN);
        ShowWindow(g_halo, SW_HIDE);
        ShowWindow(g_dim, SW_HIDE);
        if (g_lens) ShowWindow(g_lens, SW_HIDE);
        g_haloImg.Destroy();
    }
    if (g_zoom) SetTimer(g_host, TIMER_LENS, 16, nullptr);  // the magnifier only redraws when asked
    else KillTimer(g_host, TIMER_LENS);
    UpdatePresentKeys();
}

static void SetPresentation(bool on) {
    g_set.presentation = on;
    UpdateCircle();
}

static void SetZoom(bool on) {
    g_zoom = on && MakeLens();
    UpdateCircle();
}

// The drawing layer: a per-pixel-alpha window over all monitors. Its empty pixels are alpha 1/255, invisible but
// solid enough to take the mouse; strokes are drawn into it in software, one segment at a time.
static Bitmap g_drawImg;
static std::vector<POINT> g_penPoints;
static std::vector<size_t> g_strokeStarts;  // index of each stroke's first point
static const Rgb kPen = {1, 0.23f, 0.19f};
static const DWORD kBlank = 0x01000000;

static void PresentDraw(const RECT* dirty) {
    POINT src = {0, 0}, dst = {g_virtual.left, g_virtual.top};
    SIZE sz = {g_drawImg.w, g_drawImg.h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO info = {sizeof(info), nullptr, &dst, &sz, g_drawImg.dc, &src, 0, &bf, ULW_ALPHA, dirty};
    UpdateLayeredWindowIndirect(g_draw, &info);
}

// An anti-aliased round-capped line from a to b (window pixels); grows `dirty` to cover it.
static void PenSegment(POINT a, POINT b, RECT* dirty) {
    const float w = 5 * DpiScale(), reach = w / 2 + 1;
    RECT box = {std::max(0L, std::min(a.x, b.x) - (LONG)std::ceil(reach)), std::max(0L, std::min(a.y, b.y) - (LONG)std::ceil(reach)),
                std::min((LONG)g_drawImg.w, std::max(a.x, b.x) + (LONG)std::ceil(reach) + 1),
                std::min((LONG)g_drawImg.h, std::max(a.y, b.y) + (LONG)std::ceil(reach) + 1)};
    const float dx = (float)(b.x - a.x), dy = (float)(b.y - a.y), len2 = dx * dx + dy * dy;
    for (LONG y = box.top; y < box.bottom; y++)
        for (LONG x = box.left; x < box.right; x++) {
            float px = x + 0.5f - a.x, py = y + 0.5f - a.y;
            float t = len2 > 0 ? Clamp01((px * dx + py * dy) / len2) : 0;
            float ex = px - t * dx, ey = py - t * dy;
            float cov = Clamp01(w / 2 - std::sqrt(ex * ex + ey * ey) + 0.5f);
            DWORD& p = g_drawImg.px[y * g_drawImg.w + x];
            if (cov * 255 <= (p >> 24)) continue;  // overlapping strokes: keep the stronger
            auto ch = [cov](float v) { return (DWORD)std::lround(v * cov * 255); };
            p = ((DWORD)std::lround(cov * 255) << 24) | (ch(kPen.r) << 16) | (ch(kPen.g) << 8) | ch(kPen.b);
        }
    UnionRect(dirty, dirty, &box);
}

static void SetDrawing(bool on) {
    g_drawing = false;
    g_penPoints.clear();
    g_strokeStarts.clear();
    ShowWindow(g_draw, SW_HIDE);
    g_drawImg.Destroy();
    if (on) {
        PlaceDim();  // reads the virtual screen
        if (g_drawImg.Create(g_virtual.right - g_virtual.left, g_virtual.bottom - g_virtual.top)) {
            g_drawing = true;
            std::fill(g_drawImg.px, g_drawImg.px + g_drawImg.w * g_drawImg.h, kBlank);
            PresentDraw(nullptr);
            ShowWindow(g_draw, SW_SHOWNOACTIVATE);
            RaiseOverlays();
        }
    }
    UpdatePresentKeys();
}

static void UndoStroke() {
    if (g_strokeStarts.empty()) return;
    g_penPoints.resize(g_strokeStarts.back());
    g_strokeStarts.pop_back();
    std::fill(g_drawImg.px, g_drawImg.px + g_drawImg.w * g_drawImg.h, kBlank);
    RECT dirty = {};
    for (size_t s = 0; s < g_strokeStarts.size(); s++) {
        size_t first = g_strokeStarts[s], end = s + 1 < g_strokeStarts.size() ? g_strokeStarts[s + 1] : g_penPoints.size();
        for (size_t i = first; i < end; i++) PenSegment(g_penPoints[i == first ? i : i - 1], g_penPoints[i], &dirty);
    }
    PresentDraw(nullptr);
}

static LRESULT CALLBACK DrawProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
    RECT dirty = {};
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // the app underneath keeps the keyboard
        case WM_LBUTTONDOWN:
            SetCapture(h);
            g_strokeStarts.push_back(g_penPoints.size());
            g_penPoints.push_back(p);
            PenSegment(p, p, &dirty);  // a click leaves a dot
            PresentDraw(&dirty);
            return 0;
        case WM_MOUSEMOVE:
            if (GetCapture() != h || g_penPoints.empty()) return 0;
            PenSegment(g_penPoints.back(), p, &dirty);
            g_penPoints.push_back(p);
            PresentDraw(&dirty);
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void SpinHalo() {
    if (!g_haloImg.px) return;
    g_spin = std::fmod(g_spin + 33.0f / 6000, 1.0f);  // one turn every 6 s
    PaintHalo();
    ShowLayered(g_halo, g_haloImg, g_pt);
}

// Ctrl+Alt + wheel: 10 points per notch.
static void ResizeSpotlight(int delta) {
    int r = (int)g_set.spotRadius + (delta > 0 ? 10 : -10);
    g_set.spotRadius = (DWORD)std::min(std::max(r, 40), 300);
    RebuildHalo();
    PaintHalo();
    MoveHalo();
    SetTimer(g_host, TIMER_SAVE, 1000, nullptr);  // save once the wheel stops
}

// Ctrl+Alt+= / -: a quarter more or a fifth less, between 1.5x and 8x.
static void StepZoom(bool in) {
    int z = (int)std::lround(ZoomFactor() * 10);
    z = in ? std::min(80, z + std::max(2, z / 4)) : std::max(15, z - std::max(2, z / 5));
    g_set.zoomLevel = (DWORD)z;
    ApplyZoomLevel();
    MoveLens();
    SetTimer(g_host, TIMER_SAVE, 1000, nullptr);
}

// Runs from the message loop: start a ripple at `at` (screen pixels).
static void StartRipple(POINT at) {
    Ripple& r = g_ripples[g_nextRipple];
    g_nextRipple = (g_nextRipple + 1) % 3;
    r.at = at;
    r.start = GetTickCount();
    r.live = true;
    int side = (int)std::ceil(2 * 40 * DpiScale());
    if (r.img.w != side) r.img.Create(side, side);
    SetTimer(g_host, TIMER_RIPPLE, 16, nullptr);
}

// One animation frame for every live ripple: it grows and fades out over kRippleMs.
static void StepRipples() {
    bool any = false;
    for (Ripple& r : g_ripples) {
        if (!r.live) continue;
        float t = (GetTickCount() - r.start) / (float)kRippleMs;
        if (t >= 1 || !r.img.px) {
            r.live = false;
            ShowWindow(r.wnd, SW_HIDE);
            continue;
        }
        any = true;
        float s = DpiScale(), ease = 1 - (1 - t) * (1 - t);
        RenderRing(r.img, (8 + 28 * ease) * s, 4 * s, AccentFor(g_code), 1 - t);
        ShowLayered(r.wnd, r.img, r.at);
        if (!IsWindowVisible(r.wnd))
            SetWindowPos(r.wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if (!any) KillTimer(g_host, TIMER_RIPPLE);
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

// Words to learn from, handed from the hook to the message loop: checking the focus and writing the file are too
// slow for the hook.
static FixWord g_learnJobs[4];
static int g_learnHead = 0, g_learnTail = 0;

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
}

// Is the focused field a password box? The classic check only sees Win32 edit controls; browsers and other apps
// draw their own, which UI Automation reports. When it can't tell, the answer is yes, so nothing is saved.
static bool FocusedFieldIsPassword() {
    static const GUID kClsidUIAutomation = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
    static const GUID kIidIUIAutomation = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
    static IUIAutomation* uia = nullptr;
    if (FocusIsPassword()) return true;
    if (!uia && FAILED(CoCreateInstance(kClsidUIAutomation, nullptr, CLSCTX_INPROC_SERVER, kIidIUIAutomation, (void**)&uia)))
        return true;
    IUIAutomationElement* focused = nullptr;
    BOOL password = TRUE;
    if (SUCCEEDED(uia->GetFocusedElement(&focused)) && focused) {
        if (FAILED(focused->get_CurrentIsPassword(&password))) password = TRUE;
        focused->Release();
    }
    return password != FALSE;
}

static void SaveRules();

// Runs from the message loop: learn the flipped words, unless they were typed into a password field.
static void LearnPending() {
    bool changed = false;
    bool password = FocusedFieldIsPassword();
    while (g_learnHead != g_learnTail) {
        if (!password) { Learn(g_learnJobs[g_learnHead]); changed = true; }
        g_learnHead = (g_learnHead + 1) % 4;
    }
    if (changed) SaveRules();
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
        bool valid = true;  // a damaged file must not feed bad key indices to the detector
        for (int j = 0; j < n; j++) valid = valid && (buf[i + 2 + j] & ~lf::kShiftBit) < lf::kKeyCount;
        if (valid) {
            Rule& r = g_rules[g_ruleCount++];
            r.n = n;
            r.shown = flags & 1;
            r.fix = (flags >> 1) & 1;
            memcpy(r.keys, buf + i + 2, n);
        }
        i += 2 + n;
    }
}

// Queues `w` (as it is on screen now) to be flipped to the other layout, and records the flip.
static bool QueueFlip(FixWord& w) {
    int next = (g_jobTail + 1) % 4;
    if (next == g_jobHead) return false;  // the user is faster than the message loop: drop it
    g_jobs[g_jobTail] = w;
    g_jobTail = next;
    w.shown = 1 - w.shown;
    w.decided = true;
    if (g_fixLang != w.shown) g_expectLang = w.shown;
    PostMessageW(g_host, WM_APP_FIX, 0, 0);
    return true;
}

static void ToggleWord() {
    FixWord* w = g_word.n ? &g_word : (g_last.n ? &g_last : nullptr);
    if (!w) { PostMessageW(g_host, WM_APP_SELECTION, 0, 0); return; }  // nothing typed: convert the selection
    if (g_fixLang < 0 || !QueueFlip(*w)) return;
    int next = (g_learnTail + 1) % 4;
    if (next == g_learnHead) return;
    g_learnJobs[g_learnTail] = *w;
    g_learnTail = next;
    PostMessageW(g_host, WM_APP_LEARN, 0, 0);
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
    if (code == HC_ACTION && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) {
        ResetWords();  // caret moved
        if (CircleOn() && !g_drawing) {
            POINT pt = ((MSLLHOOKSTRUCT*)lp)->pt;
            PostMessageW(g_host, WM_APP_RIPPLE, (WPARAM)(INT_PTR)pt.x, (LPARAM)pt.y);
        }
    }
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL && CircleOn() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
        (GetAsyncKeyState(VK_MENU) & 0x8000)) {
        PostMessageW(g_host, WM_APP_SPOT_SIZE, (WPARAM)(short)HIWORD(((MSLLHOOKSTRUCT*)lp)->mouseData), 0);
        return 1;  // Ctrl+Alt + wheel is ours while the circle is up: don't zoom the app underneath
    }
    if (code == HC_ACTION && wp == WM_MOUSEMOVE && (g_visible || CircleOn())) {
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

// Kept in memory, read from the file once and written only when a layout changes, so the message loop that also
// services the keyboard hook never waits on the disk for this.
struct AppLanguage {
    wchar_t exe[64];
    HKL hkl;
};
static AppLanguage g_appLangs[256];
static int g_appLangCount = -1;  // -1: not read yet

static AppLanguage* FindAppLanguage(const wchar_t* exe) {
    if (g_appLangCount < 0) {
        static wchar_t section[32767];
        g_appLangCount = 0;
        DWORD len = GetPrivateProfileSectionW(L"AppLanguages", section, 32767, g_ini);
        for (const wchar_t* p = section; p < section + len && *p && g_appLangCount < 256; p += wcslen(p) + 1) {
            const wchar_t* eq = wcschr(p, L'=');
            if (!eq || eq - p >= 64) continue;
            AppLanguage& a = g_appLangs[g_appLangCount++];
            wcsncpy(a.exe, p, eq - p);
            a.exe[eq - p] = 0;
            a.hkl = (HKL)(UINT_PTR)wcstoull(eq + 1, nullptr, 16);
        }
    }
    for (int i = 0; i < g_appLangCount; i++)
        if (wcscmp(g_appLangs[i].exe, exe) == 0) return &g_appLangs[i];
    return nullptr;
}

static void RememberAppLanguage(HKL hkl) {
    if (!g_set.perAppLanguage || !hkl) return;
    if (GetTickCount() < g_appSwitchUntil && hkl != g_appSwitchTo) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid == GetCurrentProcessId()) return;  // our own menu is in front: not an app's layout
    ReadForegroundExe();  // the poll can run before the foreground-change event arrives
    if (!g_fgExe[0]) return;
    AppLanguage* a = FindAppLanguage(g_fgExe);
    if (a && a->hkl == hkl) return;
    if (!a) {
        if (g_appLangCount == 256) return;
        a = &g_appLangs[g_appLangCount++];
        wcscpy(a->exe, g_fgExe);
    }
    a->hkl = hkl;
    wchar_t v[24];
    swprintf(v, 24, L"%llx", (unsigned long long)(UINT_PTR)hkl);
    EnsureIni();
    WritePrivateProfileStringW(L"AppLanguages", g_fgExe, v, g_ini);
}

// The foreground app just changed: give it back its layout, or remember the one it has.
static void ApplyAppLanguage() {
    if (!g_set.perAppLanguage || !g_fgExe[0]) return;
    HKL now = CurrentHkl();
    const AppLanguage* a = FindAppLanguage(g_fgExe);
    if (!a) {
        RememberAppLanguage(now);
        return;
    }
    HKL want = a->hkl;
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
    RaiseOverlays();
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
    if (g_set.autoFix) { SetAutoFix(false); SetAutoFix(true); }  // also retries a hook that failed to install
    g_shiftDown = g_shiftClean = false;
    g_lastShiftTap = 0;
    ClassifyForeground();
    if (UpdateLanguage()) Refresh(true);
}

// ---------------------------------------------------------------------------
// Shortcuts of the presentation tools, and the window to change them
// ---------------------------------------------------------------------------
//
// A shortcut is stored as the hotkey control gives it: the virtual key in the low byte, HOTKEYF_* in the high byte
// (0 = none). The window uses Windows' own hotkey control, which reads the keys only while it has the focus.

static const int kToolHotkeys[3] = {HOTKEY_PRESENTATION, HOTKEY_ZOOM, HOTKEY_DRAW};
static const WORD kToolDefaults[3] = {MAKEWORD('H', HOTKEYF_CONTROL | HOTKEYF_ALT), MAKEWORD('Z', HOTKEYF_CONTROL | HOTKEYF_ALT),
                                      MAKEWORD('D', HOTKEYF_CONTROL | HOTKEYF_ALT)};
static const wchar_t* kToolNames[2][3] = {{L"Spotlight", L"Zoom", L"Draw on screen"}, {L"สปอตไลต์", L"ซูม", L"วาดบนจอ"}};
static DWORD* ToolKey(int i) {
    DWORD* keys[] = {&g_set.keySpotlight, &g_set.keyZoom, &g_set.keyDraw};
    return keys[i];
}

// Registers the tools' shortcuts; returns a bit for each tool whose keys another app already has.
static int RegisterToolKeys() {
    int failed = 0;
    for (int i = 0; i < 3; i++) {
        UnregisterHotKey(g_host, kToolHotkeys[i]);
        DWORD k = *ToolKey(i);
        BYTE f = HIBYTE(k);
        UINT mods = (f & HOTKEYF_CONTROL ? MOD_CONTROL : 0) | (f & HOTKEYF_ALT ? MOD_ALT : 0) | (f & HOTKEYF_SHIFT ? MOD_SHIFT : 0);
        if (k && !RegisterHotKey(g_host, kToolHotkeys[i], mods | MOD_NOREPEAT, LOBYTE(k))) failed |= 1 << i;
    }
    return failed;
}

static void UnregisterToolKeys() {
    for (int id : kToolHotkeys) UnregisterHotKey(g_host, id);
}

// "Ctrl+Alt+H"; empty for none.
static void KeyText(DWORD k, wchar_t* out, int n) {
    out[0] = 0;
    if (!k) return;
    BYTE f = HIBYTE(k), vk = LOBYTE(k);
    wchar_t name[32] = L"";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        name[0] = vk;  // not GetKeyNameText: under the Thai layout it would name the Thai letter
        name[1] = 0;
    } else {
        LONG sc = (LONG)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
        if (f & HOTKEYF_EXT) sc |= 1 << 24;
        if (!GetKeyNameTextW(sc, name, 32)) swprintf(name, 32, L"#%u", vk);
    }
    swprintf(out, n, L"%ls%ls%ls%ls", f & HOTKEYF_CONTROL ? L"Ctrl+" : L"", f & HOTKEYF_ALT ? L"Alt+" : L"",
             f & HOTKEYF_SHIFT ? L"Shift+" : L"", name);
}

enum { IDC_KEY_BASE = 1000, IDC_RESTORE = 1010 };
static HWND g_keysWnd, g_keyCtl[3];
static HFONT g_keysFont;

// OK: refuses a shortcut used twice, or one the tools need while on; then registers, and keeps the old ones if
// another app holds any of the new keys.
static bool ApplyKeys() {
    DWORD keys[3];
    for (int i = 0; i < 3; i++) keys[i] = (WORD)SendMessageW(g_keyCtl[i], HKM_GETHOTKEY, 0, 0);
    const WORD reserved[] = {MAKEWORD(VK_OEM_PLUS, HOTKEYF_CONTROL | HOTKEYF_ALT), MAKEWORD(VK_OEM_MINUS, HOTKEYF_CONTROL | HOTKEYF_ALT),
                             MAKEWORD('Z', HOTKEYF_CONTROL)};
    wchar_t text[32], msg[256];
    for (int i = 0; i < 3; i++) {
        if (!keys[i]) continue;
        KeyText(keys[i], text, 32);
        for (WORD r : reserved)
            if (keys[i] == r) {
                swprintf(msg, 256, TR(L"%ls is used while a tool is on. Pick another.", L"%ls ใช้อยู่ตอนเปิดเครื่องมือ เลือกคีย์อื่น"), text);
                MessageBoxW(g_keysWnd, msg, kAppName, MB_ICONWARNING);
                return false;
            }
        for (int j = i + 1; j < 3; j++)
            if (keys[i] == keys[j]) {
                swprintf(msg, 256, TR(L"%ls is set for two tools.", L"%ls ถูกตั้งไว้ให้สองเครื่องมือ"), text);
                MessageBoxW(g_keysWnd, msg, kAppName, MB_ICONWARNING);
                return false;
            }
    }
    DWORD old[3];
    for (int i = 0; i < 3; i++) {
        old[i] = *ToolKey(i);
        *ToolKey(i) = keys[i];
    }
    int failed = RegisterToolKeys();
    UnregisterToolKeys();  // the window is still open; they come back when it closes
    if (failed) {
        for (int i = 0; i < 3; i++) *ToolKey(i) = old[i];
        wchar_t list[128] = L"";
        for (int i = 0; i < 3; i++)
            if (failed & (1 << i)) {
                KeyText(keys[i], text, 32);
                if (list[0]) wcscat(list, L", ");
                wcscat(list, text);
            }
        swprintf(msg, 256, TR(L"Another app already uses %ls. Pick another.", L"มีแอปอื่นใช้ %ls อยู่แล้ว เลือกคีย์อื่น"), list);
        MessageBoxW(g_keysWnd, msg, kAppName, MB_ICONWARNING);
        return false;
    }
    SaveSettings();
    return true;
}

static LRESULT CALLBACK KeysProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK && ApplyKeys()) DestroyWindow(h);
            if (LOWORD(wp) == IDCANCEL) DestroyWindow(h);
            if (LOWORD(wp) == IDC_RESTORE)
                for (int i = 0; i < 3; i++) SendMessageW(g_keyCtl[i], HKM_SETHOTKEY, kToolDefaults[i], 0);
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            g_keysWnd = nullptr;
            DeleteObject(g_keysFont);
            g_keysFont = nullptr;
            RegisterToolKeys();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ShowShortcuts() {
    if (g_keysWnd) { SetForegroundWindow(g_keysWnd); return; }
    const int th = UiThai();
    UINT dpi = g_dpi;
    auto px = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    g_keysFont = CreateFontIndirectW(&ncm.lfMessageFont);

    RECT r = {0, 0, px(380), px(236)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, ex = WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME;
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, dpi);
    POINT pt;
    GetCursorPos(&pt);
    int w = r.right - r.left, h = r.bottom - r.top;
    g_keysWnd = CreateWindowExW(ex, L"CursorLingoKeys", TR(L"CursorLingo shortcuts", L"คีย์ลัด CursorLingo"), style,
                                std::max((int)g_workRect.left, std::min((int)pt.x - w / 2, (int)g_workRect.right - w)),
                                std::max((int)g_workRect.top, std::min((int)pt.y - h / 2, (int)g_workRect.bottom - h)), w, h,
                                nullptr, nullptr, g_inst, nullptr);
    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int x, int y, int cw, int ch, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | st, px(x), px(y), px(cw), px(ch), g_keysWnd,
                                 (HMENU)(INT_PTR)id, g_inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_keysFont, FALSE);
        return c;
    };
    for (int i = 0; i < 3; i++) {
        add(L"STATIC", kToolNames[th][i], SS_CENTERIMAGE, 16, 16 + i * 36, 130, 26, 0);
        g_keyCtl[i] = add(HOTKEY_CLASSW, L"", WS_TABSTOP | WS_BORDER, 150, 16 + i * 36, 214, 26, IDC_KEY_BASE + i);
        // Keys without Ctrl or Alt would take plain typing: the control turns them into Ctrl+Alt+key.
        SendMessageW(g_keyCtl[i], HKM_SETRULES, HKCOMB_NONE | HKCOMB_S, MAKELPARAM(HOTKEYF_CONTROL | HOTKEYF_ALT, 0));
        SendMessageW(g_keyCtl[i], HKM_SETHOTKEY, *ToolKey(i), 0);
    }
    add(L"STATIC",
        TR(L"Click a box and press the new keys, with Ctrl or Alt. Backspace clears a box. Esc, Ctrl+Z and Ctrl+Alt+= / − "
           L"stay as they are.",
           L"คลิกที่ช่องแล้วกดคีย์ใหม่ ต้องมี Ctrl หรือ Alt ด้วย กด Backspace เพื่อล้างช่อง ส่วน Esc, Ctrl+Z และ Ctrl+Alt+= / − ใช้ตามเดิม"),
        0, 16, 126, 348, 52, 0);
    add(L"BUTTON", TR(L"Restore defaults", L"คืนค่าเริ่มต้น"), WS_TABSTOP, 16, 192, 130, 30, IDC_RESTORE);
    add(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 194, 192, 82, 30, IDOK);
    add(L"BUTTON", TR(L"Cancel", L"ยกเลิก"), WS_TABSTOP, 282, 192, 82, 30, IDCANCEL);

    UnregisterToolKeys();  // so pressing the current shortcut reaches the box instead of toggling a tool
    ShowWindow(g_keysWnd, SW_SHOW);
    SetForegroundWindow(g_keysWnd);
    SetFocus(g_keyCtl[0]);
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
    HMENU present = CreatePopupMenu();
    const bool toolOn[3] = {g_set.presentation != 0, g_zoom, g_drawing};
    for (int i = 0; i < 3; i++) {
        wchar_t key[32], title[64];
        KeyText(*ToolKey(i), key, 32);
        swprintf(title, 64, L"%ls\t%ls", kToolNames[th][i], key);
        AppendMenuW(present, MF_STRING | (toolOn[i] ? MF_CHECKED : 0), ID_PRESENTATION + i, title);
    }
    AppendMenuW(present, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(present, MF_STRING | MF_GRAYED, 0, TR(L"Ctrl+Alt + wheel: resize the circle", L"Ctrl+Alt + ล้อเมาส์: ปรับขนาดวง"));
    AppendMenuW(present, MF_STRING | MF_GRAYED, 0, TR(L"Ctrl+Alt+= / \u2212: zoom in / out", L"Ctrl+Alt+= / \u2212: ซูมเข้า / ออก"));
    AppendMenuW(present, MF_STRING | MF_GRAYED, 0, TR(L"Ctrl+Z: undo last stroke", L"Ctrl+Z: ลบเส้นล่าสุด"));
    AppendMenuW(present, MF_STRING | MF_GRAYED, 0, TR(L"Esc: stop drawing, then exit", L"Esc: เลิกวาด กดอีกครั้งเพื่อออก"));
    AppendMenuW(present, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(present, MF_STRING, ID_SHORTCUTS, TR(L"Shortcuts\u2026", L"ตั้งคีย์ลัด\u2026"));
    AppendMenuW(m, MF_POPUP | (CircleOn() || g_drawing ? MF_CHECKED : 0), (UINT_PTR)present, TR(L"Presentation", L"นำเสนอ"));
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
    if (IsPackaged())  // switched in Windows Settings; the app cannot tell whether it is on
        AppendMenuW(m, MF_STRING, ID_STARTUP, TR(L"Start with Windows\u2026", L"เปิดพร้อม Windows\u2026"));
    else
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
    if (cmd == ID_STARTUP) {
        if (IsPackaged()) ShellExecuteW(nullptr, L"open", L"ms-settings:startupapps", nullptr, nullptr, SW_SHOWNORMAL);
        else SetStartup(!StartupEnabled());
        return;
    }
    if (cmd == ID_REPORT) {  // only opens the issue form; nothing typed is ever attached
        ShellExecuteW(nullptr, L"open", L"https://github.com/kittipongint/cursor-lingo/issues/new/choose", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (cmd == ID_FORGET_WORDS) { g_ruleCount = 0; SaveRules(); return; }
    if (cmd == ID_PRESENTATION) {
        SetPresentation(!g_set.presentation);
        SaveSettings();
        return;
    }
    if (cmd == ID_ZOOM) { SetZoom(!g_zoom); return; }
    if (cmd == ID_DRAW) { SetDrawing(!g_drawing); return; }
    if (cmd == ID_SHORTCUTS) { ShowShortcuts(); return; }
    if (cmd == ID_PER_APP) {
        g_set.perAppLanguage = !g_set.perAppLanguage;
        SaveSettings();  // the first app is remembered when it comes back to the front
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
            if (g_visible || CircleOn()) UpdateMonitor(g_pt);
            if (g_visible) Present(true);
            MoveHalo();
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
        case WM_APP_LEARN:
            LearnPending();
            return 0;
        case WM_APP_RIPPLE:
            StartRipple({(LONG)(INT_PTR)wp, (LONG)lp});
            return 0;
        case WM_APP_SPOT_SIZE:
            ResizeSpotlight((int)(short)wp);
            return 0;
        case WM_HOTKEY:
            if (wp == HOTKEY_PRESENTATION) {
                SetPresentation(!g_set.presentation);
                SaveSettings();
            } else if (wp == HOTKEY_ZOOM) {
                SetZoom(!g_zoom);
            } else if (wp == HOTKEY_DRAW) {
                SetDrawing(!g_drawing);
            } else if (wp == HOTKEY_ZOOM_IN || wp == HOTKEY_ZOOM_OUT) {
                StepZoom(wp == HOTKEY_ZOOM_IN);
            } else if (wp == HOTKEY_UNDO) {
                UndoStroke();
            } else if (wp == HOTKEY_ESCAPE) {
                if (g_drawing) {
                    SetDrawing(false);  // the first Esc puts the pen down, the next one leaves
                } else {
                    g_zoom = false;
                    SetPresentation(false);
                    SaveSettings();
                }
            }
            return 0;
        case WM_TIMER:
            if (wp == TIMER_COPY) CheckSelectionCopy();
            if (wp == TIMER_RESTORE) { KillTimer(h, TIMER_RESTORE); RestoreClipboard(); }
            if (wp == TIMER_RIPPLE) StepRipples();
            if (wp == TIMER_SPIN) SpinHalo();
            if (wp == TIMER_LENS) MoveLens();
            if (wp == TIMER_SAVE) { KillTimer(h, TIMER_SAVE); SaveSettings(); }
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
            if (msg == WM_DISPLAYCHANGE && CircleOn()) {
                PlaceDim();
                MoveHalo();
            }
            if (msg == WM_DISPLAYCHANGE && g_drawing) SetDrawing(true);  // a fresh layer that fits the new screens
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
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_HOTKEY_CLASS};  // the shortcuts window
    InitCommonControlsEx(&icc);
    FindSettingsFiles();
    MigrateFromRegistry();
    LoadSettings();

    WNDCLASSW wc = {};
    wc.hInstance = inst;
    wc.lpfnWndProc = BadgeProc;
    wc.lpszClassName = L"CursorLingoBadge";
    RegisterClassW(&wc);
    wc.lpfnWndProc = DefWindowProcW;  // presentation ring and ripples: drawn with UpdateLayeredWindow only
    wc.lpszClassName = L"CursorLingoOverlay";
    RegisterClassW(&wc);
    wc.lpszClassName = L"CursorLingoDim";  // the spotlight's dimming layer: a plain colour, made translucent
    wc.hbrBackground = CreateSolidBrush(RGB(0x45, 0x57, 0x61));
    RegisterClassW(&wc);
    wc.hbrBackground = nullptr;
    wc.lpfnWndProc = DrawProc;  // the drawing layer
    wc.lpszClassName = L"CursorLingoDraw";
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    RegisterClassW(&wc);
    wc.lpfnWndProc = KeysProc;  // the shortcuts window
    wc.lpszClassName = L"CursorLingoKeys";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);
    wc.hCursor = nullptr;
    wc.hbrBackground = nullptr;
    wc.hIcon = nullptr;
    wc.lpfnWndProc = HostProc;
    wc.lpszClassName = L"CursorLingoHost";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);

    g_host = CreateWindowExW(0, L"CursorLingoHost", kAppName, 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_badge = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              L"CursorLingoBadge", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    const DWORD overlay = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    g_halo = CreateWindowExW(overlay, L"CursorLingoOverlay", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    g_dim = CreateWindowExW(overlay, L"CursorLingoDim", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_dim, 0, 153, LWA_ALPHA);  // 60 %
    g_draw = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"CursorLingoDraw", L"",
                             WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    for (Ripple& r : g_ripples)
        r.wnd = CreateWindowExW(overlay, L"CursorLingoOverlay", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    GetCursorPos(&g_pt);
    UpdateMonitor(g_pt);
    UpdateLanguage();
    Rebuild();
    AddTrayIcon();
    Refresh(false);
    if (g_set.presentation) SetPresentation(true);
    // The tools' shortcuts (Ctrl+Alt+H / Z / D unless changed) work from anywhere; RegisterHotKey needs no special rights.
    RegisterToolKeys();

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
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_keysWnd && IsDialogMessageW(g_keysWnd, &msg)) continue;  // Tab, Enter and Esc in the shortcuts window
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (int id : {HOTKEY_PRESENTATION, HOTKEY_ZOOM, HOTKEY_DRAW}) UnregisterHotKey(g_host, id);
    WTSUnRegisterSessionNotification(g_host);
    UnhookWindowsHookEx(g_mouseHook);
    SetAutoFix(false);
    UnhookWinEvent(g_fgHook);
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    CoUninitialize();
    ReleaseMutex(mutex);
    return 0;
}
