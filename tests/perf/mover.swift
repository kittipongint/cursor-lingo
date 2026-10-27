import CoreGraphics
import Foundation
// Moves the pointer in a circle for N seconds at 120 Hz, then puts it back.
let secs = Double(CommandLine.arguments.dropFirst().first ?? "20") ?? 20
let start = CGEvent(source: nil)!.location
let t0 = Date()
while Date().timeIntervalSince(t0) < secs {
    let a = Date().timeIntervalSince(t0) * 3
    let p = CGPoint(x: 700 + 250 * cos(a), y: 450 + 200 * sin(a))
    CGEvent(mouseEventSource: nil, mouseType: .mouseMoved, mouseCursorPosition: p, mouseButton: .left)!.post(tap: .cghidEventTap)
    usleep(8333)
}
CGEvent(mouseEventSource: nil, mouseType: .mouseMoved, mouseCursorPosition: start, mouseButton: .left)!.post(tap: .cghidEventTap)
