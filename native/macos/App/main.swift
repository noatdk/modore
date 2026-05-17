// modore — macOS native host.
//
// Runs as a menu-bar/accessory app, registers a global conversion hotkey
// (configurable in ~/.config/modore/modore.conf), reads
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

/// Bridge backend selected from config at boot. Immutable for process
/// lifetime; changing it on disk requires a restart because the bridge is
/// initialized once and the backend owns long-lived engine/session state.
let MOZC_BACKEND: ModoreConfig.MozcBackend = ModoreConfig.loadMozcBackend()

/// Bridge tuning that maps onto env vars before bridge init.
func applyBridgeRuntimeEnv(_ runtime: ModoreConfig.BridgeRuntime) {
    setenv("MODORE_MOZC_CANDIDATE_MIXING_MODE", "\(runtime.candidateMixingMode)", 1)
    Log.config("bridge env: MODORE_MOZC_CANDIDATE_MIXING_MODE=\(runtime.candidateMixingMode)")

    if runtime.traceRawCandidates {
        setenv("MODORE_BRIDGE_TRACE_RAW_CANDIDATES", "1", 1)
    } else {
        unsetenv("MODORE_BRIDGE_TRACE_RAW_CANDIDATES")
    }
    Log.config("bridge env: MODORE_BRIDGE_TRACE_RAW_CANDIDATES=\(runtime.traceRawCandidates ? "1" : "0")")
}

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
    print("mozc backend:  \(MOZC_BACKEND.displayName)")
    print("bundle:        \(Bundle.main.bundlePath)")
    print("executable:    \(Bundle.main.executablePath ?? "?")")
    exit(0)
}

if CommandLine.arguments.contains("--print-shell-bootstrap") {
    do {
        let hotkey = ModoreConfig.loadConversionHotkey().displayName
        let hostPath = Bundle.main.executablePath
        Log.shell("printing bootstrap hotkey=\(hotkey) host=\(hostPath ?? "?")")
        let bootstrap = try MozcBridge.shellBootstrap(
            hotkeyDisplayName: hotkey,
            hostExecutablePath: hostPath)
        Log.shell("bootstrap script:\n\(bootstrap)")
        Log.shell("bootstrap bytes=\(bootstrap.utf8.count)")
        print(bootstrap, terminator: "")
        exit(0)
    } catch {
        let msg = "shell bootstrap failed: \(error)\n"
        FileHandle.standardError.write(msg.data(using: .utf8) ?? Data())
        exit(1)
    }
}

if CommandLine.arguments.contains("--shell-convert") {
    let caretArg: String? = {
        if let caretEq = CommandLine.arguments.first(where: { $0.hasPrefix("--caret=") }) {
            return String(caretEq.dropFirst("--caret=".count))
        }
        if let caretIdx = CommandLine.arguments.firstIndex(of: "--caret"),
           caretIdx + 1 < CommandLine.arguments.count {
            return CommandLine.arguments[caretIdx + 1]
        }
        return nil
    }()
    let caretUTF16: Int
    let target: MozcBridge.ConvertTarget = CommandLine.arguments.contains("--katakana")
        ? .katakana
        : .kanji
    let stdinData = FileHandle.standardInput.readDataToEndOfFile()
    let input = String(data: stdinData, encoding: .utf8) ?? ""
    if let caretArg, let caret = Int(caretArg) {
        caretUTF16 = caret
    } else {
        caretUTF16 = input.utf16.count
    }
    do {
        let sessionEnv = ProcessInfo.processInfo.environment["MODORE_SHELL_SESSION"] ?? ""
        let sessionLabel = sessionEnv.isEmpty ? "<none>" : sessionEnv
        let targetLabel = target == .katakana ? "katakana" : "primary"
        Log.shell("cli entry session=\(sessionLabel) mode=\(targetLabel) caret=\(caretUTF16) bytes=\(input.utf8.count)")
        let response = try MozcBridge.shellConvertViaLiveHost(input, caretUTF16: caretUTF16, target: target)
        if let split = response.firstIndex(of: "\n") {
            let session = String(response[..<split])
            let converted = String(response[response.index(after: split)...])
            Log.shell("cli exit session=\(session) bytes=\(converted.utf8.count)")
        } else {
            Log.shell("cli exit malformed bytes=\(response.utf8.count)")
        }
        print(response, terminator: "")
        exit(0)
    } catch {
        let msg = "shell convert failed: \(error)\n"
        FileHandle.standardError.write(msg.data(using: .utf8) ?? Data())
        exit(1)
    }
}

if let probeArg = CommandLine.arguments.first(where: { $0.hasPrefix("--probe-words=") }) {
    let probeWords = String(probeArg.dropFirst("--probe-words=".count))
    let probeBackend = CommandLine.arguments.first(where: { $0.hasPrefix("--probe-backend=") })
        .map { String($0.dropFirst("--probe-backend=".count)) }
    let probeLogPath = CommandLine.arguments.first(where: { $0.hasPrefix("--probe-log=") })
        .map { String($0.dropFirst("--probe-log=".count)) }
    func probeWrite(_ line: String) {
        print(line)
        guard let probeLogPath, !probeLogPath.isEmpty else { return }
        let data = (line + "\n").data(using: .utf8) ?? Data()
        if FileManager.default.fileExists(atPath: probeLogPath) {
            if let handle = try? FileHandle(forWritingTo: URL(fileURLWithPath: probeLogPath)) {
                try? handle.seekToEnd()
                try? handle.write(contentsOf: data)
                try? handle.close()
            }
        } else {
            try? data.write(to: URL(fileURLWithPath: probeLogPath))
        }
    }
    let words = probeWords
        .split(whereSeparator: { $0 == "," || $0 == "\n" || $0 == "\t" || $0 == " " })
        .map(String.init)
        .filter { !$0.isEmpty }
    if words.isEmpty {
        probeWrite("MODORE_PROBE_WORDS was set but empty")
        exit(2)
    }
    if let probeBackend, !probeBackend.isEmpty {
        setenv("MODORE_MOZC_BACKEND", probeBackend, /*overwrite=*/1)
    }
    do {
        try MozcBridge.initialize(userProfileDir: MOZC_PROFILE_DIR)
    } catch {
        probeWrite("bridge init failed: \(error)")
        exit(1)
    }
    defer { MozcBridge.shutdown() }
    for word in words {
        do {
            let result = try MozcBridge.convertWithCandidates(word, target: .kanji, maxCandidates: 8)
            probeWrite("INPUT=\(word) committed=\(result.committed)")
            for (idx, cand) in result.candidates.enumerated() {
                probeWrite("  [\(idx)] \(cand.value)")
            }
        } catch {
            probeWrite("INPUT=\(word) error=\(error)")
        }
    }
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

gDisabledLogNamespaces = ModoreConfig.loadDisabledLoggingNamespaces()
Log.configureDisabledNamespaces(gDisabledLogNamespaces)

describeSelf()
BootGuards.enforce()

let modoreHotkey = ModoreConfig.loadConversionHotkey()
gConversionKeyCode = modoreHotkey.keyCode
gConversionCoreFlags = modoreHotkey.coreFlags

gKatakanaModifier = ModoreConfig.loadKatakanaModifier()
Log.config("katakana modifier: \(gKatakanaModifier.displayName)")

gKatakanaModifierBehavior = ModoreConfig.loadKatakanaModifierBehavior()
Log.config("katakana modifier behavior: \(gKatakanaModifierBehavior.displayName)")

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

Log.config("mozc backend: \(MOZC_BACKEND.displayName)")

gBridgeRuntime = ModoreConfig.loadBridgeRuntime()
applyBridgeRuntimeEnv(gBridgeRuntime)

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
    kHotkeyTapQueue.async { handlePrimaryHotkeyTrigger() }
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
    gStatusItem?.flashReload(kind: "config")
}
gConfigWatcher.start()

// Mirror watcher for the scripts directory. Per-script content edits
// reload via the engine's mtime poll, but added/removed script files
// only show up if the engine re-scans the directory — that's this
// watcher's job. Assigned after the scripts dir is resolved below.
var gScriptsWatcher: ConfigWatcher? = nil

do {
    setenv("MODORE_MOZC_BACKEND", MOZC_BACKEND.envValue, 1)
    try MozcBridge.initialize(userProfileDir: MOZC_PROFILE_DIR)
    Log.mozc("bridge initialized (backend=\(MOZC_BACKEND.displayName), profile=\(MOZC_PROFILE_DIR))")
} catch {
    Log.mozc("bridge init FAILED: \(String(describing: error))")
    exit(1)
}
do {
    Log.shell("starting convert server path=\(shellConvertSocketPath())")
    try MozcBridge.startShellConvertServer()
} catch {
    Log.shell("convert server disabled: \(String(describing: error))")
}

// ML classifier for romaji/ASCII segmentation. Opt-in via
// `[conversion] classifier = on` in modore.conf.
gClassifierEnabled = ModoreConfig.loadClassifierEnabled()
if gClassifierEnabled {
    let configDir: String = {
        if let xdg = ProcessInfo.processInfo.environment["XDG_CONFIG_HOME"], !xdg.isEmpty {
            return "\(xdg)/modore"
        }
        return "\(FileManager.default.homeDirectoryForCurrentUser.path)/.config/modore"
    }()
    let modelPath = "\(configDir)/classifier.mdl"
    if FileManager.default.fileExists(atPath: modelPath) {
        if Classifier.load(modelPath: modelPath) {
            Log.boot("ML classifier loaded from \(modelPath)")
        } else {
            Log.boot("ML classifier: failed to load \(modelPath) — heuristic fallback")
            gClassifierEnabled = false
        }
    } else if let bundled = Bundle.main.path(forResource: "classifier", ofType: "mdl") {
        if Classifier.load(modelPath: bundled) {
            Log.boot("ML classifier loaded from bundle")
        } else {
            Log.boot("ML classifier: failed to load bundle model — heuristic fallback")
            gClassifierEnabled = false
        }
    } else {
        Log.boot("ML classifier: model not found — heuristic fallback")
        gClassifierEnabled = false
    }
} else {
    Log.boot("ML classifier: disabled (set [conversion] classifier = on to enable)")
}

// Lua scripting engine. Loaded after Mozc so any startup log lines from
// scripts (`modore.log.*` at top level) interleave below the rest of the
// boot sequence. Engine is opt-in via the presence of files in the dir;
// an empty/missing dir is a no-op and the host runs in pass-through.
let scriptsDir: String = {
    if let xdg = ProcessInfo.processInfo.environment["XDG_CONFIG_HOME"], !xdg.isEmpty {
        return "\(xdg)/modore/scripts"
    }
    return "\(FileManager.default.homeDirectoryForCurrentUser.path)/.config/modore/scripts"
}()
ModoreScript.boot(scriptDir: scriptsDir)

// Watch the scripts dir for adds/removes AND every *.lua file inside it
// for content edits. DispatchSourceFileSystemObject on a directory only
// fires on dir-metadata changes (add/remove/rename), not on edits to
// files inside it — so without per-file watchers the user would save a
// script and see no menu-bar feedback until the next hotkey press
// (engine's mtime poll). One ConfigWatcher per file; the dir-level
// watcher rebuilds the set when files are added/removed.
//
// All watchers held for process lifetime via the `gPerScriptWatchers`
// dict in HotkeyState. Tear-down happens via the dict's deinit.
func rescanPerScriptWatchers() {
    let fm = FileManager.default
    let entries = (try? fm.contentsOfDirectory(atPath: scriptsDir)) ?? []
    let luaFiles = Set(entries.filter { $0.hasSuffix(".lua") }
        .map { "\(scriptsDir)/\($0)" })

    // Stop watchers for files that vanished.
    for (path, _) in gPerScriptWatchers where !luaFiles.contains(path) {
        gPerScriptWatchers.removeValue(forKey: path)
    }
    // Start watchers for files we don't already track.
    for path in luaFiles where gPerScriptWatchers[path] == nil {
        let w = ConfigWatcher(path: path) {
            ModoreScript.reloadScripts(dir: scriptsDir)
            gStatusItem?.refreshScriptAssociationForFrontmost()
            gStatusItem?.flashReload(kind: "scripts")
        }
        w.start()
        gPerScriptWatchers[path] = w
    }
}

gScriptsWatcher = ConfigWatcher(path: scriptsDir) {
    ModoreScript.reloadScripts(dir: scriptsDir)
    gStatusItem?.refreshScriptAssociationForFrontmost()
    rescanPerScriptWatchers()
    gStatusItem?.flashReload(kind: "scripts")
}
gScriptsWatcher?.start()
rescanPerScriptWatchers()

Log.boot("ready: conversion hotkey installed (see ~/.config/modore/modore.conf)")
app.run()
