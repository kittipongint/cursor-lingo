<p align="center">
  <img src="assets/AppIcon.png" width="128" alt="CursorLingo icon">
</p>

<h1 align="center">CursorLingo</h1>

<p align="center">
  See your current keyboard language <b>right at the mouse pointer</b>, so you never type a whole sentence in the wrong language again.<br>
  Tiny, native, and private. Runs on <b>macOS</b> and <b>Windows</b>.
</p>

<p align="center">
  <img src="assets/styles.png" width="560" alt="Badge styles: Color, Light, Dark, Dot">
</p>

## Features

- **Badge at the pointer** shows `EN`, `TH`, `JA`, `ZH`, `KO`… for *any* input language, each in its own color.
- **4 styles** (Color · Light · Dark · Dot) and **3 sizes**.
- **Show mode:** *Always*, or *Only when switching* (pops up for about a second, then fades out).
- **Hide per language.** For example, show the badge only when you're *not* in English.
- **Launch at login / Start with Windows**, toggled from the menu.
- **Click-through.** The badge never steals clicks or focus.
- **Private.** No keyboard hooks, no keystroke access, no network, no telemetry.
- **Light on resources.** It's event-driven, with no polling timers on macOS.

|                  | macOS                                  | Windows                                        |
|------------------|----------------------------------------|------------------------------------------------|
| Tech             | Swift + AppKit, single file            | C++ / Win32 + GDI, single file, static exe     |
| Pointer tracking | `NSEvent` global mouse monitor         | Low-level mouse hook (coalesced)               |
| Language source  | `kTISNotifySelectedKeyboardInputSourceChanged` | `GetKeyboardLayout` of focused thread   |
| Memory           | ~15 MB physical footprint              | ~1–2 MB working set                            |
| Idle CPU         | 0 % (no wake-ups)                      | ~0 % (one coalesced 200 ms check)              |
| Settings         | Menu-bar icon                          | Tray icon (shows the current language)         |

The Windows tray icon itself shows the current language code. The badge also hides automatically while a full-screen app or game is in front.

## Install

### macOS (12 Monterey or later, Apple Silicon and Intel)

Download `CursorLingo-macOS.zip` from [Releases](../../releases), unzip it, and move **CursorLingo.app** to Applications.
The app is ad-hoc signed, not notarized. The first time you open it, right-click the app → **Open**, or run:

```bash
xattr -dr com.apple.quarantine /Applications/CursorLingo.app
```

**Or build from source** (needs the Xcode Command Line Tools):

```bash
git clone https://github.com/<you>/CursorLingo.git && cd CursorLingo && ./install.sh
```

No Accessibility or Input Monitoring permission is needed.

### Windows 10 / 11 (x64)

Download `CursorLingo-Windows-x64.zip` from [Releases](../../releases), unzip it, and run `CursorLingo.exe`.
Right-click the tray icon → **Start with Windows** to launch it automatically.
SmartScreen may warn about an unsigned app: click **More info → Run anyway**.

**Or build from source** with Visual Studio 2022 or Build Tools + CMake:

```bat
windows\build.bat
```

You can also cross-compile with MinGW-w64:

```bash
x86_64-w64-mingw32-windres windows/CursorLingo.rc -O coff -o res.o
x86_64-w64-mingw32-g++ -std=c++17 -Os -municode -mwindows -static windows/CursorLingo.cpp res.o \
  -o CursorLingo.exe -luser32 -lgdi32 -lshell32 -ladvapi32 -lshcore -s
```

## Usage

Click the menu-bar icon (macOS) or right-click the tray icon (Windows) to:

- see the current input language,
- hide the badge for the current language, or show it for all languages again,
- choose **Show**, **Style** and **Size**,
- toggle **Launch at Login** / **Start with Windows**,
- quit.

## Uninstall

- **macOS:** `./uninstall.sh`, or quit the app and delete it. Turn off *Launch at Login* first.
- **Windows:** untick *Start with Windows*, quit, and delete the exe. Settings live in `HKCU\Software\CursorLingo`.

## Project layout

```
macos/     CursorLingo.swift · Info.plist · build.sh (universal binary)
windows/   CursorLingo.cpp · CursorLingo.rc · manifest · CMakeLists.txt · build.bat
assets/    AppIcon.png / .icns / .ico, screenshots
tools/     make-icon.swift (vector icon generator) · ai-icon.sh (optional AI icon)
```

The icon is drawn in code by `tools/make-icon.swift`. To try an AI-generated icon, run `OPENAI_API_KEY=… ./tools/ai-icon.sh`.

## License

[MIT](LICENSE)
