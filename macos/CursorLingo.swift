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
    /// Bundle ID -> input source ID it was last used with (for perAppLanguage).
    static var appLanguages: [String: String] {
        get { d.dictionary(forKey: "appLanguages") as? [String: String] ?? [:] }
        set { d.set(newValue, forKey: "appLanguages") }
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

    static func color(for code: String) -> NSColor {
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
            NSColor.white.withAlphaComponent(0.9).setFill()
            NSBezierPath(ovalIn: r.insetBy(dx: -1, dy: -1)).fill()
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

        // Hairline border keeps the badge readable on any background.
        (style == .dark ? accent : NSColor.black.withAlphaComponent(0.18)).setStroke()
        pill.lineWidth = style == .dark ? 1 : 0.5
        pill.stroke()

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
                guard let k = UInt8(String(decoding: c[i...i+1], as: UTF8.self), radix: 16) else { return nil }
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
    private let autoFix = AutoFix()
    private var autoFixRetry: Timer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildPanel()
        buildStatusItem()
        cacheScreens()

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
        panel.hasShadow = true
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
    }

    /// Repositions the badge under the pointer. Called for every mouse event, so it stays allocation-free.
    private func follow() {
        guard visible else { return }
        let p = NSEvent.mouseLocation
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
        if Settings.perAppLanguage { rememberAppLanguage() }
        refresh(animated: true)
    }

    private func refresh(animated: Bool) {
        autoFix.inputChanged()
        let code = InputSource.currentCode()
        badge.code = code
        badge.style = Settings.style
        badge.scale = Settings.size.scale
        let size = BadgeView.size(style: Settings.style, scale: badge.scale, code: code)
        if size != badgeSize {
            badgeSize = size
            panel.setContentSize(size)
        }
        statusItem.button?.title = " " + code
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

        let hideCurrent = item(tr("Hide badge for \(code)", "ซ่อนป้ายเมื่อเป็น \(code)"), #selector(toggleHideCurrent), on: Settings.hidden.contains(code))
        menu.addItem(hideCurrent)
        if !Settings.hidden.isEmpty {
            menu.addItem(item(tr("Show for all languages", "แสดงป้ายทุกภาษา"), #selector(clearHidden)))
        }
        menu.addItem(.separator())

        menu.addItem(submenu(tr("Show", "แสดงป้าย"), DisplayMode.allCases.map { ($0.rawValue, $0.title) },
                             selected: Settings.mode.rawValue, #selector(pickMode)))
        menu.addItem(submenu(tr("Style", "รูปแบบ"), BadgeStyle.allCases.map { ($0.rawValue, $0.title) },
                             selected: Settings.style.rawValue, #selector(pickStyle)))
        menu.addItem(submenu(tr("Size", "ขนาด"), BadgeSize.allCases.map { ($0.rawValue, $0.title) },
                             selected: Settings.size.rawValue, #selector(pickSize)))
        menu.addItem(.separator())
        // One menu for the whole feature: Off, Double Shift Only (fix on request) or Automatic.
        let fixItem = submenu(tr("Fix Wrong Language (EN ⇄ TH): ", "แก้คำที่พิมพ์ผิดภาษา (EN ⇄ TH): ") + Settings.fixMode.title,
                              FixMode.allCases.map { ($0.rawValue, $0.title) }, selected: Settings.fixMode.rawValue,
                              #selector(pickFixMode))
        let fix = fixItem.submenu!
        fix.addItem(.separator())
        let editors = item(tr("In Code Editors: Double Shift Only", "ใน code editor แก้เฉพาะตอนกด Shift 2 ครั้ง"),
                           #selector(toggleCodeEditorsManual), on: Settings.codeEditorsManual)
        editors.isEnabled = Settings.fixMode == .automatic
        fix.addItem(editors)
        let learned = autoFix.rules.count
        let forget = item(learned == 0 ? tr("Learned Words: None Yet", "ยังไม่มีคำที่จำไว้")
                                       : tr("Forget \(learned) Learned Word\(learned == 1 ? "" : "s")", "ล้างคำที่จำไว้ (\(learned) คำ)"),
                          #selector(forgetLearned))
        forget.isEnabled = learned > 0
        fix.addItem(forget)
        menu.addItem(fixItem)
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
        menu.addItem(item(tr("Remember Language per App", "จำภาษาของแต่ละแอป"), #selector(togglePerAppLanguage),
                          on: Settings.perAppLanguage))
        menu.addItem(item(tr("Launch at Login", "เปิดพร้อมเครื่อง"), #selector(toggleLogin), on: LoginItem.isEnabled))
        menu.addItem(.separator())
        // Bilingual on purpose, so it can be found whichever language the menu is in.
        menu.addItem(submenu("Language / ภาษา", UILanguage.allCases.map { ($0.rawValue, $0.title) },
                             selected: Settings.uiLanguage.rawValue, #selector(pickUILanguage)))
        menu.addItem(item(tr("Report a Wrong Fix or Bug…", "แจ้งปัญหาหรือคำที่แก้ผิด…"), #selector(reportIssue)))
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

    @objc private func pickUILanguage(_ s: NSMenuItem) {
        Settings.uiLanguage = UILanguage(rawValue: s.representedObject as? String ?? "") ?? .system
    }
    @objc private func pickMode(_ s: NSMenuItem) {
        Settings.mode = DisplayMode(rawValue: s.representedObject as? String ?? "") ?? .always
        refresh(animated: Settings.mode == .onSwitch)
    }
    @objc private func pickStyle(_ s: NSMenuItem) {
        Settings.style = BadgeStyle(rawValue: s.representedObject as? String ?? "") ?? .color
        refresh(animated: true)
    }
    @objc private func pickSize(_ s: NSMenuItem) {
        Settings.size = BadgeSize(rawValue: s.representedObject as? String ?? "") ?? .medium
        refresh(animated: true)
    }
    @objc private func toggleHideCurrent() {
        let code = InputSource.currentCode()
        var h = Settings.hidden
        if h.contains(code) { h.remove(code) } else { h.insert(code) }
        Settings.hidden = h
        refresh(animated: true)
    }
    @objc private func clearHidden() {
        Settings.hidden = []
        refresh(animated: true)
    }
    @objc private func pickFixMode(_ s: NSMenuItem) {
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
    @objc private func openPrivacySettings() {
        let pane = CGPreflightListenEventAccess() ? "Privacy_Accessibility" : "Privacy_ListenEvent"
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(pane)") {
            NSWorkspace.shared.open(url)
        }
    }
    @objc private func relaunch() {
        // Start a fresh copy once this one has quit (a running copy makes a new one exit immediately).
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", "sleep 1; /usr/bin/open \"$0\"", Bundle.main.bundlePath]
        try? p.run()
        NSApp.terminate(nil)
    }
    @objc private func toggleCodeEditorsManual() {
        Settings.codeEditorsManual.toggle()
        autoFix.codeEditorsManual = Settings.codeEditorsManual
    }
    @objc private func forgetLearned() { autoFix.forgetRules() }
    @objc private func togglePerAppLanguage() {
        Settings.perAppLanguage.toggle()
        if Settings.perAppLanguage { rememberAppLanguage() }
    }
    @objc private func reportIssue() {
        // Only opens the issue form; nothing typed is ever attached.
        if let url = URL(string: "https://github.com/kittipongint/cursor-lingo/issues/new/choose") { NSWorkspace.shared.open(url) }
    }
    @objc private func toggleLogin() { LoginItem.set(!LoginItem.isEnabled) }
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
