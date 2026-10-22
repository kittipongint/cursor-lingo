// CursorLingo — shows the current keyboard input language next to the mouse pointer.
//
// Design goals:
//   * Event-driven: no polling timers. The badge moves on mouse events and redraws
//     only when the input source changes.
//   * Tiny footprint: one borderless click-through panel, one status item, no network.
//   * Keyboard monitoring exists only for the optional "Auto-fix wrong language" feature. It is
//     off by default, needs explicit macOS permission, and keeps nothing but the word being typed.
//   * Optional launch at login (SMAppService, with a LaunchAgent fallback).

import AppKit
import Carbon
import ServiceManagement

// MARK: - Settings

enum BadgeStyle: String, CaseIterable {
    case color = "Color", light = "Light", dark = "Dark", dot = "Dot"
}

enum DisplayMode: String, CaseIterable {
    case always = "Always"
    case onSwitch = "Only when switching"
}

enum BadgeSize: String, CaseIterable {
    case small = "Small", medium = "Medium", large = "Large"
    var scale: CGFloat { self == .small ? 0.85 : (self == .large ? 1.3 : 1.0) }
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
        let wanted = language == 0 ? enIDs : [thID]
        guard let list = TISCreateInputSourceList(nil, false)?.takeRetainedValue() as? [TISInputSource] else { return }
        for id in wanted {
            for source in list {
                guard let raw = TISGetInputSourceProperty(source, kTISPropertyInputSourceID),
                      (Unmanaged<CFString>.fromOpaque(raw).takeUnretainedValue() as String) == id else { continue }
                TISSelectInputSource(source)
                return
            }
        }
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
/// Only the current and the last word are kept, in memory; nothing is logged or stored, and nothing is observed
/// while secure input (password fields) is on.
final class AutoFix {
    private static let marker: Int64 = 0x434C47  // tags our own synthetic events so we never react to them
    private static let terminals: Set<String> = [
        "com.apple.Terminal", "com.googlecode.iterm2", "com.mitchellh.ghostty", "org.alacritty",
        "com.github.wez.wezterm", "net.kovidgoyal.kitty", "dev.warp.Warp-Stable", "co.zeit.hyper",
    ]

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
    private var ignored: [[UInt8]] = []  // words whose fix the user undid (this session only)
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

    /// What the menu shows under the Auto-fix item, so a silent failure is never invisible.
    var status: String {
        if !CGPreflightListenEventAccess() { return "Needs Input Monitoring permission" }
        if !CGPreflightPostEventAccess() { return "Needs Accessibility permission" }
        if tap == nil { return "Not running yet: relaunch CursorLingo" }
        if IsSecureEventInputEnabled() { return "Paused: secure input (a password field) is on" }
        if InputSource.fixLanguage() == nil { return "Paused: this layout isn't supported" }
        return "Active · double-tap Shift to convert or undo"
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
    }

    private func handle(_ type: CGEventType, _ event: CGEvent) {
        switch type {
        case .tapDisabledByTimeout, .tapDisabledByUserInput:
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
        if !word.decided, word.shown == 0, word.keys.count >= 6, !isIgnored(word.keys, prefix: true),
           lf_should_convert(word.keys, Int32(word.keys.count), 0, false) {
            convert(&word)
        }
    }

    private func endOfWord() {
        var w = word
        w.trailing = 1
        word = Word()
        if !w.decided, w.keys.count >= 2, !isIgnored(w.keys, prefix: false),
           !AutoFix.terminals.contains(NSWorkspace.shared.frontmostApplication?.bundleIdentifier ?? ""),
           lf_should_convert(w.keys, Int32(w.keys.count), Int32(w.shown), true) {
            convert(&w)
        }
        last = w
    }

    /// Double Shift: flip the word being typed, or else the last word. Flipping a word back to the layout it was
    /// typed in means the user wants it as typed, so it is never auto-fixed again this session.
    private func toggle() {
        if !word.keys.isEmpty {
            convert(&word)
            if word.shown == word.typed { ignore(word.keys) }
        } else if var w = last {
            convert(&w)
            if w.shown == w.typed { ignore(w.keys) }
            last = w
        }
    }

    private func isIgnored(_ keys: [UInt8], prefix: Bool) -> Bool {
        ignored.contains { prefix ? $0.starts(with: keys) : $0 == keys }
    }

    private func ignore(_ keys: [UInt8]) {
        if !isIgnored(keys, prefix: false) { ignored.append(keys) }
        if ignored.count > 200 { ignored.removeFirst() }
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

    private func post(keyCode: CGKeyCode, unicode: UInt16? = nil) {
        for down in [true, false] {
            guard let e = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: down) else { continue }
            e.setIntegerValueField(.eventSourceUserData, value: AutoFix.marker)
            e.flags = []
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
            self, selector: #selector(frontAppChanged),
            name: NSWorkspace.didActivateApplicationNotification, object: nil)

        refresh(animated: false)
        startAutoFix()   // never prompts at launch; see toggleAutoFix()
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

    @objc private func frontAppChanged() { autoFix.reset() }

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

    @objc private func inputSourceChanged() { refresh(animated: true) }

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

        let header = NSMenuItem(title: "Current input: \(code)", action: nil, keyEquivalent: "")
        header.isEnabled = false
        menu.addItem(header)

        let hideCurrent = item("Hide badge for \(code)", #selector(toggleHideCurrent), on: Settings.hidden.contains(code))
        menu.addItem(hideCurrent)
        if !Settings.hidden.isEmpty {
            menu.addItem(item("Show for all languages", #selector(clearHidden)))
        }
        menu.addItem(.separator())

        menu.addItem(submenu("Show", DisplayMode.allCases.map(\.rawValue), selected: Settings.mode.rawValue, #selector(pickMode)))
        menu.addItem(submenu("Style", BadgeStyle.allCases.map(\.rawValue), selected: Settings.style.rawValue, #selector(pickStyle)))
        menu.addItem(submenu("Size", BadgeSize.allCases.map(\.rawValue), selected: Settings.size.rawValue, #selector(pickSize)))
        menu.addItem(.separator())
        menu.addItem(item("Auto-fix wrong language (EN ⇄ TH)", #selector(toggleAutoFix), on: Settings.autoFix))
        if Settings.autoFix {
            let status = autoFix.status
            let line = NSMenuItem(title: "    " + status, action: nil, keyEquivalent: "")
            line.isEnabled = false
            menu.addItem(line)
            if status.hasPrefix("Needs") { menu.addItem(item("    Open Privacy Settings…", #selector(openPrivacySettings))) }
            if status.hasPrefix("Not running") { menu.addItem(item("    Relaunch CursorLingo", #selector(relaunch))) }
        }
        menu.addItem(item("Launch at Login", #selector(toggleLogin), on: LoginItem.isEnabled))
        menu.addItem(.separator())
        menu.addItem(item("Quit CursorLingo", #selector(quit), key: "q"))
    }

    private func item(_ title: String, _ action: Selector, on: Bool = false, key: String = "") -> NSMenuItem {
        let i = NSMenuItem(title: title, action: action, keyEquivalent: key)
        i.target = self
        i.state = on ? .on : .off
        return i
    }

    private func submenu(_ title: String, _ options: [String], selected: String, _ action: Selector) -> NSMenuItem {
        let sub = NSMenu()
        for o in options {
            let i = item(o, action, on: o == selected)
            i.representedObject = o
            sub.addItem(i)
        }
        let parent = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        parent.submenu = sub
        return parent
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
    @objc private func toggleAutoFix() {
        if Settings.autoFix {
            Settings.autoFix = false
            autoFixRetry?.invalidate()
            autoFix.stop()
            return
        }
        Settings.autoFix = true   // stays on while waiting for permission; the menu shows what is missing
        startAutoFix()
        if autoFix.isRunning { return }

        AutoFix.requestPermission()
        let alert = NSAlert()
        alert.messageText = "Allow keyboard access for Auto-fix"
        alert.informativeText = "CursorLingo reads each word you type, in memory only, to notice words typed in the wrong "
            + "language. Turn CursorLingo on under Privacy & Security → Input Monitoring and → Accessibility. "
            + "Auto-fix starts by itself once both are allowed (if not, choose Relaunch from the menu). "
            + "Nothing is stored or sent anywhere."
        alert.addButton(withTitle: "Open Settings")
        alert.addButton(withTitle: "Later")
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
