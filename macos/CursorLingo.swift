// CursorLingo — shows the current keyboard input language next to the mouse pointer.
//
// Design goals:
//   * Event-driven: no polling timers. The badge moves on mouse events and redraws
//     only when the input source changes.
//   * Tiny footprint: one borderless click-through panel, one status item, no
//     keyboard monitoring, no network.
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

    @objc private func inputSourceChanged() { refresh(animated: true) }

    private func refresh(animated: Bool) {
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
