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
var gConversionKeyCode: CGKeyCode = CGKeyCode(kVK_ANSI_Slash)
var gConversionCoreFlags: CGEventFlags = .maskControl

/// Secondary-chord flags (primary + the configured katakana modifier).
/// `nil` means no secondary chord is bound — same state as the pre-feature
/// build. Read by the tap callback for the CGEventTap-fallback path; written
/// only on the main thread, same race-free swap as the primary chord.
var gKatakanaChordFlags: CGEventFlags? = nil

/// Current `[conversion] katakana_modifier` setting. Kept as a single source
/// of truth so reloads can diff against it.
var gKatakanaModifier: ModoreConfig.KatakanaModifier = .none

/// True when the Carbon hotkey grab is active. The tap callback consults
/// this to decide whether to also match-and-swallow the chord. If Carbon
/// succeeded, the OS consumes the keystroke before the tap sees it anyway;
/// gating the tap path defensively avoids a double-fire if that ever changes.
var gUsingCarbonHotkey: Bool = false

/// Held for the lifetime of the process; nil if RegisterEventHotKey failed
/// at startup (we then rely on the tap-based detector). Assigned in
/// main.swift, mutated only here.
var gCarbonHotkey: CarbonHotkey?

/// Live clipboard-fallback timings. Written on the main thread (startup and
/// watcher-driven reloads); read by `doClipboardPickup` on a background queue
/// via a snapshot copy at function entry, so a plain swap is race-free.
var gClipboardTimings = ModoreConfig.ClipboardTimings()

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
            let ok = ck.registerSecondary(secondary) {
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
        gCarbonHotkey?.unregisterSecondary()
        if modifier != .none && prevFlags != nil {
            Log.hotkey("katakana chord cleared (collides with primary modifiers)")
        } else if prevFlags != nil {
            Log.hotkey("katakana chord cleared")
        }
    }
}

// MARK: - Primary chord registration

func applyConversionHotkeyChord(_ chord: ModoreConfig.ConversionHotkey) {
    gConversionKeyCode = chord.keyCode
    gConversionCoreFlags = chord.coreFlags
    if let ck = gCarbonHotkey {
        let ok = ck.register(chord)
        gUsingCarbonHotkey = ok
        if !ok {
            Log.hotkey("RegisterEventHotKey failed — falling back to tap-based detection")
        }
    }
    // The secondary chord rides on top of the primary, so any primary
    // re-registration rebinds the secondary too.
    applyKatakanaSecondaryChord(primary: chord, modifier: gKatakanaModifier)
    gStatusItem?.refresh(
        hotkey: chord,
        usingCarbonHotkey: gUsingCarbonHotkey,
        katakanaChord: secondaryChordForStatus(primary: chord))
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
/// line here just records the user-facing setting.
func applyKatakanaModifierReload() {
    let next = ModoreConfig.loadKatakanaModifier()
    if next != gKatakanaModifier {
        gKatakanaModifier = next
        Log.config("katakana modifier: \(next.displayName)")
        let primary = ModoreConfig.ConversionHotkey(
            keyCode: gConversionKeyCode, coreFlags: gConversionCoreFlags)
        applyKatakanaSecondaryChord(primary: primary, modifier: next)
        gStatusItem?.refresh(
            hotkey: primary,
            usingCarbonHotkey: gUsingCarbonHotkey,
            katakanaChord: secondaryChordForStatus(primary: primary))
    }
}

/// Single entry point for the config watcher — reloads every section.
func applyConfigReload() {
    applyConversionHotkeyReload()
    applyKatakanaModifierReload()
    applyClipboardTimingsReload()
}
