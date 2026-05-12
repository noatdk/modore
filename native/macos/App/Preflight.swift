// `modore-host --check-config`: parse-before-swap exposed as a CLI. Reads
// the same file the running host would, runs the same parsers a live
// reload uses, and reports each section's outcome on stdout. Exit code
// signals broken state so CI / pre-commit hooks can branch on validity.

import Foundation

/// Run preflight config validation, print a human-readable report to stdout,
/// and exit. Exit code: 0 on a healthy load (including "no config, defaults
/// will be used"), 1 if the file contains a `[conversion] hotkey` that
/// doesn't parse, 2 if any `[clipboard]` or `katakana_modifier` value is
/// rejected.
func runConfigCheck() -> Never {
    let url = ModoreConfig.configFileURL()
    print("config path: \(url.path)")

    var exit_code: Int32 = 0
    var resolvedPrimary: ModoreConfig.ConversionHotkey? = nil
    switch ModoreConfig.loadConversionHotkeyOutcome() {
    case .loaded(let h, let source):
        print("  [conversion]  ok      \(source)")
        resolvedPrimary = h
    case .usingDefault(let reason):
        print("  [conversion]  default \(reason)")
        resolvedPrimary = ModoreConfig.defaultConversionHotkey()
    case .invalid(let reason):
        print("  [conversion]  INVALID \(reason)")
        exit_code = 1
    }

    // Katakana modifier — reports the configured value and the resulting
    // secondary chord (or why no secondary chord was bound, when the user
    // asked for one but it collides with the primary).
    let (katakanaMod, katIssues) = ModoreConfig.parseKatakanaModifier()
    if let primary = resolvedPrimary, let extra = katakanaMod.cgFlag {
        if primary.coreFlags.contains(extra) {
            print("  [conversion]  katakana_modifier=\(katakanaMod.displayName) (no secondary chord: primary already includes \(katakanaMod.displayName))")
        } else {
            let secondaryFlags = primary.coreFlags.union(extra)
            let secondary = ModoreConfig.ConversionHotkey(
                keyCode: primary.keyCode, coreFlags: secondaryFlags,
                displayName: "\(katakanaMod.displayName)+\(primary.displayName)")
            print("  [conversion]  katakana_modifier=\(katakanaMod.displayName) → \(secondary.displayName)")
        }
    } else {
        print("  [conversion]  katakana_modifier=\(katakanaMod.displayName)")
    }
    for issue in katIssues {
        print("                \(issue)")
    }
    if !katIssues.isEmpty && exit_code == 0 {
        exit_code = 2
    }

    // Cycle modifier — same shape as katakana, including the collision
    // explanation when the user configured a modifier we can't bind.
    let (cycleMod, cycleIssues) = ModoreConfig.parseCycleModifier()
    if let primary = resolvedPrimary, let extra = cycleMod.cgFlag {
        let katExtra = katakanaMod.cgFlag
        if primary.coreFlags.contains(extra) {
            print("  [conversion]  cycle_modifier=\(cycleMod.displayName) (no cycle chord: primary already includes \(cycleMod.displayName))")
        } else if let katExtra = katExtra, katExtra == extra {
            print("  [conversion]  cycle_modifier=\(cycleMod.displayName) (no cycle chord: same modifier as katakana_modifier)")
        } else {
            let cycleFlags = primary.coreFlags.union(extra)
            let cycleChord = ModoreConfig.ConversionHotkey(
                keyCode: primary.keyCode, coreFlags: cycleFlags,
                displayName: "\(cycleMod.displayName)+\(primary.displayName)")
            print("  [conversion]  cycle_modifier=\(cycleMod.displayName) → \(cycleChord.displayName)")
        }
    } else {
        print("  [conversion]  cycle_modifier=\(cycleMod.displayName)")
    }
    for issue in cycleIssues {
        print("                \(issue)")
    }
    if !cycleIssues.isEmpty && exit_code == 0 {
        exit_code = 2
    }

    let (cycleFromUndone, cycleFromUndoneIssues) = ModoreConfig.parseCycleFromUndone()
    print("  [conversion]  cycle_from_undone=\(cycleFromUndone.displayName)")
    for issue in cycleFromUndoneIssues {
        print("                \(issue)")
    }
    if !cycleFromUndoneIssues.isEmpty && exit_code == 0 {
        exit_code = 2
    }

    // Undo window — reports the value with a "disabled" gloss on 0 so
    // the user doesn't have to remember what 0 means at a glance.
    let (undoMs, undoIssues) = ModoreConfig.parseUndoWindowMs()
    if undoMs == 0 {
        print("  [conversion]  undo_window_ms=0 (Esc-undo disabled)")
    } else {
        print("  [conversion]  undo_window_ms=\(undoMs)")
    }
    for issue in undoIssues {
        print("                \(issue)")
    }
    if !undoIssues.isEmpty && exit_code == 0 {
        exit_code = 2
    }

    let (panelMode, panelIssues) = ModoreConfig.parseCandidatePanelMode()
    print("  [ui]          candidate_panel=\(panelMode.displayName)")
    for issue in panelIssues {
        print("                \(issue)")
    }
    if !panelIssues.isEmpty && exit_code == 0 {
        exit_code = 2
    }

    let (panelDuration, panelDurationIssues) = ModoreConfig.parseCandidatePanelDurationMs()
    if panelDuration == 0 {
        print("  [ui]          candidate_panel_duration_ms=0 (no auto-hide)")
    } else {
        print("  [ui]          candidate_panel_duration_ms=\(panelDuration)")
    }
    for issue in panelDurationIssues {
        print("                \(issue)")
    }
    if !panelDurationIssues.isEmpty && exit_code == 0 {
        exit_code = 2
    }

    let (timings, issues) = ModoreConfig.parseClipboardTimings()
    print("  [clipboard]   pre_copy=\(timings.preCopyDelayMs)ms"
        + " read_timeout=\(timings.readTimeoutMs)ms"
        + " restore=\(timings.restoreClipboardDelayMs)ms")
    for issue in issues {
        print("                \(issue)")
    }
    if !issues.isEmpty && exit_code == 0 {
        exit_code = 2
    }

    exit(exit_code)
}
