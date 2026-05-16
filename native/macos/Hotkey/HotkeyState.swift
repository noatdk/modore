// Hotkey + config-section glue. Owns the mutable globals that describe the
// *current* trigger chord (primary + optional katakana variant), the
// clipboard fallback timings, and the Carbon registration. Also exposes the
// `applyConfigReload()` entry point the ConfigWatcher calls on disk edits.
//
// Globals here are intentionally non-private so other modules (EventTap,
// Pickup, main.swift) can read them. Writers are main-thread only — config
// load at startup, watcher callback on edit — so the readers see a coherent
// snapshot without locking.
//
// Globals that live in main.swift (lifecycle owned by bootstrap):
//   - gCarbonHotkey: the singleton CarbonHotkey instance. Assigned at boot,
//     never reassigned. We touch it from here via `gCarbonHotkey?.register…`
//     because the chord state is what we own.
//   - gStatusItem: the menu-bar item. We call `.refresh(...)` on every chord
//     change so the displayed hotkey stays accurate.

import Carbon
import Cocoa

// MARK: - Hotkey state

/// Resolved primary chord. Read by the tap callback on its own thread;
/// written by the main thread on startup and on watcher-driven reloads, so a
/// plain swap is race-free.
var gConversionKeyCode: CGKeyCode = CGKeyCode(kVK_ANSI_Semicolon)
var gConversionCoreFlags: CGEventFlags = .maskCommand

/// Secondary-chord flags (primary + the configured katakana modifier).
/// `nil` means no secondary chord is bound — same state as the pre-feature
/// build. Read by the tap callback for the CGEventTap-fallback path; written
/// only on the main thread, same race-free swap as the primary chord.
var gKatakanaChordFlags: CGEventFlags? = nil

/// Current `[conversion] katakana_modifier` setting. Kept as a single source
/// of truth so reloads can diff against it.
var gKatakanaModifier: ModoreConfig.KatakanaModifier = .none

/// Current `[conversion] katakana_modifier_behavior` setting. Controls
/// whether the katakana chord keeps katakana on an active session or
/// cycles backward through candidates.
var gKatakanaModifierBehavior: ModoreConfig.KatakanaModifierBehavior = .cycleBackwards

/// True when the Carbon hotkey grab is active. The tap callback consults
/// this to decide whether to also match-and-swallow the chord. If Carbon
/// succeeded, the OS consumes the keystroke before the tap sees it anyway;
/// gating the tap path defensively avoids a double-fire if that ever changes.
var gUsingCarbonHotkey: Bool = false

/// Held for the lifetime of the process; nil if RegisterEventHotKey failed
/// at startup (we then rely on the tap-based detector). Assigned in
/// main.swift, mutated only here.
var gCarbonHotkey: CarbonHotkey?

/// One ConfigWatcher per *.lua script file. Rebuilt by main.swift on every
/// dir-watcher fire so newly-added files start being watched live. Keyed by
/// absolute path. Held for process lifetime; tearing down a watcher cancels
/// its DispatchSource via ConfigWatcher.deinit.
var gPerScriptWatchers: [String: ConfigWatcher] = [:]

/// Live clipboard-fallback timings. Written on the main thread (startup and
/// watcher-driven reloads); read by `doClipboardPickup` on a background queue
/// via a snapshot copy at function entry, so a plain swap is race-free.
var gClipboardTimings = ModoreConfig.ClipboardTimings()

/// Max age (ms) of a last-conversion snapshot for Esc-undo to fire.
/// 0 disables the feature — the tap callback's Esc branch checks `> 0`
/// before touching any state. Read by the tap thread (Esc check) and by
/// `performEscUndo` on the worker; written by the main thread on startup
/// and watcher-driven reloads.
var gUndoWindowMs: Int = ModoreConfig.defaultUndoWindowMs

/// Current `[conversion] cycle_modifier` setting. Drives the optional
/// dedicated cycle chord. Default `.none` — the primary chord doubles
/// as the cycle gesture while a session is active, so most users won't
/// need a separate chord. Setting this binds an additional chord on
/// top of the primary's same-key cycle behavior.
var gCycleModifier: ModoreConfig.CycleModifier = .none

/// Tertiary-chord flags (primary + the configured cycle modifier).
/// `nil` when cycle is disabled or collides with primary/secondary
/// modifiers. Read by the tap-fallback path; written on the main thread.
var gCycleChordFlags: CGEventFlags? = nil

/// Current `[conversion] cycle_from_undone`. Consulted by
/// `performCycleNext` when the session is in the undone state.
var gCycleFromUndone: ModoreConfig.CycleFromUndone = .redo

/// Current `[ui] candidate_panel` setting. Drives whether the floating
/// candidate list appears, and on which gesture. `.none` is the
/// pre-feature default (panel disabled). Read by `doPickup` /
/// `cycleNext` to decide whether to call `CandidatePanel.show(...)`.
var gCandidatePanelMode: ModoreConfig.CandidatePanelMode = .none

/// Current `[ui] candidate_panel_duration_ms`. How long the panel stays
/// up after each `show()`; reset on every show so a chain of cycle
/// presses keeps it alive. `0` disables auto-hide (panel sticks for the
/// session). Read by `CandidatePanel.show` on the main queue; written
/// by the main thread on boot and config reload — plain swap is
/// race-free.
var gCandidatePanelDurationMs: Int = ModoreConfig.defaultCandidatePanelDurationMs

/// When true, the ML n-gram classifier is used for romaji/ASCII segmentation
/// instead of the heuristic `splitAcronymHead`. Opt-in via
/// `[conversion] classifier = on` in the config file.
var gClassifierEnabled: Bool = false

/// Launch-time bridge tuning. The bridge reads these once at init, so reloads
/// can update the process env for the next bridge start but cannot affect the
/// already-running session. Defaults are applied deterministically at boot.
var gBridgeRuntime = ModoreConfig.BridgeRuntime()

// MARK: - Secondary chord (katakana modifier)

/// Build the katakana-variant chord by layering the configured modifier on top
/// of the primary chord. Returns `nil` when no secondary chord is configured,
/// or when the modifier would collide with one already present in the primary
/// (e.g. primary already includes Shift and the user asks for `shift` — the
/// resulting chord would be indistinguishable from the primary).
func makeSecondaryChord(
    primary: ModoreConfig.ConversionHotkey,
    modifier: ModoreConfig.KatakanaModifier
) -> ModoreConfig.ConversionHotkey? {
    guard let extra = modifier.cgFlag else { return nil }
    if primary.coreFlags.contains(extra) { return nil }
    let flags = primary.coreFlags.union(extra)
    return ModoreConfig.ConversionHotkey(
        keyCode: primary.keyCode,
        coreFlags: flags,
        displayName: "\(modifier.displayName)+\(primary.displayName)")
}

/// Helper for the status item — only surface a secondary chord if one is
/// actually bound (Carbon-grabbed). Otherwise the status item shows the
/// primary chord alone, same as before this feature.
func secondaryChordForStatus(primary: ModoreConfig.ConversionHotkey)
    -> ModoreConfig.ConversionHotkey?
{
    guard gKatakanaChordFlags != nil else { return nil }
    return makeSecondaryChord(primary: primary, modifier: gKatakanaModifier)
}

/// (Re)register the katakana secondary chord against the given primary +
/// modifier. Updates `gKatakanaChordFlags` for the tap fallback path. Logs
/// only on state change (registered / cleared / collision-rejected).
func applyKatakanaSecondaryChord(
    primary: ModoreConfig.ConversionHotkey,
    modifier: ModoreConfig.KatakanaModifier
) {
    let secondary = makeSecondaryChord(primary: primary, modifier: modifier)
    let prevFlags = gKatakanaChordFlags

    if let secondary = secondary {
        gKatakanaChordFlags = secondary.coreFlags
        if let ck = gCarbonHotkey {
            let ok = ck.register(role: "katakana", chord: secondary) {
                kHotkeyTapQueue.async {
                    doPickup(PickupRequest(target: .katakana))
                }
            }
            if !ok {
                Log.hotkey("RegisterEventHotKey failed for katakana chord \(secondary.displayName) — tap fallback will still match if the primary tap path is in use")
            } else if prevFlags != secondary.coreFlags {
                Log.hotkey("katakana chord registered: \(secondary.displayName)")
            }
        }
    } else {
        gKatakanaChordFlags = nil
        gCarbonHotkey?.unregister(role: "katakana")
        if modifier != .none && prevFlags != nil {
            Log.hotkey("katakana chord cleared (collides with primary modifiers)")
        } else if prevFlags != nil {
            Log.hotkey("katakana chord cleared")
        }
    }
}

// MARK: - Tertiary chord (cycle modifier)

/// Build the cycle chord by layering the cycle modifier on top of the
/// primary chord. Returns nil when the modifier is `.none`, when the
/// primary already includes that modifier (would shadow the primary),
/// or when the modifier collides with whatever the katakana chord
/// already uses (would mean one chord drives two gestures).
func makeCycleChord(
    primary: ModoreConfig.ConversionHotkey,
    cycle: ModoreConfig.CycleModifier,
    katakana: ModoreConfig.KatakanaModifier
) -> ModoreConfig.ConversionHotkey? {
    guard let extra = cycle.cgFlag else { return nil }
    if primary.coreFlags.contains(extra) { return nil }
    if let katakanaFlag = katakana.cgFlag, katakanaFlag == extra { return nil }
    let flags = primary.coreFlags.union(extra)
    return ModoreConfig.ConversionHotkey(
        keyCode: primary.keyCode,
        coreFlags: flags,
        displayName: "\(cycle.displayName)+\(primary.displayName)")
}

/// Status-item helper, parallel to `secondaryChordForStatus`. Surfaces
/// the cycle chord only when it's actually Carbon-grabbed.
func cycleChordForStatus(primary: ModoreConfig.ConversionHotkey)
    -> ModoreConfig.ConversionHotkey?
{
    guard gCycleChordFlags != nil else { return nil }
    return makeCycleChord(primary: primary, cycle: gCycleModifier, katakana: gKatakanaModifier)
}

/// (Re)register the cycle chord. Same lifecycle as the katakana
/// secondary: state changes (registered / cleared / collision-rejected)
/// log a single `[hotkey]` line; no-op reloads stay quiet.
func applyCycleChord(
    primary: ModoreConfig.ConversionHotkey,
    cycle: ModoreConfig.CycleModifier,
    katakana: ModoreConfig.KatakanaModifier
) {
    let chord = makeCycleChord(primary: primary, cycle: cycle, katakana: katakana)
    let prevFlags = gCycleChordFlags

    if let chord = chord {
        gCycleChordFlags = chord.coreFlags
        if let ck = gCarbonHotkey {
            let ok = ck.register(role: "cycle", chord: chord) {
                kHotkeyTapQueue.async { performCycleNext() }
            }
            if !ok {
                Log.hotkey("RegisterEventHotKey failed for cycle chord \(chord.displayName)")
            } else if prevFlags != chord.coreFlags {
                Log.hotkey("cycle chord registered: \(chord.displayName)")
            }
        }
    } else {
        gCycleChordFlags = nil
        gCarbonHotkey?.unregister(role: "cycle")
        if cycle != .none && prevFlags != nil {
            Log.hotkey("cycle chord cleared (collides with primary or katakana modifier)")
        } else if prevFlags != nil {
            Log.hotkey("cycle chord cleared")
        }
    }
}

// MARK: - Primary chord registration

func applyConversionHotkeyChord(_ chord: ModoreConfig.ConversionHotkey) {
    gConversionKeyCode = chord.keyCode
    gConversionCoreFlags = chord.coreFlags
    if let ck = gCarbonHotkey {
        let ok = ck.register(role: "primary", chord: chord) {
            kHotkeyTapQueue.async { doPickup() }
        }
        gUsingCarbonHotkey = ok
        if !ok {
            Log.hotkey("RegisterEventHotKey failed — falling back to tap-based detection")
        }
    }
    // Derived chords ride on top of the primary, so any primary
    // re-registration rebinds them too.
    applyKatakanaSecondaryChord(primary: chord, modifier: gKatakanaModifier)
    applyCycleChord(primary: chord, cycle: gCycleModifier, katakana: gKatakanaModifier)
    gStatusItem?.refresh(
        hotkey: chord,
        usingCarbonHotkey: gUsingCarbonHotkey,
        katakanaChord: secondaryChordForStatus(primary: chord),
        cycleChord: cycleChordForStatus(primary: chord))
}

// MARK: - Section reloaders (one per [section] in modore.conf)

func applyConversionHotkeyReload() {
    let prev = ModoreConfig.ConversionHotkey(
        keyCode: gConversionKeyCode, coreFlags: gConversionCoreFlags)
    switch ModoreConfig.loadConversionHotkeyOutcome() {
    case .loaded(let next, let source):
        if next != prev {
            applyConversionHotkeyChord(next)
            Log.config("reloaded \(source)")
        }
    case .usingDefault(let reason):
        let def = ModoreConfig.defaultConversionHotkey()
        if def != prev {
            applyConversionHotkeyChord(def)
            Log.config("reload: \(reason) — reverted to default")
        }
    case .invalid(let reason):
        Log.config("reload rejected: \(reason) — keeping previous hotkey")
    }
}

/// Reload `[clipboard]` timings from disk. Logs only on actual change so a
/// no-op reload (e.g. user edited an unrelated section) stays quiet.
func applyClipboardTimingsReload() {
    let next = ModoreConfig.loadClipboardTimings()
    if next != gClipboardTimings {
        gClipboardTimings = next
        Log.config("clipboard timings: pre_copy=\(next.preCopyDelayMs)ms"
                 + " read_timeout=\(next.readTimeoutMs)ms"
                 + " restore=\(next.restoreClipboardDelayMs)ms")
    }
}

/// Reload `[conversion] katakana_modifier` from disk. The secondary chord
/// re-registers if and only if the modifier changed; the registration
/// itself logs through `applyKatakanaSecondaryChord`. The `[config]` log
/// line here just records the user-facing setting. Cycle chord also
/// re-applies because its collision check depends on the katakana
/// modifier flag.
func applyKatakanaModifierReload() {
    let next = ModoreConfig.loadKatakanaModifier()
    if next != gKatakanaModifier {
        gKatakanaModifier = next
        Log.config("katakana modifier: \(next.displayName)")
        let primary = ModoreConfig.ConversionHotkey(
            keyCode: gConversionKeyCode, coreFlags: gConversionCoreFlags)
        applyKatakanaSecondaryChord(primary: primary, modifier: next)
        applyCycleChord(primary: primary, cycle: gCycleModifier, katakana: next)
        gStatusItem?.refresh(
            hotkey: primary,
            usingCarbonHotkey: gUsingCarbonHotkey,
            katakanaChord: secondaryChordForStatus(primary: primary),
            cycleChord: cycleChordForStatus(primary: primary))
    }
}

/// Reload `[conversion] katakana_modifier_behavior` from disk. This
/// only changes what the katakana chord does when a conversion session
/// is already active.
func applyKatakanaModifierBehaviorReload() {
    let next = ModoreConfig.loadKatakanaModifierBehavior()
    if next != gKatakanaModifierBehavior {
        gKatakanaModifierBehavior = next
        Log.config("katakana modifier behavior: \(next.displayName)")
    }
}

/// Reload `[conversion] cycle_modifier` from disk. Mirrors the katakana
/// reload — re-bind the chord and refresh the status item if it changed.
func applyCycleModifierReload() {
    let next = ModoreConfig.loadCycleModifier()
    if next != gCycleModifier {
        gCycleModifier = next
        Log.config("cycle modifier: \(next.displayName)")
        let primary = ModoreConfig.ConversionHotkey(
            keyCode: gConversionKeyCode, coreFlags: gConversionCoreFlags)
        applyCycleChord(primary: primary, cycle: next, katakana: gKatakanaModifier)
        gStatusItem?.refresh(
            hotkey: primary,
            usingCarbonHotkey: gUsingCarbonHotkey,
            katakanaChord: secondaryChordForStatus(primary: primary),
            cycleChord: cycleChordForStatus(primary: primary))
    }
}

/// Reload `[conversion] cycle_from_undone` from disk. Pure state swap —
/// no chord re-binding, no UI refresh. The next cycle press picks up the
/// new behavior.
func applyCycleFromUndoneReload() {
    let next = ModoreConfig.loadCycleFromUndone()
    if next != gCycleFromUndone {
        gCycleFromUndone = next
        Log.config("cycle_from_undone: \(next.displayName)")
    }
}

/// Reload `[conversion] undo_window_ms` from disk. Esc-undo state itself
/// (the ConversionSession snapshot) is unaffected by reloads; we just swap
/// the window used to age it out. Logs only on actual change.
func applyUndoWindowReload() {
    let next = ModoreConfig.loadUndoWindowMs()
    if next != gUndoWindowMs {
        gUndoWindowMs = next
        if next == 0 {
            Log.config("undo window: disabled (undo_window_ms = 0)")
        } else {
            Log.config("undo window: \(next)ms")
        }
    }
}

/// Reload `[ui] candidate_panel` from disk. Pure state swap — the next
/// conversion/cycle picks up the new mode. If the panel is currently
/// visible and the new mode is `.none`, hide it so the swap is visible
/// immediately rather than lingering until the session expires.
func applyCandidatePanelReload() {
    let next = ModoreConfig.loadCandidatePanelMode()
    if next != gCandidatePanelMode {
        gCandidatePanelMode = next
        Log.config("candidate panel: \(next.displayName)")
        if next == .none {
            DispatchQueue.main.async { CandidatePanel.shared.hide() }
        }
    }
}

/// Reload `[ui] candidate_panel_duration_ms` from disk. Plain state
/// swap; takes effect on the next `show()` call (existing visibility
/// stays on the old timer, which decays within a couple of seconds).
func applyCandidatePanelDurationReload() {
    let next = ModoreConfig.loadCandidatePanelDurationMs()
    if next != gCandidatePanelDurationMs {
        gCandidatePanelDurationMs = next
        Log.config(next == 0
            ? "candidate panel duration: no auto-hide (sticks for session)"
            : "candidate panel duration: \(next)ms")
    }
}

/// Single entry point for the config watcher — reloads every section.
func applyConfigReload() {
    applyConversionHotkeyReload()
    applyBridgeRuntimeReloadNotice()
    applyMozcBackendReloadNotice()
    applyKatakanaModifierReload()
    applyKatakanaModifierBehaviorReload()
    applyCycleModifierReload()
    applyCycleFromUndoneReload()
    applyUndoWindowReload()
    applyClipboardTimingsReload()
    applyCandidatePanelReload()
    applyCandidatePanelDurationReload()
    applyClassifierReload()
}

/// Reload notice for `[bridge] mozc_backend`. The bridge is initialized
/// once at boot and owns backend-specific engine/session state, so live
/// swapping is intentionally unsupported. We still parse the key on reload
/// so the user gets immediate feedback that a restart is required.
func applyMozcBackendReloadNotice() {
    let next = ModoreConfig.loadMozcBackend()
    if next != MOZC_BACKEND {
        Log.config("mozc backend changed to \(next.displayName) — restart modore to apply")
    }
}

/// Reload notice for `[bridge]` knobs. These are boot-time env overrides for
/// the Mozc bridge, so a live bridge does not observe the change; the host
/// keeps the new values in process env for the next bridge init.
func applyBridgeRuntimeReloadNotice() {
    let next = ModoreConfig.loadBridgeRuntime()
    if next == gBridgeRuntime {
        return
    }
    gBridgeRuntime = next
    applyBridgeRuntimeEnv(next)
    Log.config("bridge candidate_mixing_mode=\(next.candidateMixingMode) (applies on next bridge init)")
    Log.config("bridge trace_raw_candidates=\(next.traceRawCandidates ? "on" : "off") (applies on next bridge init)")
}

private func applyClassifierReload() {
    let new = ModoreConfig.loadClassifierEnabled()
    if new == gClassifierEnabled { return }
    gClassifierEnabled = new
    if new {
        if !Classifier.isLoaded {
            let configDir: String = {
                if let xdg = ProcessInfo.processInfo.environment["XDG_CONFIG_HOME"], !xdg.isEmpty {
                    return "\(xdg)/modore"
                }
                return "\(FileManager.default.homeDirectoryForCurrentUser.path)/.config/modore"
            }()
            let modelPath = "\(configDir)/classifier.mdl"
            if FileManager.default.fileExists(atPath: modelPath) {
                if !Classifier.load(modelPath: modelPath) {
                    Log.config("[conversion] classifier = on but model failed to load — staying off")
                    gClassifierEnabled = false
                    return
                }
            } else if let bundled = Bundle.main.path(forResource: "classifier", ofType: "mdl") {
                if !Classifier.load(modelPath: bundled) {
                    Log.config("[conversion] classifier = on but bundle model failed to load — staying off")
                    gClassifierEnabled = false
                    return
                }
            } else {
                Log.config("[conversion] classifier = on but no model found — staying off")
                gClassifierEnabled = false
                return
            }
        }
        Log.config("[conversion] classifier = on")
    } else {
        Log.config("[conversion] classifier = off")
    }
}
