// Menu-bar presence for modore.
//
// modore runs with `NSApplication.setActivationPolicy(.accessory)` — no Dock
// tile, no main window. Without a status item there is *no* visual signal
// that the host is alive, which is confusing: "is it running? did it crash?
// did my hotkey just collide with another app?". This file is the answer.
//
// Visible surface:
//   - "ﾓﾄﾞﾚ" (half-width katakana spelling of "modore") in the menu bar.
//     Half-width over full-width because the menu bar is height-constrained
//     and full-width "モドレ" gets visually cramped next to neighboring
//     items. Plain text so it template-renders for dark/light mode without
//     an image asset.
//   - A menu listing the live hotkey, which delivery path is in use
//     (Carbon system grab vs CGEventTap fallback), and Edit/Reveal config
//     shortcuts plus Quit.
//
// All UI work runs on the main thread. The two callers — boot in main.swift
// and `applyConversionHotkeyChord` (invoked from the main-queue config
// watcher and the main-thread Carbon handler) — are both already on main,
// so `refresh(...)` can be called directly without a dispatch.

import Cocoa

final class ModoreStatusItem: NSObject {

    private let item: NSStatusItem
    private let hotkeyMenuItem: NSMenuItem
    /// Shown only when `[conversion] katakana_modifier` is bound — surfaces
    /// the second chord so the user can see at a glance which combination
    /// forces katakana. Hidden when no secondary chord is active.
    private let katakanaMenuItem: NSMenuItem
    /// Shown only when `[conversion] cycle_modifier` is bound — surfaces
    /// the chord that cycles to the next Mozc candidate. Hidden when
    /// cycle is disabled or its modifier collides.
    private let cycleMenuItem: NSMenuItem
    private let deliveryMenuItem: NSMenuItem
    /// Shown only while SecureInput is held by another app — surfaces *why*
    /// the hotkey is silently failing in password fields / sudo prompts.
    /// See SecureInputMonitor.swift for the detection path.
    private let secureInputMenuItem: NSMenuItem
    /// "Last reload: <kind> at HH:MM:SS" — populated only after the first
    /// fs-watcher fires. Hidden at boot so the user doesn't see a stale
    /// "never" line in the menu.
    private let reloadMenuItem: NSMenuItem

    /// Pending revert timer for the title flash. Held so a second reload
    /// firing during the flash window cancels the first revert instead of
    /// stomping the new flash state.
    private var reloadFlashTimer: DispatchSourceTimer?

    /// Latest hotkey description, kept here so the tooltip can be rebuilt
    /// after toggling between blocked / clear without losing the hotkey
    /// label.
    private var currentHotkeyLabel: String = "—"

    /// Last-known frontmost app id and whether a script matched. We keep
    /// the bool so SecureInput state changes can rebuild the title without
    /// re-querying the script engine on the main thread.
    private var currentAppHasScript: Bool = false

    /// Latest SecureInput holder name (nil = clear). Owned here rather than
    /// computed from the menu-item visibility so `applyTitleStyling` has a
    /// single source of truth across SecureInput / script-association.
    private var secureInputBlocker: String? = nil

    override init() {
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        hotkeyMenuItem      = NSMenuItem(title: "Hotkey: —",   action: nil, keyEquivalent: "")
        katakanaMenuItem    = NSMenuItem(title: "",            action: nil, keyEquivalent: "")
        cycleMenuItem       = NSMenuItem(title: "",            action: nil, keyEquivalent: "")
        deliveryMenuItem    = NSMenuItem(title: "Delivery: —", action: nil, keyEquivalent: "")
        secureInputMenuItem = NSMenuItem(title: "",            action: nil, keyEquivalent: "")
        reloadMenuItem      = NSMenuItem(title: "",            action: nil, keyEquivalent: "")
        super.init()

        item.button?.title = "ﾓﾄﾞﾚ"
        item.button?.toolTip = "modore — Japanese conversion (running)"

        let menu = NSMenu()
        hotkeyMenuItem.isEnabled = false
        katakanaMenuItem.isEnabled = false
        katakanaMenuItem.isHidden = true
        cycleMenuItem.isEnabled = false
        cycleMenuItem.isHidden = true
        deliveryMenuItem.isEnabled = false
        secureInputMenuItem.isEnabled = false
        secureInputMenuItem.isHidden = true
        reloadMenuItem.isEnabled = false
        reloadMenuItem.isHidden = true
        menu.addItem(hotkeyMenuItem)
        menu.addItem(katakanaMenuItem)
        menu.addItem(cycleMenuItem)
        menu.addItem(deliveryMenuItem)
        menu.addItem(secureInputMenuItem)
        menu.addItem(reloadMenuItem)
        menu.addItem(NSMenuItem.separator())

        // ⌘, is the macOS-standard "open this app's preferences" shortcut;
        // for modore that's the config file.
        let editItem = NSMenuItem(
            title: "Edit config…",
            action: #selector(handleEditConfig),
            keyEquivalent: ",")
        editItem.target = self
        menu.addItem(editItem)

        let revealItem = NSMenuItem(
            title: "Reveal config in Finder",
            action: #selector(handleRevealConfig),
            keyEquivalent: "")
        revealItem.target = self
        menu.addItem(revealItem)

        // modore-host logs via NSLog → Apple unified logging, not a file on
        // disk (see docs/PARITY.md). Export pulls the last 24h via
        // `log show --process modore-host` so bug reports have something to
        // attach.
        let openLogItem = NSMenuItem(
            title: "Open log in editor",
            action: #selector(handleOpenLog),
            keyEquivalent: "")
        openLogItem.target = self
        menu.addItem(openLogItem)

        let exportLogItem = NSMenuItem(
            title: "Export log…",
            action: #selector(handleExportLog),
            keyEquivalent: "")
        exportLogItem.target = self
        menu.addItem(exportLogItem)

        menu.addItem(NSMenuItem.separator())

        let quitItem = NSMenuItem(
            title: "Quit modore",
            action: #selector(NSApplication.terminate(_:)),
            keyEquivalent: "q")
        menu.addItem(quitItem)

        item.menu = menu

        // Frontmost-app tracking: tint the title blue while a per-app
        // script is associated with whatever the user is currently in.
        let nc = NSWorkspace.shared.notificationCenter
        nc.addObserver(
            self,
            selector: #selector(handleFrontmostAppChanged(_:)),
            name: NSWorkspace.didActivateApplicationNotification,
            object: nil)
        // Seed with the current frontmost so the title is correct before
        // the user switches apps for the first time.
        refreshScriptAssociation(
            appId: NSWorkspace.shared.frontmostApplication?.bundleIdentifier)
    }

    @objc private func handleFrontmostAppChanged(_ note: Notification) {
        let app = note.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication
        refreshScriptAssociation(appId: app?.bundleIdentifier)
    }

    /// Re-query the script engine for the current frontmost app. Called
    /// when scripts are added/removed mid-session so the title indicator
    /// updates without waiting for the next app switch.
    func refreshScriptAssociationForFrontmost() {
        refreshScriptAssociation(
            appId: NSWorkspace.shared.frontmostApplication?.bundleIdentifier)
    }

    private func refreshScriptAssociation(appId: String?) {
        let has = (appId.map { ModoreScript.hasScript(forAppId: $0) }) ?? false
        currentAppHasScript = has
        applyTitleStyling()
    }

    /// Resolve the menu-bar title style from the host's current state.
    /// Priority: SecureInput red > scripted-app blue > plain. Centralised
    /// so both the SecureInput watcher and the frontmost-app observer can
    /// trigger it without duplicating the precedence logic.
    private func applyTitleStyling() {
        guard let button = item.button else { return }
        let base = "ﾓﾄﾞﾚ"
        if let name = secureInputBlocker {
            button.attributedTitle = NSAttributedString(
                string: base,
                attributes: [.foregroundColor: NSColor.systemRed])
            button.toolTip = "modore — blocked by \(name) (secure keyboard entry)"
        } else if currentAppHasScript {
            button.attributedTitle = NSAttributedString(
                string: base,
                attributes: [.foregroundColor: NSColor.systemBlue])
            button.toolTip = "modore — \(currentHotkeyLabel) (script active)"
        } else {
            // Setting `attributedTitle` to an empty string and re-setting
            // `title` is the documented way to clear the styled override —
            // assigning `title` alone leaves the previous attributed run
            // in place on some macOS versions.
            button.attributedTitle = NSAttributedString(string: "")
            button.title = base
            button.toolTip = "modore — \(currentHotkeyLabel) (running)"
        }
    }

    /// Update the live menu after a chord change or Carbon-registration flip.
    /// Pass `nil` for any derived chord that isn't bound — the corresponding
    /// menu line is hidden. Main-thread only.
    func refresh(
        hotkey: ModoreConfig.ConversionHotkey,
        usingCarbonHotkey: Bool,
        katakanaChord: ModoreConfig.ConversionHotkey? = nil,
        cycleChord: ModoreConfig.ConversionHotkey? = nil
    ) {
        currentHotkeyLabel = hotkey.displayName
        hotkeyMenuItem.title = "Hotkey: \(hotkey.displayName)"
        if let katakana = katakanaChord {
            katakanaMenuItem.title = "Katakana: \(katakana.displayName)"
            katakanaMenuItem.isHidden = false
        } else {
            katakanaMenuItem.title = ""
            katakanaMenuItem.isHidden = true
        }
        if let cycle = cycleChord {
            cycleMenuItem.title = "Cycle next: \(cycle.displayName)"
            cycleMenuItem.isHidden = false
        } else {
            cycleMenuItem.title = ""
            cycleMenuItem.isHidden = true
        }
        deliveryMenuItem.title = "Delivery: " + (usingCarbonHotkey
            ? "Carbon (system grab)"
            : "CGEventTap (fallback)")
        applyTitleStyling()
    }

    /// Surface a filesystem-watcher reload (config or scripts) in the menu
    /// bar so the user gets confirmation that their edit landed. Two
    /// signals: a transient green-tinted "ﾓﾄﾞﾚ ↻" title flash that auto-
    /// reverts after 1.2 s, and a persistent "Last reload: <kind> at HH:MM"
    /// menu line. Main-thread only — fired from the ConfigWatcher's
    /// debounced callback, which already runs on the main queue.
    func flashReload(kind: String) {
        let stamp = Self.reloadTimeFormatter.string(from: Date())
        reloadMenuItem.title = "Last reload: \(kind) at \(stamp)"
        reloadMenuItem.isHidden = false

        // Skip the title flash while SecureInput red is showing — that
        // signal is more important and shouldn't be visually overridden.
        // The menu-item line still updates so the reload isn't lost.
        guard secureInputBlocker == nil, let button = item.button else { return }
        let title = "ﾓﾄﾞﾚ ↻"
        button.attributedTitle = NSAttributedString(
            string: title,
            attributes: [.foregroundColor: NSColor.systemGreen])
        button.toolTip = "modore — reloaded \(kind) at \(stamp)"

        reloadFlashTimer?.cancel()
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now() + .milliseconds(1200))
        t.setEventHandler { [weak self] in
            self?.reloadFlashTimer = nil
            self?.applyTitleStyling()
        }
        reloadFlashTimer = t
        t.resume()
    }

    private static let reloadTimeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        f.locale = Locale(identifier: "en_US_POSIX")
        return f
    }()

    /// Reflect the SecureInput watcher's state in the menu bar:
    ///
    ///  - `nil` → title back to plain "ﾓﾄﾞﾚ", warning menu item hidden,
    ///    tooltip restored to the running state.
    ///  - non-nil → title gets a red attributed-string treatment so the
    ///    user notices the change in their peripheral vision, the warning
    ///    menu item shows which app is holding SecureInput, and the
    ///    tooltip reports the blocker explicitly.
    ///
    /// Called from SecureInputMonitor on the main queue. Main-thread only.
    func setSecureInputBlocked(by appName: String?) {
        secureInputBlocker = appName
        if let name = appName {
            secureInputMenuItem.title = "⚠ Blocked by \(name) (secure keyboard entry)"
            secureInputMenuItem.isHidden = false
        } else {
            secureInputMenuItem.title = ""
            secureInputMenuItem.isHidden = true
        }
        applyTitleStyling()
    }

    // MARK: - Menu actions

    @objc private func handleEditConfig() {
        let url = ModoreConfig.configFileURL()
        if !FileManager.default.fileExists(atPath: url.path) {
            // First-run convenience: materialize a minimal config with the
            // default hotkey so the user has something concrete to edit.
            let dir = url.deletingLastPathComponent()
            try? FileManager.default.createDirectory(
                at: dir, withIntermediateDirectories: true)
            let seed = """
                [conversion]
                hotkey = Cmd+Semicolon

                # macOS only — clipboard fallback timings (defaults shown).
                # See docs/configuration.md → [clipboard].
                # [clipboard]
                # pre_copy_delay_ms = 20
                # read_timeout_ms = 250
                # restore_clipboard_delay_ms = 50
                """
            try? seed.write(to: url, atomically: true, encoding: .utf8)
        }
        NSWorkspace.shared.open(url)
    }

    /// Dump the last 24h of modore-host's unified-log output to a file the
     /// user picks via NSSavePanel. macOS NSLog goes to the unified log, not
     /// to `~/.config/modore/modore.log`, so without this menu item the only
     /// way to attach logs to a bug report is `log show` in Terminal — and
     /// users who hit "the hotkey didn't work" rarely have that workflow.
    @objc private func handleExportLog() {
        let panel = NSSavePanel()
        let stamp = Self.exportTimestampFormatter.string(from: Date())
        panel.nameFieldStringValue = "modore-host-\(stamp).log"
        panel.allowedContentTypes = []
        panel.canCreateDirectories = true
        panel.title = "Export modore host log"
        panel.message = "Save the last 24 hours of modore-host logs."
        // .accessory activation policy keeps modore out of the Dock, which
        // also keeps NSSavePanel from auto-focusing — bring the app forward
        // so the sheet doesn't appear behind whatever the user was editing.
        NSApp.activate(ignoringOtherApps: true)
        guard panel.runModal() == .OK, let dest = panel.url else { return }

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let ok = Self.runLogShow(to: dest, last: "24h")
            DispatchQueue.main.async {
                if ok {
                    NSWorkspace.shared.activateFileViewerSelecting([dest])
                } else {
                    self?.presentExportFailureAlert(at: dest)
                }
            }
        }
    }

    /// Dump the last hour of logs to a temp file and hand it to the user's
    /// default text editor via `NSWorkspace.open`. Cheaper than Export when
    /// the user just wants to skim — no save panel, no Finder round-trip.
    /// 1h instead of 24h because `log show` over 24h scans the whole unified
    /// log store; for the "what just happened?" use case 1h is plenty and
    /// the dump returns in well under a second.
    @objc private func handleOpenLog() {
        let stamp = Self.exportTimestampFormatter.string(from: Date())
        let dest = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("modore-host-\(stamp).log")
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let ok = Self.runLogShow(to: dest, last: "1h")
            DispatchQueue.main.async {
                if ok {
                    NSWorkspace.shared.open(dest)
                } else {
                    self?.presentExportFailureAlert(at: dest)
                }
            }
        }
    }

    private static let exportTimestampFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyyMMdd-HHmmss"
        f.locale = Locale(identifier: "en_US_POSIX")
        return f
    }()

    /// Runs `log show` filtered to modore's own subsystem, streaming stdout
    /// directly to `dest`. Returns true on a clean exit.
    ///
    /// The `subsystem == "com.modore.host"` predicate is what makes this
    /// fast: without it `--info --debug` would pull every AppKit / XPC /
    /// CoreAnalytics line the process touched (≈25k lines for a single day,
    /// mostly noise) and `log show` takes tens of seconds. Filtering to our
    /// subsystem drops that to a few hundred relevant lines and seconds of
    /// wall-clock. `--info --debug` is kept so future lower-level traces
    /// still surface — Log.swift currently emits at default level, but the
    /// flags cost nothing once the predicate is in place.
    private static func runLogShow(to dest: URL, last: String) -> Bool {
        FileManager.default.createFile(atPath: dest.path, contents: nil)
        guard let handle = try? FileHandle(forWritingTo: dest) else {
            return false
        }
        defer { try? handle.close() }

        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/log")
        task.arguments = [
            "show",
            "--predicate", "subsystem == \"\(Log.subsystem)\"",
            "--info", "--debug",
            "--last", last,
            "--style", "compact",
        ]
        task.standardOutput = handle
        task.standardError = Pipe()  // discard stderr noise
        do {
            try task.run()
        } catch {
            return false
        }
        task.waitUntilExit()
        return task.terminationStatus == 0
    }

    private func presentExportFailureAlert(at dest: URL) {
        let alert = NSAlert()
        alert.messageText = "Couldn't export logs"
        alert.informativeText =
            "`log show` failed. The output (if any) is at:\n\(dest.path)\n\n" +
            "You can also run this in Terminal:\n" +
            "log show --predicate 'subsystem == \"\(Log.subsystem)\"' --info --debug --last 24h"
        alert.alertStyle = .warning
        alert.addButton(withTitle: "OK")
        alert.runModal()
    }

    @objc private func handleRevealConfig() {
        let url = ModoreConfig.configFileURL()
        if FileManager.default.fileExists(atPath: url.path) {
            NSWorkspace.shared.activateFileViewerSelecting([url])
        } else {
            // No file yet — open the containing folder so the user can
            // create one in place. `activateFileViewerSelecting` would
            // silently fall back to home if the URL doesn't exist.
            let dir = url.deletingLastPathComponent()
            try? FileManager.default.createDirectory(
                at: dir, withIntermediateDirectories: true)
            NSWorkspace.shared.open(dir)
        }
    }
}
