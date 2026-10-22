// Renders the README demo animation (assets/demo.gif + assets/demo.mp4) frame by frame.
//
//   swiftc -O tools/make-demo.swift -o /tmp/make-demo && /tmp/make-demo [--preview t1,t2,...]
//
// Everything is drawn with CoreGraphics / CoreText: a notes window, a keyboard whose keys carry both the
// English and the Thai legend (the active layout's legend lights up), the CursorLingo badge at the pointer,
// and the auto-fix effect (shake, squiggle, flip, sweep, sparkles).

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
let enColor = rgb(0x34C77B), thColor = rgb(0x3D7BFF), bad = rgb(0xFF4D5E)
func accent(_ lang: Int) -> CGColor { lang == 0 ? enColor : thColor }

// MARK: - Keyboard model (same physical keys as core/layoutfix.h)

let enBase = Array("`1234567890-=qwertyuiop[]\\asdfghjkl;'zxcvbnm,./")
let enShift = Array("~!@#$%^&*()_+QWERTYUIOP{}|ASDFGHJKL:\"ZXCVBNM<>?")
// Unicode scalars, not Characters: Swift would merge Thai marks with their neighbours into one Character.
let thBase = "_ๅ/-ภถุึคตจขชๆไำพะัีรนยบลฃฟหกดเ้่าสวงผปแอิืทมใฝ".unicodeScalars.map { String($0) }
let thShift = "%+๑๒๓๔ู฿๕๖๗๘๙๐\"ฎฑธํ๊ณฯญฐ,ฅฤฆฏโฌ็๋ษศซ.()ฉฮฺ์?ฒฬฦ".unicodeScalars.map { String($0) }
let SPACE = 100, SHIFT = 101
precondition(thBase.count == 47 && enBase.count == 47)

func keyFor(_ c: Character) -> Int {
    if c == " " { return SPACE }
    if let i = enBase.firstIndex(of: c) { return i }
    fatalError("no key for \(c)")
}
func charFor(_ key: Int, _ lang: Int) -> String {
    if key == SPACE { return " " }
    return lang == 0 ? String(enBase[key]) : thBase[key]
}

// MARK: - Script

enum Ev {
    case caption(String, String)
    case key(Int)
    case fix(String, Int)          // new text of the current line, new layout
    case newline
    case paste(String)
    case select
    case shiftTap
    case endCard
}

var events: [(Double, Ev)] = []
var clock = 0.0
func at(_ dt: Double, _ e: Ev) { clock += dt; events.append((clock, e)) }
func type(_ s: String, gap: Double = 0.13) { for c in s { at(gap, .key(keyFor(c))) } }

at(0.25, .caption("Typed in the wrong language?", "Happens to everyone who types English and Thai."))
at(1.6, .caption("Thai on the English keyboard…", "CursorLingo reads the same keys through the other layout."))
at(0.5, .key(keyFor("l"))); type(";ylfu")
at(0.35, .fix("สวัสดี", 1))
at(0.05, .caption("…fixed while you type.", "And the keyboard switches to Thai for you."))
at(0.75, .key(keyFor("8"))); type("iy[")
at(1.5, .newline)
at(0.05, .caption("English on the Thai keyboard…", "Gibberish on screen, real words underneath."))
at(0.5, .key(keyFor("t"))); type("his")
at(0.45, .key(SPACE))
at(0.02, .fix("this ", 0))
at(0.05, .caption("…fixed when you hit Space.", "Then just keep typing."))
at(0.55, .key(keyFor("i"))); type("s cool")
at(1.4, .newline)
at(0.05, .caption("Double-tap ⇧ Shift", "Convert a word yourself, or undo a fix."))
at(0.5, .key(keyFor("m"))); type("uj")
at(0.7, .shiftTap); at(0.16, .shiftTap)
at(0.08, .fix("ที่", 1))
at(1.4, .newline)
at(0.05, .caption("Select any text, double-tap ⇧", "Whole sentences come back in the right language."))
at(0.4, .paste("ะ้รห รห ฟ ืำไ ทนกำส"))
at(0.8, .select)
at(0.85, .shiftTap); at(0.16, .shiftTap)
at(0.08, .fix("this is a new model", 0))
at(2.0, .endCard)
let TOTAL = clock + 3.6

// MARK: - State at time t

struct Fix { var line: Int; var old: String; var new: String; var start: Double; var lang: Int; var oldLang: Int }
let FLIP = 0.28   // seconds from a fix starting to the new text taking over

struct State {
    var lines: [String] = [""]
    var lang = 0
    var langChangedAt = -10.0
    var caption = ("", "")
    var prevCaption = ("", "")
    var captionAt = -10.0
    var pressed: [Int: Double] = [:]    // key -> time
    var lastKeyAt = -10.0
    var fix: Fix?
    var pasteAt = -10.0
    var selectAt: Double?
    var selectLine = -1
    var shiftTaps: [Double] = []
    var endAt: Double?
}

func state(at t: Double) -> State {
    var s = rawState(at: t)
    if let f = s.fix, t - f.start < FLIP { s.lang = f.oldLang }
    return s
}

func rawState(at t: Double) -> State {
    var s = State()
    for (time, e) in events where time <= t {
        switch e {
        case .caption(let a, let b): s.prevCaption = s.caption; s.caption = (a, b); s.captionAt = time
        case .key(let k):
            s.pressed[k] = time
            s.lastKeyAt = time
            s.lines[s.lines.count - 1] += charFor(k, s.lang)
            s.selectAt = nil
        case .fix(let text, let lang):
            let i = s.lines.count - 1
            s.fix = Fix(line: i, old: s.lines[i], new: text, start: time, lang: lang, oldLang: s.lang)
            s.lines[i] = text
            if lang != s.lang { s.lang = lang; s.langChangedAt = time + FLIP }
            s.selectAt = nil
        case .newline: s.lines.append(""); s.selectAt = nil
        case .paste(let text): s.lines[s.lines.count - 1] = text; s.pasteAt = time
        case .select: s.selectAt = time; s.selectLine = s.lines.count - 1
        case .shiftTap: s.shiftTaps.append(time); s.pressed[SHIFT] = time
        case .endCard: s.endAt = time
        }
    }
    return s
}

// MARK: - Drawing helpers

func ease(_ x: Double) -> CGFloat { let v = max(0, min(1, x)); return CGFloat(v * v * (3 - 2 * v)) }
func easeOutBack(_ x: Double) -> CGFloat {
    let v = max(0, min(1, x)), c1 = 1.70158, c3 = c1 + 1
    return CGFloat(1 + c3 * pow(v - 1, 3) + c1 * pow(v - 1, 2))
}

func font(_ size: CGFloat, _ weight: NSFont.Weight = .regular) -> CTFont {
    NSFont.systemFont(ofSize: size, weight: weight) as CTFont
}
func line(_ s: String, _ f: CTFont, _ color: CGColor) -> CTLine {
    let attr = NSAttributedString(string: s, attributes: [.font: f, .foregroundColor: color])
    return CTLineCreateWithAttributedString(attr)
}
func width(_ l: CTLine) -> CGFloat { CGFloat(CTLineGetTypographicBounds(l, nil, nil, nil)) }

// Top-left based coordinates: y grows downward, like a screen.
func draw(_ ctx: CGContext, _ l: CTLine, x: CGFloat, baseline y: CGFloat) {
    ctx.textPosition = CGPoint(x: x, y: H - y)
    CTLineDraw(l, ctx)
}
func rect(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> CGRect { CGRect(x: x, y: H - y - h, width: w, height: h) }
func rounded(_ ctx: CGContext, _ r: CGRect, _ radius: CGFloat) {
    ctx.addPath(CGPath(roundedRect: r, cornerWidth: radius, cornerHeight: radius, transform: nil))
}

// MARK: - Layout

let winX: CGFloat = 190, winY: CGFloat = 160, winW: CGFloat = 900, winH: CGFloat = 356
let textX = winX + 44, firstBaseline = winY + 102, lineH: CGFloat = 60
let textFont = font(40, .medium)
let keyU: CGFloat = 50, keyH: CGFloat = 38, keyGap: CGFloat = 6
let kbY: CGFloat = 556

struct KeyCap { var key: Int; var frame: CGRect; var label: String? }
let keycaps: [KeyCap] = {
    var caps: [KeyCap] = []
    let rows: [(Range<Int>, CGFloat)] = [(0..<13, 0), (13..<26, 0.55), (26..<37, 0.85), (37..<47, 1.45)]
    let totalW = 13 * (keyU + keyGap) + 1.6 * keyU
    let x0 = (W - totalW) / 2
    for (r, (range, offset)) in rows.enumerated() {
        let y = kbY + CGFloat(r) * (keyH + keyGap)
        var x = x0 + offset * keyU
        if r == 3 {  // left shift
            caps.append(KeyCap(key: SHIFT, frame: rect(x0, y, offset * keyU - keyGap, keyH), label: "⇧"))
        }
        for k in range {
            caps.append(KeyCap(key: k, frame: rect(x, y, keyU, keyH), label: nil))
            x += keyU + keyGap
        }
        if r == 3 {  // right shift
            caps.append(KeyCap(key: SHIFT, frame: rect(x, y, x0 + totalW - x, keyH), label: "⇧"))
        }
    }
    let sy = kbY + 4 * (keyH + keyGap)
    caps.append(KeyCap(key: SPACE, frame: rect((W - 6.2 * keyU) / 2, sy, 6.2 * keyU, keyH), label: nil))
    return caps
}()

// MARK: - Frame

let icon: CGImage? = {
    let url = URL(fileURLWithPath: "assets/AppIcon.png")
    guard let src = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
    return CGImageSourceCreateImageAtIndex(src, 0, nil)
}()

func render(_ ctx: CGContext, _ t: Double) {
    let s = state(at: t)

    // Background: vertical gradient + soft glow behind the window.
    let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: [bgBottom, bgTop] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(g, start: CGPoint(x: 0, y: 0), end: CGPoint(x: 0, y: H), options: [])
    let glow = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB),
                          colors: [accent(s.lang).copy(alpha: 0.20)!, accent(s.lang).copy(alpha: 0)!] as CFArray, locations: [0, 1])!
    ctx.drawRadialGradient(glow, startCenter: CGPoint(x: W / 2, y: H - winY - winH / 2), startRadius: 0,
                           endCenter: CGPoint(x: W / 2, y: H - winY - winH / 2), endRadius: 620, options: [])

    // Intro / outro for the whole stage.
    let intro = ease(t / 0.6)
    let outro: CGFloat = s.endAt.map { 1 - ease((t - $0) / 0.5) } ?? 1
    let stageAlpha = intro * outro
    if stageAlpha > 0.001 {
        ctx.saveGState()
        ctx.setAlpha(stageAlpha)
        let scale = (0.96 + 0.04 * intro) * (0.94 + 0.06 * outro)
        ctx.translateBy(x: W / 2, y: H / 2); ctx.scaleBy(x: scale, y: scale); ctx.translateBy(x: -W / 2, y: -H / 2)
        drawCaption(ctx, s, t)
        drawWindow(ctx, s, t)
        drawKeyboard(ctx, s, t)
        ctx.restoreGState()
    }
    if let end = s.endAt { drawEndCard(ctx, t - end - 0.35) }
}

func drawCaption(_ ctx: CGContext, _ s: State, _ t: Double) {
    let p = (t - s.captionAt) / 0.5
    func one(_ c: (String, String), alpha: CGFloat, dy: CGFloat) {
        guard alpha > 0.01, !c.0.isEmpty else { return }
        let l1 = line(c.0, font(42, .bold), rgb(0xFFFFFF, alpha))
        draw(ctx, l1, x: (W - width(l1)) / 2, baseline: 82 + dy)
        let l2 = line(c.1, font(21, .regular), rgb(0xA9B2D6, alpha))
        draw(ctx, l2, x: (W - width(l2)) / 2, baseline: 122 + dy)
    }
    one(s.prevCaption, alpha: 1 - ease(p * 2), dy: -14 * ease(p * 2))
    one(s.caption, alpha: ease(p * 2 - 1), dy: 14 * (1 - ease(p * 2 - 1)))
}

func drawWindow(_ ctx: CGContext, _ s: State, _ t: Double) {
    let frame = rect(winX, winY, winW, winH)
    // Shadow + body.
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -18), blur: 50, color: rgb(0x000000, 0.55))
    rounded(ctx, frame, 18); ctx.setFillColor(rgb(0x1C1D26)); ctx.fillPath()
    ctx.restoreGState()
    rounded(ctx, frame.insetBy(dx: 0.5, dy: 0.5), 18); ctx.setStrokeColor(rgb(0xFFFFFF, 0.10)); ctx.setLineWidth(1); ctx.strokePath()
    // Title bar.
    for (i, c) in [0xFF5F57, 0xFEBC2E, 0x28C840].enumerated() {
        ctx.setFillColor(rgb(UInt32(c)))
        ctx.fillEllipse(in: rect(winX + 22 + CGFloat(i) * 22, winY + 18, 13, 13))
    }
    let title = line("Notes", font(15, .semibold), rgb(0xFFFFFF, 0.45))
    draw(ctx, title, x: winX + (winW - width(title)) / 2, baseline: winY + 30)
    ctx.setFillColor(rgb(0xFFFFFF, 0.06)); ctx.fill(rect(winX, winY + 46, winW, 1))

    // Text lines.
    var caret = CGPoint(x: textX, y: firstBaseline)
    for (i, text) in s.lines.enumerated() {
        let y = firstBaseline + CGFloat(i) * lineH
        let isFixLine = s.fix?.line == i
        let f = s.fix
        let ft = f.map { t - $0.start } ?? 99

        // Selection highlight (sweeps across, stays until the fix lands).
        if let sel = s.selectAt, s.selectLine == i {
            let l = line(text, textFont, rgb(0xFFFFFF))
            let w = width(l) * ease((t - sel) / 0.45)
            rounded(ctx, rect(textX - 4, y - 40, w + 8, 52), 6)
            ctx.setFillColor(thColor.copy(alpha: 0.45)!); ctx.fillPath()
        }

        if isFixLine, let f, ft < 1.0 {
            if ft < FLIP {
                // 1) The wrong word shakes, with a red squiggle under it.
                let shake = CGFloat(sin(ft * 80)) * 7 * (1 - CGFloat(ft / FLIP))
                let l = line(f.old, textFont, rgb(0xFFFFFF))
                draw(ctx, l, x: textX + shake, baseline: y)
                squiggle(ctx, x: textX + shake, y: y + 10, w: width(l))
            } else {
                // 2) It lifts away, then the right word pops in and an accent underline grows under it.
                let p = (ft - FLIP) / 0.42
                if p < 0.45 {
                    let q = p / 0.45
                    let lo = line(f.old, textFont, rgb(0xFFFFFF, 1 - ease(q)))
                    draw(ctx, lo, x: textX, baseline: y - 26 * ease(q))
                }
                let q = (p - 0.3) / 0.7
                if q > 0 {
                    let pop = 1 + 0.12 * (1 - easeOutBack(q))
                    let ln = line(f.new, textFont, rgb(0xFFFFFF, ease(q * 1.6)))
                    ctx.saveGState()
                    ctx.translateBy(x: textX, y: H - y + 14); ctx.scaleBy(x: pop, y: pop); ctx.translateBy(x: -textX, y: -(H - y + 14))
                    draw(ctx, ln, x: textX, baseline: y + 14 * (1 - ease(q)))
                    ctx.restoreGState()
                    let w = width(ln) * ease(q * 1.3)
                    let fade: CGFloat = p < 0.85 ? 1 : 1 - ease((p - 0.85) / 0.6)
                    ctx.saveGState()
                    ctx.setShadow(offset: .zero, blur: 14, color: accent(f.lang).copy(alpha: fade)!)
                    rounded(ctx, rect(textX, y + 10, w, 5), 2.5)
                    ctx.setFillColor(accent(f.lang).copy(alpha: fade)!); ctx.fillPath()
                    ctx.restoreGState()
                }
            }
        } else {
            let alpha: CGFloat = (s.lines.count - 1 == i && t - s.pasteAt < 0.4) ? ease((t - s.pasteAt) / 0.4) : 1
            draw(ctx, line(text, textFont, rgb(0xFFFFFF, alpha)), x: textX, baseline: y)
        }
        // Sparkles after a fix.
        if isFixLine, let f, ft > 0.55, ft < 1.45 {
            sparkles(ctx, around: CGPoint(x: textX + width(line(f.new, textFont, rgb(0xFFFFFF))), y: y - 18),
                     p: (ft - 0.55) / 0.9, color: accent(f.lang))
        }
        let shown = (isFixLine && ft < FLIP) ? f!.old : text
        caret = CGPoint(x: textX + width(line(shown, textFont, rgb(0xFFFFFF))) + 3, y: y)
    }

    // Caret: solid while typing, blinking otherwise.
    let typing = t - s.lastKeyAt < 0.6
    if s.endAt == nil && (typing || fmod(t, 1.0) < 0.55) && s.selectAt == nil {
        ctx.setFillColor(rgb(0x7FA6FF)); ctx.fill(rect(caret.x, caret.y - 38, 3, 46))
    }

    // Pointer + CursorLingo badge, parked just below-right of the caret.
    let target = CGPoint(x: caret.x + 26, y: caret.y + 16)
    drawPointer(ctx, target, s, t)
}

func squiggle(_ ctx: CGContext, x: CGFloat, y: CGFloat, w: CGFloat) {
    let p = CGMutablePath()
    var cx = x
    p.move(to: CGPoint(x: cx, y: H - y))
    var up = true
    while cx < x + w { cx += 6; p.addLine(to: CGPoint(x: cx, y: H - y + (up ? 4 : -2))); up.toggle() }
    ctx.addPath(p); ctx.setStrokeColor(bad); ctx.setLineWidth(2.5); ctx.setLineJoin(.round); ctx.strokePath()
}

func sweep(_ ctx: CGContext, x: CGFloat, y: CGFloat, h: CGFloat, color: CGColor, alpha: CGFloat) {
    guard alpha > 0.01 else { return }
    let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB),
                       colors: [color.copy(alpha: 0)!, color.copy(alpha: 0.55 * alpha)!, color.copy(alpha: 0)!] as CFArray,
                       locations: [0, 0.5, 1])!
    ctx.saveGState()
    ctx.clip(to: rect(x, y, 120, h))
    ctx.drawLinearGradient(g, start: CGPoint(x: x, y: 0), end: CGPoint(x: x + 120, y: 0), options: [])
    ctx.restoreGState()
}

func sparkles(_ ctx: CGContext, around c: CGPoint, p: Double, color: CGColor) {
    let a = 1 - ease(p)
    let dirs: [(CGFloat, CGFloat, CGFloat)] = [(30, -26, 11), (46, 6, 8), (18, 26, 7), (-6, -34, 6), (58, -14, 6)]
    for (i, (dx, dy, r)) in dirs.enumerated() {
        let d = 0.4 + 0.6 * ease(p * 1.3 - Double(i) * 0.05)
        let pt = CGPoint(x: c.x + dx * d, y: c.y + dy * d)
        star(ctx, pt, r * (0.6 + 0.4 * a), i % 2 == 0 ? rgb(0xFFFFFF, a) : color.copy(alpha: a)!)
    }
}
func star(_ ctx: CGContext, _ c: CGPoint, _ r: CGFloat, _ color: CGColor) {
    let p = CGMutablePath(), cy = H - c.y
    p.move(to: CGPoint(x: c.x, y: cy + r))
    p.addQuadCurve(to: CGPoint(x: c.x + r, y: cy), control: CGPoint(x: c.x, y: cy))
    p.addQuadCurve(to: CGPoint(x: c.x, y: cy - r), control: CGPoint(x: c.x, y: cy))
    p.addQuadCurve(to: CGPoint(x: c.x - r, y: cy), control: CGPoint(x: c.x, y: cy))
    p.addQuadCurve(to: CGPoint(x: c.x, y: cy + r), control: CGPoint(x: c.x, y: cy))
    ctx.addPath(p); ctx.setFillColor(color); ctx.fillPath()
}

func drawPointer(_ ctx: CGContext, _ at: CGPoint, _ s: State, _ t: Double) {
    // Arrow.
    let p = CGMutablePath(), x = at.x, y = H - at.y
    let pts: [(CGFloat, CGFloat)] = [(0, 0), (0, -26), (6.5, -20), (11, -30), (15, -28), (10.5, -18.5), (19, -18.5)]
    p.move(to: CGPoint(x: x, y: y))
    for (dx, dy) in pts.dropFirst() { p.addLine(to: CGPoint(x: x + dx, y: y + dy)) }
    p.closeSubpath()
    ctx.addPath(p); ctx.setFillColor(rgb(0x000000)); ctx.fillPath()
    ctx.addPath(p); ctx.setStrokeColor(rgb(0xFFFFFF)); ctx.setLineWidth(1.6); ctx.strokePath()

    // Badge with a pulse ring when the layout changes.
    let code = s.lang == 0 ? "EN" : "TH"
    let since = t - s.langChangedAt
    let pulse: CGFloat = since >= 0 && since < 0.5 ? 1 + 0.35 * CGFloat(sin(.pi * since / 0.5)) : 1
    let bw: CGFloat = 46 * pulse, bh: CGFloat = 26 * pulse
    let bx = at.x + 18 - (bw - 46) / 2, by = at.y + 26 - (bh - 26) / 2
    if since >= 0 && since < 0.7 {
        let r = 30 + 60 * ease(since / 0.7)
        ctx.setStrokeColor(accent(s.lang).copy(alpha: 0.8 * (1 - ease(since / 0.7)))!)
        ctx.setLineWidth(3)
        ctx.strokeEllipse(in: CGRect(x: bx + bw / 2 - r, y: H - (by + bh / 2) - r, width: 2 * r, height: 2 * r))
    }
    let br = rect(bx, by, bw, bh)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -3), blur: 10, color: rgb(0x000000, 0.5))
    rounded(ctx, br, bh / 2); ctx.setFillColor(accent(s.lang)); ctx.fillPath()
    ctx.restoreGState()
    let l = line(code, font(14 * pulse, .bold), rgb(0xFFFFFF))
    draw(ctx, l, x: bx + (bw - width(l)) / 2, baseline: by + bh / 2 + 5 * pulse)
}

func drawKeyboard(_ ctx: CGContext, _ s: State, _ t: Double) {
    let a = accent(s.lang)
    let small = font(13, .semibold), smallTh = font(15, .semibold)
    for cap in keycaps {
        let pressedAt = s.pressed[cap.key] ?? -10
        let since = t - pressedAt
        let lit: CGFloat = since >= 0 && since < 0.32 ? 1 - ease(since / 0.32) : 0
        var f = cap.frame
        if lit > 0 { f = f.offsetBy(dx: 0, dy: -2 * lit) }

        ctx.saveGState()
        if lit > 0 { ctx.setShadow(offset: .zero, blur: 22 * lit, color: a.copy(alpha: 0.9 * lit)!) }
        rounded(ctx, f, 8)
        ctx.setFillColor(rgb(0x252838))
        ctx.fillPath()
        if lit > 0 { rounded(ctx, f, 8); ctx.setFillColor(a.copy(alpha: 0.85 * lit)!); ctx.fillPath() }
        ctx.restoreGState()
        rounded(ctx, f.insetBy(dx: 0.5, dy: 0.5), 8); ctx.setStrokeColor(rgb(0xFFFFFF, 0.08)); ctx.setLineWidth(1); ctx.strokePath()

        let top = H - f.maxY  // top-left y of the cap
        if let label = cap.label {
            let l = line(label, font(17, .semibold), rgb(0xFFFFFF, 0.55 + 0.45 * lit))
            draw(ctx, l, x: f.minX + (f.width - width(l)) / 2, baseline: top + 27)
        } else if cap.key < 47 {
            // Both legends; the active layout's one is bright.
            let enA: CGFloat = s.lang == 0 ? 0.95 : 0.30, thA: CGFloat = s.lang == 1 ? 0.95 : 0.30
            let le = line(String(enBase[cap.key]).uppercased(), small, rgb(0xFFFFFF, max(enA, lit)))
            draw(ctx, le, x: f.minX + 7, baseline: top + 17)
            let lt = line(thBase[cap.key], smallTh, s.lang == 1 ? rgb(0xBFD3FF, max(thA, lit)) : rgb(0xFFFFFF, max(thA, lit * 0.6)))
            draw(ctx, lt, x: f.maxX - width(lt) - 7, baseline: top + 34)
        }
    }
    // "⇧ ⇧" bubble on double Shift.
    if let last = s.shiftTaps.last, s.shiftTaps.count >= 2, t - last < 1.1 {
        let p = t - last
        let alpha = p < 0.8 ? ease(p / 0.15) : 1 - ease((p - 0.8) / 0.3)
        let l = line("⇧ ⇧", font(30, .bold), rgb(0xFFFFFF, alpha))
        // To the left of the keyboard, level with the Shift row.
        let bw = width(l) + 28
        let bx = keycaps.first { $0.key == SHIFT }!.frame.minX - bw - 16 - 10 * (1 - easeOutBack(p / 0.3))
        let y = kbY + 3 * (keyH + keyGap) + 33
        rounded(ctx, rect(bx, y - 33, bw, 46), 12); ctx.setFillColor(accent(s.lang).copy(alpha: alpha)!); ctx.fillPath()
        draw(ctx, l, x: bx + 14, baseline: y)
    }
}

func drawEndCard(_ ctx: CGContext, _ p: Double) {
    guard p > 0 else { return }
    let a = ease(p / 0.6)
    let s = 0.85 + 0.15 * easeOutBack(p / 0.7)
    let size: CGFloat = 150 * s
    if let icon {
        ctx.saveGState()
        ctx.setAlpha(a)
        ctx.setShadow(offset: CGSize(width: 0, height: -12), blur: 40, color: rgb(0x000000, 0.6))
        ctx.draw(icon, in: rect((W - size) / 2, 190 + (150 - size) / 2, size, size))
        ctx.restoreGState()
    }
    let title = line("CursorLingo", font(64, .heavy), rgb(0xFFFFFF, a))
    draw(ctx, title, x: (W - width(title)) / 2, baseline: 430)
    let sub = line("Your keyboard language at the pointer, auto-fixed EN ⇄ TH.", font(26, .medium), rgb(0xC9D2F2, a))
    draw(ctx, sub, x: (W - width(sub)) / 2, baseline: 482)
    let tag = line("macOS  ·  Windows  ·  free & open source", font(20, .semibold), rgb(0x8E98C4, ease((p - 0.25) / 0.6)))
    draw(ctx, tag, x: (W - width(tag)) / 2, baseline: 530)
    // Two badges bouncing in.
    for (i, (code, color)) in [("EN", enColor), ("TH", thColor)].enumerated() {
        let q = easeOutBack((p - 0.4 - Double(i) * 0.12) / 0.5)
        guard q > 0 else { continue }
        let bw: CGFloat = 64, bh: CGFloat = 34
        let bx = W / 2 - 76 + CGFloat(i) * 88, by: CGFloat = 572 + 20 * (1 - q)
        rounded(ctx, rect(bx, by, bw, bh), bh / 2); ctx.setFillColor(color.copy(alpha: min(1, q))!); ctx.fillPath()
        let l = line(code, font(18, .bold), rgb(0xFFFFFF, min(1, q)))
        draw(ctx, l, x: bx + (bw - width(l)) / 2, baseline: by + 24)
    }
}

// MARK: - Output

func makeContext(_ w: Int, _ h: Int) -> CGContext {
    let c = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                      space: CGColorSpace(name: CGColorSpace.sRGB)!,
                      bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
    c.setShouldSmoothFonts(false)
    c.interpolationQuality = .high
    return c
}

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
    for s in args[i + 1].split(separator: ",") {
        writePNG(frame(Double(s)!), "\(dir)/preview-\(s).png")
    }
    print("timeline \(String(format: "%.1f", TOTAL))s")
    exit(0)
}

// MP4 (H.264, 30 fps).
let mp4 = URL(fileURLWithPath: "assets/demo.mp4")
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

// GIF (15 fps, 900 px wide).
let gifW = 900, gifH = Int(Double(gifW) * Double(H / W))
let n = Int(TOTAL * FPS)
let gif = CGImageDestinationCreateWithURL(URL(fileURLWithPath: "assets/demo.gif") as CFURL, UTType.gif.identifier as CFString, n / 2 + 1, nil)!
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
        let last = i + 2 >= n
        CGImageDestinationAddImage(gif, small.makeImage()!,
                                   [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFDelayTime: last ? 3.0 : 2 / FPS]] as CFDictionary)
    }
}
input.markAsFinished()
let done = DispatchSemaphore(value: 0)
writer.finishWriting { done.signal() }
done.wait()
CGImageDestinationFinalize(gif)
print("rendered \(n) frames (\(String(format: "%.1f", TOTAL))s): assets/demo.mp4, assets/demo.gif")
