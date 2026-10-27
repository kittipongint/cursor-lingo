import CoreGraphics
import Foundation
// Drives CursorLingo for the measurements, through ordinary input events:
//   drive key ctrl+opt+d | key esc | key ctrl+opt+= | drag SECONDS | move SECONDS
// "drag" holds the button and draws loops, as with Draw on Screen; "move" only moves. Both put the pointer back.
let args = Array(CommandLine.arguments.dropFirst())
let keyCodes: [String: CGKeyCode] = ["h": 4, "z": 6, "d": 2, "esc": 53, "=": 24, "-": 27]

func press(_ spec: String) {
    var flags: CGEventFlags = []
    var key: CGKeyCode = 0
    for part in spec.lowercased().split(separator: "+") {
        switch part {
        case "ctrl": flags.insert(.maskControl)
        case "opt": flags.insert(.maskAlternate)
        case "cmd": flags.insert(.maskCommand)
        case "shift": flags.insert(.maskShift)
        default: key = keyCodes[String(part)] ?? 0
        }
    }
    for down in [true, false] {
        let e = CGEvent(keyboardEventSource: nil, virtualKey: key, keyDown: down)!
        e.flags = flags
        e.post(tap: .cghidEventTap)
        usleep(30_000)
    }
}

func loop(_ seconds: Double, drag: Bool) {
    let start = CGEvent(source: nil)!.location, t0 = Date()
    func at(_ t: Double) -> CGPoint {
        CGPoint(x: 700 + 250 * cos(t * 3) + 60 * cos(t * 11), y: 450 + 200 * sin(t * 3) + 60 * sin(t * 7))
    }
    if drag { CGEvent(mouseEventSource: nil, mouseType: .leftMouseDown, mouseCursorPosition: at(0), mouseButton: .left)!.post(tap: .cghidEventTap) }
    while Date().timeIntervalSince(t0) < seconds {
        let t = Date().timeIntervalSince(t0)
        // A new stroke every 2 s, so strokes pile up as they would in use.
        if drag && Int(t * 60) % 120 == 119 {
            CGEvent(mouseEventSource: nil, mouseType: .leftMouseUp, mouseCursorPosition: at(t), mouseButton: .left)!.post(tap: .cghidEventTap)
            CGEvent(mouseEventSource: nil, mouseType: .leftMouseDown, mouseCursorPosition: at(t), mouseButton: .left)!.post(tap: .cghidEventTap)
        }
        CGEvent(mouseEventSource: nil, mouseType: drag ? .leftMouseDragged : .mouseMoved, mouseCursorPosition: at(t),
                mouseButton: .left)!.post(tap: .cghidEventTap)
        usleep(8333)
    }
    if drag { CGEvent(mouseEventSource: nil, mouseType: .leftMouseUp, mouseCursorPosition: at(seconds), mouseButton: .left)!.post(tap: .cghidEventTap) }
    CGEvent(mouseEventSource: nil, mouseType: .mouseMoved, mouseCursorPosition: start, mouseButton: .left)!.post(tap: .cghidEventTap)
}

switch args.first {
case "key": press(args[1])
case "drag": loop(Double(args[1]) ?? 20, drag: true)
case "move": loop(Double(args[1]) ?? 20, drag: false)
default: print("drive key ctrl+opt+d | drag SECONDS | move SECONDS")
}
