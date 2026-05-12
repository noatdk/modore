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
    private let deliveryMenuItem: NSMenuItem

    override init() {
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        hotkeyMenuItem   = NSMenuItem(title: "Hotkey: —",   action: nil, keyEquivalent: "")
        deliveryMenuItem = NSMenuItem(title: "Delivery: —", action: nil, keyEquivalent: "")
        super.init()

        item.button?.title = "ﾓﾄﾞﾚ"
        item.button?.toolTip = "modore — Japanese conversion (running)"

        let menu = NSMenu()
        hotkeyMenuItem.isEnabled = false
        deliveryMenuItem.isEnabled = false
        menu.addItem(hotkeyMenuItem)
        menu.addItem(deliveryMenuItem)
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

        menu.addItem(NSMenuItem.separator())

        let quitItem = NSMenuItem(
            title: "Quit modore",
            action: #selector(NSApplication.terminate(_:)),
            keyEquivalent: "q")
        menu.addItem(quitItem)

        item.menu = menu
    }

    /// Update the live menu after a chord change or Carbon-registration flip.
    /// Main-thread only.
    func refresh(hotkey: ModoreConfig.ConversionHotkey, usingCarbonHotkey: Bool) {
        hotkeyMenuItem.title = "Hotkey: \(hotkey.displayName)"
        deliveryMenuItem.title = "Delivery: " + (usingCarbonHotkey
            ? "Carbon (system grab)"
            : "CGEventTap (fallback)")
        item.button?.toolTip = "modore — \(hotkey.displayName) (running)"
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
