#!/bin/bash
# Measures CursorLingo on macOS: CPU over 20 s, resident memory and physical footprint, in several situations.
#
#   tests/perf/measure-macos.sh [baseline|spotlight|moving|huge|garbage|tools ...]   (no argument: all of them)
#
# Runs the build in build/CursorLingo.app (./macos/build.sh first). The preferences are exported before and imported
# back after, so the stress cases leave your settings as they were. "moving" and "spotlight" move the pointer by
# themselves for 20 s (the first run may ask to let the terminal control the computer); "spotlight" dims the screen.
# "tools" turns each presentation tool on and off 60 times through its shortcut, clicks 300 ripples and draws one
# stroke of 50,000 points, and checks that memory, threads, open files and windows come back to where they were.
# Zoom needs Screen Recording for this build.
set -u
cd "$(dirname "$0")/../.."
BIN=build/CursorLingo.app/Contents/MacOS/CursorLingo
DOMAIN=io.github.cursorlingo
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
[ -x "$BIN" ] || { echo "build first: ./macos/build.sh"; exit 1; }
swiftc -O tests/perf/mover.swift -o "$WORK/mover" 2>/dev/null || { echo "could not build tests/perf/mover.swift"; exit 1; }
swiftc -O tests/perf/drive.swift -o "$WORK/drive" 2>/dev/null || { echo "could not build tests/perf/drive.swift"; exit 1; }
FULL=$PWD/$BIN

# Only one CursorLingo runs at a time, so another copy (say, the one in /Applications) would make every launch here
# quit at once. Ask for it to be closed rather than closing it.
for pid in $(pgrep -x CursorLingo); do
    path=$(ps -o comm= -p "$pid")
    [ "$path" != "$FULL" ] && { echo "Close the CursorLingo running from $path first."; exit 1; }
done
was_running=$(pgrep -x CursorLingo >/dev/null && echo 1)
defaults export $DOMAIN "$WORK/backup.plist" 2>/dev/null || : >"$WORK/backup.plist"

stop() { pkill -f "^$FULL" ; sleep 1; }

restore() {
    stop
    defaults delete $DOMAIN 2>/dev/null
    [ -s "$WORK/backup.plist" ] && defaults import $DOMAIN "$WORK/backup.plist"
}

# run NAME MOVE [app args...]
run() {
    local name=$1 move=$2; shift 2
    stop
    "$FULL" "$@" >/dev/null 2>&1 &
    local pid=$!
    disown $pid  # no "Terminated" line when the next case stops it
    PID=$pid
    sleep 5
    if ! kill -0 $pid 2>/dev/null; then echo "$name: QUIT OR CRASHED at launch"; return; fi
    cpu() { ps -o time= -p $pid | awk -F'[:.]' '{print ($1*60+$2)+$3/100}'; }
    local mover=
    [ "$move" = 1 ] && { "$WORK/mover" 21 & mover=$!; }
    local c0 c1; c0=$(cpu); sleep 20; c1=$(cpu)
    [ -n "$mover" ] && wait $mover  # not a bare wait: the app is a background job too
    if ! kill -0 $pid 2>/dev/null; then echo "$name: CRASHED while running"; return; fi
    local rss fp
    rss=$(ps -o rss= -p $pid)
    fp=$(footprint -p $pid 2>/dev/null | awk '/phys_footprint:/ {print $2, $3; exit}')
    printf '%-10s cpu %5.2f %%   rss %4d MB   footprint %s\n' "$name" "$(echo "($c1 - $c0) * 5" | bc -l)" $((rss / 1024)) "${fp:-?}"
}

# What a leak would show: footprint, threads, open files, windows.
state() {
    printf 'footprint %s   threads %d   files %d   windows %d' \
        "$(footprint -p $PID 2>/dev/null | awk '/phys_footprint:/ {print $2, $3; exit}')" \
        $(($(ps -M -p $PID | wc -l) - 1)) $(($(lsof -p $PID 2>/dev/null | wc -l) - 1)) "$("$WORK/drive" windows $PID)"
}

tools() {
    run tools-idle 0 -presentation NO
    kill -0 $PID 2>/dev/null || return
    echo "           at the start:          $(state)"
    for t in h z d; do
        local c0 c1 i
        c0=$(ps -o time= -p $PID)
        for i in $(seq 60); do "$WORK/drive" key ctrl+opt+$t; sleep 0.3; "$WORK/drive" key ctrl+opt+$t; sleep 0.2; done
        sleep 2
        kill -0 $PID 2>/dev/null || { echo "tools: CRASHED during ⌃⌥${t} on and off"; return; }
        echo "           ⌃⌥${t} on and off ×60:  $(state)   cpu time ${c0// /} → $(ps -o time= -p $PID | tr -d ' ')"
    done
    "$WORK/drive" key ctrl+opt+h; sleep 1; "$WORK/drive" click 300; sleep 1; "$WORK/drive" key ctrl+opt+h; sleep 2
    kill -0 $PID 2>/dev/null || { echo "tools: CRASHED during the ripples"; return; }
    echo "           300 ripples:           $(state)"
    "$WORK/drive" key ctrl+opt+d; sleep 1
    local t0 t1; t0=$(date +%s)
    "$WORK/drive" stroke 50000 & local drv=$!
    local peak=0 cpu0 cpu1; cpu0=$(ps -o time= -p $PID)
    while kill -0 $drv 2>/dev/null; do
        local mb; mb=$(footprint -p $PID 2>/dev/null | awk '/phys_footprint:/ {print $2; exit}')
        [ "${mb%.*}" -gt "$peak" ] 2>/dev/null && peak=${mb%.*}
        sleep 2
    done
    t1=$(date +%s); cpu1=$(ps -o time= -p $PID)
    kill -0 $PID 2>/dev/null || { echo "tools: CRASHED during the long stroke"; return; }
    echo "           50,000-point stroke:   $(state)   peak ~${peak} MB   $((t1 - t0)) s   cpu time ${cpu0// /} → ${cpu1// /}"
    "$WORK/drive" key ctrl+opt+d; sleep 2
    echo "           drawing off:           $(state)"
}

# A 40 MB preferences file: far more words, apps, colours and hidden languages than the app ever keeps, broken
# entries among them, and values no key press or slider could make.
stress() {
    python3 - "$WORK/$1.plist" "$1" <<'PY'
import plistlib, random, sys
random.seed(1)
if sys.argv[2] == "huge":
    def word():
        return random.choice("01") + random.choice("+-") + "".join("%02x" % random.randrange(47) for _ in range(random.randint(2, 10)))
    p = {
        "personalWords": [word() for _ in range(200000)] + ["zz", "1+", "0+zz", "1-" + "ff" * 5000, "+f+f+f", "", "0+c8"] * 2500,
        "appLanguages": {f"com.example.app{i}": "com.apple.keylayout.Thai" for i in range(100000)},
        "badgeColors": {**{f"X{i}": "#12ab34" for i in range(50000)}, "EN": "nothex", "TH": "#zzzzzz"},
        "hidden": [f"L{i}" for i in range(50000)],
        "shortcut.spotlight": [-1, 99999999999], "shortcut.zoom": [1, 2, 3], "shortcut.draw": [100000, -5],
        "spotlightRadius": float("nan"), "zoomLevel": 1e308,
        "mode": "Sometimes", "style": "Plaid", "size": "Enormous", "uiLanguage": "Klingon",
        "autoFix": True, "perAppLanguage": True,
    }
else:  # every value of the wrong type
    p = {"personalWords": {"a": 1}, "appLanguages": ["x"], "badgeColors": 42, "hidden": "EN", "shortcut.spotlight": "abc",
         "spotlightRadius": "big", "zoomLevel": [1], "mode": 5, "style": [], "autoFix": "maybe"}
plistlib.dump(p, open(sys.argv[1], "wb"))
PY
    defaults delete $DOMAIN 2>/dev/null
    defaults import $DOMAIN "$WORK/$1.plist"
}

cases=("$@"); [ ${#cases[@]} -eq 0 ] && cases=(baseline spotlight moving huge garbage tools)
for c in "${cases[@]}"; do
    case $c in
        baseline) run baseline 0 ;;
        moving) run moving 1 ;;
        spotlight) run spotlight 1 -presentation YES ;;
        huge)
            stress huge
            echo "huge       preferences file $(($(defaults export $DOMAIN - | wc -c) / 1024 / 1024)) MB"
            run huge-1st 0
            echo "           after the first launch: $(($(defaults export $DOMAIN - | wc -c) / 1024)) KB"
            run huge-2nd 0
            restore ;;
        garbage) stress garbage; run garbage 0; restore ;;
        tools) tools ;;
        *) echo "unknown case: $c" ;;
    esac
done
restore
[ -n "$was_running" ] && open build/CursorLingo.app
