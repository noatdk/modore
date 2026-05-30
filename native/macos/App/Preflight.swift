// `modore-host --check-config`: parse-before-swap exposed as a CLI. Reads
// the same file the running host would, runs the same parsers a live
// reload uses, and reports each section's outcome on stdout. Exit code
// signals broken state so CI / pre-commit hooks can branch on validity.

import Foundation

/// Run preflight config validation, print a human-readable report to stdout,
/// and exit. Exit code: 0 on a healthy load (including "no config, defaults
/// will be used"), 1 if the file contains a `[conversion] hotkey` that
/// doesn't parse, 2 if any `[clipboard]`, `katakana_modifier`,
/// `katakana_modifier_behavior`, `[logging] disabled`, or `[shell]` value
/// is rejected.
func runConfigCheck() -> Never {
    let url = ModoreConfig.configFileURL()
    print("config path: \(url.path)")

    var exit_code: Int32 = 0

    // Every section prints its parser's rejected values the same way and
    // bumps the exit code to 2 (config has a bad value) the first time any
    // section reports one. Captures `exit_code` so the per-section call
    // sites stay a single line.
    func reportIssues(_ issues: [String]) {
        for issue in issues {
            print("                \(issue)")
        }
        if !issues.isEmpty && exit_code == 0 {
            exit_code = 2
        }
    }

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
    reportIssues(katIssues)

    let (katakanaBehavior, katBehaviorIssues) = ModoreConfig.parseKatakanaModifierBehavior()
    print("  [conversion]  katakana_modifier_behavior=\(katakanaBehavior.displayName)")
    reportIssues(katBehaviorIssues)

    let (loggingMask, loggingIssues) = ModoreConfig.parseDisabledLoggingNamespaces()
    print("  [logging]     disabled=\(loggingMask.displayName)")
    reportIssues(loggingIssues)

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
    reportIssues(cycleIssues)

    let (cycleFromUndone, cycleFromUndoneIssues) = ModoreConfig.parseCycleFromUndone()
    print("  [conversion]  cycle_from_undone=\(cycleFromUndone.displayName)")
    reportIssues(cycleFromUndoneIssues)

    // Undo window — reports the value with a "disabled" gloss on 0 so
    // the user doesn't have to remember what 0 means at a glance.
    let (undoMs, undoIssues) = ModoreConfig.parseUndoWindowMs()
    if undoMs == 0 {
        print("  [conversion]  undo_window_ms=0 (Esc-undo disabled)")
    } else {
        print("  [conversion]  undo_window_ms=\(undoMs)")
    }
    reportIssues(undoIssues)

    let (panelMode, panelIssues) = ModoreConfig.parseCandidatePanelMode()
    print("  [ui]          candidate_panel=\(panelMode.displayName)")
    reportIssues(panelIssues)

    let (panelDuration, panelDurationIssues) = ModoreConfig.parseCandidatePanelDurationMs()
    if panelDuration == 0 {
        print("  [ui]          candidate_panel_duration_ms=0 (no auto-hide)")
    } else {
        print("  [ui]          candidate_panel_duration_ms=\(panelDuration)")
    }
    reportIssues(panelDurationIssues)

    let (classifierOn, classifierIssues) = ModoreConfig.parseClassifierEnabled()
    print("  [conversion]  classifier=\(classifierOn ? "on" : "off")")
    reportIssues(classifierIssues)

    let (bridgeRuntime, bridgeIssues) = ModoreConfig.parseBridgeRuntime()
    print("  [bridge]      candidate_mixing_mode=\(bridgeRuntime.candidateMixingMode)")
    print("  [bridge]      trace_raw_candidates=\(bridgeRuntime.traceRawCandidates ? "on" : "off")")
    reportIssues(bridgeIssues)

    let (mozcBackend, mozcBackendIssues) = ModoreConfig.parseMozcBackend()
    print("  [bridge]      mozc_backend=\(mozcBackend.displayName)")
    reportIssues(mozcBackendIssues)

    let (timings, issues) = ModoreConfig.parseClipboardTimings()
    print("  [clipboard]   pre_copy=\(timings.preCopyDelayMs)ms"
        + " read_timeout=\(timings.readTimeoutMs)ms"
        + " restore=\(timings.restoreClipboardDelayMs)ms")
    reportIssues(issues)

    let (shellWindow, shellWindowIssues) = ModoreConfig.parseShellCandidateWindow()
    print("  [shell]       candidate_window=\(shellWindow ? "on" : "off")")
    reportIssues(shellWindowIssues)

    let (shellPicker, shellPickerIssues) = ModoreConfig.parseShellPicker()
    print("  [shell]       picker=\(shellPicker.displayName)")
    reportIssues(shellPickerIssues)

    exit(exit_code)
}
