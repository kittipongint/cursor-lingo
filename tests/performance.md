# Performance

The goals: little memory, next to no CPU, and no crash whatever the settings files hold. These are the numbers,
how they were measured, and what is still to be measured.

## macOS

Apple M5, macOS 27.0.1, release build (`./macos/build.sh`), measured 2026-10-28 with
[`tests/perf/measure-macos.sh`](perf/measure-macos.sh), run twice. CPU is the share of one core over 20 s; memory is
the physical footprint (`footprint`), which is what Activity Monitor calls Memory. The script exports the
preferences first and imports them back after (checked: the same, byte for byte), and asks for any other running
CursorLingo (such as the one in /Applications) to be closed first:

```bash
tests/perf/measure-macos.sh [baseline|spotlight|moving|huge|garbage|tools ...]
```

| Case | CPU | Memory | Notes |
|------|-----|--------|-------|
| Idle, badge shown | 0.00–0.05 % | 15–16 MB | No timers: nothing runs until the language or the pointer changes. |
| Spotlight on, pointer still | 0.00 % | 16 MB | The turning ring is a Core Animation layer; the system draws it. |
| Pointer moving without a stop, 120 moves/s | 5.5–7.5 % | 16 MB | Same with the spotlight on. A `sample` shows the main thread waiting over 95 % of the time; nearly all of the rest is the system delivering mouse events (on Windows a low-level hook costs 0.2 %). |
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

`tools` in the script, every tool on and off through its shortcut (state after each step):

| Step | Memory | Threads | Open files | Windows | CPU time used |
|------|--------|---------|------------|---------|---------------|
| At the start | 15 MB | 3 | 30 | 1 | |
| Spotlight on and off ×60 | 16 MB | 12 | 30 | 5 | 0.6 s |
| Zoom on and off ×60 | 17 MB | 12 | 30 | 5 | 1.1 s |
| Drawing on and off ×60 | 17 MB | 10 | 30 | 5 | 0.5 s |
| 300 click ripples | 17 MB | 11 | 30 | 5 | |
| One stroke of 50,000 points, drawn over 65 s | 18 MB, peak 18 MB | 4 | 30 | 6 | 3.6 s (5.5 %) |
| Drawing off | 18 MB | 9 | 30 | 5 | |

No crash. Threads come and go with the system's work queues; open files stay at 30 and windows at 5–6 (the ripple
and drawing windows are made on first use and kept).

Zoom and drawing were measured with [`tests/perf/drive.swift`](perf/drive.swift), which presses the tools'
shortcuts and moves or drags the pointer (`swiftc -O tests/perf/drive.swift -o /tmp/drive && /tmp/drive key ctrl+opt+z`,
then `drive move 20`, `drive drag 20`, `drive click 300`, `drive stroke 50000`, `drive key esc`). Zoom needs Screen
Recording for the build being measured.

Not measured on macOS: typing in another app with Auto-fix on, sleep and wake, plugging and unplugging a monitor,
and a run of several hours.

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

Intel Core i7-13700 (24 threads), Windows 11 Pro build 26300, one 4K screen at 150 %, release build (Visual Studio
2026, `cmake --build build/windows --config Release`), measured 2026-10-28. The exe ran as a portable copy (an empty
`CursorLingo.ini` next to it), so the tests never touched the settings in use. CPU is the share of one core, as on
macOS; memory is private bytes (what Task Manager calls Memory (private working set) is close to it) and the working
set.

[`tests/perf/measure-windows.ps1`](perf/measure-windows.ps1), 20 s per case. It copies the settings first and puts
them back after, and asks for any other running CursorLingo to be closed first:

```powershell
powershell -ExecutionPolicy Bypass -File tests\perf\measure-windows.ps1 [-Exe path\to\CursorLingo.exe]
```


| Case | CPU | Private | Working set | Notes |
|------|-----|---------|-------------|-------|
| Idle, badge shown | 0.0–0.1 % | 3.1 MB | 0.6 MB | One coalesced 200 ms check. |
| Pointer moving (`SetCursorPos`) | 0.2 % | 3.1 MB | 0.5 MB | |
| Huge settings, first launch | 0.2 % | 3.1 MB | 0.4 MB | A 5.6 MB settings file and a 1.5 MB word list of 200,000 entries. No crash; the file is rewritten: 12 KB. |
| Huge settings, next launch | 0.0 % | 3.1 MB | 0.4 MB | |
| Garbage files | 0.2 % | 3.1 MB | 0.4 MB | 300 KB of random bytes as both files. No crash. |

Each tool, with the pointer moved by real input (`SendInput`, which the low-level mouse hook sees, about 120
moves/s), 10–15 s per case, in one session:

| Case | CPU | Private | Working set | Notes |
|------|-----|---------|-------------|-------|
| Idle, badge shown | 0.1–0.35 % | 3.1 MB | 0.4 MB | Varies between runs; never above 0.4 %. |
| Pointer moving | 0.2 % | 3.1 MB | 0.8 MB | The badge follows the pointer. |
| Settings window open | 0.5 % | 3.5 MB | 7.4 MB | Back to 3.8 MB private after 60 opens and closes. |
| Spotlight on, still | 0.6–2.6 % | 4.6 MB | 9.2 MB | The ring turns on a 33 ms timer. |
| Spotlight on, moving | 1.4 % | 4.6 MB | 9.2 MB | |
| Zoom on, still | 4.0 % | 32.5 MB | 34.8 MB | The magnifier redraws every 16 ms while it is on. |
| Zoom on, moving | 6.8 % | 32.4 MB | 34.7 MB | |
| Zoom off again | 0.1–0.3 % | 32.1 MB | 34.4 MB | See below. |
| Drawing on | 0.9 % | 63.8 MB | 66.0 MB | The drawing layer: 4 bytes per screen pixel, 32 MB on a 4K screen. |
| Drawing off again | 0.0 % | 32.0 MB | 34.3 MB | The layer and the strokes are freed. |

After the first use of zoom, Windows' magnifier keeps about 29 MB until the app quits. The app keeps its magnifier
window (shrunk to 1×1 pixel) instead of closing it: closing and opening it again lost a handle each time, so zoom
used again and again would grow; kept, it stays the same however often zoom is used.

Robustness and leaks (all passed):

- 60 cycles of opening Settings, visiting every tab, a colour-scheme change and closing; then 60 cycles of each tool
  on and off; 300 click ripples; one 50,000-point stroke. No crash; GDI objects, window handles and kernel handles
  the same after 30 and after 60 cycles, memory back to where it was.
- Every control in the Settings window changed and checked against the saved file: 55 of 55.
- A 241 KB settings file with real settings and junk: rewritten to 708 bytes on launch, with every setting, colour,
  hidden language and app layout kept (an app name in Thai included), still UTF-16, no `.old` file left.
- The language-detection core: 53 of 53 test cases, and 300,000 random words (some damaged) plus every UTF-16 code
  point with no crash. With 500 personal words, the check at the end of a word takes 0.67 µs at worst, and 0.61 µs
  per key inside a long word: a list of 500 words is no slower in use and takes no more memory (fixed arrays).
- Earlier, under AddressSanitizer: 60 damaged word lists and 40 damaged settings files, no error reported.

Not measured: typing in another app with Auto-fix on (test keys sent by a script are marked as injected, and the
app ignores those), sleep and wake, lock and unlock, plugging and unplugging a monitor, and a run of several hours.
