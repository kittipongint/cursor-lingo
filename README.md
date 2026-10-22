<p align="center">
  <img src="assets/AppIcon.png" width="128" alt="CursorLingo icon">
</p>

<h1 align="center">CursorLingo</h1>

<p align="center">
  <b>English</b> · <a href="README.th.md">ภาษาไทย</a>
</p>

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
- **Auto-fix wrong language (optional, off by default).** Typed `l;ylfu;yoouhgik` instead of `สวัสดีวันนี้เรา`, or `ะ้รห` instead of `this`? CursorLingo re-types the word and switches the layout for you. English ⇄ Thai. See [Auto-fix](#auto-fix-wrong-language).
- **Private.** No network, no telemetry. Keystrokes are only observed if you turn Auto-fix on, and then only the current and the last word are kept in memory.
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

Download `CursorLingo-macOS.zip` from [Releases](https://github.com/kittipongint/cursor-lingo/releases), unzip it, move **CursorLingo.app** to **Applications**, and double-click it. Releases from v1.1.4 on are signed with a Developer ID and notarized by Apple, so they open without warnings.

The badge needs no permissions. Input Monitoring and Accessibility are only needed for [Auto-fix](#turn-on-auto-fix-on-macos).

**Or build from source** (needs the Xcode Command Line Tools):

```bash
git clone https://github.com/kittipongint/cursor-lingo.git && cd cursor-lingo && ./install.sh
```

If your keychain has a *Developer ID Application* certificate, `install.sh` signs with it, so macOS keeps CursorLingo's permissions across rebuilds. To notarize a build yourself, see `macos/notarize.sh`.

#### Turn on Auto-fix on macOS

1. In the menu-bar menu, click **Auto-fix wrong language (EN ⇄ TH)**. The line under it shows what is still missing.
2. **Input Monitoring:** allow CursorLingo when macOS asks, or click **Open Privacy Settings…** in the menu and turn CursorLingo on under **Privacy & Security → Input Monitoring**.
3. **Accessibility:** do the same under **Privacy & Security → Accessibility**. If CursorLingo is not listed, click **+** and choose **Applications → CursorLingo**.
4. **Quit CursorLingo and open it again.** The app reads these permissions when it starts.
5. The line under Auto-fix should now say **Active**. Test it: on the English layout type `l;ylfu` and press Space; it becomes **สวัสดี**.

You only do this once: updates keep the permissions.

**Upgrading from v1.1.3 or older?** Those builds were only ad-hoc signed, so macOS treats v1.1.4 as a new app. Grant the permissions one last time: in both **Input Monitoring** and **Accessibility**, select the old CursorLingo, click **−**, add the new one with **+**, and reopen the app. If it still says *Needs … permission*, clear the old entries and let macOS ask again:

```bash
tccutil reset Accessibility io.github.cursorlingo
```

```bash
tccutil reset ListenEvent io.github.cursorlingo
```

Then reopen CursorLingo and turn Auto-fix off and on. (Builds from source signed ad-hoc behave like the old releases: macOS asks again after every rebuild.)

### Windows 10 / 11 (x64)

Download `CursorLingo-Windows-x64.zip` from [Releases](https://github.com/kittipongint/cursor-lingo/releases) and unzip it. **No administrator rights are needed**, there is no installer, and nothing is written outside your user profile. Pick one:

- **Portable:** run `CursorLingo.exe` from any folder (Desktop, USB stick, ...).
- **Per-user install** (like VS Code's user installer): in PowerShell run `.\install.ps1` (add `-Startup` to start with Windows). It copies the exe to `%LOCALAPPDATA%\Programs\CursorLingo` and adds a Start Menu shortcut. `.\install.ps1 -Uninstall` removes it.

Right-click the tray icon → **Start with Windows** to launch it automatically (it follows the exe if you move it).
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

## Auto-fix wrong language

Turn on **Auto-fix wrong language (EN ⇄ TH)** from the menu. When you finish a word with Space, and it looks like gibberish in the current layout but like a real word in the other one, CursorLingo erases it, types it again from the same keys, and switches the layout so you can keep going. Long Thai runs typed without spaces are fixed after a few characters.

**Double-tap Shift** to convert by hand, or to undo:

- Right after an automatic fix, double Shift puts the word back the way you typed it, switches the layout back, and leaves that word alone for the rest of the session.
- On any other word (the one you're typing, or the last one), double Shift converts it to the other layout. Words that are also real words in the other language, such as `ที่` = `muj` or `ทำไม` = `mew`, are never fixed automatically, so this is the quick way to fix them.
- **Select text and double-tap Shift** to convert the whole selection (several words or lines). Each character is read back through the other layout; the direction follows the script that dominates (mostly Thai → English, otherwise English → Thai), and anything that isn't on the keyboard (spaces, new lines, emoji) is kept. On macOS the selection is read through Accessibility; apps that don't expose it, and all Windows apps, use a quick copy/paste, after which your clipboard is restored. Terminals are skipped.
- Without a selection it works on the current or last word only, and only if you haven't clicked, used a shortcut or switched apps since. It doesn't add entries to the app's undo history. Note that JetBrains IDEs also use double Shift (Search Everywhere).

**How it decides.** A key press is a *physical key*, and every physical key produces one character in each layout. A word typed in the wrong layout is therefore the right text seen through the wrong key map. For each word, CursorLingo computes both readings and converts only when:

1. the reading on screen is implausible for its language, and
2. the other reading is plausible for its language.

*English plausibility* uses a letter-trigram model (real words are cheap; Thai typed on a Latin layout is full of impossible sequences and punctuation keys such as `; ' , .` inside words). *Thai plausibility* uses Thai orthography: a word can't start with a tone mark or trailing vowel, vowels sit on consonants, a leading vowel needs a consonant after it, and so on. English typed on the Thai layout breaks these rules almost immediately. Very short words, numbers, paths, URLs, e-mail addresses and code-like words are never touched, because precision matters more than recall.

On a sample of real English and Thai words, correct text was wrongly changed in fewer than 1 in 1,000 cases, and about 80 % of wrong-layout words were fixed. Words it is unsure about are left alone.

**Privacy and permissions.**
- Off by default. The keyboard hook / event tap only exists while the option is on.
- Only the current and the last word are kept, in memory, plus the words you undid this session. Nothing is logged, stored or sent anywhere.
- macOS needs **Input Monitoring** and **Accessibility** (see [Turn on Auto-fix on macOS](#turn-on-auto-fix-on-macos)). Secure input (password fields) and Terminal apps are skipped.
- Windows needs no special rights, but cannot see windows running as administrator. Classic password boxes and consoles are skipped.
- Only the stock US-English and Thai (Kedmanee) layouts are supported; other layouts are left alone.

## Usage

Click the menu-bar icon (macOS) or right-click the tray icon (Windows) to:

- see the current input language,
- hide the badge for the current language, or show it for all languages again,
- choose **Show**, **Style** and **Size**,
- toggle **Auto-fix wrong language** (double-tap Shift to convert the last word or the selection, or to undo a fix),
- toggle **Launch at Login** / **Start with Windows**,
- quit.

## Uninstall

- **macOS:** `./uninstall.sh`, or quit the app and delete it. Turn off *Launch at Login* first.
- **Windows:** `.\install.ps1 -Uninstall` if you used the installer; otherwise untick *Start with Windows*, quit, and delete the exe. Settings live in `HKCU\Software\CursorLingo`.

## Project layout

```
core/      layoutfix.h (wrong-layout detector, shared) · en_model.inc (generated) · C interface for Swift
macos/     CursorLingo.swift · Info.plist · build.sh (universal binary)
windows/   CursorLingo.cpp · CursorLingo.rc · manifest · CMakeLists.txt · build.bat · install.ps1
tests/     layoutfix_test.cpp
assets/    AppIcon.png / .icns / .ico, screenshots
tools/     make-icon.swift (vector icon generator) · ai-icon.sh (optional AI icon) · gen-en-model.py
```

The icon is drawn in code by `tools/make-icon.swift`. To try an AI-generated icon, run `OPENAI_API_KEY=… ./tools/ai-icon.sh`.

## License

[MIT](LICENSE)
