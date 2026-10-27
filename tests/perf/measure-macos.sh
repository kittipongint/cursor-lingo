#!/bin/bash
# Measures CursorLingo on macOS: CPU over 20 s, resident memory and physical footprint, in several situations.
#
#   tests/perf/measure-macos.sh [baseline|spotlight|moving|huge|garbage ...]   (no argument: all of them)
#
# Runs the build in build/CursorLingo.app (./macos/build.sh first). The preferences are exported before and imported
# back after, so the stress cases leave your settings as they were. "moving" and "spotlight" move the pointer by
# themselves for 20 s (the first run may ask to let the terminal control the computer); "spotlight" dims the screen.
set -u
cd "$(dirname "$0")/../.."
BIN=build/CursorLingo.app/Contents/MacOS/CursorLingo
DOMAIN=io.github.cursorlingo
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
[ -x "$BIN" ] || { echo "build first: ./macos/build.sh"; exit 1; }
swiftc -O tests/perf/mover.swift -o "$WORK/mover" 2>/dev/null || { echo "could not build tests/perf/mover.swift"; exit 1; }

was_running=$(pgrep -x CursorLingo >/dev/null && echo 1)
defaults export $DOMAIN "$WORK/backup.plist" 2>/dev/null || : >"$WORK/backup.plist"

restore() {
    pkill -x CursorLingo; sleep 1
    defaults delete $DOMAIN 2>/dev/null
    [ -s "$WORK/backup.plist" ] && defaults import $DOMAIN "$WORK/backup.plist"
}

# run NAME MOVE [app args...]
run() {
    local name=$1 move=$2; shift 2
    pkill -x CursorLingo; sleep 1
    "$BIN" "$@" >/dev/null 2>&1 &
    local pid=$!
    disown $pid  # no "Terminated" line when the next case stops it
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

cases=("$@"); [ ${#cases[@]} -eq 0 ] && cases=(baseline spotlight moving huge garbage)
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
        *) echo "unknown case: $c" ;;
    esac
done
restore
[ -n "$was_running" ] && open build/CursorLingo.app
