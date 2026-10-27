import CoreGraphics
import Foundation
// Drives CursorLingo for the measurements, through ordinary input events:
//   drive key ctrl+opt+d | key esc | key ctrl+opt+= | drag SECONDS | move SECONDS | click COUNT | stroke POINTS
// "drag" holds the button and draws loops, as with Draw on Screen; "move" only moves. Both put the pointer back.
// "click" clicks in place, 20 a second (ripples); "stroke" draws one stroke of that many points, 1,000 a second.
// "windows PID" prints how many windows that process has, on screen or not.
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

func click(_ count: Int) {
    let p = CGEvent(source: nil)!.location
    for _ in 0..<count {
        for type in [CGEventType.leftMouseDown, .leftMouseUp] {
            CGEvent(mouseEventSource: nil, mouseType: type, mouseCursorPosition: p, mouseButton: .left)!.post(tap: .cghidEventTap)
        }
        usleep(50_000)
    }
}

func stroke(_ points: Int) {
    let start = CGEvent(source: nil)!.location
    func at(_ i: Int) -> CGPoint {
        let t = Double(i) / 300
        return CGPoint(x: 700 + 250 * cos(t) + 80 * cos(t * 7), y: 450 + 200 * sin(t) + 80 * sin(t * 5))
    }
    CGEvent(mouseEventSource: nil, mouseType: .leftMouseDown, mouseCursorPosition: at(0), mouseButton: .left)!.post(tap: .cghidEventTap)
    for i in 1..<max(points, 2) {
        CGEvent(mouseEventSource: nil, mouseType: .leftMouseDragged, mouseCursorPosition: at(i), mouseButton: .left)!.post(tap: .cghidEventTap)
        usleep(1000)
    }
    CGEvent(mouseEventSource: nil, mouseType: .leftMouseUp, mouseCursorPosition: at(points), mouseButton: .left)!.post(tap: .cghidEventTap)
    CGEvent(mouseEventSource: nil, mouseType: .mouseMoved, mouseCursorPosition: start, mouseButton: .left)!.post(tap: .cghidEventTap)
}

switch args.first {
case "key": press(args[1])
case "drag": loop(Double(args[1]) ?? 20, drag: true)
case "move": loop(Double(args[1]) ?? 20, drag: false)
case "click": click(Int(args[1]) ?? 300)
case "stroke": stroke(Int(args[1]) ?? 50_000)
case "windows":
    let all = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]] ?? []
    print(all.filter { ($0[kCGWindowOwnerPID as String] as? Int32) == Int32(args[1]) }.count)
default: print("drive key ctrl+opt+d | drag SECONDS | move SECONDS | click COUNT | stroke POINTS | windows PID")
}
