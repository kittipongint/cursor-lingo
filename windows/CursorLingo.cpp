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
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <winver.h>
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
    DWORD badgeBorder = 1;      // the badge's thin border and shadow
    wchar_t hidden[256] = L"";  // comma separated codes, e.g. ",EN,JA,"
} g_set;

// The Settings window (see below) shows the open tab again when something it shows changes.
enum { TAB_BADGE, TAB_FIX, TAB_PRESENT, TAB_GENERAL, TAB_COUNT };
static void SettingsReload(int tab);

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
    {L"BadgeBorder", &g_set.badgeBorder, 1, 2},
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

// Badge colours picked in Settings, by language code; stored in the [Colors] section as RRGGBB.
struct CustomColor {
    wchar_t code[4];
    COLORREF c;
};
static CustomColor g_colors[32];
static int g_colorCount = 0;

static const CustomColor* FindColor(const wchar_t* code) {
    for (int i = 0; i < g_colorCount; i++)
        if (wcscmp(g_colors[i].code, code) == 0) return &g_colors[i];
    return nullptr;
}

// CLR_NONE goes back to the language's own colour.
static void SetColor(const wchar_t* code, COLORREF c) {
    int i = 0;
    while (i < g_colorCount && wcscmp(g_colors[i].code, code) != 0) i++;
    if (c == CLR_NONE) {
        if (i < g_colorCount) g_colors[i] = g_colors[--g_colorCount];
    } else if (i < g_colorCount || g_colorCount < 32) {
        if (i == g_colorCount) wcscpy(g_colors[g_colorCount++].code, code);
        g_colors[i].c = c;
    }
    EnsureIni();
    wchar_t hex[8];
    swprintf(hex, 8, L"%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    WritePrivateProfileStringW(L"Colors", code, c == CLR_NONE ? nullptr : hex, g_ini);
}

static void LoadColors() {
    static wchar_t buf[2048];
    g_colorCount = 0;
    GetPrivateProfileSectionW(L"Colors", buf, 2048, g_ini);
    for (const wchar_t* line = buf; *line && g_colorCount < 32; line += wcslen(line) + 1) {
        const wchar_t* eq = wcschr(line, L'=');
        if (!eq || eq - line < 1 || eq - line > 3 || wcslen(eq + 1) != 6) continue;
        unsigned long v = wcstoul(eq + 1, nullptr, 16);
        CustomColor& c = g_colors[g_colorCount++];
        wcsncpy(c.code, line, eq - line);
        c.code[eq - line] = 0;
        c.c = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    }
}

static void LoadSettings() {
    LoadColors();
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

// Two or three letter code of a layout's language: "EN", "TH" ("??" if unknown).
static void CodeOf(HKL hkl, wchar_t* code) {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"";
    wcscpy(code, L"??");
    if (LCIDToLocaleName(MAKELCID(LOWORD((UINT_PTR)hkl), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) > 0) {
        int i = 0;
        for (; i < 3 && name[i] && name[i] != L'-'; i++) code[i] = (wchar_t)towupper(name[i]);
        code[i] = 0;
        if (i == 0) wcscpy(code, L"??");
    }
}

// Returns true when the language code changed.
static bool UpdateLanguage() {
    HKL hkl = CurrentHkl();
    if (!hkl || hkl == g_lastHkl) return false;
    g_lastHkl = hkl;
    g_fixLang = FixLangOf(hkl);
    if (g_expectLang >= 0 && g_fixLang == g_expectLang) g_expectLang = -1;  // our own switch
    else if (!(g_word.n && g_word.shown == g_fixLang)) ResetWords();         // a word never spans the user's

    wchar_t code[4];
    CodeOf(hkl, code);
    if (wcscmp(code, g_code) == 0) return false;
    wcscpy(g_code, code);
    return true;
}

// ---------------------------------------------------------------------------
// Rendering (premultiplied BGRA, analytic anti-aliasing — no GDI+ / Direct2D)
// ---------------------------------------------------------------------------

struct Rgb { float r, g, b; };

static Rgb AccentFor(const wchar_t* code) {
    if (const CustomColor* own = FindColor(code))
        return {GetRValue(own->c) / 255.0f, GetGValue(own->c) / 255.0f, GetBValue(own->c) / 255.0f};
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
    const bool edge = g_set.badgeBorder != 0;  // off: no border, no white ring round the dot, no shadow
    shadow = shadow && edge;
    Rgb fill, text, border;
    float borderW = 0.75f, borderA = 0.22f;
    switch (style) {
        case STYLE_LIGHT: fill = {1, 1, 1}; text = {accent.r * 0.75f, accent.g * 0.75f, accent.b * 0.75f}; border = {0, 0, 0}; break;
        case STYLE_DARK: fill = {0.1f, 0.1f, 0.1f}; text = {1, 1, 1}; border = accent; borderW = 1.25f; borderA = 1; break;
        default: fill = accent; text = {1, 1, 1}; border = {0, 0, 0}; break;
    }
    if (!edge) borderA = 0;
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
                    float core = edge ? Clamp01(0.5f - (d + 1.5f)) : 1;
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
enum { ID_QUIT = 100, ID_SETTINGS, ID_PRESENTATION, ID_ZOOM, ID_DRAW, ID_SHORTCUTS, ID_FIX_BASE = 150 };

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

static float DpiScaleOf(UINT dpi) { return dpi / 96.0f; }
static float Scale() { return kSizeScale[g_set.size] * DpiScaleOf(g_dpi); }

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

static bool g_magReady = false;  // MagInitialize done, once per run

static bool MakeLens() {
    if (g_magnifier) return true;
    if (!g_magReady) {
        HMODULE dll = LoadLibraryExW(L"Magnification.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll) return false;
        BOOL(WINAPI* init)() = nullptr;
        LoadProc(dll, "MagInitialize", init);
        LoadProc(dll, "MagSetWindowSource", g_magSetSource);
        LoadProc(dll, "MagSetWindowTransform", g_magSetTransform);
        LoadProc(dll, "MagSetWindowFilterList", g_magSetFilter);
        if (!init || !g_magSetSource || !g_magSetTransform || !g_magSetFilter || !init()) return false;
        g_magReady = true;
    }
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
    SettingsReload(TAB_PRESENT);
}

// Zoom off: the magnifier shrinks to one pixel and stops being aimed at the screen until zoom is used again. It is
// kept rather than destroyed: every new magnifier control (and every MagUninitialize) leaks a handle in Windows,
// and tearing it all down does not give its runtime's memory back either.
static void ReleaseLens() {
    if (!g_magnifier) return;
    SetWindowPos(g_magnifier, nullptr, 0, 0, 1, 1, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g_lens, nullptr, 0, 0, 1, 1, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    g_magSetSource(g_magnifier, RECT{0, 0, 1, 1});
    g_lensSide = 0;  // MoveLens sizes it again
}

static void SetZoom(bool on) {
    g_zoom = on && MakeLens();
    UpdateCircle();
    if (!g_zoom) ReleaseLens();
    SettingsReload(TAB_PRESENT);
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
    std::vector<POINT>().swap(g_penPoints);  // give the memory back, not just empty the lists
    std::vector<size_t>().swap(g_strokeStarts);
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
    SettingsReload(TAB_PRESENT);
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
    SettingsReload(TAB_PRESENT);
}

// Ctrl+Alt+= / -: a quarter more or a fifth less, between 1.5x and 8x.
static void StepZoom(bool in) {
    int z = (int)std::lround(ZoomFactor() * 10);
    z = in ? std::min(80, z + std::max(2, z / 4)) : std::max(15, z - std::max(2, z / 5));
    g_set.zoomLevel = (DWORD)z;
    ApplyZoomLevel();
    MoveLens();
    SetTimer(g_host, TIMER_SAVE, 1000, nullptr);
    SettingsReload(TAB_PRESENT);
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
// The Settings window names them: keep these lists in step with the ones checked here and in ForegroundIsTerminal.
static const wchar_t kCodeEditorNames[] = L"VS Code, VSCodium, Cursor, Windsurf, Zed, Visual Studio, Sublime Text, Notepad++, "
                                          L"Android Studio, JetBrains IDEs (IntelliJ IDEA, PyCharm, WebStorm\u2026), Fleet, gVim";
static const wchar_t kTerminalNames[] = L"Command Prompt, PowerShell, Windows Terminal, Git Bash";
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
    if (!uia) {
        if (FAILED(CoCreateInstance(kClsidUIAutomation, nullptr, CLSCTX_INPROC_SERVER, kIidIUIAutomation, (void**)&uia)))
            return true;
        // This runs on the thread of the keyboard and mouse hooks: an app that hangs must not hold it (Windows drops
        // hooks that stop answering). Past the limit the call fails, which counts as a password field.
        IUIAutomation2* uia2 = nullptr;
        if (SUCCEEDED(uia->QueryInterface(__uuidof(IUIAutomation2), (void**)&uia2))) {
            uia2->put_ConnectionTimeout(250);
            uia2->put_TransactionTimeout(250);
            uia2->Release();
        }
    }
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
    if (changed) {
        SaveRules();
        SettingsReload(TAB_FIX);
    }
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
                // Within the block even if an app left out the terminating zero.
                const SIZE_T units = GlobalSize(h) / sizeof(char16_t);
                while (n < 2001 && (SIZE_T)n < units && p[n]) { in[n] = p[n]; n++; }
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
// Shortcuts of the presentation tools
// ---------------------------------------------------------------------------
//
// A shortcut is stored as the hotkey control gives it: the virtual key in the low byte, HOTKEYF_* in the high byte
// (0 = none). Settings changes them with Windows' own hotkey control, which reads the keys only while it has the focus.

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

// ---------------------------------------------------------------------------
// Settings window
// ---------------------------------------------------------------------------
//
// One window with a tab for each part of the app, built from plain Win32 controls. The page of the open tab is built
// again from the settings whenever something changes, here or elsewhere (the tray menu, a shortcut), so it never
// shows a stale value. Layout is in 96-dpi units, scaled to the window's monitor.

enum {
    CTL_CAPTION = 2000, CTL_STATUS, CTL_PREVIEW, CTL_MODE, CTL_STYLE, CTL_SIZE, CTL_BORDER,
    CTL_FIX_BASE = 2100, CTL_EDITORS = 2110, CTL_FORGET, CTL_PER_APP,
    CTL_TOOL_BASE = 2200, CTL_KEY_BASE = 2210, CTL_RESTORE = 2220, CTL_RADIUS, CTL_ZOOM_LEVEL, CTL_RADIUS_VALUE, CTL_ZOOM_VALUE,
    CTL_STARTUP = 2300, CTL_UI_LANG, CTL_REPORT, CTL_STAR, CTL_CONTRIBUTE, CTL_KOFI, CTL_QR, CTL_COPY_NUMBER,
    CTL_COLOR_BASE = 2400, CTL_DEFAULT_BASE = 2500, CTL_SHOW_BASE = 2600, kMaxRows = 64,
};
#define WM_APP_RELOAD (WM_APP + 20)
static const wchar_t* kTabNames[2][4] = {{L"Badge", L"Fix Wrong Language", L"Presentation", L"General"},
                                         {L"ป้ายภาษา", L"แก้คำผิดภาษา", L"นำเสนอ", L"ทั่วไป"}};

// PromptPay, Thailand's bank transfer, for donations. The QR code is a static PromptPay QR (EMVCo) for this number
// with no amount, so the payer types it in their banking app. Windows has no QR encoder, so the modules are kept
// here ('#' = dark), made from the payload
// 00020101021129370016A000000677010111011300668047828435802TH530376463045D5D
// and checked by decoding them; make them again if the number changes.
static const wchar_t kPromptPay[] = L"0804782843", kPromptPayShown[] = L"080-478-2843";
static const char* const kPromptPayQr[] = {
    "#######..#....#.#..#..#######",
    "#.....#.##...##..#.#..#.....#",
    "#.###.#...#...##..###.#.###.#",
    "#.###.#...###.........#.###.#",
    "#.###.#.#.####.####...#.###.#",
    "#.....#..#...#.#..#...#.....#",
    "#######.#.#.#.#.#.#.#.#######",
    "...........#......#..........",
    "#.#.#.#....####.#.......#..#.",
    ".#.##..#.#....##...##..#####.",
    "..###.#..##...####.##.#.#...#",
    ".####..####.###..##..#.#.#.#.",
    ".#.#.###.####.....#.#.####.##",
    ".##.##.#...##.....##.#.#...##",
    ".#.#.####..##.#.###...#...#.#",
    "#.#..#.#....#.##.##.##.##.#.#",
    ".##..###.#..#...#####.#.###.#",
    ".#####...##..#.#..#.##...#.#.",
    "#.#...#...####..#...#.#.#...#",
    ".##.##.####.#......#.#.#...#.",
    "#.######..##..#...#########.#",
    "........###..#..#..##...####.",
    "#######...#.#.#.#.###.#.#...#",
    "#.....#....###.#...##...#.#..",
    "#.###.#.#...#.###.#.######.##",
    "#.###.#..##......#.#.#.#.#.#.",
    "#.###.#.#.#.#######.#...#.#.#",
    "#.....#...#.##.###...##..##..",
    "#######.#.#.##...####...#.#.#",
};

static HWND g_setWnd, g_setTabs, g_setPage;
static int g_setTab = TAB_BADGE;
static UINT g_setDpi = 96;
static HFONT g_fontText, g_fontBold, g_fontHead;
static bool g_setActive = false, g_keysPaused = false, g_reloadPosted = false;
static wchar_t g_keyStatus[128];  // the last shortcut problem, shown under the tools

// The look of the Settings window: Windows 11's light or dark colours (following "app mode" in Windows), or the system
// colours under high contrast. Each section sits on a rounded card, and the controls are drawn to match.
struct SetTheme {
    bool dark = false, contrast = false;
    COLORREF back, card, cardLine, text, sub, dim, accent, onAccent, control, controlHot, controlDown, controlLine, strong,
        field, knob, tabHot, status;
    HBRUSH backBrush = nullptr, cardBrush = nullptr;
};
static SetTheme g_th;
static std::vector<std::pair<int, int>> g_cards;  // the open page's cards: top and bottom, in 96-dpi units
static HFONT g_fontIcon;                          // Segoe Fluent Icons (or MDL2 Assets), for the check mark
static int g_tabHot = -1;                         // the tab under the pointer

static int SetPx(int v) { return MulDiv(v, (int)g_setDpi, 96); }

static COLORREF Mix(COLORREF a, COLORREF b, float t) {
    auto ch = [t](int x, int y) { return (BYTE)std::lround(x + (y - x) * t); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

static void LoadTheme() {
    SetTheme& t = g_th;
    HIGHCONTRASTW hc = {sizeof(hc)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
    t.contrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    DWORD light = 1, accent = 0, cb = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &light, &cb);
    t.dark = !t.contrast && light == 0;
    cb = sizeof(accent);
    // Stored as 0xAABBGGRR: the same byte order as a COLORREF.
    const COLORREF base = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                                       nullptr, &accent, &cb) == ERROR_SUCCESS
                              ? accent & 0xFFFFFF
                              : RGB(0x00, 0x78, 0xD4);
    if (t.contrast) {
        t.back = t.card = t.control = t.controlHot = t.controlDown = t.field = t.knob = t.tabHot = GetSysColor(COLOR_WINDOW);
        t.text = t.sub = t.status = t.cardLine = t.controlLine = t.strong = GetSysColor(COLOR_WINDOWTEXT);
        t.dim = GetSysColor(COLOR_GRAYTEXT);
        t.accent = GetSysColor(COLOR_HIGHLIGHT);
        t.onAccent = GetSysColor(COLOR_HIGHLIGHTTEXT);
    } else if (t.dark) {
        t.back = RGB(0x20, 0x20, 0x20), t.card = RGB(0x2B, 0x2B, 0x2B), t.cardLine = RGB(0x33, 0x33, 0x33);
        t.text = RGB(0xFF, 0xFF, 0xFF), t.sub = RGB(0xB4, 0xB4, 0xB4), t.dim = RGB(0x70, 0x70, 0x70);
        t.accent = Mix(base, RGB(0xFF, 0xFF, 0xFF), 0.45f), t.onAccent = RGB(0, 0, 0);
        t.control = RGB(0x37, 0x37, 0x37), t.controlHot = RGB(0x3D, 0x3D, 0x3D), t.controlDown = RGB(0x30, 0x30, 0x30);
        t.controlLine = RGB(0x46, 0x46, 0x46), t.strong = RGB(0x9E, 0x9E, 0x9E), t.field = RGB(0x1E, 0x1E, 0x1E);
        t.knob = RGB(0x45, 0x45, 0x45), t.tabHot = RGB(0x2D, 0x2D, 0x2D), t.status = RGB(0xFF, 0x99, 0xA4);
    } else {
        t.back = RGB(0xF3, 0xF3, 0xF3), t.card = RGB(0xFF, 0xFF, 0xFF), t.cardLine = RGB(0xE5, 0xE5, 0xE5);
        t.text = RGB(0x1A, 0x1A, 0x1A), t.sub = RGB(0x5F, 0x5F, 0x5F), t.dim = RGB(0xA0, 0xA0, 0xA0);
        t.accent = Mix(base, RGB(0, 0, 0), 0.1f), t.onAccent = RGB(0xFF, 0xFF, 0xFF);
        t.control = RGB(0xFB, 0xFB, 0xFB), t.controlHot = RGB(0xF3, 0xF3, 0xF3), t.controlDown = RGB(0xEC, 0xEC, 0xEC);
        t.controlLine = RGB(0xD6, 0xD6, 0xD6), t.strong = RGB(0x85, 0x85, 0x85), t.field = RGB(0xFF, 0xFF, 0xFF);
        t.knob = RGB(0xFF, 0xFF, 0xFF), t.tabHot = RGB(0xE9, 0xE9, 0xE9), t.status = RGB(0xC4, 0x2B, 0x1C);
    }
    for (HBRUSH* b : {&t.backBrush, &t.cardBrush})
        if (*b) DeleteObject(*b);
    t.backBrush = CreateSolidBrush(t.back);
    t.cardBrush = CreateSolidBrush(t.card);
}

// A rounded rectangle with a smooth edge, blended onto `dc`: filled with `fill` and outlined `edge` pixels wide in
// `line`. Either may be CLR_NONE.
static void FillRound(HDC dc, RECT r, float radius, COLORREF fill, COLORREF line = CLR_NONE, float edge = 1) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    Bitmap b;
    if (w <= 0 || h <= 0 || !b.Create(w, h)) return;
    const bool hasFill = fill != CLR_NONE, hasLine = line != CLR_NONE;
    // Pixels further than this from every side are plain fill: only the edge needs the distance worked out.
    const int m = (int)std::ceil(radius + edge) + 1;
    const DWORD inside = hasFill ? 0xFF000000 | (DWORD)GetRValue(fill) << 16 | (DWORD)GetGValue(fill) << 8 | GetBValue(fill) : 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (y >= m && y < h - m && x == m && w - 2 * m > 0) {
                std::fill(b.px + y * w + m, b.px + y * w + (w - m), inside);
                x = w - m - 1;
                continue;
            }
            const float d = SdRoundRect(x + 0.5f, y + 0.5f, w / 2.0f, h / 2.0f, w / 2.0f, h / 2.0f, radius);
            const float out = Clamp01(0.5f - d), in = !hasLine ? out : hasFill ? Clamp01(0.5f - d - edge) : 0;
            const float a = hasFill ? out : out - Clamp01(0.5f - d - edge), ln = hasFill ? out - in : a;
            auto ch = [&](int f, int l) { return (DWORD)std::lround((hasFill ? f * in : 0) + (hasLine ? l * ln : 0)); };
            b.px[y * w + x] = (DWORD)std::lround(a * 255) << 24 | ch(GetRValue(fill), GetRValue(line)) << 16 |
                              ch(GetGValue(fill), GetGValue(line)) << 8 | ch(GetBValue(fill), GetBValue(line));
        }
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, r.left, r.top, w, h, b.dc, 0, 0, w, h, bf);
}

static int CALLBACK FoundFont(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
    *(bool*)found = true;
    return 0;
}

static bool FontExists(const wchar_t* face) {
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, FoundFont, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

// Keyboard cues (focus rings) show once the keyboard is used in the window, as elsewhere in Windows.
static bool CuesHidden(HWND h) { return (SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0; }

static void FocusRing(HDC dc, RECT r) { FillRound(dc, r, 4 * DpiScaleOf(g_setDpi), CLR_NONE, g_th.text, 1.5f * DpiScaleOf(g_setDpi)); }

// The keyboards added in Windows, one row per language code.
struct LangRow {
    wchar_t code[4];
    wchar_t names[128];
};
static std::vector<LangRow> g_langRows;

// Name of a keyboard layout as Windows shows it ("US", "Thai Kedmanee"), read from its entry under Keyboard Layouts.
static void LayoutName(HKL hkl, wchar_t* out, int n) {
    out[0] = 0;
    const WORD dev = HIWORD((UINT_PTR)hkl);
    const wchar_t* root = L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts";
    wchar_t klid[16] = L"";
    if ((dev & 0xF000) == 0xF000) {  // a variant of a layout: the entry with its Layout Id
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            wchar_t want[8], sub[16], id[8];
            swprintf(want, 8, L"%04x", dev & 0x0FFF);
            for (DWORD i = 0; !klid[0]; i++) {
                DWORD cch = 16, cb = sizeof(id);
                if (RegEnumKeyExW(k, i, sub, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                if (RegGetValueW(k, sub, L"Layout Id", RRF_RT_REG_SZ, nullptr, id, &cb) == ERROR_SUCCESS && _wcsicmp(id, want) == 0)
                    wcscpy(klid, sub);
            }
            RegCloseKey(k);
        }
    } else if ((dev & 0xF000) == 0xE000) {  // an old-style IME
        swprintf(klid, 16, L"%08X", (unsigned)(UINT_PTR)hkl);
    } else {
        swprintf(klid, 16, L"%08X", (unsigned)dev);
    }
    if (!klid[0]) return;
    wchar_t path[128];
    swprintf(path, 128, L"%ls\\%ls", root, klid);
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    if (RegLoadMUIStringW(k, L"Layout Display Name", out, n * sizeof(wchar_t), nullptr, 0, nullptr) != ERROR_SUCCESS) {
        DWORD cb = (n - 1) * sizeof(wchar_t);
        if (RegQueryValueExW(k, L"Layout Text", nullptr, nullptr, (BYTE*)out, &cb) != ERROR_SUCCESS) cb = 0;
        out[cb / sizeof(wchar_t)] = 0;
    }
    RegCloseKey(k);
}

static void ReadLanguages() {
    g_langRows.clear();
    HKL list[64];
    int n = GetKeyboardLayoutList(64, list);
    for (int i = 0; i < n; i++) {
        wchar_t code[4], name[96];
        CodeOf(list[i], code);
        LayoutName(list[i], name, 96);
        LangRow* row = nullptr;
        for (LangRow& r : g_langRows)
            if (wcscmp(r.code, code) == 0) row = &r;
        if (!row) {
            if (g_langRows.size() >= (size_t)kMaxRows) continue;
            g_langRows.push_back({});
            row = &g_langRows.back();
            wcscpy(row->code, code);
        }
        if (name[0] && !wcsstr(row->names, name) && wcslen(row->names) + wcslen(name) + 3 < 128) {
            if (row->names[0]) wcscat(row->names, L", ");
            wcscat(row->names, name);
        }
    }
}

// "1.2.1", from the exe's own version resource.
static void AppVersion(wchar_t* out, int n) {
    wcscpy(out, L"?");
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(1), RT_VERSION);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const BYTE* p = g ? (const BYTE*)LockResource(g) : nullptr;
    const DWORD size = r ? SizeofResource(nullptr, r) : 0;
    for (DWORD i = 0; p && i + sizeof(VS_FIXEDFILEINFO) <= size; i += 4) {
        const VS_FIXEDFILEINFO* fi = (const VS_FIXEDFILEINFO*)(p + i);
        if (fi->dwSignature != 0xFEEF04BD) continue;
        swprintf(out, n, L"%u.%u.%u", HIWORD(fi->dwProductVersionMS), LOWORD(fi->dwProductVersionMS), HIWORD(fi->dwProductVersionLS));
        break;
    }
}

static void OpenUrl(const wchar_t* url) { ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL); }

// Off, Double Shift only, Automatic: stored as AutoFix (on/off) + FixAsYouType, so older settings carry over.
static void SetFixMode(int pick) {
    g_set.autoFix = pick != 0;
    if (pick) g_set.fixAsYouType = pick == 2;
    SetAutoFix(g_set.autoFix != 0);
    SaveSettings();
}

// The tools' shortcuts are released while the Presentation tab is in front, so pressing one reaches its box.
static void UpdateKeyPause() {
    bool pause = g_setWnd && g_setActive && g_setTab == TAB_PRESENT;
    if (pause == g_keysPaused) return;
    g_keysPaused = pause;
    if (pause) UnregisterToolKeys();
    else RegisterToolKeys();
}

// Rebuilds the page soon, if the window is open (on `tab`, or on any tab for -1).
static void SettingsReload(int tab) {
    if (!g_setWnd || (tab >= 0 && tab != g_setTab) || g_reloadPosted) return;
    g_reloadPosted = true;
    PostMessageW(g_setWnd, WM_APP_RELOAD, 0, 0);
}

static void DeleteFonts() {
    for (HFONT* f : {&g_fontText, &g_fontBold, &g_fontHead, &g_fontIcon}) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
}

static void MakeFonts() {
    DeleteFonts();
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_setDpi);
    LOGFONTW lf = ncm.lfMessageFont;
    const bool variable = FontExists(L"Segoe UI Variable Text");  // Windows 11's UI font
    if (variable) wcscpy(lf.lfFaceName, L"Segoe UI Variable Text");
    g_fontText = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    g_fontBold = CreateFontIndirectW(&lf);
    lf.lfHeight = lf.lfHeight * 6 / 5;
    if (variable) wcscpy(lf.lfFaceName, L"Segoe UI Variable Display");
    g_fontHead = CreateFontIndirectW(&lf);
    LOGFONTW icon = {};
    icon.lfHeight = -SetPx(12);
    icon.lfCharSet = DEFAULT_CHARSET;
    icon.lfQuality = CLEARTYPE_QUALITY;
    wcscpy(icon.lfFaceName, FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets");
    g_fontIcon = CreateFontIndirectW(&icon);
}

static COLORREF ToRef(Rgb c) { return RGB(std::lround(c.r * 255), std::lround(c.g * 255), std::lround(c.b * 255)); }

// Puts controls on the page from top to bottom; `y` is where the next one goes.
struct Page {
    HWND wnd;
    static const int L = 20, W = 480;  // left margin and content width
    static const int Indent = 26;      // text under a check box or radio button lines up with its title
    static const int CardTop = 8, Pad = 14, Gap = 12;  // cards: first top, inner padding, space between
    int cardTop = CardTop, y = CardTop + Pad;

    explicit Page(HWND w) : wnd(w) { g_cards.clear(); }
    void EndCard() { g_cards.push_back({cardTop, y + Pad}); }

    int Px(int v) const { return MulDiv(v, (int)g_setDpi, 96); }
    HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int top, int w, int h, int id, HFONT font = g_fontText) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, Px(x), Px(top), Px(w), Px(h), wnd,
                                 (HMENU)(INT_PTR)id, g_inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
        return c;
    }
    int Measure(const wchar_t* text, int w, HFONT font) const {
        HDC dc = GetDC(wnd);
        HGDIOBJ old = SelectObject(dc, font);
        RECT r = {0, 0, Px(w), 0};
        DrawTextW(dc, text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(wnd, dc);
        return MulDiv(r.bottom, 96, (int)g_setDpi) + 1;
    }
    // Wrapped text at (x, y); returns its height.
    int Text(const wchar_t* text, int x, int top, int w, HFONT font, int id = 0) {
        int h = Measure(text, w, font);
        Add(L"STATIC", text, SS_NOPREFIX, x, top, w, h, id, font);
        return h;
    }
    void Header(const wchar_t* text) { y += Text(text, L, y, W, g_fontHead) + 4; }
    void Caption(const wchar_t* text, int x = L, int w = W) { y += Text(text, x, y, w, g_fontText, CTL_CAPTION); }
    // Ends this section's card and starts the next.
    void Separator() {
        EndCard();
        cardTop = y + Pad + Gap;
        y = cardTop + Pad;
    }
    HWND Check(const wchar_t* title, bool on, int id, int w = W, bool enabled = true) {
        HWND c = Add(L"BUTTON", title, BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, L, y, w, 22, id);
        SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(c, enabled);
        y += 22;
        return c;
    }
    // A switch with its explanation under it.
    void Option(const wchar_t* title, const wchar_t* detail, bool on, int id, bool enabled = true) {
        Check(title, on, id, W, enabled);
        Caption(detail, L + Indent, W - Indent);
        y += 10;
    }
    // A title and explanation on the left, a button on the right.
    HWND Action(const wchar_t* title, const wchar_t* detail, const wchar_t* button, int id) {
        const int bw = 140, tw = W - bw - 16;
        int top = y;
        y += Text(title, L, y, tw, g_fontText);
        Caption(detail, L, tw);
        HWND b = Add(L"BUTTON", button, WS_TABSTOP, L + W - bw, top, bw, 28, id);
        y = std::max(y, top + 28) + 10;
        return b;
    }
};

static void BuildBadgeTab(Page& p) {
    const int th = UiThai();
    p.Add(L"STATIC", L"", SS_OWNERDRAW, Page::L, p.y, 180, 128, CTL_PREVIEW);
    struct { const wchar_t* label; int id; const wchar_t* const* names; int count; DWORD value; } pops[] = {
        {TR(L"Show:", L"แสดง:"), CTL_MODE, kModeNames[th], MODE_COUNT, g_set.mode},
        {TR(L"Style:", L"รูปแบบ:"), CTL_STYLE, kStyleNames[th], STYLE_COUNT, g_set.style},
        {TR(L"Size:", L"ขนาด:"), CTL_SIZE, kSizeNames[th], SIZE_COUNT, g_set.size},
    };
    int top = p.y + 4;
    for (auto& pop : pops) {
        p.Add(L"STATIC", pop.label, SS_RIGHT | SS_CENTERIMAGE, 216, top, 84, 24, 0);
        HWND c = p.Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 308, top, 192, 200, pop.id);
        for (int i = 0; i < pop.count; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)pop.names[i]);
        SendMessageW(c, CB_SETCURSEL, pop.value, 0);
        top += 32;
    }
    p.Add(L"STATIC", TR(L"Border:", L"เส้นขอบ:"), SS_RIGHT | SS_CENTERIMAGE, 216, top, 84, 24, 0);
    HWND border = p.Add(L"BUTTON", TR(L"On", L"เปิด"), BS_AUTOCHECKBOX | WS_TABSTOP, 308, top, 192, 24, CTL_BORDER);
    SendMessageW(border, BM_SETCHECK, g_set.badgeBorder ? BST_CHECKED : BST_UNCHECKED, 0);
    p.y += 128 + 22;

    p.Header(TR(L"Languages", L"ภาษา"));
    p.Caption(TR(L"The keyboards added in Windows Settings \u2192 Time & language \u2192 Language & region.",
                 L"แป้นพิมพ์ที่เพิ่มไว้ใน Settings ของ Windows \u2192 Time & language \u2192 Language & region"));
    p.y += 10;
    p.Text(TR(L"Keyboard", L"แป้นพิมพ์"), Page::L, p.y, 240, g_fontBold);
    p.Text(TR(L"Color", L"สี"), 270, p.y, 120, g_fontBold);
    p.Text(TR(L"Show badge", L"แสดงป้าย"), 410, p.y, 90, g_fontBold);
    p.y += 24;
    for (size_t i = 0; i < g_langRows.size(); i++) {
        const LangRow& row = g_langRows[i];
        p.Add(L"STATIC", row.code, SS_CENTERIMAGE | SS_NOPREFIX, Page::L, p.y, 40, 26, 0, g_fontBold);
        p.Add(L"STATIC", row.names, SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS, 64, p.y, 196, 26, 0);
        p.Add(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, 270, p.y, 48, 26, CTL_COLOR_BASE + (int)i);
        HWND reset = p.Add(L"BUTTON", TR(L"Default", L"สีเดิม"), WS_TABSTOP, 326, p.y, 72, 26, CTL_DEFAULT_BASE + (int)i);
        EnableWindow(reset, FindColor(row.code) != nullptr);
        HWND show = p.Add(L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, 430, p.y, 22, 26, CTL_SHOW_BASE + (int)i);
        SendMessageW(show, BM_SETCHECK, IsHidden(row.code) ? BST_UNCHECKED : BST_CHECKED, 0);
        p.y += 32;
    }
}

static void BuildFixTab(Page& p) {
    const int th = UiThai();
    const int mode = !g_set.autoFix ? 0 : g_set.fixAsYouType ? 2 : 1;
    p.Header(TR(L"Fix Wrong Language (EN \u21C4 TH)", L"แก้คำที่พิมพ์ผิดภาษา (EN \u21C4 TH)"));
    const wchar_t* details[3] = {
        TR(L"Nothing is fixed, and CursorLingo doesn't look at your typing at all.", L"ไม่แก้อะไรให้ และไม่ดูสิ่งที่คุณพิมพ์เลย"),
        TR(L"Tap Shift twice to switch the last word (or the selected text) to the other language.",
           L"พิมพ์ผิดภาษาเมื่อไหร่ กด Shift 2 ครั้งติดกัน คำล่าสุด (หรือข้อความที่เลือกไว้) จะเปลี่ยนเป็นอีกภาษาให้"),
        TR(L"Words typed in the wrong language are fixed as you type. If a fix is wrong, double Shift puts it back.",
           L"แก้คำที่พิมพ์ผิดภาษาให้เองระหว่างพิมพ์ ถ้าแก้ไม่ตรงใจ กด Shift 2 ครั้งเพื่อย้อนคืน"),
    };
    for (int i = 0; i < 3; i++) {
        HWND r = p.Add(L"BUTTON", kFixNames[th][i], BS_AUTORADIOBUTTON | WS_TABSTOP | (i == 0 ? WS_GROUP : 0), Page::L, p.y, Page::W,
                       22, CTL_FIX_BASE + i);
        SendMessageW(r, BM_SETCHECK, mode == i ? BST_CHECKED : BST_UNCHECKED, 0);
        p.y += 22;
        p.Caption(details[i], Page::L + Page::Indent, Page::W - Page::Indent);
        p.y += 8;
    }
    p.Separator();
    wchar_t detail[512];
    swprintf(detail, 512,
             TR(L"Names in code often look like wrong-language words, so these apps aren't fixed automatically: %ls. Terminals "
                L"(%ls) never are, whatever this is set to. Both lists come with CursorLingo; ask for more on GitHub.",
                L"ชื่อตัวแปรในโค้ดหน้าตาคล้ายคำพิมพ์ผิดภาษาบ่อยๆ เลยไม่แก้เองในแอปพวกนี้: %ls ส่วน Terminal (%ls) ไม่แก้เองเลย "
                L"ไม่ว่าจะตั้งตรงนี้ไว้ยังไง รายชื่อแอปมีมากับ CursorLingo อยากให้เพิ่มแอปไหน บอกได้ที่ GitHub"),
             kCodeEditorNames, kTerminalNames);
    p.Option(TR(L"Code editors: fix only on double Shift", L"โปรแกรมเขียนโค้ด: แก้เฉพาะตอนกด Shift 2 ครั้ง"), detail,
             g_set.codeEditorsManual != 0, CTL_EDITORS, mode == 2);
    wchar_t learned[64];
    if (!g_ruleCount) wcscpy(learned, TR(L"Learned words: none yet", L"คำที่จำไว้: ยังไม่มี"));
    else swprintf(learned, 64, TR(L"Learned words: %d", L"คำที่จำไว้: %d คำ"), g_ruleCount);
    HWND forget = p.Action(learned,
                           TR(L"When you convert or undo a word with double Shift, CursorLingo remembers it and does the same next "
                              L"time. Kept on this PC only.",
                              L"คำไหนที่คุณกด Shift 2 ครั้งเพื่อแปลงหรือย้อนคืน จะถูกจำไว้ แล้วครั้งหน้าทำแบบเดียวกันให้เอง "
                              L"เก็บไว้ในเครื่องนี้เท่านั้น"),
                           TR(L"Forget", L"ล้าง"), CTL_FORGET);
    EnableWindow(forget, g_ruleCount > 0);
    p.Separator();
    p.Option(TR(L"Remember language per app", L"จำภาษาของแต่ละแอป"),
             TR(L"Going back to an app switches to the keyboard you last used there. Works even with fixing off.",
                L"กลับไปแอปไหน ก็สลับเป็นภาษาที่ใช้ล่าสุดในแอปนั้นให้เอง ใช้ได้แม้จะปิดการแก้คำไว้"),
             g_set.perAppLanguage != 0, CTL_PER_APP);
}

static void BuildPresentTab(Page& p) {
    const int th = UiThai();
    p.Header(TR(L"Tools", L"เครื่องมือ"));
    p.Caption(TR(L"To change a shortcut, click its box and press the new keys, with Ctrl or Alt. Backspace clears it.",
                 L"อยากเปลี่ยนคีย์ลัด คลิกที่ช่องแล้วกดคีย์ใหม่ (ต้องมี Ctrl หรือ Alt ด้วย) ถ้าไม่อยากใช้คีย์ลัด กด Backspace"));
    p.y += 12;
    const bool on[3] = {g_set.presentation != 0, g_zoom, g_drawing};
    const wchar_t* details[3] = {
        TR(L"Dims the screen except for a circle around the pointer. You can still click and type as usual.",
           L"หรี่จอ เหลือวงกลมใสรอบเมาส์ ยังคลิกและพิมพ์ได้ตามปกติ"),
        TR(L"Magnifies what is under the pointer, inside the circle.", L"ขยายจุดที่เมาส์ชี้ให้ใหญ่ขึ้นในวงกลม"),
        TR(L"Draw over the screen with the mouse. While it's on, clicks draw instead of reaching apps; Esc stops.",
           L"ลากเมาส์ขีดเขียนบนจอได้เลย ระหว่างนี้คลิกจะเป็นการวาด ไม่โดนแอปข้างใต้ กด Esc เพื่อเลิกวาด"),
    };
    for (int i = 0; i < 3; i++) {
        int top = p.y;
        p.Check(kToolNames[th][i], on[i], CTL_TOOL_BASE + i, 300);
        p.Caption(details[i], Page::L + Page::Indent, 300 - Page::Indent);
        HWND key = p.Add(HOTKEY_CLASSW, L"", WS_TABSTOP, 350, top, 150, 28, CTL_KEY_BASE + i);
        // Keys without Ctrl or Alt would take plain typing: the control turns them into Ctrl+Alt+key.
        SendMessageW(key, HKM_SETRULES, HKCOMB_NONE | HKCOMB_S, MAKELPARAM(HOTKEYF_CONTROL | HOTKEYF_ALT, 0));
        SendMessageW(key, HKM_SETHOTKEY, *ToolKey(i), 0);
        p.y = std::max(p.y, top + 28) + 12;
    }
    p.Add(L"STATIC", g_keyStatus, SS_NOPREFIX | SS_CENTERIMAGE, Page::L, p.y, 320, 28, CTL_STATUS);
    p.Add(L"BUTTON", TR(L"Restore defaults", L"คืนค่าคีย์ลัดเริ่มต้น"), WS_TABSTOP, 350, p.y, 150, 28, CTL_RESTORE);
    p.y += 28;
    p.Separator();

    p.Header(TR(L"Circle", L"วงกลม"));
    p.y += 4;
    struct { const wchar_t* label; int id, valueId, lo, hi; DWORD value; } sliders[] = {
        {TR(L"Size", L"ขนาดวง"), CTL_RADIUS, CTL_RADIUS_VALUE, 40, 300, g_set.spotRadius},
        {TR(L"Magnification", L"ระดับซูม"), CTL_ZOOM_LEVEL, CTL_ZOOM_VALUE, 15, 80, g_set.zoomLevel},
    };
    for (auto& s : sliders) {
        p.Add(L"STATIC", s.label, SS_CENTERIMAGE, Page::L, p.y, 110, 28, 0);
        HWND bar = p.Add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 130, p.y, 270, 28, s.id);
        SendMessageW(bar, TBM_SETRANGE, FALSE, MAKELPARAM(s.lo, s.hi));
        SendMessageW(bar, TBM_SETPAGESIZE, 0, 10);
        SendMessageW(bar, TBM_SETPOS, TRUE, std::min(std::max((int)s.value, s.lo), s.hi));
        p.Add(L"STATIC", L"", SS_CENTERIMAGE, 410, p.y, 90, 28, s.valueId);
        p.y += 34;
    }
    p.Separator();

    p.Header(TR(L"While a Tool Is On", L"ระหว่างใช้งาน"));
    p.y += 4;
    const wchar_t* legend[][2] = {
        {TR(L"Ctrl+Alt + wheel", L"Ctrl+Alt + ล้อเมาส์"), TR(L"Resize the circle", L"ปรับขนาดวง")},
        {L"Ctrl+Alt+= / \u2212", TR(L"Zoom in / out", L"ซูมเข้า / ออก")},
        {L"Ctrl+Z", TR(L"Take back the last stroke", L"ลบเส้นที่วาดล่าสุด")},
        {L"Esc", TR(L"Stop drawing; press again to turn everything off", L"เลิกวาด กดอีกครั้งเพื่อปิดทั้งหมด")},
        {TR(L"Click", L"คลิก"), TR(L"A ring in the color of the language in use", L"มีวงกระเพื่อมออกมา เป็นสีของภาษาที่ใช้อยู่")},
    };
    for (auto& l : legend) {
        int h = std::max(p.Text(l[0], Page::L, p.y, 180, g_fontBold), p.Text(l[1], 210, p.y, 290, g_fontText));
        p.y += h + 6;
    }
}

static void BuildGeneralTab(Page& p) {
    const int th = UiThai();
    HWND icon = p.Add(L"STATIC", L"", SS_ICON | SS_REALSIZEIMAGE, Page::L, p.y, 48, 48, 0);
    SendMessageW(icon, STM_SETICON, (WPARAM)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, p.Px(48), p.Px(48), LR_SHARED), 0);
    wchar_t version[16], about[160];
    AppVersion(version, 16);
    swprintf(about, 160, TR(L"Version %ls \u00B7 Free and open source \u00B7 No network access, no tracking",
                            L"เวอร์ชัน %ls \u00B7 ฟรีและโอเพนซอร์ส \u00B7 ไม่ต่อเน็ต ไม่เก็บข้อมูลการใช้งาน"),
             version);
    int top = p.y + 4;
    top += p.Text(kAppName, 84, top, 416, g_fontHead) + 2;
    top += p.Text(about, 84, top, 416, g_fontText, CTL_CAPTION);
    p.y = std::max(p.y + 48, top);  // the about line wraps at larger text sizes
    p.Separator();

    if (IsPackaged()) {  // switched in Windows Settings; the app cannot tell whether it is on
        p.Action(TR(L"Open CursorLingo automatically", L"เปิด CursorLingo อัตโนมัติ"),
                 TR(L"Installed from the Microsoft Store, so Windows looks after this: switch CursorLingo on under Settings "
                    L"\u2192 Apps \u2192 Startup.",
                    L"ติดตั้งจาก Microsoft Store เรื่องนี้ Windows เป็นคนดูแล เปิดได้ที่ Settings \u2192 Apps \u2192 Startup"),
                 TR(L"Startup apps\u2026", L"เปิด Startup apps\u2026"), CTL_STARTUP);
    } else {
        p.Option(TR(L"Open CursorLingo automatically", L"เปิด CursorLingo อัตโนมัติ"),
                 TR(L"Starts by itself when you sign in to Windows, so the badge is always there.",
                    L"เปิดเครื่องหรือล็อกอินเมื่อไหร่ CursorLingo ก็ทำงานเลย ไม่ต้องคอยเปิดเอง"),
                 StartupEnabled(), CTL_STARTUP);
    }
    int langTop = p.y;
    p.y += p.Text(L"Language / ภาษา", Page::L, p.y, 300, g_fontText);
    p.Caption(TR(L"The language of the menu and this window.", L"ภาษาที่ใช้ในเมนูและหน้าต่างนี้"), Page::L, 300);
    HWND lang = p.Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 340, langTop, 160, 200, CTL_UI_LANG);
    for (int i = 0; i < 3; i++) SendMessageW(lang, CB_ADDSTRING, 0, (LPARAM)kUiLangNames[th][i]);
    SendMessageW(lang, CB_SETCURSEL, g_set.uiLang, 0);
    p.y += 10;
    p.Action(TR(L"Found a wrong fix or a bug?", L"เจอคำที่แก้ผิด หรือเจอบั๊ก?"),
             TR(L"Opens the report form on GitHub in your browser. Nothing you typed is attached.",
                L"จะเปิดหน้าแจ้งปัญหาบน GitHub ในเบราว์เซอร์ ไม่มีอะไรที่คุณพิมพ์ติดไปด้วย"),
             TR(L"Report\u2026", L"แจ้งปัญหา\u2026"), CTL_REPORT);
    p.Separator();

    p.Header(TR(L"Support CursorLingo", L"สนับสนุน CursorLingo"));
    // The Store allows donations only through a purchase API (policy 10.8.2), so its build leaves out Ko-fi and
    // PromptPay; they stay in the build from GitHub.
    if (IsPackaged()) {
        p.Caption(TR(L"CursorLingo is a free, spare-time project. If it saves you some retyping, a star on GitHub or a share "
                     L"with friends means a lot. Got an idea, a word it got wrong, or some code? Come say hi.",
                     L"CursorLingo ทำในเวลาว่าง และแจกให้ใช้ฟรี ถ้ามันช่วยให้ไม่ต้องลบแล้วพิมพ์ใหม่บ่อยๆ ฝากกดดาวบน GitHub "
                     L"หรือแชร์ให้เพื่อนๆ ก็ได้นะ มีไอเดีย เจอคำที่แก้ผิด หรืออยากช่วยเขียนโค้ด ทักมาได้เลย"));
        p.y += 10;
        p.Add(L"BUTTON", TR(L"\u2605 Star on GitHub", L"\u2605 ให้ดาวบน GitHub"), WS_TABSTOP, Page::L, p.y, 150, 30, CTL_STAR);
        p.Add(L"BUTTON", TR(L"How to contribute", L"วิธีร่วมพัฒนา"), WS_TABSTOP, Page::L + 160, p.y, 130, 30, CTL_CONTRIBUTE);
        p.y += 30;
        return;
    }
    p.Caption(TR(L"CursorLingo is a free, spare-time project. If it saves you some retyping, a star on GitHub, a share with "
                 L"friends or a coffee means a lot. Got an idea, a word it got wrong, or some code? Come say hi.",
                 L"CursorLingo ทำในเวลาว่าง และแจกให้ใช้ฟรี ถ้ามันช่วยให้ไม่ต้องลบแล้วพิมพ์ใหม่บ่อยๆ ฝากกดดาวบน GitHub "
                 L"แชร์ให้เพื่อนๆ หรือเลี้ยงกาแฟสักแก้วก็ได้นะ มีไอเดีย เจอคำที่แก้ผิด หรืออยากช่วยเขียนโค้ด ทักมาได้เลย"));
    p.y += 10;
    p.Add(L"BUTTON", TR(L"\u2605 Star on GitHub", L"\u2605 ให้ดาวบน GitHub"), WS_TABSTOP, Page::L, p.y, 150, 30, CTL_STAR);
    p.Add(L"BUTTON", TR(L"How to contribute", L"วิธีร่วมพัฒนา"), WS_TABSTOP, Page::L + 160, p.y, 130, 30, CTL_CONTRIBUTE);
    p.Add(L"BUTTON", TR(L"\u2615 Buy a coffee on Ko-fi", L"\u2615 เลี้ยงกาแฟผ่าน Ko-fi"), WS_TABSTOP, Page::L + 300, p.y, 180, 30,
          CTL_KOFI);
    p.y += 30 + 14;

    // PromptPay: the QR code on white (it must stay dark on light to scan), the number and a copy button.
    const int qrTop = p.y, x = Page::L + 132, w = Page::W - 132;
    p.Add(L"STATIC", L"", SS_OWNERDRAW, Page::L, p.y, 116, 116, CTL_QR);
    p.y += p.Text(TR(L"PromptPay (Thailand)", L"พร้อมเพย์ (PromptPay)"), x, p.y, w, g_fontBold) + 2;
    p.y += p.Text(kPromptPayShown, x, p.y, w, g_fontHead) + 4;
    p.Caption(TR(L"Scan with a Thai banking app and type any amount, or copy the number.",
                 L"สแกนด้วยแอปธนาคาร แล้วใส่จำนวนเงินเท่าไหร่ก็ได้ หรือคัดลอกเลขไปโอนก็ได้"), x, w);
    p.y += 8;
    p.Add(L"BUTTON", TR(L"Copy number", L"คัดลอกเลข"), WS_TABSTOP, x, p.y, 120, 28, CTL_COPY_NUMBER);
    p.y = std::max(p.y + 28, qrTop + 116);
}

static void ShowSliderValues() {
    wchar_t text[32];
    int r = (int)SendDlgItemMessageW(g_setPage, CTL_RADIUS, TBM_GETPOS, 0, 0);
    swprintf(text, 32, TR(L"%d pt", L"%d พอยต์"), r);
    SetDlgItemTextW(g_setPage, CTL_RADIUS_VALUE, text);
    int z = (int)SendDlgItemMessageW(g_setPage, CTL_ZOOM_LEVEL, TBM_GETPOS, 0, 0);
    swprintf(text, 32, L"%d.%d\u00D7", z / 10, z % 10);
    SetDlgItemTextW(g_setPage, CTL_ZOOM_VALUE, text);
}

static void ApplyKey(int i, HWND box);

// Keeps what the shortcut boxes on the page say, before the page goes away.
static void ApplyKeyBoxes() {
    for (int i = 0; i < 3 && g_setPage; i++) {
        HWND box = GetDlgItem(g_setPage, CTL_KEY_BASE + i);
        if (box && (WORD)SendMessageW(box, HKM_GETHOTKEY, 0, 0) != *ToolKey(i)) ApplyKey(i, box);
    }
}

static void StylePageControls();

// Builds the open tab's page and fits the window around it.
static void BuildPage() {
    ApplyKeyBoxes();
    int focusId = 0;
    HWND focus = GetFocus();
    if (g_setPage && focus && GetParent(focus) == g_setPage) focusId = GetDlgCtrlID(focus);
    SendMessageW(g_setWnd, WM_SETREDRAW, FALSE, 0);
    if (g_setPage) DestroyWindow(g_setPage);
    g_setPage = CreateWindowExW(WS_EX_CONTROLPARENT, L"CursorLingoPage", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                0, 0, 1, 1, g_setWnd, nullptr, g_inst, nullptr);
    Page p{g_setPage};
    switch (g_setTab) {
        case TAB_BADGE: BuildBadgeTab(p); break;
        case TAB_FIX: BuildFixTab(p); break;
        case TAB_PRESENT: BuildPresentTab(p); ShowSliderValues(); break;
        default: BuildGeneralTab(p); break;
    }
    p.EndCard();
    StylePageControls();
    // The tab strip on top, the page of cards under it, the window round both.
    RECT item = {};
    TabCtrl_GetItemRect(g_setTabs, 0, &item);
    const int edge = p.Px(6), width = p.Px(520), tabsHeight = item.bottom - item.top + p.Px(10);
    const int pageTop = p.Px(6) + tabsHeight, pageHeight = p.Px(p.y + Page::Pad + 8);
    MoveWindow(g_setTabs, edge + p.Px(6), p.Px(6), width - p.Px(12), tabsHeight, FALSE);
    SetWindowPos(g_setPage, HWND_TOP, edge, pageTop, width, pageHeight, SWP_NOACTIVATE | SWP_NOREDRAW);
    const DWORD style = GetWindowLongW(g_setWnd, GWL_STYLE), ex = GetWindowLongW(g_setWnd, GWL_EXSTYLE);
    RECT frame = {0, 0, edge * 2 + width, pageTop + pageHeight + edge};
    AdjustWindowRectExForDpi(&frame, style, FALSE, ex, g_setDpi);
    SetWindowPos(g_setWnd, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(g_setWnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(g_setWnd, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
    if (HWND again = focusId ? GetDlgItem(g_setPage, focusId) : nullptr) SetFocus(again);
    else if (g_setActive && (!GetFocus() || GetFocus() == g_setWnd)) SetFocus(g_setTabs);
}

// The badge next to a pointer, on a soft background, the way it looks on screen.
static void DrawPreview(HDC dc, RECT r) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    Bitmap canvas;
    if (!canvas.Create(w, h)) return;
    const COLORREF bg = g_th.card;
    const Rgb back = {GetRValue(bg) / 255.0f, GetGValue(bg) / 255.0f, GetBValue(bg) / 255.0f};
    const Rgb a = {0.36f, 0.46f, 0.68f}, b = {0.64f, 0.52f, 0.74f};
    const float radius = 10 * DpiScaleOf(g_setDpi);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float t = (x + y) / (float)(w + h), in = Clamp01(0.5f - SdRoundRect(x + 0.5f, y + 0.5f, w / 2.0f, h / 2.0f, w / 2.0f, h / 2.0f, radius));
            Rgb c = {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
            c = {back.r + (c.r - back.r) * in, back.g + (c.g - back.g) * in, back.b + (c.b - back.b) * in};
            canvas.px[y * w + x] = 0xFF000000 | ToRef({c.b, c.g, c.r});  // a DIB stores blue in the low byte
        }
    const int cx = w * 34 / 100, cy = h * 36 / 100;
    const int cursor = MulDiv(32, (int)g_setDpi, 96);
    DrawIconEx(canvas.dc, cx, cy, LoadCursorW(nullptr, IDC_ARROW), cursor, cursor, 0, nullptr, DI_NORMAL);

    const float s = kSizeScale[g_set.size] * DpiScaleOf(g_setDpi);
    const bool dot = g_set.style == STYLE_DOT;
    const int bw = (int)std::lround((dot ? 12 : (wcslen(g_code) > 2 ? 30 : 24)) * s), bh = (int)std::lround((dot ? 12 : 17) * s);
    const int pad = (int)std::ceil(3 * DpiScaleOf(g_setDpi));
    Bitmap badge;
    if (badge.Create(bw + 2 * pad, bh + 2 * pad)) {
        RenderBadge(badge, g_code, g_set.style, bw, bh, 11.5f * s, true);
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        AlphaBlend(canvas.dc, cx + (int)(14 * s) - pad, cy + (int)(20 * s) - pad, badge.w, badge.h, badge.dc, 0, 0, badge.w, badge.h, bf);
    }
    BitBlt(dc, r.left, r.top, w, h, canvas.dc, 0, 0, SRCCOPY);
}

// The PromptPay QR code: whole pixels per module, centred on a white card with a quiet zone round it.
static void DrawPromptPayQr(HDC dc, RECT r) {
    FillRect(dc, &r, g_th.cardBrush);
    const int n = (int)(sizeof(kPromptPayQr) / sizeof(kPromptPayQr[0])), side = std::min(r.right - r.left, r.bottom - r.top);
    const int module = std::max(1, side / (n + 4)), size = module * n;
    FillRound(dc, {r.left, r.top, r.left + side, r.top + side}, 10 * DpiScaleOf(g_setDpi), RGB(255, 255, 255), g_th.cardLine);
    const int x0 = r.left + (side - size) / 2, y0 = r.top + (side - size) / 2;
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (kPromptPayQr[y][x] == '#') {
                RECT m = {x0 + x * module, y0 + y * module, x0 + (x + 1) * module, y0 + (y + 1) * module};
                FillRect(dc, &m, black);
            }
}

// Puts the PromptPay number on the clipboard, as the user asked; the button says so for a moment.
static void CopyPromptPay(HWND button) {
    if (!OpenClipboard(g_setWnd)) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(kPromptPay))) {
        memcpy(GlobalLock(mem), kPromptPay, sizeof(kPromptPay));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
    SetWindowTextW(button, TR(L"Copied \u2713", L"คัดลอกแล้ว \u2713"));
    SetTimer(g_setPage, 1, 1500, nullptr);
}

static void DrawSwatch(const DRAWITEMSTRUCT* d, const wchar_t* code) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    FillRect(dc, &r, g_th.cardBrush);
    const float s = DpiScaleOf(g_setDpi);
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) FocusRing(dc, r);
    InflateRect(&r, -SetPx(3), -SetPx(3));
    // The edge is the colour itself, nudged away from the card, so every colour keeps an outline.
    const COLORREF fill = ToRef(AccentFor(code));
    FillRound(dc, r, 4 * s, fill, Mix(fill, g_th.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0), 0.18f));
}

// Push buttons, check boxes and radio buttons in the Windows 11 style, drawn on the card.
static LRESULT DrawButton(const NMCUSTOMDRAW* cd) {
    HWND b = cd->hdr.hwndFrom;
    const LONG type = GetWindowLongW(b, GWL_STYLE) & BS_TYPEMASK;
    if (cd->dwDrawStage != CDDS_PREPAINT || type == BS_OWNERDRAW) return CDRF_DODEFAULT;
    HDC dc = cd->hdc;
    RECT r = cd->rc;
    const LRESULT state = SendMessageW(b, BM_GETSTATE, 0, 0);
    const bool enabled = IsWindowEnabled(b), hot = enabled && (cd->uItemState & CDIS_HOT), down = enabled && (state & BST_PUSHED);
    const bool focus = GetFocus() == b && !CuesHidden(b);
    const float s = DpiScaleOf(g_setDpi);
    wchar_t text[128];
    GetWindowTextW(b, text, 128);
    FillRect(dc, &r, g_th.cardBrush);
    SetBkMode(dc, TRANSPARENT);
    HFONT font = (HFONT)SendMessageW(b, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = SelectObject(dc, font);
    if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) {
        FillRound(dc, r, 4 * s, down ? g_th.controlDown : hot ? g_th.controlHot : g_th.control, g_th.controlLine);
        SetTextColor(dc, !enabled ? g_th.dim : down ? g_th.sub : g_th.text);
        DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX);
        if (focus) FocusRing(dc, r);
    } else {
        const bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON, on = (state & BST_CHECKED) != 0;
        const int side = SetPx(18), top = (r.top + r.bottom - side) / 2;
        RECT box = {r.left + SetPx(1), top, r.left + SetPx(1) + side, top + side};
        const float round = radio ? side / 2.0f : 4 * s;
        if (on) {
            const COLORREF fill = !enabled ? g_th.dim : hot ? Mix(g_th.accent, g_th.card, 0.12f) : g_th.accent;
            FillRound(dc, box, round, fill);
            if (radio) {
                RECT dot = box;
                InflateRect(&dot, -SetPx(hot ? 4 : 5), -SetPx(hot ? 4 : 5));
                FillRound(dc, dot, (dot.right - dot.left) / 2.0f, g_th.onAccent);
            } else {
                SelectObject(dc, g_fontIcon);
                SetTextColor(dc, g_th.onAccent);
                DrawTextW(dc, L"", 1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);  // CheckMark
                SelectObject(dc, font);
            }
        } else {
            FillRound(dc, box, round, hot ? g_th.controlHot : g_th.control, enabled ? g_th.strong : g_th.dim);
        }
        RECT t = r;
        t.left = box.right + SetPx(8);
        int right = box.right + SetPx(2);
        if (text[0]) {
            SetTextColor(dc, enabled ? g_th.text : g_th.dim);
            DrawTextW(dc, text, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX | DT_END_ELLIPSIS);
            RECT m = t;
            DrawTextW(dc, text, -1, &m, DT_CALCRECT | DT_SINGLELINE | DT_HIDEPREFIX);
            right = std::min((int)r.right, (int)m.right + SetPx(4));
        }
        if (focus) FocusRing(dc, {r.left, r.top, right, r.bottom});
    }
    SelectObject(dc, oldFont);
    return CDRF_SKIPDEFAULT;
}

// A slider in the Windows 11 style: a thin track, filled in the accent colour up to a round thumb.
static LRESULT DrawSlider(const NMCUSTOMDRAW* cd) {
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    HWND bar = cd->hdr.hwndFrom;
    RECT thumb;
    SendMessageW(bar, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
    const int cx = (thumb.left + thumb.right) / 2, cy = (thumb.top + thumb.bottom) / 2;
    if (cd->dwItemSpec == TBCD_CHANNEL) {
        const int h = SetPx(4);
        RECT track = {cd->rc.left, cy - h / 2, cd->rc.right, cy - h / 2 + h}, done = track;
        done.right = cx;
        FillRound(cd->hdc, track, h / 2.0f, g_th.strong);
        FillRound(cd->hdc, done, h / 2.0f, g_th.accent);
    } else if (cd->dwItemSpec == TBCD_THUMB) {
        const int d = SetPx(20), inner = GetFocus() == bar ? SetPx(12) : SetPx(10);
        FillRound(cd->hdc, {cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d}, d / 2.0f, g_th.knob, g_th.controlLine);
        FillRound(cd->hdc, {cx - inner / 2, cy - inner / 2, cx - inner / 2 + inner, cy - inner / 2 + inner}, inner / 2.0f, g_th.accent);
    }
    return CDRF_SKIPDEFAULT;
}

// "Ctrl + Alt + H", as a shortcut box shows it; while keys are held, the modifiers so far.
static void ShortcutLabel(DWORD k, wchar_t* out, int n) {
    const BYTE f = HIBYTE(k), vk = LOBYTE(k);
    wchar_t key[32] = L"";
    if (vk) KeyText(MAKEWORD(vk, f & HOTKEYF_EXT), key, 32);
    swprintf(out, n, L"%ls%ls%ls%ls", f & HOTKEYF_CONTROL ? L"Ctrl + " : L"", f & HOTKEYF_ALT ? L"Alt + " : L"",
             f & HOTKEYF_SHIFT ? L"Shift + " : L"", key);
}

// The shortcut boxes keep the hotkey control's keyboard handling but are painted like Windows 11 text boxes.
static void PaintKeyBox(HWND h, HDC target) {
    RECT r;
    GetClientRect(h, &r);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, r.right, r.bottom), oldBmp = (HBITMAP)SelectObject(dc, bmp);
    const bool focus = GetFocus() == h, enabled = IsWindowEnabled(h);
    const float s = DpiScaleOf(g_setDpi);
    FillRect(dc, &r, g_th.cardBrush);
    FillRound(dc, r, 4 * s, focus ? g_th.field : g_th.control, g_th.controlLine);
    if (focus) FillRound(dc, {r.left + SetPx(2), r.bottom - SetPx(2), r.right - SetPx(2), r.bottom}, 1, g_th.accent);
    wchar_t text[64];
    const DWORD k = (DWORD)SendMessageW(h, HKM_GETHOTKEY, 0, 0);
    ShortcutLabel(k, text, 64);
    const bool none = !text[0];
    if (none) wcscpy(text, TR(L"None", L"ไม่มี"));
    RECT t = r;
    t.left += SetPx(10);
    t.right -= SetPx(6);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, none || !enabled ? g_th.dim : g_th.text);
    HGDIOBJ oldFont = SelectObject(dc, g_fontText);
    DrawTextW(dc, text, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
    BitBlt(target, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static LRESULT CALLBACK KeyBoxProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            PaintKeyBox(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, KeyBoxProc, id);
            break;
    }
    const LRESULT result = DefSubclassProc(h, msg, wp, lp);
    switch (msg) {
        case WM_SETFOCUS: case WM_KILLFOCUS: case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR: case HKM_SETHOTKEY: case WM_ENABLE:
            if (GetFocus() == h) DestroyCaret();  // the focus line under the box shows where typing goes
            InvalidateRect(h, nullptr, FALSE);
            break;
    }
    return result;
}

// The tab strip: the tab control's own keyboard and mouse handling, painted as Windows 11 draws a selector bar.
static void PaintTabs(HWND h, HDC target) {
    RECT c;
    GetClientRect(h, &c);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, c.right, c.bottom), oldBmp = (HBITMAP)SelectObject(dc, bmp);
    FillRect(dc, &c, g_th.backBrush);
    SetBkMode(dc, TRANSPARENT);
    const int sel = TabCtrl_GetCurSel(h), count = TabCtrl_GetItemCount(h);
    const float s = DpiScaleOf(g_setDpi);
    for (int i = 0; i < count; i++) {
        RECT item;
        TabCtrl_GetItemRect(h, i, &item);
        item.top = c.top + SetPx(2);
        item.bottom = c.bottom - SetPx(2);
        if (i == g_tabHot && i != sel) FillRound(dc, item, 4 * s, g_th.tabHot);
        wchar_t text[64];
        TCITEMW ti = {TCIF_TEXT};
        ti.pszText = text;
        ti.cchTextMax = 64;
        TabCtrl_GetItem(h, i, &ti);
        HGDIOBJ oldFont = SelectObject(dc, i == sel ? g_fontBold : g_fontText);
        SetTextColor(dc, i == sel || i == g_tabHot ? g_th.text : g_th.sub);
        RECT t = item;
        t.bottom -= SetPx(3);
        DrawTextW(dc, text, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        if (i == sel) {
            const int w = SetPx(18), x = (item.left + item.right - w) / 2;
            FillRound(dc, {x, item.bottom - SetPx(4), x + w, item.bottom - SetPx(1)}, 1.5f * s, g_th.accent);
            if (GetFocus() == h && !CuesHidden(h)) FocusRing(dc, item);
        }
    }
    BitBlt(target, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static LRESULT CALLBACK TabsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            PaintTabs(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintTabs(h, (HDC)wp);
            return 0;
        case WM_MOUSEMOVE: {
            TCHITTESTINFO hit = {{(short)LOWORD(lp), (short)HIWORD(lp)}};
            const int hot = TabCtrl_HitTest(h, &hit);
            if (hot != g_tabHot) {
                g_tabHot = hot;
                InvalidateRect(h, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h};
            TrackMouseEvent(&tme);
            break;
        }
        case WM_MOUSELEAVE:
            g_tabHot = -1;
            InvalidateRect(h, nullptr, FALSE);
            break;
        case WM_NCDESTROY:
            g_tabHot = -1;
            RemoveWindowSubclass(h, TabsProc, id);
            break;
    }
    const LRESULT result = DefSubclassProc(h, msg, wp, lp);
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_UPDATEUISTATE || msg == WM_KEYDOWN || msg == WM_LBUTTONDOWN)
        InvalidateRect(h, nullptr, FALSE);
    return result;
}

// Dark combo boxes come from Windows' own dark theme for them; the shortcut boxes get their painting.
static void StylePageControls() {
    for (HWND c = GetWindow(g_setPage, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        wchar_t cls[32];
        GetClassNameW(c, cls, 32);
        if (_wcsicmp(cls, WC_COMBOBOXW) == 0) SetWindowTheme(c, g_th.dark ? L"DarkMode_CFD" : nullptr, nullptr);
        else if (_wcsicmp(cls, HOTKEY_CLASSW) == 0) {
            SetWindowSubclass(c, KeyBoxProc, 1, 0);
            // The control adds a sunken edge of its own; the rounded box replaces it.
            SetWindowLongW(c, GWL_EXSTYLE, GetWindowLongW(c, GWL_EXSTYLE) & ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
            SetWindowLongW(c, GWL_STYLE, GetWindowLongW(c, GWL_STYLE) & ~WS_BORDER);
            SetWindowPos(c, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }
}

// The title bar takes the window's background colour, dark in dark mode (Windows 11; older versions ignore it).
static void StyleTitleBar() {
    const BOOL dark = g_th.dark;
    DwmSetWindowAttribute(g_setWnd, 20, &dark, sizeof(dark));  // DWMWA_USE_IMMERSIVE_DARK_MODE
    const COLORREF caption = g_th.contrast ? 0xFFFFFFFF : g_th.back;  // DWMWA_COLOR_DEFAULT under high contrast
    DwmSetWindowAttribute(g_setWnd, 35, &caption, sizeof(caption));  // DWMWA_CAPTION_COLOR
}

static void PickColor(const wchar_t* code) {
    static COLORREF custom[16];
    CHOOSECOLORW cc = {sizeof(cc)};
    cc.hwndOwner = g_setWnd;
    cc.rgbResult = ToRef(AccentFor(code));
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;
    SetColor(code, cc.rgbResult);
    Refresh(true);
    SettingsReload(TAB_BADGE);
}

// A shortcut box changed: keep the new keys unless a tool needs them, another tool has them, or another app does.
static void ApplyKey(int i, HWND box) {
    const DWORD k = (WORD)SendMessageW(box, HKM_GETHOTKEY, 0, 0);
    if (k && !LOBYTE(k)) return;  // only modifiers so far
    wchar_t text[32];
    KeyText(k, text, 32);
    g_keyStatus[0] = 0;
    const DWORD reserved[] = {MAKEWORD(VK_OEM_PLUS, HOTKEYF_CONTROL | HOTKEYF_ALT), MAKEWORD(VK_OEM_MINUS, HOTKEYF_CONTROL | HOTKEYF_ALT),
                              MAKEWORD('Z', HOTKEYF_CONTROL)};
    for (DWORD r : reserved)
        if (k == r) swprintf(g_keyStatus, 128, TR(L"%ls is taken while a tool is on. Pick another.", L"%ls ใช้ตอนเปิดเครื่องมืออยู่แล้ว เลือกคีย์อื่นนะ"), text);
    for (int j = 0; j < 3 && !g_keyStatus[0]; j++)
        if (j != i && k && *ToolKey(j) == k)
            swprintf(g_keyStatus, 128, TR(L"%ls is already used for %ls.", L"%ls ใช้กับ%lsอยู่แล้ว"), text, kToolNames[UiThai()][j]);
    if (!g_keyStatus[0]) {
        const DWORD old = *ToolKey(i);
        *ToolKey(i) = k;
        const int failed = RegisterToolKeys();
        if (g_keysPaused) UnregisterToolKeys();
        if (failed & (1 << i)) {
            *ToolKey(i) = old;
            if (!g_keysPaused) RegisterToolKeys();
            swprintf(g_keyStatus, 128, TR(L"Another app already uses %ls.", L"มีแอปอื่นใช้ %ls อยู่แล้ว"), text);
        } else {
            SaveSettings();
        }
    }
    // A refused key goes back to the one in use, or rebuilding the page would offer it again and repeat the message.
    if (g_keyStatus[0]) SendMessageW(box, HKM_SETHOTKEY, *ToolKey(i), 0);
    SetDlgItemTextW(g_setPage, CTL_STATUS, g_keyStatus);
}

static void SetSettingsTexts() {
    const int th = UiThai();
    SetWindowTextW(g_setWnd, TR(L"CursorLingo Settings", L"ตั้งค่า CursorLingo"));
    for (int i = 0; i < TAB_COUNT; i++) {
        TCITEMW item = {TCIF_TEXT};
        item.pszText = (LPWSTR)kTabNames[th][i];
        if (TabCtrl_GetItemCount(g_setTabs) > i) TabCtrl_SetItem(g_setTabs, i, &item);
        else TabCtrl_InsertItem(g_setTabs, i, &item);
    }
}

static void PageCommand(int id, int code, HWND ctl) {
    const bool checked = SendMessageW(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (id == CTL_MODE || id == CTL_STYLE || id == CTL_SIZE) {
        if (code != CBN_SELCHANGE) return;
        DWORD v = (DWORD)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        (id == CTL_MODE ? g_set.mode : id == CTL_STYLE ? g_set.style : g_set.size) = v;
        SaveSettings();
        Refresh(true);
        InvalidateRect(GetDlgItem(g_setPage, CTL_PREVIEW), nullptr, FALSE);
    } else if (id == CTL_BORDER) {
        g_set.badgeBorder = checked;
        SaveSettings();
        Refresh(true);
        InvalidateRect(GetDlgItem(g_setPage, CTL_PREVIEW), nullptr, FALSE);
    } else if (id >= CTL_COLOR_BASE && id < CTL_COLOR_BASE + (int)g_langRows.size()) {
        PickColor(g_langRows[id - CTL_COLOR_BASE].code);
    } else if (id >= CTL_DEFAULT_BASE && id < CTL_DEFAULT_BASE + (int)g_langRows.size()) {
        SetColor(g_langRows[id - CTL_DEFAULT_BASE].code, CLR_NONE);
        Refresh(true);
        SettingsReload(TAB_BADGE);
    } else if (id >= CTL_SHOW_BASE && id < CTL_SHOW_BASE + (int)g_langRows.size()) {
        if (IsHidden(g_langRows[id - CTL_SHOW_BASE].code) == checked) ToggleHidden(g_langRows[id - CTL_SHOW_BASE].code);
        SaveSettings();
        Refresh(false);
    } else if (id >= CTL_FIX_BASE && id < CTL_FIX_BASE + 3) {
        if (!checked) return;
        SetFixMode(id - CTL_FIX_BASE);
        SettingsReload(TAB_FIX);  // code editors only apply to Automatic
    } else if (id == CTL_EDITORS) {
        g_set.codeEditorsManual = checked;
        SaveSettings();
    } else if (id == CTL_FORGET) {
        g_ruleCount = 0;
        SaveRules();
        SettingsReload(TAB_FIX);
    } else if (id == CTL_PER_APP) {
        g_set.perAppLanguage = checked;
        SaveSettings();  // the first app is remembered when it comes back to the front
    } else if (id >= CTL_TOOL_BASE && id < CTL_TOOL_BASE + 3) {
        if (id == CTL_TOOL_BASE) {
            SetPresentation(checked);
            SaveSettings();
        } else if (id == CTL_TOOL_BASE + 1) {
            SetZoom(checked);
        } else {
            SetDrawing(checked);
        }
        SettingsReload(TAB_PRESENT);  // Zoom may not be there
    } else if (id >= CTL_KEY_BASE && id < CTL_KEY_BASE + 3) {
        if (code == EN_CHANGE) ApplyKey(id - CTL_KEY_BASE, ctl);
    } else if (id == CTL_RESTORE) {
        for (int i = 0; i < 3; i++) {
            *ToolKey(i) = kToolDefaults[i];
            // The boxes too: the page is rebuilt next, and it keeps what the boxes say.
            SendDlgItemMessageW(g_setPage, CTL_KEY_BASE + i, HKM_SETHOTKEY, *ToolKey(i), 0);
        }
        const int failed = RegisterToolKeys();
        if (g_keysPaused) UnregisterToolKeys();
        g_keyStatus[0] = 0;
        for (int i = 0; i < 3; i++)
            if (failed & (1 << i)) {
                wchar_t text[32];
                KeyText(*ToolKey(i), text, 32);
                swprintf(g_keyStatus, 128, TR(L"Another app already uses %ls.", L"มีแอปอื่นใช้ %ls อยู่แล้ว"), text);
            }
        SaveSettings();
        SettingsReload(TAB_PRESENT);
    } else if (id == CTL_STARTUP) {
        if (IsPackaged()) OpenUrl(L"ms-settings:startupapps");
        else SetStartup(checked);
    } else if (id == CTL_UI_LANG) {
        if (code != CBN_SELCHANGE) return;
        g_set.uiLang = (DWORD)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        SaveSettings();
        SetSettingsTexts();
        SettingsReload(-1);
    } else if (id == CTL_REPORT) {  // only opens the issue form; nothing typed is ever attached
        OpenUrl(L"https://github.com/kittipongint/cursor-lingo/issues/new/choose");
    } else if (id == CTL_STAR) {
        OpenUrl(L"https://github.com/kittipongint/cursor-lingo");
    } else if (id == CTL_CONTRIBUTE) {
        OpenUrl(L"https://github.com/kittipongint/cursor-lingo/blob/main/CONTRIBUTING.md");
    } else if (id == CTL_KOFI) {
        OpenUrl(L"https://ko-fi.com/kintstudio");
    } else if (id == CTL_COPY_NUMBER) {
        CopyPromptPay(ctl);
    }
}

static LRESULT CALLBACK PageProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX: {  // the list of a combo box
            HDC dc = (HDC)wp;
            const int id = GetDlgCtrlID((HWND)lp);
            SetBkMode(dc, TRANSPARENT);
            SetBkColor(dc, g_th.card);
            SetTextColor(dc, id == CTL_CAPTION ? g_th.sub : id == CTL_STATUS ? g_th.status : g_th.text);
            return (LRESULT)g_th.cardBrush;
        }
        case WM_NOTIFY: {
            const NMHDR* n = (const NMHDR*)lp;
            if (n->code != (UINT)NM_CUSTOMDRAW) break;
            wchar_t cls[32];
            GetClassNameW(n->hwndFrom, cls, 32);
            if (_wcsicmp(cls, WC_BUTTONW) == 0) return DrawButton((const NMCUSTOMDRAW*)lp);
            if (_wcsicmp(cls, TRACKBAR_CLASSW) == 0) return DrawSlider((const NMCUSTOMDRAW*)lp);
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {  // the background and the cards
            PAINTSTRUCT ps;
            HDC target = BeginPaint(h, &ps);
            RECT c;
            GetClientRect(h, &c);
            HDC dc = CreateCompatibleDC(target);
            HBITMAP bmp = CreateCompatibleBitmap(target, c.right, c.bottom), oldBmp = (HBITMAP)SelectObject(dc, bmp);
            FillRect(dc, &c, g_th.backBrush);
            for (auto& card : g_cards)
                FillRound(dc, {SetPx(8), SetPx(card.first), c.right - SetPx(8), SetPx(card.second)}, 8 * DpiScaleOf(g_setDpi), g_th.card,
                          g_th.cardLine);
            BitBlt(target, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, dc,
                   ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            SelectObject(dc, oldBmp);
            DeleteObject(bmp);
            DeleteDC(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* d = (const DRAWITEMSTRUCT*)lp;
            if (d->CtlID == CTL_PREVIEW) DrawPreview(d->hDC, d->rcItem);
            else if (d->CtlID == CTL_QR) DrawPromptPayQr(d->hDC, d->rcItem);
            else if (d->CtlID >= CTL_COLOR_BASE && d->CtlID < CTL_COLOR_BASE + g_langRows.size())
                DrawSwatch(d, g_langRows[d->CtlID - CTL_COLOR_BASE].code);
            return TRUE;
        }
        case WM_HSCROLL: {
            const int id = GetDlgCtrlID((HWND)lp), v = (int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0);
            if (id == CTL_RADIUS) {
                g_set.spotRadius = (DWORD)v;
                RebuildHalo();
                PaintHalo();
                MoveHalo();
            } else if (id == CTL_ZOOM_LEVEL) {
                g_set.zoomLevel = (DWORD)v;
                ApplyZoomLevel();
                MoveLens();
            }
            ShowSliderValues();
            InvalidateRect((HWND)lp, nullptr, TRUE);  // the round thumb is wider than the one the slider tracks
            SetTimer(g_host, TIMER_SAVE, 1000, nullptr);  // save once the slider stops
            return 0;
        }
        case WM_COMMAND:
            PageCommand(LOWORD(wp), HIWORD(wp), (HWND)lp);
            return 0;
        case WM_TIMER:  // "Copied" has been shown long enough
            KillTimer(h, wp);
            SetDlgItemTextW(h, CTL_COPY_NUMBER, TR(L"Copy number", L"คัดลอกเลข"));
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Light or dark mode, the accent colour or high contrast changed: everything is drawn again in the new colours.
static void SettingsRetheme() {
    LoadTheme();
    StyleTitleBar();
    BuildPage();
    RedrawWindow(g_setWnd, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT c;
            GetClientRect(h, &c);
            FillRect((HDC)wp, &c, g_th.backBrush);
            return 1;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) SettingsRetheme();
            break;
        case WM_SYSCOLORCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            SettingsRetheme();
            break;
        case WM_NOTIFY:
            if (((NMHDR*)lp)->code == TCN_SELCHANGE) {
                g_setTab = TabCtrl_GetCurSel(g_setTabs);
                g_keyStatus[0] = 0;
                if (g_setTab == TAB_BADGE) ReadLanguages();
                BuildPage();
                UpdateKeyPause();
            }
            return 0;
        case WM_ACTIVATE: {
            g_setActive = LOWORD(wp) != WA_INACTIVE;
            UpdateKeyPause();
            LRESULT r = DefWindowProcW(h, msg, wp, lp);
            if (g_setActive && GetFocus() == h) SetFocus(g_setTabs);
            return r;
        }
        case WM_APP_RELOAD:
            g_reloadPosted = false;
            if (g_setTab == TAB_BADGE) ReadLanguages();
            BuildPage();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == IDCANCEL) DestroyWindow(h);  // Esc
            return 0;
        case WM_DPICHANGED: {
            g_setDpi = HIWORD(wp);
            MakeFonts();
            SendMessageW(g_setTabs, WM_SETFONT, (WPARAM)g_fontText, FALSE);
            TabCtrl_SetPadding(g_setTabs, SetPx(12), SetPx(6));
            const RECT* r = (const RECT*)lp;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            BuildPage();
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            ApplyKeyBoxes();
            g_setWnd = g_setTabs = g_setPage = nullptr;
            g_setActive = g_reloadPosted = false;
            UpdateKeyPause();
            DeleteFonts();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Opens the settings on `tab` (-1: the last one), or brings them to the front.
static void ShowSettings(int tab) {
    if (tab >= 0) g_setTab = tab;
    if (g_setWnd) {
        TabCtrl_SetCurSel(g_setTabs, g_setTab);
        g_keyStatus[0] = 0;
        SettingsReload(-1);
        UpdateKeyPause();
        if (IsIconic(g_setWnd)) ShowWindow(g_setWnd, SW_RESTORE);
        SetForegroundWindow(g_setWnd);
        return;
    }
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    // Always on top, like a palette: it stays visible while you try a setting out in another app. Only a close button,
    // as a fixed-size window that cannot be maximised would otherwise show a greyed-out one.
    LoadTheme();
    g_setWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_CONTROLPARENT, L"CursorLingoSettings", L"",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, mi.rcWork.left + 40,
                               mi.rcWork.top + 40, 100, 100, nullptr, nullptr, g_inst, nullptr);
    StyleTitleBar();
    g_setDpi = GetDpiForWindow(g_setWnd);
    MakeFonts();
    g_setTabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, 0, 0, 1, 1, g_setWnd,
                                nullptr, g_inst, nullptr);
    SetWindowSubclass(g_setTabs, TabsProc, 1, 0);
    SendMessageW(g_setTabs, WM_SETFONT, (WPARAM)g_fontText, FALSE);
    TabCtrl_SetPadding(g_setTabs, SetPx(12), SetPx(6));
    // Focus rings stay hidden until the keyboard is used, as the window opens from a mouse click on the tray.
    SendMessageW(g_setWnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_INITIALIZE, 0), 0);
    SetSettingsTexts();
    TabCtrl_SetCurSel(g_setTabs, g_setTab);
    g_keyStatus[0] = 0;
    ReadLanguages();
    BuildPage();
    RECT wr;
    GetWindowRect(g_setWnd, &wr);
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    SetWindowPos(g_setWnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
                 std::max(mi.rcWork.top, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 3), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(g_setWnd, SW_SHOW);
    SetForegroundWindow(g_setWnd);
}

static void ShowMenu() {
    HMENU m = CreatePopupMenu(), fix = CreatePopupMenu(), present = CreatePopupMenu();
    const int th = UiThai();
    wchar_t buf[64];
    swprintf(buf, 64, TR(L"Current input: %ls", L"ภาษาที่ใช้อยู่: %ls"), g_code);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, buf);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
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
    const int fixMode = !g_set.autoFix ? 0 : g_set.fixAsYouType ? 2 : 1;
    for (int i = 0; i < 3; i++) AppendMenuW(fix, MF_STRING | (fixMode == i ? MF_CHECKED : 0), ID_FIX_BASE + i, kFixNames[th][i]);
    wchar_t fixTitle[96];
    swprintf(fixTitle, 96, TR(L"Fix wrong language (EN \u21C4 TH): %ls", L"แก้คำที่พิมพ์ผิดภาษา (EN \u21C4 TH): %ls"), kFixNames[th][fixMode]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)fix, fixTitle);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_SETTINGS, TR(L"Settings\u2026", L"ตั้งค่า\u2026"));
    AppendMenuW(m, MF_STRING, ID_QUIT, TR(L"Quit CursorLingo", L"ปิด CursorLingo"));

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_host);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_host, nullptr);
    DestroyMenu(m);  // destroys submenus too

    if (cmd == ID_QUIT) DestroyWindow(g_host);
    else if (cmd == ID_SETTINGS) ShowSettings(-1);
    else if (cmd == ID_SHORTCUTS) ShowSettings(TAB_PRESENT);
    else if (cmd == ID_PRESENTATION) {
        SetPresentation(!g_set.presentation);
        SaveSettings();
    } else if (cmd == ID_ZOOM) SetZoom(!g_zoom);
    else if (cmd == ID_DRAW) SetDrawing(!g_drawing);
    else if (cmd >= ID_FIX_BASE && cmd < ID_FIX_BASE + 3) {
        SetFixMode(cmd - ID_FIX_BASE);
        SettingsReload(TAB_FIX);
    }
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
                if (UpdateLanguage()) {
                    Refresh(true);
                    SettingsReload(TAB_BADGE);  // the preview shows the language in use
                }
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
                    ReleaseLens();
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

// The settings file only grows by a line at a time, but it can be edited, or damaged. Windows reads the whole file
// for every setting it gets or writes, so a huge one would make each save slow. When it is far bigger than anything
// the app writes, write it again from what was read: the settings, colours and app layouts, all within their limits.
static void TidyIni() {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(g_ini, GetFileExInfoStandard, &fa) || (fa.nFileSizeHigh == 0 && fa.nFileSizeLow < 64 * 1024)) return;
    FindAppLanguage(L"");  // reads the app layouts (up to 256) before the file goes
    wchar_t old[MAX_PATH + 4];
    swprintf(old, MAX_PATH + 4, L"%ls.old", g_ini);
    if (!MoveFileExW(g_ini, old, MOVEFILE_REPLACE_EXISTING)) return;
    SaveSettings();
    for (int i = 0; i < g_colorCount; i++) {
        wchar_t hex[8];
        swprintf(hex, 8, L"%02X%02X%02X", GetRValue(g_colors[i].c), GetGValue(g_colors[i].c), GetBValue(g_colors[i].c));
        WritePrivateProfileStringW(L"Colors", g_colors[i].code, hex, g_ini);
    }
    for (int i = 0; i < g_appLangCount; i++) {
        wchar_t v[24];
        swprintf(v, 24, L"%llx", (unsigned long long)(UINT_PTR)g_appLangs[i].hkl);
        WritePrivateProfileStringW(L"AppLanguages", g_appLangs[i].exe, v, g_ini);
    }
    DeleteFileW(old);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\CursorLingo.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // for the Startup-folder shortcut
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_HOTKEY_CLASS | ICC_BAR_CLASSES | ICC_TAB_CLASSES};  // the Settings window
    InitCommonControlsEx(&icc);
    FindSettingsFiles();
    MigrateFromRegistry();
    LoadSettings();
    TidyIni();

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
    wc.lpfnWndProc = SettingsProc;  // the Settings window, and the page inside it
    wc.lpszClassName = L"CursorLingoSettings";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);
    wc.lpfnWndProc = PageProc;
    wc.lpszClassName = L"CursorLingoPage";
    wc.hIcon = nullptr;
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
        if (g_setWnd && IsDialogMessageW(g_setWnd, &msg)) continue;  // Tab, arrows and Esc in the Settings window
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
