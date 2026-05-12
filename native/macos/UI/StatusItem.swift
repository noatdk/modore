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

    /// Latest hotkey description, kept here so the tooltip can be rebuilt
    /// after toggling between blocked / clear without losing the hotkey
    /// label.
    private var currentHotkeyLabel: String = "—"

    override init() {
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        hotkeyMenuItem      = NSMenuItem(title: "Hotkey: —",   action: nil, keyEquivalent: "")
        katakanaMenuItem    = NSMenuItem(title: "",            action: nil, keyEquivalent: "")
        cycleMenuItem       = NSMenuItem(title: "",            action: nil, keyEquivalent: "")
        deliveryMenuItem    = NSMenuItem(title: "Delivery: —", action: nil, keyEquivalent: "")
        secureInputMenuItem = NSMenuItem(title: "",            action: nil, keyEquivalent: "")
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
        menu.addItem(hotkeyMenuItem)
        menu.addItem(katakanaMenuItem)
        menu.addItem(cycleMenuItem)
        menu.addItem(deliveryMenuItem)
        menu.addItem(secureInputMenuItem)
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
        // Only rewrite the tooltip if SecureInput isn't currently blocking;
        // the blocked tooltip is more important to keep visible.
        if secureInputMenuItem.isHidden {
            item.button?.toolTip = "modore — \(hotkey.displayName) (running)"
        }
    }

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
        guard let button = item.button else { return }
        if let name = appName {
            let attrs: [NSAttributedString.Key: Any] = [
                .foregroundColor: NSColor.systemRed,
            ]
            button.attributedTitle = NSAttributedString(string: "ﾓﾄﾞﾚ", attributes: attrs)
            button.toolTip = "modore — blocked by \(name) (secure keyboard entry)"
            secureInputMenuItem.title = "⚠ Blocked by \(name) (secure keyboard entry)"
            secureInputMenuItem.isHidden = false
        } else {
            // Setting `attributedTitle` to an empty string and re-setting
            // `title` is the documented way to clear the styled override —
            // assigning `title` alone leaves the previous attributed run
            // in place on some macOS versions.
            button.attributedTitle = NSAttributedString(string: "")
            button.title = "ﾓﾄﾞﾚ"
            button.toolTip = "modore — \(currentHotkeyLabel) (running)"
            secureInputMenuItem.title = ""
            secureInputMenuItem.isHidden = true
        }
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
                hotkey = Ctrl+Slash

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
            let ok = Self.runLogShow(to: dest)
            DispatchQueue.main.async {
                if ok {
                    NSWorkspace.shared.activateFileViewerSelecting([dest])
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

    /// Runs `log show --process modore-host --info --debug --last 24h`,
    /// streaming stdout directly to `dest`. Returns true on a clean exit.
    /// `--info --debug` so we capture the Df-level lines (clipboard/pickup/
    /// ax/cycle traces) — without those flags `log show` defaults to
    /// default-level only, which strips out almost everything modore writes.
    private static func runLogShow(to dest: URL) -> Bool {
        FileManager.default.createFile(atPath: dest.path, contents: nil)
        guard let handle = try? FileHandle(forWritingTo: dest) else {
            return false
        }
        defer { try? handle.close() }

        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/log")
        task.arguments = [
            "show",
            "--process", "modore-host",
            "--info", "--debug",
            "--last", "24h",
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
            "log show --process modore-host --info --debug --last 24h"
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
