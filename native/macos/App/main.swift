// modore — macOS native host.
//
// Runs as a menu-bar/accessory app, registers a global conversion hotkey
// (default Ctrl+/; configurable in ~/.config/modore/modore.conf), reads
// the focused text field (any app) via Accessibility, hands the picked span
// to the in-process Mozc engine, and writes the result back.
//
// This file is intentionally thin — it only holds bootstrap-only globals,
// the CLI flag dispatch, and the launch sequence. The pickup pipeline lives
// in Pickup.swift; hotkey + reload state in HotkeyState.swift; the rest is
// split by concern (AccessibilityIO, ClipboardSupport, SyntheticEvents,
// EventTap, Preflight). Re-read this file to understand the *order* things
// happen at launch; re-read the others to understand *what* each step does.
//
// Build:  make -C native/macos
// Run:    open native/macos/build/modore.app
// First run prompts for Accessibility permission. Grant it in
//   System Settings → Privacy & Security → Accessibility, then re-launch.

import Cocoa
import Carbon
import ApplicationServices

// MARK: - Bootstrap-owned globals
//
// Lifetime: created during bootstrap, never reassigned. State that mutates
// at runtime (current chord, clipboard timings, modifier setting) lives in
// HotkeyState.swift.

// Where the mozc engine keeps its on-disk state (user dictionary, history).
// Bootstrapping this from a user's existing Google Japanese Input / OSS Mozc
// profile is a follow-up task.
let MOZC_PROFILE_DIR: String = {
    let home = FileManager.default.homeDirectoryForCurrentUser.path
    return "\(home)/Library/Application Support/modore"
}()

/// Menu-bar item showing "running" state + current hotkey. Created late in
/// boot so its initial refresh sees the post-Carbon-registration values.
/// `applyConversionHotkeyChord` calls `refresh` on every chord update.
var gStatusItem: ModoreStatusItem?

/// Polls `IOConsoleUsers` for the SecureInput holder pid (sudo / password
/// fields / Lock Screen). Held for the lifetime of the process; tears down
/// its DispatchSourceTimer on deinit. Initialized after the status item so
/// the very first transition has somewhere to land.
var gSecureInputMonitor: SecureInputMonitor?

func describeSelf() {
    let bundle = Bundle.main
    Log.boot("pid=\(ProcessInfo.processInfo.processIdentifier)")
    Log.boot("executable=\(bundle.executablePath ?? "?")")
    Log.boot("bundle id=\(bundle.bundleIdentifier ?? "(no bundle)")")
    Log.boot("bundle path=\(bundle.bundlePath)")
}

// MARK: - CLI flag dispatch (no-side-effect preflights)
//
// All of these short-circuit before the main app starts — they read state,
// print, exit. Order matters only insofar as the dispatch is first-match;
// each flag is independent and produces deterministic output suitable for
// pasting into bug reports or branching on in CI / pre-commit hooks.

if CommandLine.arguments.contains("--check-config") {
    runConfigCheck()
}

// `--secure-input-status` is a one-shot diagnostic: query the IORegistry
// once, print who (if anyone) is holding SecureInput, and exit. Same code
// path the long-running watcher uses, so a "it works here but the menu bar
// doesn't update" report can be triaged in one command.
if CommandLine.arguments.contains("--secure-input-status") {
    if let pid = SecureInputMonitor.currentHolderPid() {
        let desc = SecureInputMonitor.describeProcess(pid: pid)
        print("secure input: held by pid \(pid)")
        if let desc = desc {
            print("  app: \(desc.name)")
            print("  path: \(desc.path)")
        } else {
            print("  app: unknown (pid path unreadable)")
        }
        exit(1)
    } else {
        print("secure input: clear")
        exit(0)
    }
}

// `--print-config-path` prints only the resolved conversion-config path on
// stdout (no labels, no extra lines) so it composes with the shell:
//
//     $EDITOR "$(modore-host --print-config-path)"
//
// `--print-paths` prints every path the host knows about, labeled — useful
// for poking around or for pasting into bug reports.
if CommandLine.arguments.contains("--print-config-path") {
    print(ModoreConfig.configFileURL().path)
    exit(0)
}
if CommandLine.arguments.contains("--print-paths") {
    print("config:        \(ModoreConfig.configFileURL().path)")
    print("mozc profile:  \(MOZC_PROFILE_DIR)")
    print("bundle:        \(Bundle.main.bundlePath)")
    print("executable:    \(Bundle.main.executablePath ?? "?")")
    exit(0)
}

// MARK: - Main launch sequence
//
// Order is load-bearing:
//
//   1. NSApplication exists before we touch any AppKit (status item, etc.).
//   2. Boot guards (translocation, root) bail out before we do anything
//      side-effecting that the user would have to undo.
//   3. Config load before tap install — the tap callback reads `gConversion*`.
//   4. Accessibility prompt before tap install — the tap *needs* AX trust.
//   5. Carbon hotkey after tap install so the tap-fallback path is ready
//      if Carbon fails.
//   6. Status item after Carbon registration so the first refresh shows
//      the right delivery path.
//   7. Mozc bridge init last — slow, blocking, and not safe to retry; if
//      it fails we exit rather than ship a half-broken host.

let app = NSApplication.shared
app.setActivationPolicy(.accessory)

describeSelf()
BootGuards.enforce()

let modoreHotkey = ModoreConfig.loadConversionHotkey()
gConversionKeyCode = modoreHotkey.keyCode
gConversionCoreFlags = modoreHotkey.coreFlags

gKatakanaModifier = ModoreConfig.loadKatakanaModifier()
Log.config("katakana modifier: \(gKatakanaModifier.displayName)")

gCycleModifier = ModoreConfig.loadCycleModifier()
Log.config("cycle modifier: \(gCycleModifier.displayName)")

gCycleFromUndone = ModoreConfig.loadCycleFromUndone()
Log.config("cycle_from_undone: \(gCycleFromUndone.displayName)")

gUndoWindowMs = ModoreConfig.loadUndoWindowMs()
Log.config(gUndoWindowMs == 0
    ? "undo window: disabled (undo_window_ms = 0)"
    : "undo window: \(gUndoWindowMs)ms")

gCandidatePanelMode = ModoreConfig.loadCandidatePanelMode()
Log.config("candidate panel: \(gCandidatePanelMode.displayName)")

gCandidatePanelDurationMs = ModoreConfig.loadCandidatePanelDurationMs()
Log.config(gCandidatePanelDurationMs == 0
    ? "candidate panel duration: no auto-hide (sticks for session)"
    : "candidate panel duration: \(gCandidatePanelDurationMs)ms")

gClipboardTimings = ModoreConfig.loadClipboardTimings()
Log.config("clipboard timings: pre_copy=\(gClipboardTimings.preCopyDelayMs)ms"
         + " read_timeout=\(gClipboardTimings.readTimeoutMs)ms"
         + " restore=\(gClipboardTimings.restoreClipboardDelayMs)ms")

// First, check silently. If not trusted, prompt the user. macOS will add the
// bundle to the Accessibility list at this point.
if !isTrusted(prompt: false) {
    Log.ax("not trusted yet — requesting Accessibility permission.")
    Log.ax("look for 'modore' in:")
    Log.ax("  System Settings → Privacy & Security → Accessibility")
    Log.ax("if it does NOT appear, click the '+' button and add:")
    Log.ax("  \(Bundle.main.bundlePath)")
    Log.ax("then quit and re-launch this host.")
    _ = isTrusted(prompt: true)
} else {
    Log.ax("trusted")
}

if !installEventTap() {
    Log.hotkey("failed to create event tap — Accessibility permission missing?")
    exit(1)
}

// Install Carbon hotkey grab. If it succeeds the tap's hotkey-match branch
// becomes a no-op (gated by `gUsingCarbonHotkey`); if it fails we fall back
// to the tap's existing behavior with no functional change for the user.
gCarbonHotkey = CarbonHotkey()
if gCarbonHotkey?.register(role: "primary", chord: modoreHotkey, onFire: {
    kHotkeyTapQueue.async { doPickup() }
}) == true {
    gUsingCarbonHotkey = true
    Log.hotkey("Carbon hotkey registered — using RegisterEventHotKey for delivery")
} else {
    Log.hotkey("Carbon hotkey unavailable — using CGEventTap fallback")
}

// Derived chords (katakana, cycle) — bound after the primary so their
// callbacks can reuse the same dispatch queue. Each is a no-op when its
// modifier is `.none` or collides with another modifier in use.
applyKatakanaSecondaryChord(primary: modoreHotkey, modifier: gKatakanaModifier)
applyCycleChord(primary: modoreHotkey, cycle: gCycleModifier, katakana: gKatakanaModifier)

// Menu-bar item. Created after Carbon registration so the very first
// refresh shows the correct delivery path; subsequent refreshes come from
// `applyConversionHotkeyChord` on every chord change.
gStatusItem = ModoreStatusItem()
gStatusItem?.refresh(
    hotkey: modoreHotkey,
    usingCarbonHotkey: gUsingCarbonHotkey,
    katakanaChord: secondaryChordForStatus(primary: modoreHotkey),
    cycleChord: cycleChordForStatus(primary: modoreHotkey))
Log.boot("status item installed in menu bar")

// SecureInput watcher. Starts polling immediately so a password prompt
// already on screen at boot is reflected in the menu bar within ~1 s. The
// closure runs on the main queue (see SecureInputMonitor.swift), so it can
// touch the status item directly.
gSecureInputMonitor = SecureInputMonitor { state in
    switch state {
    case .blocked(let appName, let appPath, let pid):
        Log.secureInput("acquired by '\(appName)' pid=\(pid) path=\(appPath)")
        gStatusItem?.setSecureInputBlocked(by: appName)
    case .clear:
        Log.secureInput("released")
        gStatusItem?.setSecureInputBlocked(by: nil)
    }
}
gSecureInputMonitor?.start()

// Held for the lifetime of the process; tears down its DispatchSource on deinit.
let gConfigWatcher = ConfigWatcher(path: ModoreConfig.configFileURL().path) {
    applyConfigReload()
}
gConfigWatcher.start()

do {
    try MozcBridge.initialize(userProfileDir: MOZC_PROFILE_DIR)
    Log.mozc("bridge initialized (profile=\(MOZC_PROFILE_DIR))")
} catch {
    Log.mozc("bridge init FAILED: \(String(describing: error))")
    exit(1)
}

Log.boot("ready: conversion hotkey installed (see ~/.config/modore/modore.conf)")
app.run()
