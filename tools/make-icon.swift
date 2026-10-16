// Generates the CursorLingo app icon (vector, CoreGraphics) as:
//   assets/AppIcon.png  (1024 px master)
//   assets/AppIcon.icns (macOS)
//   assets/AppIcon.ico  (Windows, PNG-compressed entries 16…256)
//
// Usage:  swift tools/make-icon.swift            (run from the repo root)
// To use an AI-generated master instead, put a 1024×1024 PNG at assets/AppIcon.png and
// run with --from-png; only the .icns / .ico packaging step is performed.

import AppKit

let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
let assets = root.appendingPathComponent("assets")
let fromPNG = CommandLine.arguments.contains("--from-png")

func drawIcon(_ ctx: CGContext, _ s: CGFloat) {
    // Background: rounded square with a deep indigo → blue gradient.
    let inset = s * 0.09
    let rect = CGRect(x: inset, y: inset, width: s - 2 * inset, height: s - 2 * inset)
    let bg = CGPath(roundedRect: rect, cornerWidth: rect.width * 0.225, cornerHeight: rect.width * 0.225, transform: nil)

    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -s * 0.012), blur: s * 0.03, color: CGColor(gray: 0, alpha: 0.35))
    ctx.addPath(bg); ctx.setFillColor(CGColor(gray: 0, alpha: 1)); ctx.fillPath()
    ctx.restoreGState()

    ctx.saveGState()
    ctx.addPath(bg); ctx.clip()
    let grad = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB),
                          colors: [CGColor(srgbRed: 0.33, green: 0.24, blue: 0.86, alpha: 1),
                                   CGColor(srgbRed: 0.12, green: 0.52, blue: 0.98, alpha: 1)] as CFArray,
                          locations: [0, 1])!
    ctx.drawLinearGradient(grad, start: CGPoint(x: rect.minX, y: rect.maxY), end: CGPoint(x: rect.maxX, y: rect.minY), options: [])
    // Soft top highlight.
    let hi = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB),
                        colors: [CGColor(gray: 1, alpha: 0.22), CGColor(gray: 1, alpha: 0)] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(hi, start: CGPoint(x: 0, y: rect.maxY), end: CGPoint(x: 0, y: rect.midY), options: [])
    ctx.restoreGState()

    // Mouse pointer (classic arrow), tip toward the upper-left.
    let u = s / 1024
    let tip = CGPoint(x: 250 * u, y: 790 * u)
    let pts: [(CGFloat, CGFloat)] = [(0, 0), (0, -430), (100, -335), (165, -480), (235, -450), (172, -308), (300, -308)]
    let arrow = CGMutablePath()
    arrow.move(to: CGPoint(x: tip.x, y: tip.y))
    for p in pts.dropFirst() { arrow.addLine(to: CGPoint(x: tip.x + p.0 * u, y: tip.y + p.1 * u)) }
    arrow.closeSubpath()

    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -10 * u), blur: 24 * u, color: CGColor(gray: 0, alpha: 0.35))
    ctx.addPath(arrow); ctx.setFillColor(CGColor(gray: 1, alpha: 1)); ctx.fillPath()
    ctx.restoreGState()
    ctx.addPath(arrow); ctx.setStrokeColor(CGColor(gray: 0.08, alpha: 1)); ctx.setLineWidth(22 * u); ctx.setLineJoin(.round)
    ctx.strokePath()
    ctx.addPath(arrow); ctx.setFillColor(CGColor(gray: 1, alpha: 1)); ctx.fillPath()

    // Language badge at the pointer — the app's whole idea in one glance.
    let pill = CGRect(x: 450 * u, y: 225 * u, width: 400 * u, height: 220 * u)
    let pillPath = CGPath(roundedRect: pill, cornerWidth: pill.height / 2, cornerHeight: pill.height / 2, transform: nil)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -12 * u), blur: 30 * u, color: CGColor(gray: 0, alpha: 0.4))
    ctx.addPath(pillPath); ctx.setFillColor(CGColor(srgbRed: 0.20, green: 0.78, blue: 0.45, alpha: 1)); ctx.fillPath()
    ctx.restoreGState()
    ctx.addPath(pillPath); ctx.setStrokeColor(CGColor(gray: 1, alpha: 0.95)); ctx.setLineWidth(16 * u); ctx.strokePath()

    let ns = NSGraphicsContext(cgContext: ctx, flipped: false)
    NSGraphicsContext.saveGraphicsState(); NSGraphicsContext.current = ns
    let attrs: [NSAttributedString.Key: Any] = [
        .font: NSFont.systemFont(ofSize: 150 * u, weight: .heavy),
        .foregroundColor: NSColor.white, .kern: 4 * u,
    ]
    let text = "Aก" as NSString
    let ts = text.size(withAttributes: attrs)
    text.draw(at: CGPoint(x: pill.midX - ts.width / 2, y: pill.midY - ts.height / 2 + 4 * u), withAttributes: attrs)
    NSGraphicsContext.restoreGraphicsState()
}

func render(_ size: Int, master: CGImage?) -> Data {
    let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.interpolationQuality = .high
    if let master { ctx.draw(master, in: CGRect(x: 0, y: 0, width: size, height: size)) }
    else { drawIcon(ctx, CGFloat(size)) }
    return NSBitmapImageRep(cgImage: ctx.makeImage()!).representation(using: .png, properties: [:])!
}

var master: CGImage? = nil
if fromPNG {
    guard let img = NSImage(contentsOf: assets.appendingPathComponent("AppIcon.png"))?
        .cgImage(forProposedRect: nil, context: nil, hints: nil) else { fatalError("assets/AppIcon.png missing") }
    master = img
} else {
    try! render(1024, master: nil).write(to: assets.appendingPathComponent("AppIcon.png"))
}

// macOS .icns via iconutil
let iconset = FileManager.default.temporaryDirectory.appendingPathComponent("AppIcon.iconset")
try? FileManager.default.removeItem(at: iconset)
try! FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
for base in [16, 32, 128, 256, 512] {
    try! render(base, master: master).write(to: iconset.appendingPathComponent("icon_\(base)x\(base).png"))
    try! render(base * 2, master: master).write(to: iconset.appendingPathComponent("icon_\(base)x\(base)@2x.png"))
}
let p = Process()
p.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
p.arguments = ["-c", "icns", iconset.path, "-o", assets.appendingPathComponent("AppIcon.icns").path]
try! p.run(); p.waitUntilExit()

// Windows .ico with PNG entries
let sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
let pngs = sizes.map { render($0, master: master) }
var ico = Data()
func le16(_ v: Int) { var x = UInt16(v).littleEndian; ico.append(Data(bytes: &x, count: 2)) }
func le32(_ v: Int) { var x = UInt32(v).littleEndian; ico.append(Data(bytes: &x, count: 4)) }
le16(0); le16(1); le16(sizes.count)
var offset = 6 + 16 * sizes.count
for (i, sz) in sizes.enumerated() {
    ico.append(UInt8(sz >= 256 ? 0 : sz)); ico.append(UInt8(sz >= 256 ? 0 : sz))
    ico.append(0); ico.append(0)
    le16(1); le16(32); le32(pngs[i].count); le32(offset)
    offset += pngs[i].count
}
pngs.forEach { ico.append($0) }
try! ico.write(to: assets.appendingPathComponent("AppIcon.ico"))
print("Wrote assets/AppIcon.{png,icns,ico}")
