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
