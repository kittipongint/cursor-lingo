# Performance

The goals: little memory, next to no CPU, and no crash whatever the settings files hold. These are the numbers,
how they were measured, and what is still to be measured.

## macOS

Apple M5, macOS 27.0.1, release build (`./macos/build.sh`), measured 2026-10-28 with
[`tests/perf/measure-macos.sh`](perf/measure-macos.sh). CPU is the share of one core over 20 s; memory is the
physical footprint (`footprint`), which is what Activity Monitor calls Memory.

| Case | CPU | Memory | Notes |
|------|-----|--------|-------|
| Idle, badge shown | 0.00–0.05 % | 15–16 MB | No timers: nothing runs until the language or the pointer changes. |
| Spotlight on, pointer still | 0.00 % | 16 MB | The turning ring is a Core Animation layer; the system draws it. |
| Pointer moving without a stop, 120 moves/s | ~5.5 % | 16 MB | Same with the spotlight on. A `sample` shows almost all of it is the system delivering mouse events. |
| 60 s of pointer movement | – | 16 MB before and after | No growth. `leaks` finds 19 KB, all in one system XPC connection (`LNDaemonApplicationInterface`), none in CursorLingo's code. |
| 41–43 MB preferences file (see below), first launch | 0.05 % | 23–40 MB | No crash. The file is cleaned up and saved back: 44–46 KB. |
| Same, next launch | 0.00 % | 15–16 MB | Back to normal. |
| Every preference of the wrong type | 0.00–0.05 % | 15–16 MB | No crash; each falls back to its default. |
| Draw on Screen on, pointer still | 0 % | 17 MB | Turning it on costs 0.02 s of CPU. Two screens, a drawing layer on each. |
| Drawing without a stop for 20 s, a new stroke every 2 s | ~9 % | 17 MB | Strokes are shape layers: memory doesn't grow with them. ⌘Z and Esc leave 17 MB, no layer behind. |
| Zoom on, pointer still | ~5 % | 17 MB | It captures the screen while on: other apps redrawing (here, a chat streaming text) mean new frames. Unchanged frames are skipped. |
| Zoom on, pointer moving | ~14 % | 17 MB | The same at 2× and after zooming in twice with ⌃⌥=. |
| Zoom on and off 20 times, then on with the pointer moving for 60 s | – | 17 MB throughout | No growth. |
| Settings window open | – | 44–47 MB | Most of the extra 28 MB is AppKit's own: the toolbar tabs, pop-up buttons and switches are SwiftUI-backed on macOS 27. |
| Settings closed and opened 12 times | – | 54–56 MB, level | The pages are emptied when the window closes and the number of controls stays the same; the rest is AppKit's caches, kept until CursorLingo quits. |

Zoom and drawing were measured with [`tests/perf/drive.swift`](perf/drive.swift), which presses the tools'
shortcuts and moves or drags the pointer (`swiftc -O tests/perf/drive.swift -o /tmp/drive && /tmp/drive key ctrl+opt+z`,
then `drive move 20`, `drive drag 20`, `drive key esc`). Zoom needs Screen Recording for the build being measured.

Found while measuring the Settings window: on macOS 27, AppKit keeps every view of a closed window alive (an
observer from `-[NSView _commonAwake]` holds them), so making a new window each time it opens would add a full set
of pages every time. The window is made once; its pages are emptied when it closes and built again when it opens.

The 43 MB preferences file holds 220,000 words (17,500 of them damaged), layouts for 100,000 apps, colours for
50,000 languages, 50,000 hidden languages, shortcuts no key press can make (negative, too large, three numbers), a
NaN spotlight size and an absurd zoom level, and unknown values for every menu choice. Before the launch clean-up
(`Settings.tidy()` and `AutoFix.loadRules()`) it kept 73 MB in use for the whole session.

What the app keeps, and what the clean-up holds it to:

| List | macOS | Windows |
|------|-------|---------|
| Personal words | newest 500, 48 keys each | 500, 48 keys each (fixed arrays) |
| App layouts | 300 | 256 |
| Badge colours | 64 | 32 |
| Hidden languages | 64 | 255 characters |
| Settings file | rewritten when anything above is exceeded or unreadable | rewritten when over 64 KB |

## Windows

Not measured on Windows yet: the app has only been compiled (MinGW, `-Wall -Wextra`, no warnings) since the
Settings window and the launch clean-up were added. The README's earlier figures (about 1–2 MB working set, about 0 %
CPU idle) predate them.

Run on a Windows 10 or 11 machine, from a normal PowerShell in the repository:

```powershell
powershell -ExecutionPolicy Bypass -File tests\perf\measure-windows.ps1
```

It finds the exe in `build\windows\Release` or `build\windows-mingw` (or pass `-Exe`), copies the settings folder
first and puts it back after, and runs:

- **baseline**: idle, badge shown.
- **moving**: the pointer moving for 20 s.
- **huge**: a settings file of several MB (100,000 app layouts, 50,000 colours, broken lines, values out of range)
  and a word list of 200,000 entries with damaged ones; launched twice, to see the clean-up shrink the file.
- **garbage**: random bytes as both the settings file and the word list.

Also check by hand, with Task Manager's Details tab (Memory (active private working set) and CPU):

- [ ] Idle with the badge, then with the Settings window open on each tab.
- [ ] Spotlight on, still and while moving the pointer.
- [ ] Zoom on, still and while moving (the magnifier redraws every 16 ms while it is on).
- [ ] Drawing on: memory grows by about 4 bytes per screen pixel while the drawing layer is up (about 33 MB on one
      4K screen) and must drop back when drawing stops.
- [ ] Auto-fix on, typing quickly in Notepad for a minute: CPU stays low, nothing lags.
- [ ] Sleep and wake, lock and unlock, unplug and plug a monitor: still running, badge still follows.
- [ ] Settings window: switch all tabs, change every option, pick colours, set shortcuts (including ones another
      app holds), change the language; no crash, no leftover windows.

Fill in the numbers here when done.

| Case | CPU | Working set | Private | Notes |
|------|-----|-------------|---------|-------|
| Idle, badge shown | | | | |
| Pointer moving | | | | |
| Huge settings, first launch | | | | |
| Huge settings, next launch | | | | |
| Garbage files | | | | |
| Spotlight on, moving | | | | |
| Zoom on, moving | | | | |
| Drawing on | | | | |
