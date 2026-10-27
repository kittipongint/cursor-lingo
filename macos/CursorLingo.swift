// CursorLingo — shows the current keyboard input language next to the mouse pointer.
//
// Design goals:
//   * Event-driven: no polling timers. The badge moves on mouse events and redraws
//     only when the input source changes.
//   * Tiny footprint: one borderless click-through panel, one status item, no network.
//   * Keyboard monitoring exists only for the optional "Fix Wrong Language" feature. It is
//     off by default, needs explicit macOS permission, and keeps nothing but the word being typed.
//   * Optional launch at login (SMAppService, with a LaunchAgent fallback).

import AppKit
import Carbon
import ScreenCaptureKit
import ServiceManagement

// MARK: - Menu language

/// The menu speaks English or Thai: the system's language by default, or what the user picked.
enum UILanguage: String, CaseIterable {
    case system = "System", english = "English", thai = "Thai"
    var title: String { self == .system ? tr("System", "ตามภาษาของระบบ") : self == .english ? "English" : "ไทย" }
}

/// The English or the Thai text, whichever the menu currently speaks.
func tr(_ en: String, _ th: String) -> String {
    switch Settings.uiLanguage {
    case .english: return en
    case .thai: return th
    case .system: return Locale.preferredLanguages.first?.hasPrefix("th") == true ? th : en
    }
}

// MARK: - Settings

// Raw values are what is stored in the preferences; `title` is what the menu shows.

enum BadgeStyle: String, CaseIterable {
    case color = "Color", light = "Light", dark = "Dark", dot = "Dot"
    var title: String { [.color: tr("Color", "สีสด"), .light: tr("Light", "โทนสว่าง"), .dark: tr("Dark", "โทนมืด"), .dot: tr("Dot", "จุดเล็ก")][self]! }
}

/// Fix Wrong Language: off, only when double Shift asks for it, or also by itself as you type.
enum FixMode: String, CaseIterable {
    case off = "Off", manual = "Double Shift Only", automatic = "Automatic"
    var title: String {
        [.off: tr("Off", "ปิด"), .manual: tr("Double Shift Only", "กด Shift 2 ครั้งเพื่อแก้"),
         .automatic: tr("Automatic", "แก้ให้อัตโนมัติ")][self]!
    }
}

enum DisplayMode: String, CaseIterable {
    case always = "Always"
    case onSwitch = "Only when switching"
    var title: String { self == .always ? tr("Always", "ตลอดเวลา") : tr("Only when switching", "เฉพาะตอนเปลี่ยนภาษา") }
}

enum BadgeSize: String, CaseIterable {
    case small = "Small", medium = "Medium", large = "Large"
    var scale: CGFloat { self == .small ? 0.85 : (self == .large ? 1.3 : 1.0) }
    var title: String { [.small: tr("Small", "เล็ก"), .medium: tr("Medium", "กลาง"), .large: tr("Large", "ใหญ่")][self]! }
}

struct Settings {
    private static let d = UserDefaults.standard

    static var style: BadgeStyle {
        get { BadgeStyle(rawValue: d.string(forKey: "style") ?? "") ?? .color }
        set { d.set(newValue.rawValue, forKey: "style") }
    }
    static var mode: DisplayMode {
        get { DisplayMode(rawValue: d.string(forKey: "mode") ?? "") ?? .always }
        set { d.set(newValue.rawValue, forKey: "mode") }
    }
    static var size: BadgeSize {
        get { BadgeSize(rawValue: d.string(forKey: "size") ?? "") ?? .medium }
        set { d.set(newValue.rawValue, forKey: "size") }
    }
    /// Re-type words that were typed in the wrong layout (EN <-> TH). Off by default.
    static var autoFix: Bool {
        get { d.bool(forKey: "autoFix") }
        set { d.set(newValue, forKey: "autoFix") }
    }
    /// With Auto-fix on: fix words by themselves (true), or only when double Shift asks for it (false).
    static var fixAsYouType: Bool {
        get { d.object(forKey: "fixAsYouType") as? Bool ?? true }
        set { d.set(newValue, forKey: "fixAsYouType") }
    }
    /// In code editors identifiers often look like gibberish, so there only double Shift converts. On by default.
    static var codeEditorsManual: Bool {
        get { d.object(forKey: "codeEditorsManual") as? Bool ?? true }
        set { d.set(newValue, forKey: "codeEditorsManual") }
    }
    /// The Fix Wrong Language menu: stored as autoFix (on/off) + fixAsYouType, so older settings carry over.
    static var fixMode: FixMode { !autoFix ? .off : fixAsYouType ? .automatic : .manual }
    /// The personal word list (see AutoFix.Rule), one entry per word: "<layout><+|->" then the keys in hex.
    static var personalWords: [String] {
        get { d.stringArray(forKey: "personalWords") ?? [] }
        set { d.set(newValue, forKey: "personalWords") }
    }
    /// Switch to the layout each app was last used with when it comes to the front. Off by default.
    static var perAppLanguage: Bool {
        get { d.bool(forKey: "perAppLanguage") }
        set { d.set(newValue, forKey: "perAppLanguage") }
    }
    static var uiLanguage: UILanguage {
        get { UILanguage(rawValue: d.string(forKey: "uiLanguage") ?? "") ?? .system }
        set { d.set(newValue.rawValue, forKey: "uiLanguage") }
    }
    /// Presentation spotlight: the screen dimmed except around the pointer. Off by default.
    static var presentation: Bool {
        get { d.bool(forKey: "presentation") }
        set { d.set(newValue, forKey: "presentation") }
    }
    /// Radius of the presentation spotlight, in points.
    static var spotlightRadius: Double {
        get { d.object(forKey: "spotlightRadius") as? Double ?? 100 }
        set { d.set(newValue, forKey: "spotlightRadius") }
    }
    /// The shortcut of a presentation tool: its default until changed, nil once cleared.
    static func shortcut(_ tool: PresentTool) -> Shortcut? {
        guard let keys = d.array(forKey: "shortcut." + tool.rawValue) as? [Int] else { return tool.defaultShortcut }
        return keys.count == 2 ? Shortcut(keyCode: keys[0], modifiers: keys[1]) : nil
    }
    static func setShortcut(_ s: Shortcut?, for tool: PresentTool) {
        d.set(s.map { [$0.keyCode, $0.modifiers] } ?? [], forKey: "shortcut." + tool.rawValue)
    }
    static func resetShortcuts() {
        for tool in PresentTool.allCases { d.removeObject(forKey: "shortcut." + tool.rawValue) }
    }
    /// Magnification of the presentation zoom lens.
    static var zoomLevel: Double {
        get { d.object(forKey: "zoomLevel") as? Double ?? 2 }
        set { d.set(newValue, forKey: "zoomLevel") }
    }
    /// Bundle ID -> input source ID it was last used with (for perAppLanguage).
    static var appLanguages: [String: String] {
        get { d.dictionary(forKey: "appLanguages") as? [String: String] ?? [:] }
        set { d.set(newValue, forKey: "appLanguages") }
    }
    /// The thin outline around the badge. On by default.
    static var badgeBorder: Bool {
        get { d.object(forKey: "badgeBorder") as? Bool ?? true }
        set { d.set(newValue, forKey: "badgeBorder") }
    }
    /// Badge colours picked in Settings: language code -> "#RRGGBB".
    static var badgeColors: [String: String] {
        get { d.dictionary(forKey: "badgeColors") as? [String: String] ?? [:] }
        set { d.set(newValue, forKey: "badgeColors") }
    }
    /// Language codes (e.g. "EN") for which the badge is hidden.
    static var hidden: Set<String> {
        get { Set(d.stringArray(forKey: "hidden") ?? []) }
        set { d.set(Array(newValue).sorted(), forKey: "hidden") }
    }
}

// MARK: - Input source

enum InputSource {
    /// Short uppercase code for the selected keyboard input source, e.g. "EN", "TH", "JA".
    static func currentCode() -> String {
        guard let source = TISCopyCurrentKeyboardInputSource()?.takeRetainedValue() else { return "??" }
        return code(of: source)
    }

    static func code(of source: TISInputSource) -> String {
        if let raw = TISGetInputSourceProperty(source, kTISPropertyInputSourceLanguages),
           let langs = Unmanaged<CFArray>.fromOpaque(raw).takeUnretainedValue() as? [String],
           let first = langs.first, !first.isEmpty {
            let base = first.split(separator: "-").first.map(String.init) ?? first
            return String(base.prefix(3)).uppercased()
        }
        if let raw = TISGetInputSourceProperty(source, kTISPropertyInputSourceID) {
            let id = Unmanaged<CFString>.fromOpaque(raw).takeUnretainedValue() as String
            if id.localizedCaseInsensitiveContains("thai") { return "TH" }
        }
        return "??"
    }

    /// The keyboards added in System Settings, as badge codes with the names of the layouts behind each one.
    static func installedLanguages() -> [(code: String, layouts: [String])] {
        let filter = [kTISPropertyInputSourceCategory as String: kTISCategoryKeyboardInputSource as String,
                      kTISPropertyInputSourceIsSelectCapable as String: true] as CFDictionary
        guard let list = TISCreateInputSourceList(filter, false)?.takeRetainedValue() as? [TISInputSource] else { return [] }
        var out: [(code: String, layouts: [String])] = []
        for source in list {
            let code = code(of: source)
            var name = code
            if let raw = TISGetInputSourceProperty(source, kTISPropertyLocalizedName) {
                name = Unmanaged<CFString>.fromOpaque(raw).takeUnretainedValue() as String
            }
            if let i = out.firstIndex(where: { $0.code == code }) {
                if !out[i].layouts.contains(name) { out[i].layouts.append(name) }
            } else {
                out.append((code, [name]))
            }
        }
        return out
    }
}

extension InputSource {
    private static let enIDs = ["com.apple.keylayout.ABC", "com.apple.keylayout.US", "com.apple.keylayout.USExtended"]
    private static let thID = "com.apple.keylayout.Thai"

    static func currentID() -> String? {
        guard let source = TISCopyCurrentKeyboardInputSource()?.takeRetainedValue(),
              let raw = TISGetInputSourceProperty(source, kTISPropertyInputSourceID) else { return nil }
        return Unmanaged<CFString>.fromOpaque(raw).takeUnretainedValue() as String
    }

    /// The layout the auto-fixer understands: 0 = US/ABC, 1 = Thai (Kedmanee), nil = anything else.
    static func fixLanguage() -> Int? {
        guard let id = currentID() else { return nil }
        if enIDs.contains(id) { return 0 }
        return id == thID ? 1 : nil
    }

    /// Selects an enabled US/ABC (0) or Thai (1) layout.
    static func select(_ language: Int) {
        for id in language == 0 ? enIDs : [thID] where select(id: id) { return }
    }

    /// Selects the enabled input source with this ID; false if there is none.
    @discardableResult
    static func select(id: String) -> Bool {
        guard let list = TISCreateInputSourceList(nil, false)?.takeRetainedValue() as? [TISInputSource] else { return false }
        for source in list {
            guard let raw = TISGetInputSourceProperty(source, kTISPropertyInputSourceID),
                  (Unmanaged<CFString>.fromOpaque(raw).takeUnretainedValue() as String) == id else { continue }
            return TISSelectInputSource(source) == noErr
        }
        return false
    }
}

// MARK: - Palette

extension NSColor {
    /// "#RRGGBB" in sRGB.
    var hex: String {
        let c = usingColorSpace(.sRGB) ?? self
        let v = [c.redComponent, c.greenComponent, c.blueComponent].map { Int(($0 * 255).rounded()) }
        return String(format: "#%02X%02X%02X", v[0], v[1], v[2])
    }
    convenience init?(hex: String) {
        guard hex.hasPrefix("#"), hex.count == 7, let v = UInt32(hex.dropFirst(), radix: 16) else { return nil }
        self.init(srgbRed: CGFloat(v >> 16 & 0xFF) / 255, green: CGFloat(v >> 8 & 0xFF) / 255, blue: CGFloat(v & 0xFF) / 255, alpha: 1)
    }
}

enum Palette {
    private static let known: [String: NSColor] = [
        "EN": NSColor(srgbRed: 0.20, green: 0.68, blue: 0.40, alpha: 1),
        "TH": NSColor(srgbRed: 0.17, green: 0.45, blue: 0.95, alpha: 1),
        "JA": NSColor(srgbRed: 0.90, green: 0.25, blue: 0.30, alpha: 1),
        "ZH": NSColor(srgbRed: 0.95, green: 0.55, blue: 0.10, alpha: 1),
        "KO": NSColor(srgbRed: 0.58, green: 0.35, blue: 0.90, alpha: 1),
        "RU": NSColor(srgbRed: 0.10, green: 0.65, blue: 0.75, alpha: 1),
        "VI": NSColor(srgbRed: 0.85, green: 0.35, blue: 0.65, alpha: 1),
        "??": NSColor.systemGray,
    ]

    /// The colour picked in Settings, or the built-in one.
    static func color(for code: String) -> NSColor {
        Settings.badgeColors[code].flatMap(NSColor.init(hex:)) ?? defaultColor(for: code)
    }
    static func hasCustomColor(for code: String) -> Bool { Settings.badgeColors[code] != nil }
    static func setColor(_ color: NSColor?, for code: String) {
        var colors = Settings.badgeColors
        colors[code] = color?.hex
        Settings.badgeColors = colors
    }

    static func defaultColor(for code: String) -> NSColor {
        if let c = known[code] { return c }
        // Stable hue for any other language.
        let hash = code.unicodeScalars.reduce(UInt32(5381)) { ($0 &* 33) &+ $1.value }
        return NSColor(hue: CGFloat(hash % 360) / 360, saturation: 0.6, brightness: 0.85, alpha: 1)
    }
}

// MARK: - Badge view

final class BadgeView: NSView {
    var code = "EN" { didSet { if code != oldValue { needsDisplay = true } } }
    var style = Settings.style { didSet { needsDisplay = true } }
    var scale = Settings.size.scale { didSet { needsDisplay = true } }

    override var isOpaque: Bool { false }

    static func size(style: BadgeStyle, scale: CGFloat, code: String) -> NSSize {
        switch style {
        case .dot: return NSSize(width: 12 * scale, height: 12 * scale)
        default:
            let w: CGFloat = code.count > 2 ? 30 : 24
            return NSSize(width: (w * scale).rounded(), height: (17 * scale).rounded())
        }
    }

    override func draw(_ dirtyRect: NSRect) {
        let b = bounds
        let accent = Palette.color(for: code)

        if style == .dot {
            let r = b.insetBy(dx: 1.5 * scale, dy: 1.5 * scale)
            if Settings.badgeBorder {
                NSColor.white.withAlphaComponent(0.9).setFill()
                NSBezierPath(ovalIn: r.insetBy(dx: -1, dy: -1)).fill()
            }
            accent.setFill()
            NSBezierPath(ovalIn: r).fill()
            return
        }

        let pill = NSBezierPath(roundedRect: b.insetBy(dx: 0.5, dy: 0.5),
                                xRadius: b.height / 2 - 0.5, yRadius: b.height / 2 - 0.5)
        let fill: NSColor, text: NSColor
        switch style {
        case .light: fill = NSColor(white: 1, alpha: 0.95); text = accent.blended(withFraction: 0.25, of: .black) ?? accent
        case .dark:  fill = NSColor(white: 0.1, alpha: 0.92); text = .white
        default:     fill = accent; text = .white
        }
        fill.setFill()
        pill.fill()

        // Hairline border keeps the badge readable on any background (unless turned off in Settings).
        if Settings.badgeBorder {
            (style == .dark ? accent : NSColor.black.withAlphaComponent(0.18)).setStroke()
            pill.lineWidth = style == .dark ? 1 : 0.5
            pill.stroke()
        }

        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 10 * scale, weight: .bold),
            .foregroundColor: text,
            .kern: 0.3,
        ]
        let s = code as NSString
        let ts = s.size(withAttributes: attrs)
        s.draw(at: NSPoint(x: (b.width - ts.width) / 2, y: (b.height - ts.height) / 2), withAttributes: attrs)
    }
}

// MARK: - Auto-fix wrong language

/// Watches typing (listen-only event tap) and, at the end of a word, re-types it in the other layout when the
/// detector in core/layoutfix.h says it was typed with the wrong one. Double-tapping Shift converts the current or
/// last word by hand, and undoes an automatic fix (that word is then left alone for the rest of the session).
/// With no word to work on, double Shift converts the selected text instead.
/// Only the current and the last word are kept, in memory; nothing is logged or stored, and nothing is observed
/// while secure input (password fields) is on.
final class AutoFix {
    private static let marker: Int64 = 0x434C47  // tags our own synthetic events so we never react to them
    private static let terminals: Set<String> = [
        "com.apple.Terminal", "com.googlecode.iterm2", "com.mitchellh.ghostty", "org.alacritty",
        "com.github.wez.wezterm", "net.kovidgoyal.kitty", "dev.warp.Warp-Stable", "co.zeit.hyper",
    ]
    private static let codeEditors: Set<String> = [
        "com.microsoft.VSCode", "com.microsoft.VSCodeInsiders", "com.vscodium", "com.todesktop.230313mzl4w4u92",
        "com.exafunction.windsurf", "dev.zed.Zed", "dev.zed.Zed-Preview", "com.apple.dt.Xcode", "com.sublimetext.4",
        "com.sublimetext.3", "com.panic.Nova", "com.google.android.studio", "org.vim.MacVim",
    ]
    private static func isCodeEditor(_ id: String) -> Bool { codeEditors.contains(id) || id.hasPrefix("com.jetbrains.") }
    /// The apps above by name, for Settings (keep in step with `codeEditors` and `terminals`).
    static let codeEditorNames = "VS Code, VSCodium, Cursor, Windsurf, Zed, Xcode, Sublime Text, Nova, Android Studio, MacVim, "
        + "JetBrains IDEs (IntelliJ IDEA, PyCharm, WebStorm…)"
    static let terminalNames = "Terminal, iTerm2, Ghostty, Alacritty, WezTerm, kitty, Warp, Hyper"

    /// Fix words by themselves; when false only double Shift converts.
    var fixAsYouType = true
    /// Leave code editors to double Shift.
    var codeEditorsManual = true
    private var frontApp = ""            // bundle ID of the app being typed in
    private var autoAllowed: Bool {
        fixAsYouType && !AutoFix.terminals.contains(frontApp) && !(codeEditorsManual && AutoFix.isCodeEditor(frontApp))
    }

    /// A word on screen: its physical keys, the layout it is shown in, the layout it was typed in, and how many
    /// spaces follow it. `decided` stops the detector from touching it again (fixed or toggled already).
    private struct Word {
        var keys: [UInt8] = []
        var shown = 0
        var typed = 0
        var trailing = 0
        var decided = false
    }

    private var tap: CFMachPort?
    private var runLoopSource: CFRunLoopSource?
    private var word = Word()            // being typed
    private var last: Word?              // finished, and nothing but spaces typed after it
    /// A word the user decided about with double Shift: in layout `shown`, always convert it (`fix`), or never.
    struct Rule: Equatable {
        var keys: [UInt8]
        var shown: Int
        var fix: Bool
    }
    private(set) var rules: [Rule] = []  // the personal word list; see learn(_:)
    private var language: Int?           // layout currently on screen: 0 = EN, 1 = TH, nil = unsupported
    private var expectedLanguage: Int?   // a layout switch we made ourselves: it must not end the word
    private var shiftDownAt: TimeInterval = 0
    private var shiftClean = false       // no other key since Shift went down
    private var lastShiftTap: TimeInterval = 0
    private let source = CGEventSource(stateID: .privateState)

    var isRunning: Bool { tap != nil }

    static var hasPermission: Bool { CGPreflightListenEventAccess() && CGPreflightPostEventAccess() }
    static func requestPermission() {
        if !CGPreflightListenEventAccess() { _ = CGRequestListenEventAccess() }
        if !CGPreflightPostEventAccess() { _ = CGRequestPostEventAccess() }
    }

    enum Problem { case permission, notRunning }

    /// What the menu shows under Fix Wrong Language, so a silent failure is never invisible.
    var status: (text: String, problem: Problem?) {
        if !CGPreflightListenEventAccess() {
            return (tr("Needs Input Monitoring permission", "ยังไม่ได้อนุญาต Input Monitoring"), .permission)
        }
        if !CGPreflightPostEventAccess() { return (tr("Needs Accessibility permission", "ยังไม่ได้อนุญาต Accessibility"), .permission) }
        if tap == nil { return (tr("Not running yet: relaunch CursorLingo", "ยังไม่เริ่มทำงาน ลองเปิด CursorLingo ใหม่"), .notRunning) }
        if IsSecureEventInputEnabled() {
            return (tr("Paused: secure input (a password field) is on", "พักไว้ระหว่างพิมพ์รหัสผ่าน"), nil)
        }
        if InputSource.fixLanguage() == nil { return (tr("Paused: this layout isn't supported", "พักไว้: ยังไม่รองรับแป้นพิมพ์นี้"), nil) }
        if !fixAsYouType {
            return (tr("Active · double-tap Shift converts a word or selection",
                       "พร้อมใช้ · กด Shift 2 ครั้งเพื่อแก้คำหรือข้อความที่เลือก"), nil)
        }
        return (tr("Active · double-tap Shift: convert word / selection, or undo",
                   "พร้อมใช้ · กด Shift 2 ครั้งเพื่อแก้คำ ข้อความที่เลือก หรือย้อนคืน"), nil)
    }

    @discardableResult
    func start() -> Bool {
        if tap != nil { return true }
        guard AutoFix.hasPermission else { return false }
        let types: [CGEventType] = [.keyDown, .flagsChanged, .leftMouseDown, .rightMouseDown, .otherMouseDown]
        let mask = types.reduce(CGEventMask(0)) { $0 | (CGEventMask(1) << $1.rawValue) }
        let callback: CGEventTapCallBack = { _, type, event, refcon in
            if let refcon { Unmanaged<AutoFix>.fromOpaque(refcon).takeUnretainedValue().handle(type, event) }
            return Unmanaged.passUnretained(event)
        }
        guard let port = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap, options: .listenOnly,
                                           eventsOfInterest: mask, callback: callback,
                                           userInfo: Unmanaged.passUnretained(self).toOpaque()) else { return false }
        tap = port
        runLoopSource = CFMachPortCreateRunLoopSource(nil, port, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), runLoopSource, .commonModes)
        inputChanged()
        return true
    }

    func stop() {
        if let runLoopSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), runLoopSource, .commonModes) }
        if let tap { CGEvent.tapEnable(tap: tap, enable: false) }
        tap = nil
        runLoopSource = nil
        reset()
    }

    /// The input source changed. A word never spans a switch the user made; ours are expected.
    func inputChanged() {
        let now = InputSource.fixLanguage()
        if let expected = expectedLanguage, now == expected {
            expectedLanguage = nil
            language = now
            return
        }
        reset()
        language = now
    }

    /// The caret may have moved (another app, a click, a shortcut): forget both words.
    func reset() {
        word = Word()
        last = nil
        lastShiftTap = 0
        frontApp = NSWorkspace.shared.frontmostApplication?.bundleIdentifier ?? ""
    }

    /// After sleep, a screen lock or a user switch: make sure the tap still listens, and start from a clean slate.
    func recover() {
        guard let port = tap else { return }
        if !CFMachPortIsValid(port) {
            stop()
            start()
            return
        }
        if !CGEvent.tapIsEnabled(tap: port) { CGEvent.tapEnable(tap: port, enable: true) }
        shiftClean = false
        inputChanged()
        reset()
    }

    private func handle(_ type: CGEventType, _ event: CGEvent) {
        switch type {
        case .tapDisabledByTimeout, .tapDisabledByUserInput:  // macOS switched the tap off: switch it back on
            if let tap { CGEvent.tapEnable(tap: tap, enable: true) }
            reset()
            return
        case .keyDown, .flagsChanged:
            break
        default:
            reset()  // a click moves the caret
            return
        }
        if event.getIntegerValueField(.eventSourceUserData) == AutoFix.marker { return }
        guard let language, !IsSecureEventInputEnabled() else { reset(); return }
        let code = Int(event.getIntegerValueField(.keyboardEventKeycode))
        let flags = event.flags

        if type == .flagsChanged {
            guard code == 56 || code == 60 else { shiftClean = false; return }  // left / right Shift only
            let now = ProcessInfo.processInfo.systemUptime
            if flags.contains(.maskShift) {
                shiftDownAt = now
                shiftClean = true
            } else if shiftClean, now - shiftDownAt < 0.35 {  // a tap, not a hold
                if now - lastShiftTap < 0.45 {
                    lastShiftTap = 0
                    toggle()
                } else {
                    lastShiftTap = now
                }
            }
            return
        }

        shiftClean = false
        lastShiftTap = 0
        if !flags.isDisjoint(with: [.maskCommand, .maskControl, .maskAlternate]) { reset(); return }

        switch code {
        case 51:  // delete
            if word.keys.isEmpty { last = nil } else { word.keys.removeLast() }
            return
        case 49:  // space
            if word.keys.isEmpty { last?.trailing += 1 } else { endOfWord() }
            return
        default: break
        }
        let index = lf_key_from_mac_keycode(Int32(code))
        guard index >= 0, word.keys.count < 48 else { reset(); return }  // return, tab, arrows, ...
        if word.keys.isEmpty {
            last = nil
            word = Word(shown: language, typed: language)
        }
        word.keys.append(UInt8(index) | (flags.contains(.maskShift) ? 0x80 : 0))

        // A long run with no spaces (typical for Thai) is checked as it grows, so it is fixed early.
        if !word.decided, word.shown == 0, word.keys.count >= 6, autoAllowed, !isKept(word, prefix: true),
           lf_should_convert(word.keys, Int32(word.keys.count), 0, false) {
            convert(&word)
        }
    }

    private func endOfWord() {
        var w = word
        w.trailing = 1
        word = Word()
        if !w.decided, w.keys.count >= 2, autoAllowed, !isKept(w, prefix: false),
           rules.contains(Rule(keys: w.keys, shown: w.shown, fix: true))
            || lf_should_convert(w.keys, Int32(w.keys.count), Int32(w.shown), true) {
            convert(&w)
        }
        last = w
    }

    /// Double Shift: flip the word being typed, or else the last word, and learn from it (see learn(_:)).
    private func toggle() {
        if !word.keys.isEmpty {
            convert(&word)
            learn(word)
        } else if var w = last {
            convert(&w)
            learn(w)
            last = w
        } else {
            convertSelection()
        }
    }

    // MARK: Selection

    private enum Selection { case text(String), none, unknown }

    /// Asks the focused text field for its selection through Accessibility (no clipboard involved).
    private static func accessibilitySelection() -> Selection {
        let system = AXUIElementCreateSystemWide()
        AXUIElementSetMessagingTimeout(system, 0.25)
        var focused: CFTypeRef?
        guard AXUIElementCopyAttributeValue(system, kAXFocusedUIElementAttribute as CFString, &focused) == .success,
              let focused, CFGetTypeID(focused) == AXUIElementGetTypeID() else { return .unknown }
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(focused as! AXUIElement, kAXSelectedTextAttribute as CFString, &value) == .success
        else { return .unknown }
        guard let text = value as? String, !text.isEmpty else { return .none }
        return .text(text)
    }

    private func convertSelection() {
        guard !AutoFix.terminals.contains(frontApp) else { return }
        switch AutoFix.accessibilitySelection() {
        case .text(let text): replaceSelection(text)
        case .none: return
        case .unknown:  // the app doesn't expose its selection: copy it, then put the clipboard back
            withClipboardSaved { restore in
                let pb = NSPasteboard.general
                let before = pb.changeCount
                self.post(keyCode: 8, flags: .maskCommand)  // ⌘C
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) {
                    let text = pb.changeCount != before ? pb.string(forType: .string) : nil
                    restore()
                    // Editors copy the whole line ("...\n") when nothing is selected: never act on that.
                    if let text, !text.hasSuffix("\n") { self.replaceSelection(text) }
                }
            }
        }
    }

    /// Types (or, for several lines, pastes) the other layout's reading of `text` over the selection.
    private func replaceSelection(_ text: String) {
        let units = Array(text.utf16)
        guard !units.isEmpty, units.count <= 2000 else { return }
        var out = [UInt16](repeating: 0, count: units.count)
        let target = Int(lf_convert_text(units, Int32(units.count), &out))
        guard target >= 0 else { return }
        if language != target { expectedLanguage = target }
        InputSource.select(target)

        if out.contains(0x0A) || out.contains(0x0D) {
            // Typing a new line could press Return (and send a chat message): paste instead.
            withClipboardSaved { restore in
                let pb = NSPasteboard.general
                pb.clearContents()
                pb.setString(String(utf16CodeUnits: out, count: out.count), forType: .string)
                self.post(keyCode: 9, flags: .maskCommand)  // ⌘V
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { restore() }
            }
        } else {
            for unit in out { post(keyCode: 0, unicode: unit) }  // the first character replaces the selection
        }
        // The converted text is now "the last word" if it is one, so double Shift flips it straight back.
        let keys = out.map { lf_key_for_char($0, Int32(target)) }
        if !keys.contains(-1) {
            last = Word(keys: keys.map { UInt8($0) }, shown: target, typed: 1 - target, trailing: 0, decided: true)
        }
    }

    /// Runs `body` with a snapshot of the clipboard; `body` calls the closure it gets to put the snapshot back.
    private func withClipboardSaved(_ body: (@escaping () -> Void) -> Void) {
        let pb = NSPasteboard.general
        let saved = (pb.pasteboardItems ?? []).map { item in
            item.types.compactMap { type in item.data(forType: type).map { (type, $0) } }
        }
        body {
            pb.clearContents()
            let items = saved.map { pairs -> NSPasteboardItem in
                let item = NSPasteboardItem()
                for (type, data) in pairs { item.setData(data, forType: type) }
                return item
            }
            if !items.isEmpty { pb.writeObjects(items) }
        }
    }

    // MARK: Personal word list

    /// Should the word (or, mid-word, a word starting like it) be left as typed?
    private func isKept(_ w: Word, prefix: Bool) -> Bool {
        rules.contains { !$0.fix && $0.shown == w.shown && (prefix ? $0.keys.starts(with: w.keys) : $0.keys == w.keys) }
    }

    /// A word the user just flipped with double Shift. Back to the layout it was typed in: they want it as typed,
    /// so it is never fixed again. Into the other layout: fix it by itself from now on.
    private func learn(_ w: Word) {
        let typedRule = Rule(keys: w.keys, shown: w.typed, fix: w.shown != w.typed)
        if typedRule.fix && w.keys.count < 2 { return }
        rules.removeAll { $0.keys == w.keys && $0.shown == w.typed }
        rules.append(typedRule)
        if rules.count > 500 { rules.removeFirst() }
        save()
    }

    func loadRules() {
        rules = Settings.personalWords.compactMap { entry in
            let c = Array(entry.utf8)
            guard c.count >= 4, c.count % 2 == 0, c[0] == 48 || c[0] == 49, c[1] == 43 || c[1] == 45 else { return nil }
            var keys: [UInt8] = []
            for i in stride(from: 2, to: c.count, by: 2) {
                // A damaged entry must not feed bad key indices to the detector (47 keys, plus the Shift bit).
                guard let k = UInt8(String(decoding: c[i...i+1], as: UTF8.self), radix: 16), k & 0x7F < 47 else { return nil }
                keys.append(k)
            }
            return Rule(keys: keys, shown: Int(c[0] - 48), fix: c[1] == 43)
        }
    }

    private func save() {
        Settings.personalWords = rules.map { r in
            "\(r.shown)\(r.fix ? "+" : "-")" + r.keys.map { String(format: "%02x", $0) }.joined()
        }
    }

    func forgetRules() {
        rules = []
        save()
    }

    /// Replaces the word on screen (and the spaces after it) with the other layout's reading of the same keys,
    /// and switches to that layout.
    private func convert(_ w: inout Word) {
        let target = 1 - w.shown
        var buffer = [UInt16](repeating: 0, count: w.keys.count + 1)
        _ = lf_render(w.keys, Int32(w.keys.count), Int32(target), &buffer)
        let text = Array(buffer.prefix(w.keys.count)) + Array(repeating: UInt16(0x20), count: w.trailing)
        let erase = w.keys.count + w.trailing
        w.shown = target
        w.decided = true
        if language != target { expectedLanguage = target }

        DispatchQueue.main.async { [self] in
            InputSource.select(target)   // first, so keys typed meanwhile already land in the right layout
            for _ in 0..<erase { post(keyCode: 51) }
            for unit in text { post(keyCode: 0, unicode: unit) }
        }
    }

    private func post(keyCode: CGKeyCode, unicode: UInt16? = nil, flags: CGEventFlags = []) {
        for down in [true, false] {
            guard let e = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: down) else { continue }
            e.setIntegerValueField(.eventSourceUserData, value: AutoFix.marker)
            e.flags = flags
            if var u = unicode { e.keyboardSetUnicodeString(stringLength: 1, unicodeString: &u) }
            e.post(tap: .cghidEventTap)
        }
    }
}

// MARK: - Presentation mode

/// Tools for screen sharing and recordings. Each can be used alone or together:
///   * Spotlight (after Hazyzh/cursor-highlight): the screen is dimmed except for a circle around the pointer,
///     ringed by a slowly turning rainbow. It stays click-through, so you keep working underneath.
///   * Zoom: the circle shows what is under the pointer, magnified. The only tool that needs a permission
///     (Screen Recording, asked for the first time it is turned on), since it has to see the screen.
///   * Draw: pen strokes over everything until you leave; while drawing, clicks go to the pen.
/// Every click sends out a ripple in the current language's colour. ⌃⌥ + scroll resizes the circle, ⌃⌥ = / −
/// zoom in and out, ⌘Z takes back the last stroke, Esc leaves drawing first, then everything else.
final class Presenter {
    static let minRadius: CGFloat = 40, maxRadius: CGFloat = 300
    static let minZoom: CGFloat = 1.5, maxZoom: CGFloat = 8
    private static let dimColor = NSColor(srgbRed: 0.27, green: 0.34, blue: 0.38, alpha: 0.6).cgColor
    private static let rainbow: [CGColor] = [
        NSColor.cyan, NSColor(srgbRed: 0.70, green: 1, blue: 0.53, alpha: 1), NSColor(srgbRed: 0.91, green: 1, blue: 0.31, alpha: 1),
        NSColor(srgbRed: 1, green: 0.31, blue: 0.31, alpha: 1), NSColor(srgbRed: 0.04, green: 0.96, blue: 0.48, alpha: 1),
        NSColor.white, NSColor.cyan,
    ].map(\.cgColor)
    private static let rippleRadius: CGFloat = 36
    /// Zoom is built on ScreenCaptureKit, which arrived in macOS 12.3.
    static var zoomAvailable: Bool { if #available(macOS 12.3, *) { return true } else { return false } }

    private var dims: [(panel: NSPanel, mask: CAShapeLayer)] = []
    private let ringPanel: NSPanel
    private let gradient = CAGradientLayer()
    private let ringShape = CAShapeLayer()
    private let lens = CALayer()
    private var stopFeed: (() -> Void)?     // stops the ScreenFeed, while zoomed in
    private var feedScreen = NSRect.zero    // the screen being captured, in global coordinates
    private var sketches: [NSPanel] = []
    private var strokes: [CALayer] = []
    private var ripples: [(panel: NSPanel, layer: CAShapeLayer)] = []
    private var nextRipple = 0
    private var keys: [HotKey] = []         // Esc and friends, registered only while something is on
    private var pointer = NSPoint.zero
    private(set) var spotlight = false, zoom = false, drawing = false
    /// Zoom was asked for but Screen Recording isn't allowed yet.
    private(set) var zoomBlocked = false
    var isOn: Bool { spotlight || zoom }
    /// Called whenever a tool goes on or off, including from a shortcut.
    var onChange: (() -> Void)?

    var radius: CGFloat = 100 {
        didSet {
            radius = min(max(radius, Presenter.minRadius), Presenter.maxRadius)
            if radius != oldValue { layoutRing(); rebuildMasks() }
        }
    }
    var zoomLevel: CGFloat = 2 {
        didSet { zoomLevel = min(max(zoomLevel, Presenter.minZoom), Presenter.maxZoom); aimLens() }
    }
    var color: NSColor = .systemGreen {
        didSet { for r in ripples { r.layer.strokeColor = color.cgColor } }
    }

    init() {
        let side = 2 * (Presenter.maxRadius + 16)
        ringPanel = Presenter.makePanel(frame: NSRect(x: 0, y: 0, width: side, height: side))
        lens.masksToBounds = true
        lens.contentsGravity = .resize
        lens.backgroundColor = NSColor.black.cgColor
        lens.isHidden = true
        ringPanel.contentView?.layer?.addSublayer(lens)
        gradient.type = .conic
        gradient.colors = Presenter.rainbow
        gradient.startPoint = CGPoint(x: 0.5, y: 0.5)
        gradient.endPoint = CGPoint(x: 0.5, y: 1)
        gradient.opacity = 0.85
        ringShape.fillColor = nil
        ringShape.strokeColor = NSColor.black.cgColor
        gradient.mask = ringShape
        ringPanel.contentView?.layer?.addSublayer(gradient)
        let spin = CABasicAnimation(keyPath: "transform.rotation.z")
        spin.fromValue = 0
        spin.toValue = -2 * CGFloat.pi
        spin.duration = 6
        spin.repeatCount = .infinity
        gradient.add(spin, forKey: "spin")
        gradient.speed = 0  // paused until shown

        for _ in 0..<3 {
            let side = 2 * (Presenter.rippleRadius + 4)
            let panel = Presenter.makePanel(frame: NSRect(x: 0, y: 0, width: side, height: side))
            let layer = CAShapeLayer()
            layer.frame = CGRect(x: 0, y: 0, width: side, height: side)
            layer.path = CGPath(ellipseIn: layer.frame.insetBy(dx: 4, dy: 4), transform: nil)
            layer.fillColor = nil
            layer.lineWidth = 4
            layer.opacity = 0
            panel.contentView?.layer?.addSublayer(layer)
            ripples.append((panel, layer))
        }
        color = .systemGreen
        layoutRing()
    }

    private static func makePanel(frame: NSRect, level: CGWindowLevelKey = .assistiveTechHighWindow) -> NSPanel {
        let panel = NSPanel(contentRect: frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: true)
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.ignoresMouseEvents = true
        panel.hidesOnDeactivate = false
        panel.isReleasedWhenClosed = false
        panel.animationBehavior = .none
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        panel.level = NSWindow.Level(rawValue: Int(CGWindowLevelForKey(level)))
        panel.contentView?.wantsLayer = true
        return panel
    }

    /// The rainbow ring sits just outside the clear circle, its width growing a little with the radius.
    private func layoutRing() {
        let side = ringPanel.frame.width, width = max(radius / 30, 3)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        gradient.frame = CGRect(x: 0, y: 0, width: side, height: side)
        ringShape.frame = gradient.bounds
        let r = radius + width / 2 - 1  // overlaps the edge of the hole by a pixel
        ringShape.path = CGPath(ellipseIn: CGRect(x: side / 2 - r, y: side / 2 - r, width: 2 * r, height: 2 * r), transform: nil)
        ringShape.lineWidth = width
        lens.frame = CGRect(x: side / 2 - radius, y: side / 2 - radius, width: 2 * radius, height: 2 * radius)
        lens.cornerRadius = radius
        CATransaction.commit()
        aimLens()
    }

    /// One dimming panel per screen. Its mask is a big rectangle with a round hole in the middle; following the
    /// pointer only moves the mask, so nothing is redrawn.
    private func rebuildDims() {
        for d in dims { d.panel.orderOut(nil) }
        dims = NSScreen.screens.map { screen in
            let panel = Presenter.makePanel(frame: screen.frame, level: .screenSaverWindow)
            let dim = CALayer()
            dim.frame = CGRect(origin: .zero, size: screen.frame.size)
            dim.backgroundColor = Presenter.dimColor
            let mask = CAShapeLayer()
            mask.fillRule = .evenOdd
            dim.mask = mask
            panel.contentView?.layer?.addSublayer(dim)
            return (panel, mask)
        }
        rebuildMasks()
    }

    private func rebuildMasks() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        for d in dims {
            let size = d.panel.frame.size, w = 4 * size.width, h = 4 * size.height
            d.mask.bounds = CGRect(x: 0, y: 0, width: w, height: h)
            let path = CGMutablePath()
            path.addRect(d.mask.bounds)
            path.addEllipse(in: CGRect(x: w / 2 - radius, y: h / 2 - radius, width: 2 * radius, height: 2 * radius))
            d.mask.path = path
        }
        CATransaction.commit()
        follow(pointer)
    }

    func screensChanged() {
        if spotlight { rebuildDims(); for d in dims { d.panel.orderFrontRegardless() } }
        if drawing { setDrawing(false); setDrawing(true) }
        feedScreen = .zero  // capture again, at the new size
        if zoom { follow(pointer) }
    }

    func setSpotlight(_ on: Bool) {
        spotlight = on
        if on {
            rebuildDims()
            for d in dims { d.panel.orderFrontRegardless() }
        } else {
            for d in dims { d.panel.orderOut(nil) }
            dims = []
        }
        update()
    }

    func setZoom(_ on: Bool) {
        zoom = false
        stopFeed?()
        stopFeed = nil
        feedScreen = .zero
        if on, #available(macOS 12.3, *) {
            if CGPreflightScreenCaptureAccess() {
                zoom = true
                zoomBlocked = false
            } else {
                zoomBlocked = true
                CGRequestScreenCaptureAccess()  // macOS asks once; after that, the menu links to the setting
            }
        }
        lens.contents = nil
        update()
        follow(NSEvent.mouseLocation)
    }

    /// Full-screen panels that take the mouse while drawing, just under the ring and the badge.
    func setDrawing(_ on: Bool) {
        drawing = on
        for p in sketches { p.orderOut(nil) }
        sketches = []
        strokes = []
        if on {
            for screen in NSScreen.screens {
                let panel = Presenter.makePanel(frame: screen.frame)
                panel.level = NSWindow.Level(rawValue: Int(CGWindowLevelForKey(.screenSaverWindow)) + 1)
                panel.ignoresMouseEvents = false  // every click on it is a pen stroke, transparent or not
                let view = SketchView(frame: NSRect(origin: .zero, size: screen.frame.size))
                view.onStroke = { [weak self] in self?.strokes.append($0) }
                panel.contentView = view
                panel.orderFrontRegardless()
                sketches.append(panel)
            }
        } else {
            NSCursor.arrow.set()
        }
        update()
    }

    func undoStroke() { strokes.popLast()?.removeFromSuperlayer() }

    /// Shows what is on, and registers the shortcuts that only make sense while it is.
    private func update() {
        if isOn {
            ringPanel.orderFrontRegardless()
            gradient.speed = 1
        } else {
            gradient.speed = 0
            ringPanel.orderOut(nil)
        }
        lens.isHidden = !zoom
        // Hot keys run their action from inside the Carbon handler; changing them there is left to the next turn.
        let later: (@escaping () -> Void) -> () -> Void = { action in { DispatchQueue.main.async(execute: action) } }
        self.keys = []  // a key can't be registered twice: let the old ones go first
        var keys: [HotKey] = []
        if isOn || drawing {
            keys.append(HotKey(keyCode: kVK_Escape, modifiers: 0, action: later { [weak self] in self?.escape() }))
        }
        if zoom {
            keys.append(HotKey(keyCode: kVK_ANSI_Equal, modifiers: controlKey | optionKey,
                               action: later { [weak self] in self?.stepZoom(1.25) }))
            keys.append(HotKey(keyCode: kVK_ANSI_Minus, modifiers: controlKey | optionKey,
                               action: later { [weak self] in self?.stepZoom(0.8) }))
        }
        if drawing {
            keys.append(HotKey(keyCode: kVK_ANSI_Z, modifiers: cmdKey, action: later { [weak self] in self?.undoStroke() }))
        }
        self.keys = keys
        follow(pointer)
        onChange?()
    }

    private func escape() {
        if drawing { setDrawing(false); return }  // first Esc puts the pen down, the next one leaves
        if zoom { setZoom(false) }
        if spotlight { setSpotlight(false) }
    }

    private func stepZoom(_ factor: CGFloat) {
        zoomLevel *= factor
        Settings.zoomLevel = Double(zoomLevel)
    }

    func follow(_ p: NSPoint) {
        pointer = p
        guard isOn else { return }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        for d in dims {
            let f = d.panel.frame, margin = radius + 8
            // Off this screen: keep the hole just outside it, so the mask still covers the whole screen.
            let x = min(max(p.x - f.minX, -margin), f.width + margin)
            let y = min(max(p.y - f.minY, -margin), f.height + margin)
            d.mask.position = CGPoint(x: x, y: y)
        }
        CATransaction.commit()
        let half = ringPanel.frame.width / 2
        ringPanel.setFrameOrigin(NSPoint(x: (p.x - half).rounded(), y: (p.y - half).rounded()))
        if zoom {
            if !feedScreen.contains(p) { capture(screenAt: p) }
            aimLens()
        }
    }

    /// Streams the screen under the pointer (without CursorLingo's own windows) into the lens.
    private func capture(screenAt p: NSPoint) {
        guard #available(macOS 12.3, *),
              let screen = NSScreen.screens.first(where: { $0.frame.contains(p) }),
              let id = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? CGDirectDisplayID else { return }
        feedScreen = screen.frame
        let feed = ScreenFeed()
        feed.onFrame = { [weak self] surface in self?.lens.contents = surface }
        feed.onStop = { [weak self] in if self?.zoom == true { self?.setZoom(false) } }
        feed.start(display: id, scale: screen.backingScaleFactor)
        stopFeed?()
        stopFeed = { feed.stop() }
    }

    /// Points the lens at the circle around the pointer, `zoomLevel` times smaller than the lens itself.
    private func aimLens() {
        let f = feedScreen
        guard zoom, f.width > 0 else { return }
        let half = radius / zoomLevel
        let x = min(max(pointer.x - f.minX, half), f.width - half)   // near an edge, stay on the screen
        let y = min(max(pointer.y - f.minY, half), f.height - half)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        lens.contentsRect = CGRect(x: (x - half) / f.width, y: (y - half) / f.height,
                                   width: 2 * half / f.width, height: 2 * half / f.height)
        CATransaction.commit()
    }

    func click(at p: NSPoint) {
        guard isOn, !drawing else { return }
        let (panel, layer) = ripples[nextRipple]
        nextRipple = (nextRipple + 1) % ripples.count
        let half = panel.frame.width / 2
        panel.setFrameOrigin(NSPoint(x: (p.x - half).rounded(), y: (p.y - half).rounded()))
        panel.orderFrontRegardless()

        let grow = CABasicAnimation(keyPath: "transform.scale")
        grow.fromValue = 0.25
        grow.toValue = 1
        let fade = CABasicAnimation(keyPath: "opacity")
        fade.fromValue = 1.0
        fade.toValue = 0
        let group = CAAnimationGroup()
        group.animations = [grow, fade]
        group.duration = 0.4
        group.timingFunction = CAMediaTimingFunction(name: .easeOut)
        CATransaction.begin()
        CATransaction.setCompletionBlock { [weak panel] in panel?.orderOut(nil) }
        layer.removeAllAnimations()
        layer.add(group, forKey: "ripple")
        CATransaction.commit()
    }
}

/// The pen: each drag becomes a stroke layer.
final class SketchView: NSView {
    var onStroke: ((CALayer) -> Void)?
    private var stroke: CAShapeLayer?
    private var path = CGMutablePath()

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.activeAlways, .inVisibleRect, .cursorUpdate, .mouseMoved],
                                       owner: self))
    }
    required init?(coder: NSCoder) { nil }

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override func cursorUpdate(with event: NSEvent) { NSCursor.crosshair.set() }
    override func mouseMoved(with event: NSEvent) { NSCursor.crosshair.set() }

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        path = CGMutablePath()
        path.move(to: p)
        path.addLine(to: p)  // a click leaves a dot
        let s = CAShapeLayer()
        s.frame = bounds
        s.fillColor = nil
        s.strokeColor = NSColor.systemRed.cgColor
        s.lineWidth = 5
        s.lineCap = .round
        s.lineJoin = .round
        s.path = path
        layer?.addSublayer(s)
        stroke = s
        onStroke?(s)
    }

    override func mouseDragged(with event: NSEvent) {
        guard let stroke else { return }
        path.addLine(to: convert(event.locationInWindow, from: nil))
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        stroke.path = path
        CATransaction.commit()
    }

    override func mouseUp(with event: NSEvent) { stroke = nil }
}

/// A live stream of one display, minus CursorLingo's own windows, handed over as IOSurfaces on the main thread.
@available(macOS 12.3, *)
final class ScreenFeed: NSObject, SCStreamOutput, SCStreamDelegate {
    var onFrame: ((IOSurface) -> Void)?
    var onStop: (() -> Void)?
    private var stream: SCStream?
    private let queue = DispatchQueue(label: "io.github.cursorlingo.zoom")
    private var stopped = false

    func start(display id: CGDirectDisplayID, scale: CGFloat) {
        SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: true) { [weak self] content, _ in
            DispatchQueue.main.async {
                guard let self, !self.stopped, let content,
                      let display = content.displays.first(where: { $0.displayID == id }) else { self?.onStop?(); return }
                let me = content.applications.filter { $0.processID == getpid() }
                let filter = SCContentFilter(display: display, excludingApplications: me, exceptingWindows: [])
                let config = SCStreamConfiguration()
                config.width = Int(CGFloat(display.width) * scale)
                config.height = Int(CGFloat(display.height) * scale)
                config.minimumFrameInterval = CMTime(value: 1, timescale: 60)
                config.pixelFormat = kCVPixelFormatType_32BGRA
                config.showsCursor = false  // the real pointer is drawn on top anyway
                config.queueDepth = 4
                let stream = SCStream(filter: filter, configuration: config, delegate: self)
                do {
                    try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: self.queue)
                } catch {
                    self.onStop?()
                    return
                }
                stream.startCapture { error in if error != nil { DispatchQueue.main.async { self.onStop?() } } }
                self.stream = stream
            }
        }
    }

    /// The stream holds on to its output, so it has to be stopped explicitly.
    func stop() {
        stopped = true
        onFrame = nil
        onStop = nil
        stream?.stopCapture(completionHandler: nil)
        stream = nil
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen,
              let info = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let status = info.first?[.status] as? Int, status == SCFrameStatus.complete.rawValue,
              let buffer = sampleBuffer.imageBuffer,
              let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue() else { return }
        let frame = surface as IOSurface
        DispatchQueue.main.async { [weak self] in self?.onFrame?(frame) }
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        DispatchQueue.main.async { [weak self] in self?.onStop?() }
    }
}

/// A system-wide shortcut through Carbon's hot key API, which needs no permission (unlike watching keys).
final class HotKey {
    private static var nextID: UInt32 = 1
    private let id: UInt32
    private var ref: EventHotKeyRef?
    private var handler: EventHandlerRef?
    private let action: () -> Void
    /// False when the key couldn't be had, for example because another app holds it.
    private(set) var registered = false

    init(keyCode: Int, modifiers: Int, action: @escaping () -> Void) {
        self.action = action
        id = HotKey.nextID
        HotKey.nextID += 1
        var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        // Every handler sees every hot key of the app: act only on our own.
        let callback: EventHandlerUPP = { _, event, userData in
            guard let event, let userData else { return OSStatus(eventNotHandledErr) }
            var pressed = EventHotKeyID()
            GetEventParameter(event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID), nil,
                              MemoryLayout<EventHotKeyID>.size, nil, &pressed)
            let me = Unmanaged<HotKey>.fromOpaque(userData).takeUnretainedValue()
            guard pressed.id == me.id else { return OSStatus(eventNotHandledErr) }
            me.action()
            return noErr
        }
        InstallEventHandler(GetApplicationEventTarget(), callback, 1, &spec, Unmanaged.passUnretained(self).toOpaque(), &handler)
        let hotKeyID = EventHotKeyID(signature: OSType(0x434C_4750), id: id)  // "CLGP"
        registered = RegisterEventHotKey(UInt32(keyCode), UInt32(modifiers), hotKeyID, GetApplicationEventTarget(), 0, &ref) == noErr
    }

    deinit {
        if let ref { UnregisterEventHotKey(ref) }
        if let handler { RemoveEventHandler(handler) }
    }
}

// MARK: - Shortcuts

/// The presentation tools that have a shortcut of their own.
enum PresentTool: String, CaseIterable {
    case spotlight, zoom, draw
    var title: String {
        switch self {
        case .spotlight: return tr("Spotlight", "สปอตไลต์")
        case .zoom: return tr("Zoom", "ซูม")
        case .draw: return tr("Draw on Screen", "วาดบนจอ")
        }
    }
    var defaultShortcut: Shortcut {
        let key = [PresentTool.spotlight: kVK_ANSI_H, .zoom: kVK_ANSI_Z, .draw: kVK_ANSI_D][self]!
        return Shortcut(keyCode: key, modifiers: controlKey | optionKey)
    }
}

/// A key plus Carbon modifiers (controlKey, optionKey, shiftKey, cmdKey), as RegisterEventHotKey takes them.
struct Shortcut: Equatable {
    var keyCode: Int
    var modifiers: Int

    /// The keys a shortcut can use, by their US position, with the name shown for each.
    private static let names: [Int: String] = {
        var n: [Int: String] = [
            kVK_ANSI_A: "A", kVK_ANSI_B: "B", kVK_ANSI_C: "C", kVK_ANSI_D: "D", kVK_ANSI_E: "E", kVK_ANSI_F: "F",
            kVK_ANSI_G: "G", kVK_ANSI_H: "H", kVK_ANSI_I: "I", kVK_ANSI_J: "J", kVK_ANSI_K: "K", kVK_ANSI_L: "L",
            kVK_ANSI_M: "M", kVK_ANSI_N: "N", kVK_ANSI_O: "O", kVK_ANSI_P: "P", kVK_ANSI_Q: "Q", kVK_ANSI_R: "R",
            kVK_ANSI_S: "S", kVK_ANSI_T: "T", kVK_ANSI_U: "U", kVK_ANSI_V: "V", kVK_ANSI_W: "W", kVK_ANSI_X: "X",
            kVK_ANSI_Y: "Y", kVK_ANSI_Z: "Z",
            kVK_ANSI_0: "0", kVK_ANSI_1: "1", kVK_ANSI_2: "2", kVK_ANSI_3: "3", kVK_ANSI_4: "4",
            kVK_ANSI_5: "5", kVK_ANSI_6: "6", kVK_ANSI_7: "7", kVK_ANSI_8: "8", kVK_ANSI_9: "9",
            kVK_ANSI_Equal: "=", kVK_ANSI_Minus: "-", kVK_ANSI_LeftBracket: "[", kVK_ANSI_RightBracket: "]",
            kVK_ANSI_Semicolon: ";", kVK_ANSI_Quote: "'", kVK_ANSI_Comma: ",", kVK_ANSI_Period: ".",
            kVK_ANSI_Slash: "/", kVK_ANSI_Backslash: "\\", kVK_ANSI_Grave: "`",
        ]
        let fKeys = [kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6, kVK_F7, kVK_F8, kVK_F9, kVK_F10, kVK_F11, kVK_F12]
        for (i, key) in fKeys.enumerated() { n[key] = "F\(i + 1)" }
        return n
    }()
    /// In use while a tool is on (zoom in / out, take back a stroke), so they can't be picked.
    static let reserved = [Shortcut(keyCode: kVK_ANSI_Equal, modifiers: controlKey | optionKey),
                           Shortcut(keyCode: kVK_ANSI_Minus, modifiers: controlKey | optionKey),
                           Shortcut(keyCode: kVK_ANSI_Z, modifiers: cmdKey)]

    init(keyCode: Int, modifiers: Int) {
        self.keyCode = keyCode
        self.modifiers = modifiers
    }

    /// From a key press: needs ⌃, ⌥ or ⌘ (so it never takes plain typing), and a key from the list above.
    init?(event: NSEvent) {
        let f = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        guard !f.isDisjoint(with: [.control, .option, .command]), Shortcut.names[Int(event.keyCode)] != nil else { return nil }
        var m = 0
        if f.contains(.control) { m |= controlKey }
        if f.contains(.option) { m |= optionKey }
        if f.contains(.shift) { m |= shiftKey }
        if f.contains(.command) { m |= cmdKey }
        self.init(keyCode: Int(event.keyCode), modifiers: m)
    }

    var display: String {
        var s = ""
        if modifiers & controlKey != 0 { s += "⌃" }
        if modifiers & optionKey != 0 { s += "⌥" }
        if modifiers & shiftKey != 0 { s += "⇧" }
        if modifiers & cmdKey != 0 { s += "⌘" }
        return s + (Shortcut.names[keyCode] ?? "?")
    }

    /// For showing it next to a menu item.
    var keyEquivalent: String {
        let name = Shortcut.names[keyCode] ?? ""
        if name.count > 1, let n = Int(name.dropFirst()), let f = UnicodeScalar(NSF1FunctionKey + n - 1) {
            return String(Character(f))
        }
        return name.lowercased()
    }
    var modifierFlags: NSEvent.ModifierFlags {
        var f: NSEvent.ModifierFlags = []
        if modifiers & controlKey != 0 { f.insert(.control) }
        if modifiers & optionKey != 0 { f.insert(.option) }
        if modifiers & shiftKey != 0 { f.insert(.shift) }
        if modifiers & cmdKey != 0 { f.insert(.command) }
        return f
    }
}

/// A box that records a shortcut: click it, then press the keys. Delete clears it, Esc gives up.
final class ShortcutField: NSView {
    var shortcut: Shortcut? { didSet { needsDisplay = true } }
    /// The user picked a new shortcut, or cleared it (nil).
    var onRecord: ((Shortcut?) -> Void)?
    /// Listening for keys starts or stops (the app's own shortcuts are paused meanwhile, so the keys reach the box).
    var onListening: ((Bool) -> Void)?
    private var listening = false {
        didSet {
            needsDisplay = true
            if listening != oldValue { onListening?(listening) }
        }
    }

    override var acceptsFirstResponder: Bool { true }
    override var intrinsicContentSize: NSSize { NSSize(width: 150, height: 24) }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        listening = true
    }
    override func resignFirstResponder() -> Bool {
        listening = false
        return true
    }
    override func keyDown(with event: NSEvent) {
        if listening { record(event) } else { super.keyDown(with: event) }
    }
    override func performKeyEquivalent(with event: NSEvent) -> Bool {  // ⌘ combinations arrive here, not in keyDown
        guard listening, event.type == .keyDown else { return false }
        record(event)
        return true
    }

    private func record(_ event: NSEvent) {
        switch Int(event.keyCode) {
        case kVK_Escape:
            break
        case kVK_Delete, kVK_ForwardDelete:
            onRecord?(nil)
        default:
            guard let s = Shortcut(event: event) else { NSSound.beep(); return }
            onRecord?(s)
        }
        window?.makeFirstResponder(nil)
    }

    override func draw(_ dirtyRect: NSRect) {
        let box = NSBezierPath(roundedRect: bounds.insetBy(dx: 0.5, dy: 0.5), xRadius: 5, yRadius: 5)
        (listening ? NSColor.controlAccentColor.withAlphaComponent(0.15) : NSColor.controlBackgroundColor).setFill()
        box.fill()
        (listening ? NSColor.controlAccentColor : NSColor.separatorColor).setStroke()
        box.stroke()
        let text = listening ? tr("Type shortcut…", "กดคีย์ที่ต้องการ…") : shortcut?.display ?? tr("None", "ไม่มี")
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 13),
            .foregroundColor: listening || shortcut == nil ? NSColor.secondaryLabelColor : NSColor.labelColor,
        ]
        let size = text.size(withAttributes: attrs)
        text.draw(at: NSPoint(x: (bounds.width - size.width) / 2, y: (bounds.height - size.height) / 2), withAttributes: attrs)
    }
}

// MARK: - Settings window

/// The badge as it looks next to the pointer, for the Settings window.
final class BadgePreview: NSView {
    private let badge = BadgeView(frame: .zero)
    var code = "EN" { didSet { update() } }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.cornerRadius = 8
        layer?.borderWidth = 1
        addSubview(badge)
        translatesAutoresizingMaskIntoConstraints = false
        widthAnchor.constraint(equalToConstant: 180).isActive = true
        heightAnchor.constraint(equalToConstant: 128).isActive = true
    }
    required init?(coder: NSCoder) { nil }

    /// Picks up the style, size and colour as they are now.
    func update() {
        badge.code = code
        badge.style = Settings.style
        badge.scale = Settings.size.scale
        badge.needsDisplay = true
        needsLayout = true
        needsDisplay = true
    }

    private var pointer: NSPoint { NSPoint(x: bounds.midX - 30, y: bounds.midY + 18) }

    override func layout() {
        super.layout()
        // The same offset as the real badge (see AppDelegate.follow()).
        let s = badge.scale, size = BadgeView.size(style: badge.style, scale: s, code: code)
        badge.frame = NSRect(x: (pointer.x + 12 * s).rounded(), y: (pointer.y - 22 * s - size.height / 2).rounded(),
                             width: size.width, height: size.height)
    }

    override func draw(_ dirtyRect: NSRect) {
        layer?.borderColor = NSColor.separatorColor.cgColor
        NSGradient(starting: NSColor(srgbRed: 0.33, green: 0.45, blue: 0.62, alpha: 1),
                   ending: NSColor(srgbRed: 0.62, green: 0.50, blue: 0.70, alpha: 1))?.draw(in: bounds, angle: -30)
        let arrow = NSCursor.arrow, image = arrow.image
        image.draw(at: NSPoint(x: pointer.x - arrow.hotSpot.x, y: pointer.y - (image.size.height - arrow.hotSpot.y)),
                   from: .zero, operation: .sourceOver, fraction: 1)
    }
}

/// Settings in four tabs: Badge, Fix Wrong Language, Presentation and General. Every change applies at once.
final class SettingsWindow: NSObject, NSWindowDelegate {
    enum Tab: Int { case badge, fix, presentation, general }

    weak var app: AppDelegate?
    private var window: NSWindow?
    private let tabs = SettingsTabs()
    private let preview = BadgePreview()
    private var previewCode: String?  // the language picked in the list; the current one until then
    private var languages: [(code: String, layouts: [String])] = []
    private var wells: [NSColorWell] = []
    private var resets: [NSButton] = []
    private var shortcutFields: [PresentTool: ShortcutField] = [:]
    private let shortcutStatus = NSTextField(labelWithString: "")
    private let radiusValue = NSTextField(labelWithString: "")
    private let zoomValue = NSTextField(labelWithString: "")
    private var reloadPending = false

    func show(_ tab: Tab? = nil) {
        if window == nil { makeWindow() }
        build()
        if let tab { tabs.selectedTabViewItemIndex = tab.rawValue }
        tabs.fitWindow()
        NSApp.activate(ignoringOtherApps: true)
        window?.makeKeyAndOrderFront(nil)
    }

    /// After a change made somewhere else (the menu, a shortcut): shows it, if the window is open.
    func reload() {
        guard window?.isVisible == true, !reloadPending else { return }
        reloadPending = true
        DispatchQueue.main.async { [weak self] in  // never inside the action of a control that is about to be replaced
            guard let self else { return }
            self.reloadPending = false
            self.build()
        }
    }

    /// The keyboard language changed: the preview follows it, unless a language was picked in the list.
    func inputChanged() {
        if previewCode == nil { preview.code = InputSource.currentCode() }
    }

    private func makeWindow() {
        tabs.tabStyle = .toolbar
        let symbols = ["character.cursor.ibeam", "keyboard", "cursorarrow.rays", "gearshape"]
        for symbol in symbols {
            let page = NSViewController()
            page.view = NSView()  // stays for good; build() swaps what is inside, which the tab view shows at once
            let item = NSTabViewItem(viewController: page)
            item.image = NSImage(systemSymbolName: symbol, accessibilityDescription: nil)
            tabs.addTabViewItem(item)
        }
        let w = NSWindow(contentViewController: tabs)
        w.styleMask = [.titled, .closable]
        w.level = .floating  // stays above other windows, so changes can be tried out in the app underneath
        w.isReleasedWhenClosed = false
        w.delegate = self
        w.center()
        window = w
    }

    /// Fills every tab in the menu's current language, keeping the selected one.
    private func build() {
        window?.makeFirstResponder(nil)  // a shortcut box still listening lets go first
        let titles = [tr("Badge", "ป้ายภาษา"), tr("Fix Wrong Language", "แก้คำผิดภาษา"), tr("Presentation", "นำเสนอ"),
                      tr("General", "ทั่วไป")]
        let views = [badgeTab(), fixTab(), presentationTab(), generalTab()]
        for (i, item) in tabs.tabViewItems.enumerated() {
            item.label = titles[i]
            item.viewController?.title = titles[i]  // the tab controller makes the page's title the window's title
            guard let page = item.viewController?.view else { continue }
            page.subviews = [views[i]]
            views[i].translatesAutoresizingMaskIntoConstraints = false
            // Only the top-left corner is pinned: the content keeps its own size, and the window is fitted to it.
            NSLayoutConstraint.activate([
                views[i].topAnchor.constraint(equalTo: page.topAnchor),
                views[i].leadingAnchor.constraint(equalTo: page.leadingAnchor),
            ])
        }
        window?.title = tabs.tabViewItems[tabs.selectedTabViewItemIndex].label
        tabs.fitWindow()
    }

    // MARK: Tabs

    private func badgeTab() -> NSView {
        languages = InputSource.installedLanguages()
        let current = InputSource.currentCode()
        if !languages.contains(where: { $0.code == current }) { languages.insert((current, []), at: 0) }
        if let p = previewCode, !languages.contains(where: { $0.code == p }) { previewCode = nil }
        preview.code = previewCode ?? current
        preview.update()

        let looks = NSGridView(views: [
            [label(tr("Show:", "แสดง:")), popup(DisplayMode.allCases.map { ($0.rawValue, $0.title) }, Settings.mode.rawValue,
                                               #selector(AppDelegate.pickMode))],
            [label(tr("Style:", "รูปแบบ:")), popup(BadgeStyle.allCases.map { ($0.rawValue, $0.title) }, Settings.style.rawValue,
                                                 #selector(AppDelegate.pickStyle))],
            [label(tr("Size:", "ขนาด:")), popup(BadgeSize.allCases.map { ($0.rawValue, $0.title) }, Settings.size.rawValue,
                                               #selector(AppDelegate.pickSize))],
            [label(tr("Border:", "เส้นขอบ:")), toggle(Settings.badgeBorder, #selector(toggleBorder(_:)))],
        ])
        looks.rowSpacing = 8
        looks.column(at: 0).xPlacement = .trailing
        looks.rowAlignment = .firstBaseline
        looks.setContentHuggingPriority(.required, for: .vertical)  // its own height, not the preview's
        let top = NSStackView(views: [preview, looks])
        top.spacing = 20
        top.alignment = .top

        wells = []
        resets = []
        let rows: [[NSView]] = languages.enumerated().map { i, lang in
            let name = NSMutableAttributedString(string: lang.code, attributes: [.font: NSFont.boldSystemFont(ofSize: NSFont.systemFontSize)])
            if !lang.layouts.isEmpty {
                name.append(NSAttributedString(string: "   " + lang.layouts.joined(separator: ", "),
                                               attributes: [.font: NSFont.systemFont(ofSize: NSFont.systemFontSize),
                                                            .foregroundColor: NSColor.secondaryLabelColor]))
            }
            let title = NSTextField(labelWithAttributedString: name)
            title.widthAnchor.constraint(greaterThanOrEqualToConstant: 150).isActive = true
            let show = toggle(!Settings.hidden.contains(lang.code), #selector(toggleLanguage(_:)))
            show.tag = i
            let well = NSColorWell()
            well.color = Palette.color(for: lang.code)
            well.target = self
            well.action = #selector(colorChanged(_:))
            well.tag = i
            well.widthAnchor.constraint(equalToConstant: 44).isActive = true
            well.heightAnchor.constraint(equalToConstant: 24).isActive = true
            wells.append(well)
            let reset = NSButton(title: tr("Default", "สีเดิม"), target: self, action: #selector(resetColor(_:)))
            reset.tag = i
            reset.controlSize = .small
            reset.isEnabled = Palette.hasCustomColor(for: lang.code)
            resets.append(reset)
            return [title, well, reset, show]
        }
        let column = { (text: String) -> NSTextField in
            let l = NSTextField(labelWithString: text)
            l.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
            l.textColor = .secondaryLabelColor
            return l
        }
        let list = NSGridView(views: [[column(tr("Keyboard", "แป้นพิมพ์")), column(tr("Colour", "สี")), NSGridCell.emptyContentView,
                                       column(tr("Show badge", "แสดงป้าย"))]] + rows)
        list.column(at: 3).xPlacement = .center
        list.row(at: 0).bottomPadding = -2
        list.rowSpacing = 8
        list.columnSpacing = 12
        list.rowAlignment = .none
        for r in 0..<list.numberOfRows { list.row(at: r).yPlacement = .center }

        let heading = NSStackView(views: [header(tr("Languages", "ภาษา")),
                                          caption(tr("The keyboards added in System Settings → Keyboard → Input Sources.",
                                                     "แป้นพิมพ์ที่เพิ่มไว้ใน System Settings → Keyboard → Input Sources"))])
        heading.orientation = .vertical
        heading.alignment = .leading
        heading.spacing = 2
        return tab([top, heading, list])
    }

    private func fixTab() -> NSView {
        guard let app else { return NSView() }
        let mode = Settings.fixMode
        let modes: [(FixMode, String)] = [
            (.off, tr("Nothing is fixed, and CursorLingo doesn't look at your typing at all.", "ไม่แก้อะไรให้ และไม่ดูสิ่งที่คุณพิมพ์เลย")),
            (.manual, tr("Tap Shift twice to convert the word before the cursor, or the selected text.",
                         "พิมพ์ผิดภาษาเมื่อไหร่ กด Shift 2 ครั้งติดกัน คำล่าสุด (หรือข้อความที่เลือกไว้) จะเปลี่ยนเป็นอีกภาษาให้")),
            (.automatic, tr("Words typed in the wrong language are fixed as you type. If a fix is wrong, double Shift puts it back.",
                            "แก้คำที่พิมพ์ผิดภาษาให้เองระหว่างพิมพ์ ถ้าแก้ไม่ตรงใจ กด Shift 2 ครั้งเพื่อย้อนคืน")),
        ]
        var views: [NSView] = [header(tr("Fix Wrong Language (EN ⇄ TH)", "แก้คำที่พิมพ์ผิดภาษา (EN ⇄ TH)"))]
        for (i, (m, detail)) in modes.enumerated() {
            let radio = NSButton(radioButtonWithTitle: m.title, target: self, action: #selector(fixModeChanged(_:)))
            radio.tag = i
            radio.state = m == mode ? .on : .off
            let detailText = caption(detail)
            detailText.preferredMaxLayoutWidth = 450  // indented under the radio
            let item = NSStackView(views: [radio, indented(detailText)])
            item.orientation = .vertical
            item.alignment = .leading
            item.spacing = 2
            views.append(item)
        }
        if mode != .off {
            let status = app.autoFix.status
            let dot = NSTextField(labelWithString: "●")
            dot.textColor = status.problem == nil ? .systemGreen : .systemOrange
            var parts: [NSView] = [dot, label(status.text)]
            if status.problem == .permission {
                parts.append(button(tr("Open Privacy Settings…", "เปิด Privacy & Security…"), #selector(AppDelegate.openPrivacySettings), app))
            }
            if status.problem == .notRunning {
                parts.append(button(tr("Relaunch CursorLingo", "เปิด CursorLingo ใหม่"), #selector(AppDelegate.relaunch), app))
            }
            views.append(line(parts))
        }

        views.append(separator())
        let editors = toggle(Settings.codeEditorsManual, #selector(codeEditorsChanged(_:)))
        editors.isEnabled = mode == .automatic
        views.append(optionRow(tr("Code editors: fix only on double Shift", "โปรแกรมเขียนโค้ด: แก้เฉพาะตอนกด Shift 2 ครั้ง"),
                               tr("Names in code often look like wrong-language words, so these apps aren't fixed automatically: "
                                  + AutoFix.codeEditorNames + ". Terminals (" + AutoFix.terminalNames + ") never are, whatever this is set to. "
                                  + "Both lists come with CursorLingo; ask for more on GitHub.",
                                  "ชื่อตัวแปรในโค้ดหน้าตาคล้ายคำพิมพ์ผิดภาษาบ่อยๆ เลยไม่แก้เองในแอปพวกนี้: " + AutoFix.codeEditorNames
                                  + " ส่วน Terminal (" + AutoFix.terminalNames + ") ไม่แก้เองเลย ไม่ว่าจะตั้งตรงนี้ไว้ยังไง "
                                  + "รายชื่อแอปมีมากับ CursorLingo อยากให้เพิ่มแอปไหน บอกได้ที่ GitHub"),
                               editors))
        let learned = app.autoFix.rules.count
        let forget = button(tr("Forget", "ล้าง"), #selector(AppDelegate.forgetLearned), app)
        forget.isEnabled = learned > 0
        views.append(optionRow(learned == 0 ? tr("Learned words: none yet", "คำที่จำไว้: ยังไม่มี")
                                            : tr("Learned words: \(learned)", "คำที่จำไว้: \(learned) คำ"),
                               tr("When you convert or undo a word with double Shift, CursorLingo remembers it and does the same next time. Kept on this Mac only.",
                                  "คำไหนที่คุณกด Shift 2 ครั้งเพื่อแปลงหรือย้อนคืน จะถูกจำไว้ แล้วครั้งหน้าทำแบบเดียวกันให้เอง เก็บไว้ในเครื่องนี้เท่านั้น"),
                               forget))
        views.append(separator())
        views.append(optionRow(tr("Remember language per app", "จำภาษาของแต่ละแอป"),
                               tr("Going back to an app switches to the keyboard you last used there. Works even with fixing off.",
                                  "กลับไปแอปไหน ก็สลับเป็นภาษาที่ใช้ล่าสุดในแอปนั้นให้เอง ใช้ได้แม้จะปิดการแก้คำไว้"),
                               toggle(Settings.perAppLanguage, #selector(perAppChanged(_:)))))
        return tab(views)
    }

    private func presentationTab() -> NSView {
        guard let app else { return NSView() }
        let tools: [(PresentTool, Bool, Selector, String)] = [
            (.spotlight, app.presenter.spotlight, #selector(spotlightChanged(_:)),
             tr("Dims the screen except for a circle around the pointer. You can still click and type as usual.",
                "หรี่จอ เหลือวงกลมใสรอบเมาส์ ยังคลิกและพิมพ์ได้ตามปกติ")),
            (.zoom, app.presenter.zoom, #selector(zoomToolChanged(_:)),
             tr("Magnifies what is under the pointer, inside the circle. Needs Screen Recording permission.",
                "ขยายจุดที่เมาส์ชี้ให้ใหญ่ขึ้นในวงกลม (ต้องอนุญาต Screen Recording ก่อน)")),
            (.draw, app.presenter.drawing, #selector(drawChanged(_:)),
             tr("Draw over the screen with the mouse. While it's on, clicks draw instead of reaching apps; Esc stops.",
                "ลากเมาส์ขีดเขียนบนจอได้เลย ระหว่างนี้คลิกจะเป็นการวาด ไม่โดนแอปข้างใต้ กด Esc เพื่อเลิกวาด")),
        ]
        shortcutFields = [:]
        let heading = NSStackView(views: [header(tr("Tools", "เครื่องมือ")),
                                          caption(tr("To change a shortcut, click it and press the new keys (⌃, ⌥ or ⌘ with a letter, "
                                                     + "number or F-key). Delete clears it.",
                                                     "อยากเปลี่ยนคีย์ลัด คลิกที่ช่องแล้วกดคีย์ใหม่ (⌃ ⌥ หรือ ⌘ คู่กับตัวอักษร ตัวเลข หรือปุ่ม F) "
                                                     + "ถ้าไม่อยากใช้คีย์ลัด กด Delete"))])
        heading.orientation = .vertical
        heading.alignment = .leading
        heading.spacing = 2
        var views: [NSView] = [heading]
        for (tool, on, action, detail) in tools {
            let field = ShortcutField()
            field.shortcut = Settings.shortcut(tool)
            field.onRecord = { [weak self] s in self?.setShortcut(tool, s) }
            field.onListening = { [weak app] listening in app?.pauseShortcuts(listening) }
            field.widthAnchor.constraint(equalToConstant: 104).isActive = true
            field.toolTip = tr("Click, then press the new keys. Delete clears it.", "คลิกแล้วกดคีย์ใหม่ กด Delete เพื่อล้าง")
            shortcutFields[tool] = field
            let onOff = toggle(on, action)
            if tool == .zoom { onOff.isEnabled = Presenter.zoomAvailable }
            let controls = NSStackView(views: [field, onOff])
            controls.spacing = 14
            views.append(optionRow(tool.title, detail, controls, detailWidth: 300))
            if tool == .zoom && app.presenter.zoomBlocked && !CGPreflightScreenCaptureAccess() {
                let dot = NSTextField(labelWithString: "●")
                dot.textColor = .systemOrange
                views.append(line(dot, label(tr("Screen Recording isn't allowed yet.", "ยังไม่ได้อนุญาต Screen Recording")),
                                  button(tr("Open Privacy Settings…", "เปิด Privacy & Security…"),
                                         #selector(AppDelegate.openScreenRecordingSettings), app)))
            }
        }
        showShortcutStatus("")
        shortcutStatus.textColor = .systemRed
        let restore = NSButton(title: tr("Restore Default Shortcuts", "คืนค่าคีย์ลัดเริ่มต้น"), target: self,
                               action: #selector(restoreShortcuts))
        restore.controlSize = .small
        let restoreRow = NSStackView()
        restoreRow.addView(shortcutStatus, in: .leading)
        restoreRow.addView(restore, in: .trailing)
        restoreRow.widthAnchor.constraint(equalToConstant: 472).isActive = true
        views.append(restoreRow)

        views.append(separator())
        views.append(header(tr("Circle", "วงกลม")))
        let size = NSSlider(value: Double(app.presenter.radius), minValue: Double(Presenter.minRadius),
                            maxValue: Double(Presenter.maxRadius), target: self, action: #selector(radiusChanged(_:)))
        let zoom = NSSlider(value: Double(app.presenter.zoomLevel), minValue: Double(Presenter.minZoom),
                            maxValue: Double(Presenter.maxZoom), target: self, action: #selector(zoomChanged(_:)))
        for s in [size, zoom] { s.widthAnchor.constraint(equalToConstant: 260).isActive = true }
        radiusChanged(size)
        zoomChanged(zoom)
        for v in [radiusValue, zoomValue] {
            v.font = .monospacedDigitSystemFont(ofSize: NSFont.systemFontSize, weight: .regular)
            v.textColor = .secondaryLabelColor
        }
        let sliders = NSGridView(views: [[label(tr("Size", "ขนาดวง")), size, radiusValue],
                                         [label(tr("Magnification", "ระดับซูม")), zoom, zoomValue]])
        sliders.rowSpacing = 10
        sliders.columnSpacing = 12
        for r in 0..<sliders.numberOfRows { sliders.row(at: r).yPlacement = .center }
        views.append(sliders)

        views.append(separator())
        views.append(header(tr("While a Tool Is On", "ระหว่างใช้งาน")))
        let keys: [(String, String)] = [
            ("⌃⌥ + " + tr("scroll", "ล้อเมาส์"), tr("Resize the circle", "ปรับขนาดวง")),
            ("⌃⌥ =   ⌃⌥ −", tr("Zoom in / out", "ซูมเข้า / ออก")),
            ("⌘Z", tr("Take back the last stroke", "ลบเส้นที่วาดล่าสุด")),
            ("Esc", tr("Stop drawing; press again to turn everything off", "เลิกวาด กดอีกครั้งเพื่อปิดทั้งหมด")),
            (tr("Click", "คลิก"), tr("A ring in the colour of the language in use", "มีวงกระเพื่อมออกมา เป็นสีของภาษาที่ใช้อยู่")),
        ]
        let legend = NSGridView(views: keys.map { [keycap($0.0), label($0.1)] })
        legend.rowSpacing = 6
        legend.columnSpacing = 14
        for r in 0..<legend.numberOfRows { legend.row(at: r).yPlacement = .center }
        views.append(legend)
        return tab(views)
    }

    private func generalTab() -> NSView {
        guard let app else { return NSView() }
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
        let language = popup(UILanguage.allCases.map { ($0.rawValue, $0.title) }, Settings.uiLanguage.rawValue,
                             #selector(AppDelegate.pickUILanguage))

        let icon = NSImageView(image: NSApp.applicationIconImage)
        icon.widthAnchor.constraint(equalToConstant: 48).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 48).isActive = true
        let name = NSTextField(labelWithString: "CursorLingo")
        name.font = .systemFont(ofSize: 15, weight: .semibold)
        let about = NSStackView(views: [name, caption(tr("Version \(version) · Free and open source · No network access, no tracking",
                                                          "เวอร์ชัน \(version) · ฟรีและโอเพนซอร์ส · ไม่ต่อเน็ต ไม่เก็บข้อมูลการใช้งาน"))])
        about.orientation = .vertical
        about.alignment = .leading
        about.spacing = 2
        let top = NSStackView(views: [icon, about])
        top.spacing = 12

        let pitch = caption(tr("CursorLingo is a free, spare-time project. If it saves you some retyping, a star on GitHub or a "
                               + "share with friends means a lot. Got an idea, a word it got wrong, or some code? Come say hi.",
                               "CursorLingo ทำในเวลาว่าง และแจกให้ใช้ฟรี ถ้ามันช่วยให้ไม่ต้องลบแล้วพิมพ์ใหม่บ่อยๆ "
                               + "ฝากกดดาวบน GitHub หรือแชร์ให้เพื่อนๆ หน่อยนะ มีไอเดีย เจอคำที่แก้ผิด หรืออยากช่วยเขียนโค้ด ทักมาได้เลย"))
        let support = NSStackView(views: [
            header(tr("Support CursorLingo", "สนับสนุน CursorLingo")),
            pitch,
            line(NSButton(title: tr("★ Star on GitHub", "★ ให้ดาวบน GitHub"), target: self, action: #selector(openRepo)),
                 NSButton(title: tr("How to Contribute", "วิธีร่วมพัฒนา"), target: self, action: #selector(openContributing))),
        ])
        support.orientation = .vertical
        support.alignment = .leading
        support.spacing = 8

        return tab([
            top,
            separator(),
            optionRow(tr("Open at login", "เปิด CursorLingo อัตโนมัติ"),
                      tr("Starts by itself when you log in to your Mac, so the badge is always there.",
                         "เปิดเครื่องหรือล็อกอินเมื่อไหร่ CursorLingo ก็ทำงานเลย ไม่ต้องคอยเปิดเอง"),
                      toggle(LoginItem.isEnabled, #selector(loginChanged(_:)))),
            optionRow("Language / ภาษา", tr("The language of the menu and this window.", "ภาษาที่ใช้ในเมนูและหน้าต่างนี้"), language),
            optionRow(tr("Found a wrong fix or a bug?", "เจอคำที่แก้ผิด หรือเจอบั๊ก?"),
                      tr("Opens the report form on GitHub in your browser. Nothing you typed is attached.",
                         "จะเปิดหน้าแจ้งปัญหาบน GitHub ในเบราว์เซอร์ ไม่มีอะไรที่คุณพิมพ์ติดไปด้วย"),
                      button(tr("Report…", "แจ้งปัญหา…"), #selector(AppDelegate.reportIssue), app)),
            separator(),
            support,
        ])
    }

    // MARK: Actions

    @objc private func fixModeChanged(_ sender: NSButton) {
        let item = NSMenuItem()
        item.representedObject = [FixMode.off, .manual, .automatic][sender.tag].rawValue
        app?.pickFixMode(item)
        reload()
    }

    @objc private func loginChanged(_ sender: NSSwitch) {
        if (sender.state == .on) != LoginItem.isEnabled { app?.toggleLogin() }
    }
    @objc private func openRepo() {
        if let url = URL(string: "https://github.com/kittipongint/cursor-lingo") { NSWorkspace.shared.open(url) }
    }
    @objc private func openContributing() {
        if let url = URL(string: "https://github.com/kittipongint/cursor-lingo/blob/main/CONTRIBUTING.md") { NSWorkspace.shared.open(url) }
    }

    @objc private func spotlightChanged(_ sender: NSSwitch) {
        if (sender.state == .on) != app?.presenter.spotlight { app?.toggleSpotlight() }
    }
    @objc private func zoomToolChanged(_ sender: NSSwitch) {
        if (sender.state == .on) != app?.presenter.zoom { app?.toggleZoom() }
        reload()  // zoom may have been refused for want of permission
    }
    @objc private func drawChanged(_ sender: NSSwitch) {
        if (sender.state == .on) != app?.presenter.drawing { app?.toggleDrawing() }
    }

    @objc private func codeEditorsChanged(_ sender: NSSwitch) {
        if (sender.state == .on) != Settings.codeEditorsManual { app?.toggleCodeEditorsManual() }
    }

    @objc private func perAppChanged(_ sender: NSSwitch) {
        if (sender.state == .on) != Settings.perAppLanguage { app?.togglePerAppLanguage() }
    }

    @objc private func toggleBorder(_ sender: NSControl) {
        Settings.badgeBorder = (sender as? NSSwitch)?.state == .on
        preview.update()
        app?.refresh(animated: false)
    }

    @objc private func toggleLanguage(_ sender: NSSwitch) {
        let code = languages[sender.tag].code
        var hidden = Settings.hidden
        if sender.state == .on { hidden.remove(code) } else { hidden.insert(code) }
        Settings.hidden = hidden
        previewCode = code
        preview.code = code
        app?.refresh(animated: true)
    }

    @objc private func colorChanged(_ sender: NSColorWell) {
        let code = languages[sender.tag].code
        Palette.setColor(sender.color, for: code)
        resets[sender.tag].isEnabled = true
        previewCode = code
        preview.code = code
        preview.update()
        app?.refresh(animated: false)
    }

    @objc private func resetColor(_ sender: NSButton) {
        let code = languages[sender.tag].code
        Palette.setColor(nil, for: code)
        wells[sender.tag].color = Palette.color(for: code)
        sender.isEnabled = false
        previewCode = code
        preview.code = code
        preview.update()
        app?.refresh(animated: false)
    }

    @objc private func radiusChanged(_ sender: NSSlider) {
        app?.presenter.radius = CGFloat(sender.doubleValue)
        Settings.spotlightRadius = sender.doubleValue
        radiusValue.stringValue = tr("\(Int(sender.doubleValue)) pt", "\(Int(sender.doubleValue)) พอยต์")
    }

    @objc private func zoomChanged(_ sender: NSSlider) {
        app?.presenter.zoomLevel = CGFloat(sender.doubleValue)
        Settings.zoomLevel = sender.doubleValue
        zoomValue.stringValue = String(format: "%.1f×", sender.doubleValue)
    }

    /// Popups pass their selected item, like the menu does.
    @objc private func popupChanged(_ sender: NSPopUpButton) {
        guard let item = sender.selectedItem, let action = sender.identifier?.rawValue else { return }
        NSApp.sendAction(NSSelectorFromString(action), to: app, from: item)
        reload()
    }

    @objc private func appAction(_ sender: NSControl) {
        guard let action = sender.cell?.representedObject as? String else { return }
        NSApp.sendAction(NSSelectorFromString(action), to: app, from: sender)
        reload()
    }

    private func setShortcut(_ tool: PresentTool, _ s: Shortcut?) {
        if let s {
            if Shortcut.reserved.contains(s) {
                return shortcutFailed(tr("\(s.display) is taken while a tool is on. Pick another.", "\(s.display) ใช้ตอนเปิดเครื่องมืออยู่แล้ว เลือกคีย์อื่นนะ"))
            }
            if let other = PresentTool.allCases.first(where: { $0 != tool && Settings.shortcut($0) == s }) {
                return shortcutFailed(tr("\(s.display) is already used for \(other.title).", "\(s.display) ใช้กับ\(other.title)อยู่แล้ว"))
            }
        }
        let old = Settings.shortcut(tool)
        Settings.setShortcut(s, for: tool)
        let failed = app?.registerShortcuts() ?? []
        if let s, failed.contains(tool) {
            Settings.setShortcut(old, for: tool)
            _ = app?.registerShortcuts()
            return shortcutFailed(tr("Another app already uses \(s.display).", "มีแอปอื่นใช้ \(s.display) อยู่แล้ว"))
        }
        showShortcutStatus("")
        for (tool, field) in shortcutFields { field.shortcut = Settings.shortcut(tool) }
    }

    private func showShortcutStatus(_ message: String) {
        shortcutStatus.stringValue = message
        shortcutStatus.isHidden = message.isEmpty
    }

    private func shortcutFailed(_ message: String) {
        showShortcutStatus(message)
        NSSound.beep()
        for (tool, field) in shortcutFields { field.shortcut = Settings.shortcut(tool) }
    }


    @objc private func restoreShortcuts() {
        Settings.resetShortcuts()
        let failed = app?.registerShortcuts() ?? []
        showShortcutStatus(failed.isEmpty ? "" : tr("Another app already uses: ", "มีแอปอื่นใช้คีย์นี้อยู่แล้ว: ")
            + failed.map { $0.defaultShortcut.display }.joined(separator: ", "))
        for (tool, field) in shortcutFields { field.shortcut = Settings.shortcut(tool) }
    }

    func windowWillClose(_ notification: Notification) {
        window?.makeFirstResponder(nil)  // a shortcut box still listening lets go, and the shortcuts come back
        NSColorPanel.shared.close()
    }

    // MARK: Building blocks

    private func tab(_ views: [NSView]) -> NSView {
        for v in views where v is NSGridView || v is NSStackView { v.setContentHuggingPriority(.required, for: .horizontal) }
        let stack = NSStackView(views: views)
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 14
        stack.edgeInsets = NSEdgeInsets(top: 20, left: 24, bottom: 24, right: 24)
        stack.widthAnchor.constraint(greaterThanOrEqualToConstant: 520).isActive = true
        return stack
    }

    private func label(_ text: String) -> NSTextField { NSTextField(labelWithString: text) }

    private func header(_ text: String) -> NSTextField {
        let l = NSTextField(labelWithString: text)
        l.font = .boldSystemFont(ofSize: NSFont.systemFontSize)
        return l
    }

    /// A setting as a row: title and explanation on the left, its control on the right.
    private func optionRow(_ title: String, _ detail: String, _ control: NSView, detailWidth: CGFloat = 370) -> NSView {
        let detailText = caption(detail)
        detailText.preferredMaxLayoutWidth = detailWidth
        let text = NSStackView(views: [label(title), detailText])
        text.orientation = .vertical
        text.alignment = .leading
        text.spacing = 2
        let row = NSStackView()
        row.addView(text, in: .leading)
        row.addView(control, in: .trailing)
        row.alignment = .top
        row.widthAnchor.constraint(equalToConstant: 472).isActive = true
        return row
    }

    /// A key as printed on a keyboard, for the list of keys.
    private func keycap(_ text: String) -> NSView {
        let l = NSTextField(labelWithString: text)
        l.font = .systemFont(ofSize: NSFont.smallSystemFontSize, weight: .medium)
        let box = NSStackView(views: [l])
        box.edgeInsets = NSEdgeInsets(top: 2, left: 7, bottom: 2, right: 7)
        box.wantsLayer = true
        box.layer?.cornerRadius = 5
        box.layer?.borderWidth = 1
        box.layer?.borderColor = NSColor.separatorColor.cgColor
        box.layer?.backgroundColor = NSColor.quaternaryLabelColor.withAlphaComponent(0.15).cgColor
        let cell = NSStackView(views: [box])  // left-aligned in its column
        cell.widthAnchor.constraint(equalToConstant: 104).isActive = true
        cell.alignment = .centerY
        return cell
    }

    private func separator() -> NSView {
        let box = NSBox()
        box.boxType = .separator
        box.widthAnchor.constraint(equalToConstant: 472).isActive = true
        return box
    }

    /// Lines a caption up with the title of the radio button above it.
    private func indented(_ view: NSView) -> NSView {
        let s = NSStackView(views: [view])
        s.edgeInsets = NSEdgeInsets(top: 0, left: 20, bottom: 0, right: 0)
        return s
    }

    /// Explanations: a grey clearly softer than the titles, in light and dark mode.
    private static let captionColor = NSColor(name: nil) { look in
        look.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua ? NSColor(white: 0.6, alpha: 1) : NSColor(white: 0.45, alpha: 1)
    }

    /// A quiet one-liner under a heading.
    private func caption(_ text: String) -> NSTextField {
        let l = NSTextField(wrappingLabelWithString: text)
        l.textColor = Self.captionColor
        l.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        l.preferredMaxLayoutWidth = 470
        return l
    }

    private func toggle(_ on: Bool, _ action: Selector) -> NSSwitch {
        let s = NSSwitch()
        s.controlSize = .small
        s.state = on ? .on : .off
        s.target = self
        s.action = action
        return s
    }

    private func note(_ text: String) -> NSTextField {
        let l = NSTextField(wrappingLabelWithString: text)
        l.textColor = .secondaryLabelColor
        l.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        l.preferredMaxLayoutWidth = 470
        return l
    }

    private func line(_ views: NSView...) -> NSStackView { line(views) }
    private func line(_ views: [NSView]) -> NSStackView {
        let s = NSStackView(views: views)
        s.spacing = 8
        s.alignment = .firstBaseline
        return s
    }

    /// A popup whose items carry the value and the AppDelegate action, exactly like the menu's submenus.
    private func popup(_ options: [(String, String)], _ selected: String, _ action: Selector) -> NSPopUpButton {
        let p = NSPopUpButton(frame: .zero, pullsDown: false)
        for (value, title) in options {
            p.addItem(withTitle: title)
            p.lastItem?.representedObject = value
        }
        p.identifier = NSUserInterfaceItemIdentifier(NSStringFromSelector(action))
        p.selectItem(at: options.firstIndex { $0.0 == selected } ?? 0)
        p.target = self
        p.action = #selector(popupChanged(_:))
        return p
    }

    private func check(_ title: String, _ on: Bool, _ action: Selector, _ target: AnyObject) -> NSButton {
        let b = NSButton(checkboxWithTitle: title, target: self, action: #selector(appAction(_:)))
        b.cell?.representedObject = NSStringFromSelector(action)
        b.state = on ? .on : .off
        return b
    }

    private func button(_ title: String, _ action: Selector, _ target: AnyObject) -> NSButton {
        if target === self { return NSButton(title: title, target: self, action: action) }
        let b = NSButton(title: title, target: self, action: #selector(appAction(_:)))
        b.cell?.representedObject = NSStringFromSelector(action)
        return b
    }
}

/// Toolbar tabs, with the window resized to fit the tab that is showing.
final class SettingsTabs: NSTabViewController {
    override func tabView(_ tabView: NSTabView, didSelect item: NSTabViewItem?) {
        super.tabView(tabView, didSelect: item)
        view.window?.title = item?.label ?? ""
        fitWindow()
    }

    func fitWindow() {
        guard let window = view.window,
              let content = tabViewItems[selectedTabViewItemIndex].viewController?.view.subviews.first else { return }
        content.layoutSubtreeIfNeeded()
        let size = content.fittingSize
        var frame = window.frameRect(forContentRect: NSRect(origin: .zero, size: size))
        frame.origin = NSPoint(x: window.frame.minX, y: window.frame.maxY - frame.height)  // keep the top edge
        window.setFrame(frame, display: true, animate: window.isVisible)
    }
}

// MARK: - Launch at login

enum LoginItem {
    private static let agentLabel = "io.github.cursorlingo"
    private static var agentURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/LaunchAgents/\(agentLabel).plist")
    }

    static var isEnabled: Bool {
        if #available(macOS 13.0, *), SMAppService.mainApp.status == .enabled { return true }
        return FileManager.default.fileExists(atPath: agentURL.path)
    }

    static func set(_ on: Bool) {
        if #available(macOS 13.0, *) {
            do {
                if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
                if !on { try? FileManager.default.removeItem(at: agentURL) }
                return
            } catch {
                // Unsigned / relocated builds may be rejected; fall through to a LaunchAgent.
            }
        }
        if on {
            let plist: [String: Any] = [
                "Label": agentLabel,
                "ProgramArguments": ["/usr/bin/open", "-a", Bundle.main.bundlePath],
                "RunAtLoad": true,
            ]
            try? FileManager.default.createDirectory(at: agentURL.deletingLastPathComponent(),
                                                     withIntermediateDirectories: true)
            (plist as NSDictionary).write(to: agentURL, atomically: true)
        } else {
            try? FileManager.default.removeItem(at: agentURL)
        }
    }
}

// MARK: - App

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var panel: NSPanel!
    private let badge = BadgeView(frame: .zero)
    private var statusItem: NSStatusItem!
    private var monitors: [Any] = []
    private var hideWork: DispatchWorkItem?
    private var screenFrames: [NSRect] = []
    private var badgeSize = NSSize.zero
    private var visible = false
    let autoFix = AutoFix()
    private var autoFixRetry: Timer?
    let presenter = Presenter()
    private var presentationKeys: [HotKey] = []
    private let settingsWindow = SettingsWindow()

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildPanel()
        buildStatusItem()
        cacheScreens()

        let clicks: NSEvent.EventTypeMask = [.leftMouseDown, .rightMouseDown, .otherMouseDown]
        if let m = NSEvent.addGlobalMonitorForEvents(matching: clicks, handler: { [weak self] _ in
            self?.presenter.click(at: NSEvent.mouseLocation)
        }) {
            monitors.append(m)
        }
        if let m = NSEvent.addLocalMonitorForEvents(matching: clicks, handler: { [weak self] e in
            self?.presenter.click(at: NSEvent.mouseLocation)
            return e
        }) {
            monitors.append(m)
        }
        // In presentation mode, ⌃⌥ + scroll resizes the spotlight (the scroll still reaches the app underneath).
        let resize: (NSEvent) -> Void = { [weak self] e in
            guard let self, self.presenter.isOn, e.modifierFlags.contains([.control, .option]) else { return }
            self.presenter.radius -= e.scrollingDeltaY * (e.hasPreciseScrollingDeltas ? 1 : 8)
            Settings.spotlightRadius = Double(self.presenter.radius)
        }
        if let m = NSEvent.addGlobalMonitorForEvents(matching: .scrollWheel, handler: resize) { monitors.append(m) }
        if let m = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel, handler: { resize($0); return $0 }) {
            monitors.append(m)
        }
        presenter.radius = CGFloat(Settings.spotlightRadius)
        presenter.zoomLevel = CGFloat(Settings.zoomLevel)
        presenter.onChange = { [weak self] in
            guard let self else { return }
            Settings.presentation = self.presenter.spotlight
            self.settingsWindow.reload()
        }
        registerShortcuts()
        settingsWindow.app = self

        let mouseEvents: NSEvent.EventTypeMask = [.mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged]
        if let m = NSEvent.addGlobalMonitorForEvents(matching: mouseEvents, handler: { [weak self] _ in self?.follow() }) {
            monitors.append(m)
        }
        if let m = NSEvent.addLocalMonitorForEvents(matching: mouseEvents, handler: { [weak self] e in self?.follow(); return e }) {
            monitors.append(m)
        }

        DistributedNotificationCenter.default().addObserver(
            self, selector: #selector(inputSourceChanged),
            name: NSNotification.Name(kTISNotifySelectedKeyboardInputSourceChanged as String),
            object: nil, suspensionBehavior: .deliverImmediately)
        NotificationCenter.default.addObserver(
            self, selector: #selector(cacheScreens),
            name: NSApplication.didChangeScreenParametersNotification, object: nil)

        NSWorkspace.shared.notificationCenter.addObserver(
            self, selector: #selector(frontAppChanged(_:)),
            name: NSWorkspace.didActivateApplicationNotification, object: nil)
        // Sleep, lid close, screen lock and fast user switching can leave a stale word or a disabled tap behind.
        for name in [NSWorkspace.didWakeNotification, NSWorkspace.screensDidWakeNotification,
                     NSWorkspace.sessionDidBecomeActiveNotification] {
            NSWorkspace.shared.notificationCenter.addObserver(self, selector: #selector(systemResumed), name: name, object: nil)
        }
        DistributedNotificationCenter.default().addObserver(
            self, selector: #selector(systemResumed), name: NSNotification.Name("com.apple.screenIsUnlocked"), object: nil)

        autoFix.fixAsYouType = Settings.fixAsYouType
        autoFix.codeEditorsManual = Settings.codeEditorsManual
        autoFix.loadRules()
        refresh(animated: false)
        presenter.setSpotlight(Settings.presentation)
        startAutoFix()   // never prompts at launch; see enableAutoFix()
    }

    /// Starts Auto-fix if it is wanted. Until macOS grants the permissions, retries every 2 s so that it
    /// switches on by itself right after the user allows it, without a second trip to the menu.
    private func startAutoFix() {
        autoFixRetry?.invalidate()
        autoFixRetry = nil
        guard Settings.autoFix, !autoFix.start() else { return }
        autoFixRetry = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] t in
            guard let self, Settings.autoFix else { t.invalidate(); return }
            if self.autoFix.start() { t.invalidate(); self.autoFixRetry = nil }
        }
    }

    @objc private func frontAppChanged(_ note: Notification) {
        autoFix.reset()
        guard Settings.perAppLanguage,
              let app = note.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication,
              let id = app.bundleIdentifier, id != Bundle.main.bundleIdentifier else { return }
        // Each app gets back the layout it was last used with.
        if let wanted = appLanguages[id] {
            if wanted != InputSource.currentID() { InputSource.select(id: wanted) }
        } else {
            rememberAppLanguage()
        }
    }

    private lazy var appLanguages = Settings.appLanguages

    private func rememberAppLanguage() {
        guard let id = NSWorkspace.shared.frontmostApplication?.bundleIdentifier, id != Bundle.main.bundleIdentifier,
              let source = InputSource.currentID(), appLanguages[id] != source else { return }
        appLanguages[id] = source
        if appLanguages.count > 300 { appLanguages.removeValue(forKey: appLanguages.keys.first!) }
        Settings.appLanguages = appLanguages
    }

    @objc private func systemResumed() {
        autoFix.recover()
        if Settings.autoFix && !autoFix.isRunning { startAutoFix() }  // retries until the tap can be created again
        refresh(animated: false)
    }

    func applicationWillTerminate(_ notification: Notification) {
        monitors.forEach(NSEvent.removeMonitor)
    }

    // MARK: Badge

    private func buildPanel() {
        panel = NSPanel(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel],
                        backing: .buffered, defer: true)
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = Settings.badgeBorder  // the window shadow outlines the badge too, so it goes with the border
        panel.ignoresMouseEvents = true
        panel.hidesOnDeactivate = false
        panel.isReleasedWhenClosed = false
        panel.animationBehavior = .none
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        panel.level = NSWindow.Level(rawValue: Int(CGWindowLevelForKey(.assistiveTechHighWindow)))
        panel.contentView = badge
    }

    @objc private func cacheScreens() {
        screenFrames = NSScreen.screens.map(\.frame)
        presenter.screensChanged()
    }

    /// Repositions the badge under the pointer. Called for every mouse event, so it stays allocation-free.
    private func follow() {
        let p = NSEvent.mouseLocation
        presenter.follow(p)
        guard visible else { return }
        var frame = screenFrames.first(where: { $0.contains(p) }) ?? screenFrames.first ?? .zero
        if frame == .zero { frame = NSRect(x: -1e5, y: -1e5, width: 2e5, height: 2e5) }

        let s = badge.scale
        var x = p.x + 12 * s
        var y = p.y - 22 * s - badgeSize.height / 2
        if x + badgeSize.width > frame.maxX { x = p.x - 8 * s - badgeSize.width }   // flip left
        if y < frame.minY { y = p.y + 8 * s }                                         // flip up
        panel.setFrameOrigin(NSPoint(x: x.rounded(), y: y.rounded()))
    }

    @objc private func inputSourceChanged() {
        settingsWindow.inputChanged()
        if Settings.perAppLanguage { rememberAppLanguage() }
        refresh(animated: true)
    }

    func refresh(animated: Bool) {
        autoFix.inputChanged()
        let code = InputSource.currentCode()
        badge.code = code
        badge.style = Settings.style
        badge.scale = Settings.size.scale
        panel.hasShadow = Settings.badgeBorder
        panel.invalidateShadow()
        let size = BadgeView.size(style: Settings.style, scale: badge.scale, code: code)
        if size != badgeSize {
            badgeSize = size
            panel.setContentSize(size)
        }
        statusItem.button?.title = " " + code
        presenter.color = Palette.color(for: code)
        hideWork?.cancel()

        let wanted = !Settings.hidden.contains(code)
        guard wanted else { setVisible(false, animated: animated); return }

        switch Settings.mode {
        case .always:
            setVisible(true, animated: animated)
        case .onSwitch:
            guard animated else { setVisible(false, animated: false); return }
            setVisible(true, animated: true)
            let work = DispatchWorkItem { [weak self] in self?.setVisible(false, animated: true) }
            hideWork = work
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.2, execute: work)
        }
        if animated, visible { pulse() }
    }

    private func setVisible(_ on: Bool, animated: Bool) {
        if on {
            let wasVisible = visible
            visible = true
            follow()
            if !wasVisible {
                panel.alphaValue = animated ? 0 : 1
                panel.orderFrontRegardless()
                if animated { NSAnimationContext.runAnimationGroup { $0.duration = 0.12; panel.animator().alphaValue = 1 } }
            }
        } else if visible {
            visible = false
            guard animated else { panel.orderOut(nil); return }
            NSAnimationContext.runAnimationGroup({ $0.duration = 0.25; panel.animator().alphaValue = 0 },
                                                 completionHandler: { [weak self] in
                guard let self, !self.visible else { return }
                self.panel.orderOut(nil)
            })
        }
    }

    /// Brief flash so a language switch is noticeable even in "Always" mode.
    private func pulse() {
        panel.alphaValue = 0.35
        NSAnimationContext.runAnimationGroup { $0.duration = 0.18; panel.animator().alphaValue = 1 }
    }

    // MARK: Menu

    private func buildStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = statusItem.button {
            button.image = NSImage(systemSymbolName: "character.cursor.ibeam", accessibilityDescription: "CursorLingo")
            button.imagePosition = .imageLeading
            button.font = .monospacedSystemFont(ofSize: 11, weight: .semibold)
        }
        let menu = NSMenu()
        menu.delegate = self
        statusItem.menu = menu
    }

    /// The menu is rebuilt lazily each time it opens, so nothing menu-related lives in memory between uses.
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        let code = InputSource.currentCode()

        let header = NSMenuItem(title: tr("Current input: \(code)", "ภาษาที่ใช้อยู่: \(code)"), action: nil, keyEquivalent: "")
        header.isEnabled = false
        menu.addItem(header)

        menu.addItem(.separator())
        menu.addItem(presentationMenu())
        // One menu for the whole feature: Off, Double Shift Only (fix on request) or Automatic. The rest is in Settings.
        menu.addItem(submenu(tr("Fix Wrong Language (EN ⇄ TH): ", "แก้คำที่พิมพ์ผิดภาษา (EN ⇄ TH): ") + Settings.fixMode.title,
                             FixMode.allCases.map { ($0.rawValue, $0.title) }, selected: Settings.fixMode.rawValue,
                             #selector(pickFixMode)))
        if Settings.fixMode != .off {
            let status = autoFix.status
            let line = NSMenuItem(title: "    " + status.text, action: nil, keyEquivalent: "")
            line.isEnabled = false
            menu.addItem(line)
            if status.problem == .permission {
                menu.addItem(item("    " + tr("Open Privacy Settings…", "เปิด Privacy & Security…"), #selector(openPrivacySettings)))
            }
            if status.problem == .notRunning {
                menu.addItem(item("    " + tr("Relaunch CursorLingo", "เปิด CursorLingo ใหม่"), #selector(relaunch)))
            }
        }
        menu.addItem(.separator())
        menu.addItem(item(tr("Settings…", "ตั้งค่า…"), #selector(showSettings), key: ","))
        menu.addItem(item(tr("Quit CursorLingo", "ปิด CursorLingo"), #selector(quit), key: "q"))
    }

    private func item(_ title: String, _ action: Selector, on: Bool = false, key: String = "") -> NSMenuItem {
        let i = NSMenuItem(title: title, action: action, keyEquivalent: key)
        i.target = self
        i.state = on ? .on : .off
        return i
    }

    /// `options` are (stored value, title shown) pairs.
    private func submenu(_ title: String, _ options: [(String, String)], selected: String, _ action: Selector) -> NSMenuItem {
        let sub = NSMenu()
        for (value, label) in options {
            let i = item(label, action, on: value == selected)
            i.representedObject = value
            sub.addItem(i)
        }
        let parent = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        parent.submenu = sub
        return parent
    }

    @objc func pickUILanguage(_ s: NSMenuItem) {
        Settings.uiLanguage = UILanguage(rawValue: s.representedObject as? String ?? "") ?? .system
    }
    @objc func pickMode(_ s: NSMenuItem) {
        Settings.mode = DisplayMode(rawValue: s.representedObject as? String ?? "") ?? .always
        refresh(animated: Settings.mode == .onSwitch)
    }
    @objc func pickStyle(_ s: NSMenuItem) {
        Settings.style = BadgeStyle(rawValue: s.representedObject as? String ?? "") ?? .color
        refresh(animated: true)
    }
    @objc func pickSize(_ s: NSMenuItem) {
        Settings.size = BadgeSize(rawValue: s.representedObject as? String ?? "") ?? .medium
        refresh(animated: true)
    }
    @objc func pickFixMode(_ s: NSMenuItem) {
        let mode = FixMode(rawValue: s.representedObject as? String ?? "") ?? .off
        guard mode != .off else {
            Settings.autoFix = false
            autoFixRetry?.invalidate()
            autoFix.stop()
            return
        }
        Settings.fixAsYouType = mode == .automatic
        autoFix.fixAsYouType = Settings.fixAsYouType
        if !Settings.autoFix { enableAutoFix() }
    }
    private func enableAutoFix() {
        Settings.autoFix = true   // stays on while waiting for permission; the menu shows what is missing
        startAutoFix()
        if autoFix.isRunning { return }

        AutoFix.requestPermission()
        let alert = NSAlert()
        alert.messageText = tr("Allow keyboard access to fix the wrong language",
                               "ขอสิทธิ์เข้าถึงแป้นพิมพ์ เพื่อแก้คำที่พิมพ์ผิดภาษา")
        alert.informativeText = tr(
            "CursorLingo reads each word you type, in memory only, to notice words typed in the wrong language. "
                + "Turn CursorLingo on under Privacy & Security → Input Monitoring and → Accessibility. "
                + "It starts by itself once both are allowed (if not, choose Relaunch from the menu). "
                + "Only words you convert with double Shift are remembered, on this Mac; nothing is sent anywhere.",
            "CursorLingo จะอ่านคำที่คุณพิมพ์ในหน่วยความจำเท่านั้น เพื่อดูว่าคำไหนพิมพ์ผิดภาษา "
                + "โปรดเปิดสิทธิ์ให้ CursorLingo ใน System Settings → Privacy & Security → Input Monitoring และ Accessibility "
                + "เมื่ออนุญาตครบแล้วจะเริ่มทำงานเอง (ถ้ายังไม่ทำงาน ให้เลือก “เปิด CursorLingo ใหม่” จากเมนู) "
                + "คำที่ถูกจำไว้มีเฉพาะคำที่คุณสั่งแก้ด้วย Shift 2 ครั้ง เก็บในเครื่องนี้เท่านั้น และไม่มีการส่งข้อมูลออกไปไหน")
        alert.addButton(withTitle: tr("Open Settings", "เปิด System Settings"))
        alert.addButton(withTitle: tr("Later", "ไว้ทีหลัง"))
        NSApp.activate(ignoringOtherApps: true)
        if alert.runModal() == .alertFirstButtonReturn { openPrivacySettings() }
    }
    @objc func openPrivacySettings() {
        let pane = CGPreflightListenEventAccess() ? "Privacy_Accessibility" : "Privacy_ListenEvent"
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(pane)") {
            NSWorkspace.shared.open(url)
        }
    }
    @objc func relaunch() {
        // Start a fresh copy once this one has quit (a running copy makes a new one exit immediately).
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", "sleep 1; /usr/bin/open \"$0\"", Bundle.main.bundlePath]
        try? p.run()
        NSApp.terminate(nil)
    }
    @objc func toggleCodeEditorsManual() {
        Settings.codeEditorsManual.toggle()
        autoFix.codeEditorsManual = Settings.codeEditorsManual
    }
    @objc func forgetLearned() { autoFix.forgetRules() }
    /// The tools' shortcuts (⌃⌥H, ⌃⌥Z, ⌃⌥D unless changed) work from anywhere. Returns the tools whose shortcut
    /// another app already has.
    @discardableResult
    func registerShortcuts() -> [PresentTool] {
        presentationKeys = []  // a key can't be registered twice: let the old ones go first
        var failed: [PresentTool] = []
        for tool in PresentTool.allCases {
            guard let s = Settings.shortcut(tool) else { continue }
            let key = HotKey(keyCode: s.keyCode, modifiers: s.modifiers) { [weak self] in self?.toggle(tool) }
            if key.registered { presentationKeys.append(key) } else { failed.append(tool) }
        }
        return failed
    }
    private func toggle(_ tool: PresentTool) {
        switch tool {
        case .spotlight: toggleSpotlight()
        case .zoom: toggleZoom()
        case .draw: toggleDrawing()
        }
    }
    /// While a shortcut box in Settings listens, the keys must reach it instead of toggling a tool.
    func pauseShortcuts(_ paused: Bool) {
        if paused { presentationKeys = [] } else { registerShortcuts() }
    }
    @objc private func showShortcuts() { settingsWindow.show(.presentation) }
    @objc private func showSettings() { settingsWindow.show() }
    @objc func toggleSpotlight() { presenter.setSpotlight(!presenter.spotlight) }
    @objc func toggleZoom() { presenter.setZoom(!presenter.zoom) }
    @objc func toggleDrawing() { presenter.setDrawing(!presenter.drawing) }
    @objc func openScreenRecordingSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture") {
            NSWorkspace.shared.open(url)
        }
    }

    /// Spotlight, Zoom and Draw, with their shortcuts (shown as hints; they work anywhere).
    private func presentationMenu() -> NSMenuItem {
        let sub = NSMenu()
        let tools: [(PresentTool, Selector, Bool)] = [
            (.spotlight, #selector(toggleSpotlight), presenter.spotlight),
            (.zoom, #selector(toggleZoom), presenter.zoom),
            (.draw, #selector(toggleDrawing), presenter.drawing),
        ]
        for (tool, action, on) in tools {
            let i = item(tool.title, action, on: on)
            if let s = Settings.shortcut(tool) {
                i.keyEquivalent = s.keyEquivalent
                i.keyEquivalentModifierMask = s.modifierFlags
            }
            sub.addItem(i)
        }
        sub.autoenablesItems = false
        sub.items[1].isEnabled = Presenter.zoomAvailable
        if presenter.zoomBlocked && !CGPreflightScreenCaptureAccess() {
            let line = NSMenuItem(title: "    " + tr("Zoom needs Screen Recording permission",
                                                    "ซูมต้องได้รับสิทธิ์ Screen Recording ก่อน"), action: nil, keyEquivalent: "")
            line.isEnabled = false
            sub.addItem(line)
            sub.addItem(item("    " + tr("Open Privacy Settings…", "เปิด Privacy & Security…"), #selector(openScreenRecordingSettings)))
        }
        sub.addItem(.separator())
        for hint in [tr("⌃⌥ + Scroll: Resize the Circle", "⌃⌥ + เลื่อนล้อเมาส์: ปรับขนาดวง"),
                     tr("⌃⌥ = / −: Zoom In / Out", "⌃⌥ = / −: ซูมเข้า / ออก"),
                     tr("⌘Z: Undo Last Stroke", "⌘Z: ลบเส้นล่าสุด"),
                     tr("Esc: Stop Drawing, Then Exit", "Esc: เลิกวาด กดอีกครั้งเพื่อออก")] {
            let i = NSMenuItem(title: hint, action: nil, keyEquivalent: "")
            i.isEnabled = false
            sub.addItem(i)
        }
        sub.addItem(.separator())
        sub.addItem(item(tr("Shortcuts…", "ตั้งคีย์ลัด…"), #selector(showShortcuts)))
        let top = NSMenuItem(title: tr("Presentation", "นำเสนอ"), action: nil, keyEquivalent: "")
        top.state = presenter.isOn || presenter.drawing ? .on : .off
        top.submenu = sub
        return top
    }
    @objc func togglePerAppLanguage() {
        Settings.perAppLanguage.toggle()
        if Settings.perAppLanguage { rememberAppLanguage() }
    }
    @objc func reportIssue() {
        // Only opens the issue form; nothing typed is ever attached.
        if let url = URL(string: "https://github.com/kittipongint/cursor-lingo/issues/new/choose") { NSWorkspace.shared.open(url) }
    }
    @objc func toggleLogin() { LoginItem.set(!LoginItem.isEnabled) }
    @objc private func quit() { NSApp.terminate(nil) }
}

// Single instance: if another copy is already running, quit.
let me = Bundle.main.bundleIdentifier ?? "io.github.cursorlingo"
if NSRunningApplication.runningApplications(withBundleIdentifier: me).count > 1 { exit(0) }

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let delegate = AppDelegate()
app.delegate = delegate
app.run()
