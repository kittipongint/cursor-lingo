// Renders the README animation of the presentation tools (assets/presentation-demo.gif + .mp4) frame by frame.
//
//   swiftc -O tools/make-presentation-demo.swift -o /tmp/make-presentation-demo && /tmp/make-presentation-demo [--preview t1,t2,...]
//
// Same look as tools/make-demo.swift: a slide in a window, then the spotlight with its rainbow ring, resizing the
// circle, the zoom lens, drawing on screen with undo, and click ripples in the colour of the language in use.

import AppKit
import AVFoundation
import CoreText
import ImageIO
import UniformTypeIdentifiers

// MARK: - Canvas & palette

let W: CGFloat = 1280, H: CGFloat = 800
let FPS = 30.0

func rgb(_ hex: UInt32, _ a: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: a)
}
let bgTop = rgb(0x0B0E1F), bgBottom = rgb(0x161A36)
let enColor = rgb(0x34C77B), thColor = rgb(0x3D7BFF)
let pen = rgb(0xFF3B30)
func accent(_ lang: Int) -> CGColor { lang == 0 ? enColor : thColor }

// MARK: - Script

var clock = 0.0
var captions: [(Double, String, String)] = []
var pills: [(Double, String, String)] = []      // keys shown at the bottom: macOS, Windows
var clicks: [(Double, CGPoint)] = []
var moves: [(Double, CGPoint)] = []             // pointer keyframes
var langAt: [(Double, Int)] = [(0, 0)]
var spotlight: [(Double, Bool)] = [(0, false)]
var zoomOn: [(Double, Bool)] = [(0, false)]
var radius: [(Double, CGFloat)] = [(0, 110)]
var zoomLevel: [(Double, CGFloat)] = [(0, 2)]
struct Stroke { var points: [CGPoint]; var start: Double; var duration: Double; var removedAt: Double = 1e9 }
var strokes: [Stroke] = []
var endAt = 1e9

func at(_ dt: Double) { clock += dt }
func caption(_ a: String, _ b: String) { captions.append((clock, a, b)) }
func pill(_ mac: String, _ win: String) { pills.append((clock, mac, win)) }
func move(_ x: CGFloat, _ y: CGFloat, over d: Double) { moves.append((clock, moves.last?.1 ?? CGPoint(x: x, y: y))); clock += d; moves.append((clock, CGPoint(x: x, y: y))) }
func click() { clicks.append((clock, pointer(clock))) }

// Pointer at time t: eased between keyframes.
func pointer(_ t: Double) -> CGPoint {
    guard let first = moves.first else { return CGPoint(x: 640, y: 450) }
    if t <= first.0 { return first.1 }
    for i in 1..<moves.count where t <= moves[i].0 {
        let (t0, p0) = moves[i - 1], (t1, p1) = moves[i]
        let k = t1 > t0 ? ease((t - t0) / (t1 - t0)) : 1
        return CGPoint(x: p0.x + (p1.x - p0.x) * k, y: p0.y + (p1.y - p0.y) * k)
    }
    return moves.last!.1
}

// A hand-drawn stroke: the pointer follows the pen tip while it is drawn.
func draw(_ points: [CGPoint], over d: Double) {
    strokes.append(Stroke(points: points, start: clock, duration: d))
    let n = 12
    for i in 1...n {
        let p = points[min(points.count - 1, i * (points.count - 1) / n)]
        moves.append((clock + d * Double(i) / Double(n), p))
    }
    clock += d
}
func ellipse(cx: CGFloat, cy: CGFloat, rx: CGFloat, ry: CGFloat) -> [CGPoint] {
    (0...64).map { i in
        let a = -2.2 + Double(i) / 64 * 2 * .pi * 1.08           // a little past one turn, like a real pen
        let wobble = 1 + 0.04 * sin(Double(i) * 0.7)
        return CGPoint(x: cx + rx * CGFloat(cos(a) * wobble), y: cy + ry * CGFloat(sin(a) * wobble))
    }
}
func segment(_ a: CGPoint, _ b: CGPoint, bow: CGFloat = 0) -> [CGPoint] {
    (0...24).map { i in
        let k = CGFloat(i) / 24, s = sin(k * .pi) * bow
        let nx = -(b.y - a.y), ny = b.x - a.x, len = max(1, hypot(nx, ny))
        return CGPoint(x: a.x + (b.x - a.x) * k + nx / len * s, y: a.y + (b.y - a.y) * k + ny / len * s)
    }
}

// Slide layout (top-left coordinates, like a screen).
let winX: CGFloat = 150, winY: CGFloat = 160, winW: CGFloat = 980, winH: CGFloat = 540
let slide = CGRect(x: winX + 24, y: winY + 62, width: winW - 48, height: winH - 86)
let bars: [(String, String, CGFloat)] = [("Jul", "1.2M", 120), ("Aug", "1.6M", 160), ("Sep", "1.5M", 150), ("Oct", "2.3M", 230)]
let barX0: CGFloat = 240, barW: CGFloat = 62, barGap: CGFloat = 30, barBase: CGFloat = 630
let lastBarTop = barBase - bars.last!.2
let callout = CGPoint(x: barX0 + 3 * (barW + barGap) + barW / 2 + 84, y: lastBarTop - 34)

at(0.3)
caption("Show people where to look", "Presentation tools for screen sharing and recordings.")
move(700, 470, over: 0.01)
at(0.6); move(420, 520, over: 0.9); click()
at(0.35); move(760, 430, over: 0.8); click()
at(0.7)
caption("Spotlight", "Everything dims, except a circle around the pointer.")
pill("⌃ ⌥ H", "Ctrl+Alt+H")
at(0.25); spotlight.append((clock, true))
at(0.3); move(barX0 + 3 * (barW + barGap) + barW / 2, lastBarTop + 40, over: 1.1)
at(0.4); move(800, 470, over: 1.0)
at(0.5)
caption("Hold ⌃⌥ and scroll", "to make the circle bigger or smaller.")
pill("⌃ ⌥ + scroll", "Ctrl+Alt + wheel")
at(0.3); radius.append((clock, 110)); at(0.8); radius.append((clock, 165))
at(0.4); radius.append((clock, 165)); at(0.5); radius.append((clock, 120))
at(0.6)
caption("Zoom", "The circle magnifies what's under the pointer.")
pill("⌃ ⌥ Z", "Ctrl+Alt+Z")
at(0.25); zoomOn.append((clock, true))
at(0.2); move(760, 662, over: 1.0)
at(0.5); pill("⌃ ⌥ =", "Ctrl+Alt+=")
at(0.15); zoomLevel.append((clock, 2)); at(0.4); zoomLevel.append((clock, 3))
at(0.3); move(930, 662, over: 1.2)
at(0.4); pill("esc", "Esc")
at(0.1); spotlight.append((clock, false)); zoomOn.append((clock, false))
at(0.5)
caption("Draw on screen", "Circle it, point at it. ⌘Z takes back the last stroke.")
pill("⌃ ⌥ D", "Ctrl+Alt+D")
at(0.4); move(callout.x - 70, callout.y - 6, over: 0.5)
at(0.1); draw(ellipse(cx: callout.x, cy: callout.y - 9, rx: 68, ry: 30), over: 0.85)
at(0.35); move(690, 486, over: 0.45)
at(0.1); draw(segment(CGPoint(x: 690, y: 486), CGPoint(x: barX0 + 4 * (barW + barGap) - 10, y: lastBarTop + 60), bow: -18), over: 0.5)
let tip = CGPoint(x: barX0 + 4 * (barW + barGap) - 10, y: lastBarTop + 60)
at(0.05); draw([tip, CGPoint(x: tip.x + 20, y: tip.y - 18)], over: 0.15)
at(0.05); draw([tip, CGPoint(x: tip.x + 24, y: tip.y + 12)], over: 0.15)
at(0.3); move(slide.minX + 40, slide.minY + 88, over: 0.5)
at(0.1); draw(segment(CGPoint(x: slide.minX + 40, y: slide.minY + 88), CGPoint(x: slide.minX + 262, y: slide.minY + 86), bow: 4), over: 0.45)
at(0.5); pill("⌘ Z", "Ctrl+Z")
at(0.1); strokes[strokes.count - 1].removedAt = clock
at(0.7); pill("esc", "Esc")
at(0.1); for i in strokes.indices where strokes[i].removedAt > clock { strokes[i].removedAt = clock }
at(0.6)
caption("Every click ripples in the language's color", "Green for English, blue for Thai: you see what you'll type in.")
at(0.4); move(880, 600, over: 0.5); click()
at(0.45); langAt.append((clock, 1))
at(0.55); move(990, 560, over: 0.4); click()
at(0.35); move(800, 610, over: 0.4); click()
at(1.2)
endAt = clock
let TOTAL = clock + 3.6

// MARK: - Helpers

func ease(_ x: Double) -> CGFloat { let v = max(0, min(1, x)); return CGFloat(v * v * (3 - 2 * v)) }
func easeOutBack(_ x: Double) -> CGFloat {
    let v = max(0, min(1, x)), c1 = 1.70158, c3 = c1 + 1
    return CGFloat(1 + c3 * pow(v - 1, 3) + c1 * pow(v - 1, 2))
}
// A switch that fades over `fade` seconds.
func level(_ list: [(Double, Bool)], _ t: Double, fade: Double = 0.3) -> CGFloat {
    var v: CGFloat = 0, from: CGFloat = 0, start = -10.0, on = false
    for (time, value) in list where time <= t { from = v; on = value; start = time; v = value ? 1 : 0 }
    let k = ease((t - start) / fade)
    return from + ((on ? 1 : 0) - from) * k
}
func value(_ list: [(Double, CGFloat)], _ t: Double) -> CGFloat {
    if t <= list[0].0 { return list[0].1 }
    for i in 1..<list.count where t <= list[i].0 {
        let (t0, a) = list[i - 1], (t1, b) = list[i]
        return a + (b - a) * (t1 > t0 ? ease((t - t0) / (t1 - t0)) : 1)
    }
    return list.last!.1
}
func lang(_ t: Double) -> Int { langAt.last { $0.0 <= t }!.1 }

func font(_ size: CGFloat, _ weight: NSFont.Weight = .regular) -> CTFont { NSFont.systemFont(ofSize: size, weight: weight) as CTFont }
func line(_ s: String, _ f: CTFont, _ color: CGColor) -> CTLine {
    CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: [.font: f, .foregroundColor: color]))
}
func width(_ l: CTLine) -> CGFloat { CGFloat(CTLineGetTypographicBounds(l, nil, nil, nil)) }
func draw(_ ctx: CGContext, _ l: CTLine, x: CGFloat, baseline y: CGFloat) {
    ctx.textPosition = CGPoint(x: x, y: H - y)
    CTLineDraw(l, ctx)
}
func rect(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> CGRect { CGRect(x: x, y: H - y - h, width: w, height: h) }
func flip(_ p: CGPoint) -> CGPoint { CGPoint(x: p.x, y: H - p.y) }
func rounded(_ ctx: CGContext, _ r: CGRect, _ radius: CGFloat) {
    ctx.addPath(CGPath(roundedRect: r, cornerWidth: radius, cornerHeight: radius, transform: nil))
}

func makeContext(_ w: Int, _ h: Int) -> CGContext {
    let c = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                      space: CGColorSpace(name: CGColorSpace.sRGB)!,
                      bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
    c.setShouldSmoothFonts(false)
    c.interpolationQuality = .high
    return c
}

// MARK: - The screen: a window with a slide (drawn once, at 2x so the zoom stays sharp)

func drawScreen(_ ctx: CGContext) {
    let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: [bgBottom, bgTop] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(g, start: .zero, end: CGPoint(x: 0, y: H), options: [])
    let frame = rect(winX, winY, winW, winH)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -18), blur: 50, color: rgb(0x000000, 0.55))
    rounded(ctx, frame, 18); ctx.setFillColor(rgb(0x1C1D26)); ctx.fillPath()
    ctx.restoreGState()
    rounded(ctx, frame.insetBy(dx: 0.5, dy: 0.5), 18); ctx.setStrokeColor(rgb(0xFFFFFF, 0.10)); ctx.setLineWidth(1); ctx.strokePath()
    for (i, c) in [0xFF5F57, 0xFEBC2E, 0x28C840].enumerated() {
        ctx.setFillColor(rgb(UInt32(c))); ctx.fillEllipse(in: rect(winX + 22 + CGFloat(i) * 22, winY + 18, 13, 13))
    }
    let title = line("Q3 Review", font(15, .semibold), rgb(0xFFFFFF, 0.45))
    draw(ctx, title, x: winX + (winW - width(title)) / 2, baseline: winY + 30)

    // The slide.
    rounded(ctx, rect(slide.minX, slide.minY, slide.width, slide.height), 10); ctx.setFillColor(rgb(0xF6F7FB)); ctx.fillPath()
    let dark = rgb(0x1D2233), grey = rgb(0x7A8199)
    draw(ctx, line("Q3 Results", font(44, .bold), dark), x: slide.minX + 40, baseline: slide.minY + 72)
    draw(ctx, line("Revenue by month", font(20), grey), x: slide.minX + 42, baseline: slide.minY + 112)
    ctx.setFillColor(rgb(0xD9DDEA)); ctx.fill(rect(barX0 - 20, barBase, 4 * (barW + barGap) + 10, 2))
    for (i, (month, amount, h)) in bars.enumerated() {
        let x = barX0 + CGFloat(i) * (barW + barGap), last = i == bars.count - 1
        rounded(ctx, rect(x, barBase - h, barW, h), 8); ctx.setFillColor(last ? thColor : rgb(0xBAC3DE)); ctx.fillPath()
        let m = line(month, font(16, .medium), grey)
        draw(ctx, m, x: x + (barW - width(m)) / 2, baseline: barBase + 26)
        let a = line(amount, font(15, .semibold), last ? thColor : grey)
        draw(ctx, a, x: x + (barW - width(a)) / 2, baseline: barBase - h - 10)
    }
    let up = line("+42%", font(34, .heavy), enColor)
    draw(ctx, up, x: callout.x - width(up) / 2, baseline: callout.y + 4)
    for (i, text) in ["Best month so far", "New Thai users +65%", "Churn down to 2.1%"].enumerated() {
        let y = slide.minY + 214 + CGFloat(i) * 50
        ctx.setFillColor(thColor); ctx.fillEllipse(in: rect(676, y - 13, 9, 9))
        draw(ctx, line(text, font(24, .medium), dark), x: 700, baseline: y)
    }
    draw(ctx, line("Source: demo data for CursorLingo, not real numbers · page 3 of 12", font(11), grey), x: 676, baseline: 666)
}

let screen2x: CGImage = {
    let ctx = makeContext(Int(W) * 2, Int(H) * 2)
    ctx.scaleBy(x: 2, y: 2)
    drawScreen(ctx)
    return ctx.makeImage()!
}()

// MARK: - Frame

func render(_ ctx: CGContext, _ t: Double) {
    let intro = ease(t / 0.6), outro = 1 - ease((t - endAt) / 0.5)
    let stage = intro * outro
    let p = pointer(t)
    if stage > 0.001 {
        ctx.saveGState()
        ctx.setAlpha(stage)
        let scale = (0.96 + 0.04 * intro) * (0.94 + 0.06 * outro)
        ctx.translateBy(x: W / 2, y: H / 2); ctx.scaleBy(x: scale, y: scale); ctx.translateBy(x: -W / 2, y: -H / 2)
        ctx.draw(screen2x, in: CGRect(x: 0, y: 0, width: W, height: H))
        drawStrokes(ctx, t)
        drawCircle(ctx, t, p)
        drawRipples(ctx, t)
        drawPointer(ctx, p, t)
        drawCaption(ctx, t)
        drawPill(ctx, t)
        ctx.restoreGState()
    } else {
        let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: [bgBottom, bgTop] as CFArray, locations: [0, 1])!
        ctx.drawLinearGradient(g, start: .zero, end: CGPoint(x: 0, y: H), options: [])
    }
    if t > endAt { drawEndCard(ctx, t - endAt - 0.35) }
}

func drawStrokes(_ ctx: CGContext, _ t: Double) {
    for s in strokes where t >= s.start {
        let fade = 1 - ease((t - s.removedAt) / 0.25)
        guard fade > 0.01 else { continue }
        let k = min(1, (t - s.start) / s.duration)
        let n = max(2, Int(Double(s.points.count - 1) * k) + 1)
        let path = CGMutablePath()
        path.addLines(between: s.points.prefix(n).map(flip))
        ctx.addPath(path)
        ctx.setStrokeColor(pen.copy(alpha: fade)!); ctx.setLineWidth(6); ctx.setLineCap(.round); ctx.setLineJoin(.round)
        ctx.strokePath()
    }
}

// The spotlight's dim layer, the zoom lens and the turning rainbow ring.
func drawCircle(_ ctx: CGContext, _ t: Double, _ p: CGPoint) {
    let spot = level(spotlight, t), zoom = level(zoomOn, t)
    let on = max(spot, zoom)
    guard on > 0.01 else { return }
    let r = value(radius, t) * (0.85 + 0.15 * on), c = flip(p)
    let circle = CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r)
    if spot > 0.01 {
        ctx.saveGState()
        ctx.addRect(CGRect(x: 0, y: 0, width: W, height: H)); ctx.addEllipse(in: circle)
        ctx.setFillColor(rgb(0x455761, 0.6 * spot)); ctx.fillPath(using: .evenOdd)
        ctx.restoreGState()
    }
    if zoom > 0.01 {
        let z = 1 + (value(zoomLevel, t) - 1) * zoom
        ctx.saveGState()
        ctx.addEllipse(in: circle); ctx.clip()
        ctx.translateBy(x: c.x, y: c.y); ctx.scaleBy(x: z, y: z); ctx.translateBy(x: -c.x, y: -c.y)
        ctx.setAlpha(zoom)
        ctx.draw(screen2x, in: CGRect(x: 0, y: 0, width: W, height: H))
        ctx.restoreGState()
    }
    // Rainbow ring: one turn every 6 s.
    let turn = CGFloat(t / 6).truncatingRemainder(dividingBy: 1) * 2 * .pi
    ctx.saveGState()
    ctx.setLineWidth(5); ctx.setLineCap(.butt)
    ctx.setShadow(offset: .zero, blur: 10, color: rgb(0xFFFFFF, 0.35 * on))
    let parts = 90
    for i in 0..<parts {
        let a0 = turn + CGFloat(i) / CGFloat(parts) * 2 * .pi, a1 = a0 + 2 * .pi / CGFloat(parts) + 0.01
        let hue = CGFloat(i) / CGFloat(parts)
        ctx.setStrokeColor(NSColor(hue: hue, saturation: 0.75, brightness: 1, alpha: on).cgColor)
        ctx.addArc(center: c, radius: r, startAngle: a0, endAngle: a1, clockwise: false)
        ctx.strokePath()
    }
    ctx.restoreGState()
}

func drawRipples(_ ctx: CGContext, _ t: Double) {
    for (time, at) in clicks where t >= time && t - time < 0.45 {
        let k = (t - time) / 0.45, r = 8 + 30 * (1 - (1 - CGFloat(k)) * (1 - CGFloat(k)))
        let c = flip(at)
        ctx.setStrokeColor(accent(lang(time)).copy(alpha: 1 - CGFloat(k))!)
        ctx.setLineWidth(4)
        ctx.strokeEllipse(in: CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r))
    }
}

func drawPointer(_ ctx: CGContext, _ at: CGPoint, _ t: Double) {
    let p = CGMutablePath(), x = at.x, y = H - at.y
    let pts: [(CGFloat, CGFloat)] = [(0, 0), (0, -26), (6.5, -20), (11, -30), (15, -28), (10.5, -18.5), (19, -18.5)]
    p.move(to: CGPoint(x: x, y: y))
    for (dx, dy) in pts.dropFirst() { p.addLine(to: CGPoint(x: x + dx, y: y + dy)) }
    p.closeSubpath()
    ctx.addPath(p); ctx.setFillColor(rgb(0x000000)); ctx.fillPath()
    ctx.addPath(p); ctx.setStrokeColor(rgb(0xFFFFFF)); ctx.setLineWidth(1.6); ctx.strokePath()

    let l = lang(t), since = t - (langAt.last { $0.0 <= t }!.0)
    let pulse: CGFloat = l == 1 && since < 0.5 ? 1 + 0.35 * CGFloat(sin(.pi * since / 0.5)) : 1
    let bw: CGFloat = 46 * pulse, bh: CGFloat = 26 * pulse
    let bx = at.x + 18 - (bw - 46) / 2, by = at.y + 26 - (bh - 26) / 2
    let br = rect(bx, by, bw, bh)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -3), blur: 10, color: rgb(0x000000, 0.5))
    rounded(ctx, br, bh / 2); ctx.setFillColor(accent(l)); ctx.fillPath()
    ctx.restoreGState()
    let code = line(l == 0 ? "EN" : "TH", font(14 * pulse, .bold), rgb(0xFFFFFF))
    draw(ctx, code, x: bx + (bw - width(code)) / 2, baseline: by + bh / 2 + 5 * pulse)
}

func drawCaption(_ ctx: CGContext, _ t: Double) {
    guard let i = captions.lastIndex(where: { $0.0 <= t }) else { return }
    let p = (t - captions[i].0) / 0.5
    func one(_ a: String, _ b: String, alpha: CGFloat, dy: CGFloat) {
        guard alpha > 0.01 else { return }
        let l1 = line(a, font(40, .bold), rgb(0xFFFFFF, alpha))
        draw(ctx, l1, x: (W - width(l1)) / 2, baseline: 78 + dy)
        let l2 = line(b, font(21), rgb(0xA9B2D6, alpha))
        draw(ctx, l2, x: (W - width(l2)) / 2, baseline: 116 + dy)
    }
    if i > 0 { one(captions[i - 1].1, captions[i - 1].2, alpha: 1 - ease(p * 2), dy: -14 * ease(p * 2)) }
    one(captions[i].1, captions[i].2, alpha: ease(p * 2 - (i > 0 ? 1 : 0)), dy: 14 * (1 - ease(p * 2 - (i > 0 ? 1 : 0))))
}

// The keys just pressed, macOS and Windows, in a pill under the window.
func drawPill(_ ctx: CGContext, _ t: Double) {
    guard let last = pills.last(where: { $0.0 <= t }), t - last.0 < 1.4 else { return }
    let p = t - last.0
    let alpha = p < 1.1 ? ease(p / 0.15) : 1 - ease((p - 1.1) / 0.3)
    let keys = line(last.1, font(28, .bold), rgb(0xFFFFFF, alpha))
    let win = line(last.2, font(15, .semibold), rgb(0xFFFFFF, 0.7 * alpha))
    let w = width(keys) + width(win) + 64
    let x = (W - w) / 2, y: CGFloat = 724 + 8 * (1 - easeOutBack(p / 0.3))
    rounded(ctx, rect(x, y, w, 52), 14); ctx.setFillColor(rgb(0x2A2F4A, 0.92 * alpha)); ctx.fillPath()
    rounded(ctx, rect(x, y, w, 52).insetBy(dx: 0.5, dy: 0.5), 14); ctx.setStrokeColor(rgb(0xFFFFFF, 0.15 * alpha)); ctx.strokePath()
    draw(ctx, keys, x: x + 24, baseline: y + 36)
    ctx.setFillColor(rgb(0xFFFFFF, 0.2 * alpha)); ctx.fill(rect(x + 24 + width(keys) + 14, y + 14, 1, 24))
    draw(ctx, win, x: x + 40 + width(keys) + 8, baseline: y + 32)
}

let icon: CGImage? = {
    guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: "assets/AppIcon.png") as CFURL, nil) else { return nil }
    return CGImageSourceCreateImageAtIndex(src, 0, nil)
}()

func drawEndCard(_ ctx: CGContext, _ p: Double) {
    guard p > 0 else { return }
    let a = ease(p / 0.6), s = 0.85 + 0.15 * easeOutBack(p / 0.7), size: CGFloat = 150 * s
    if let icon {
        ctx.saveGState()
        ctx.setAlpha(a)
        ctx.setShadow(offset: CGSize(width: 0, height: -12), blur: 40, color: rgb(0x000000, 0.6))
        ctx.draw(icon, in: rect((W - size) / 2, 190 + (150 - size) / 2, size, size))
        ctx.restoreGState()
    }
    let title = line("CursorLingo", font(64, .heavy), rgb(0xFFFFFF, a))
    draw(ctx, title, x: (W - width(title)) / 2, baseline: 430)
    let sub = line("Spotlight, zoom and draw on screen when you present.", font(26, .medium), rgb(0xC9D2F2, a))
    draw(ctx, sub, x: (W - width(sub)) / 2, baseline: 482)
    let tag = line("macOS  ·  Windows  ·  free & open source", font(20, .semibold), rgb(0x8E98C4, ease((p - 0.25) / 0.6)))
    draw(ctx, tag, x: (W - width(tag)) / 2, baseline: 530)
}

// MARK: - Output

func frame(_ t: Double) -> CGImage {
    let ctx = makeContext(Int(W), Int(H))
    render(ctx, t)
    return ctx.makeImage()!
}

func writePNG(_ img: CGImage, _ path: String) {
    let d = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(d, img, nil); CGImageDestinationFinalize(d)
}

let args = CommandLine.arguments
if let i = args.firstIndex(of: "--preview"), i + 1 < args.count {
    let dir = args.count > i + 2 ? args[i + 2] : "/tmp"
    for s in args[i + 1].split(separator: ",") { writePNG(frame(Double(s)!), "\(dir)/pres-\(s).png") }
    print("timeline \(String(format: "%.1f", TOTAL))s, clicks at", clicks.map { String(format: "%.2f", $0.0) })
    exit(0)
}

let mp4 = URL(fileURLWithPath: "assets/presentation-demo.mp4")
try? FileManager.default.removeItem(at: mp4)
let writer = try! AVAssetWriter(outputURL: mp4, fileType: .mp4)
let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
    AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: Int(W), AVVideoHeightKey: Int(H),
    AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: 4_000_000],
])
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
    kCVPixelBufferWidthKey as String: Int(W), kCVPixelBufferHeightKey as String: Int(H),
])
writer.add(input)
writer.startWriting()
writer.startSession(atSourceTime: .zero)

let gifW = 900, gifH = Int(Double(gifW) * Double(H / W))

// The dimmed screen and the turning ring change a little of every frame, so a GIF of whole frames is huge. So every
// frame is mapped to one palette made for this animation, and keeps only the pixels whose colour differs from what
// is already on screen; the rest is transparent and shows the frame before (see the disposal fix-up at the end).
// Comparing palette entries, not raw colours, means nothing drifts.
let palette: [(UInt8, UInt8, UInt8)] = {
    var samples: [(Float, Float, Float)] = []
    for t in [1.5, 4.5, 7.0, 11.0, 15.0, 19.5, 24.3, 27.0] {
        let c = makeContext(gifW, gifH)
        c.draw(frame(t), in: CGRect(x: 0, y: 0, width: gifW, height: gifH))
        let px = c.data!.bindMemory(to: UInt8.self, capacity: gifH * c.bytesPerRow)
        for y in stride(from: 0, to: gifH, by: 3) {
            for x in stride(from: 0, to: gifW, by: 3) {
                let o = y * c.bytesPerRow + x * 4
                samples.append((Float(px[o + 2]), Float(px[o + 1]), Float(px[o])))
            }
        }
    }
    // Fixed entries for what must look exact (the ring's hues, the pen, the badges), then k-means for the rest:
    // 255 colours in all, as the encoder adds the transparent one.
    func comps(_ c: CGColor) -> (Float, Float, Float) {
        let v = c.converted(to: CGColorSpace(name: CGColorSpace.sRGB)!, intent: .defaultIntent, options: nil)!.components!
        return (Float(v[0] * 255), Float(v[1] * 255), Float(v[2] * 255))
    }
    var fixed = (0..<48).map { comps(NSColor(hue: CGFloat($0) / 48, saturation: 0.75, brightness: 1, alpha: 1).cgColor) }
    fixed += [pen, enColor, thColor, rgb(0xFFFFFF), rgb(0x000000)].map(comps)
    var centres = fixed + (0..<(255 - fixed.count)).map { samples[$0 * samples.count / (255 - fixed.count)] }
    for _ in 0..<10 {
        var sum = [(Float, Float, Float, Float)](repeating: (0, 0, 0, 0), count: 255)
        for s in samples {
            var best = 0, bestD = Float.infinity
            for (i, c) in centres.enumerated() {
                let d = (s.0 - c.0) * (s.0 - c.0) + (s.1 - c.1) * (s.1 - c.1) + (s.2 - c.2) * (s.2 - c.2)
                if d < bestD { bestD = d; best = i }
            }
            sum[best].0 += s.0; sum[best].1 += s.1; sum[best].2 += s.2; sum[best].3 += 1
        }
        for i in fixed.count..<255 where sum[i].3 > 0 { centres[i] = (sum[i].0 / sum[i].3, sum[i].1 / sum[i].3, sum[i].2 / sum[i].3) }
    }
    return centres.map { (UInt8($0.0.rounded()), UInt8($0.1.rounded()), UInt8($0.2.rounded())) }
}()
// Nearest palette entry for each colour, at 6 bits per channel.
let nearest: [UInt8] = (0..<(1 << 18)).map { v in
    let r = Int((v >> 12) & 63) << 2 | 2, g = Int((v >> 6) & 63) << 2 | 2, b = Int(v & 63) << 2 | 2
    var best = 0, bestD = Int.max
    for (i, c) in palette.enumerated() {
        let d = (r - Int(c.0)) * (r - Int(c.0)) + (g - Int(c.1)) * (g - Int(c.1)) + (b - Int(c.2)) * (b - Int(c.2))
        if d < bestD { bestD = d; best = i }
    }
    return UInt8(best)
}
var shown = [Int16](repeating: -1, count: gifW * gifH)  // palette entry on screen so far
func onlyChanges(_ ctx: CGContext) -> CGImage {
    let px = ctx.data!.bindMemory(to: UInt8.self, capacity: gifH * ctx.bytesPerRow)
    for y in 0..<gifH {
        for x in 0..<gifW {
            let o = y * ctx.bytesPerRow + x * 4, k = y * gifW + x  // BGRA, opaque
            let i = nearest[Int(px[o + 2] >> 2) << 12 | Int(px[o + 1] >> 2) << 6 | Int(px[o] >> 2)]
            if shown[k] == Int16(i) {
                px[o] = 0; px[o + 1] = 0; px[o + 2] = 0; px[o + 3] = 0
            } else {
                let c = palette[Int(i)]
                px[o] = c.2; px[o + 1] = c.1; px[o + 2] = c.0; px[o + 3] = 255
                shown[k] = Int16(i)
            }
        }
    }
    return ctx.makeImage()!
}
let n = Int(TOTAL * FPS)
let gif = CGImageDestinationCreateWithURL(URL(fileURLWithPath: "assets/presentation-demo.gif") as CFURL, UTType.gif.identifier as CFString,
                                          n / 2 + 1, nil)!
CGImageDestinationSetProperties(gif, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFLoopCount: 0]] as CFDictionary)

for i in 0..<n {
    let t = Double(i) / FPS
    let img = frame(t)
    while !input.isReadyForMoreMediaData { usleep(1000) }
    var pb: CVPixelBuffer?
    CVPixelBufferPoolCreatePixelBuffer(nil, adaptor.pixelBufferPool!, &pb)
    CVPixelBufferLockBaseAddress(pb!, [])
    let pctx = CGContext(data: CVPixelBufferGetBaseAddress(pb!), width: Int(W), height: Int(H), bitsPerComponent: 8,
                         bytesPerRow: CVPixelBufferGetBytesPerRow(pb!), space: CGColorSpace(name: CGColorSpace.sRGB)!,
                         bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
    pctx.draw(img, in: CGRect(x: 0, y: 0, width: W, height: H))
    CVPixelBufferUnlockBaseAddress(pb!, [])
    adaptor.append(pb!, withPresentationTime: CMTime(value: CMTimeValue(i), timescale: CMTimeScale(FPS)))

    if i % 2 == 0 {
        let small = makeContext(gifW, gifH)
        small.draw(img, in: CGRect(x: 0, y: 0, width: gifW, height: gifH))
        CGImageDestinationAddImage(gif, onlyChanges(small),
                                   [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFDelayTime: i + 2 >= n ? 3.0 : 2 / FPS]] as CFDictionary)
    }
}
input.markAsFinished()
let done = DispatchSemaphore(value: 0)
writer.finishWriting { done.signal() }
done.wait()
CGImageDestinationFinalize(gif)

// ImageIO marks every frame "restore to background", which would blank the transparent pixels; walk the GIF's
// blocks and mark them "leave in place" instead, so each frame draws over the one before.
var bytes = [UInt8](try! Data(contentsOf: URL(fileURLWithPath: "assets/presentation-demo.gif")))
func table(_ flags: UInt8) -> Int { flags & 0x80 != 0 ? 3 << (Int(flags & 7) + 1) : 0 }
func skipSubBlocks(_ i: inout Int) { while bytes[i] != 0 { i += Int(bytes[i]) + 1 }; i += 1 }
var pos = 13 + table(bytes[10])
while pos < bytes.count && bytes[pos] != 0x3B {
    if bytes[pos] == 0x21 {
        if bytes[pos + 1] == 0xF9 { bytes[pos + 3] = (bytes[pos + 3] & ~0x1C) | (1 << 2) }
        pos += 2
        skipSubBlocks(&pos)
    } else {  // 0x2C, an image: descriptor, local colour table, LZW code size, data
        pos += 10 + table(bytes[pos + 9]) + 1
        skipSubBlocks(&pos)
    }
}
try! Data(bytes).write(to: URL(fileURLWithPath: "assets/presentation-demo.gif"))
print("rendered \(n) frames (\(String(format: "%.1f", TOTAL))s): assets/presentation-demo.mp4, assets/presentation-demo.gif")
