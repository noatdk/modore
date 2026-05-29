// Loads ~/.config/modore/modore.conf (same path on macOS/Linux via XDG layout).
//
// Sections:
//   [conversion] hotkey=...             — global trigger chord (same format as Linux)
//   [conversion] katakana_modifier=...  — extra modifier that forces katakana (macOS only)
//   [conversion] katakana_modifier_behavior=...  — katakana vs cycle_backwards (macOS only)
//   [bridge]     mozc_backend=...       — built-in Mozc vs system Google IME (macOS only)
//   [bridge]     ...                    — launch-time knobs for bridge/env-gated backend tweaks
//   [logging]    disabled=...           — comma-separated namespaces to suppress
//   [clipboard]  *_ms=<integer>         — fallback-path timings (macOS only)

import Carbon
import Foundation

enum ModoreConfig {

    /// Which bridge backend the macOS host should request at startup.
    /// `built-in` keeps the existing bundled Mozc path. `.googleIme`
    /// targets the system-installed Google Japanese Input service via the
    /// bridge's macOS-only backend.
    enum MozcBackend: Equatable {
        case oss
        case googleIme

        var envValue: String {
            switch self {
            case .oss:       return "oss"
            case .googleIme: return "google-ime"
            }
        }

        var displayName: String {
            switch self {
            case .oss:       return "built-in"
            case .googleIme: return "google_ime"
            }
        }
    }

    /// Launch-time bridge tuning that the host maps onto env vars before the
    /// Mozc bridge is initialized. These are cheap, deterministic values:
    /// parse once at boot, set once, and the bridge consumes them on init.
    struct BridgeRuntime: Equatable {
        var candidateMixingMode: Int = 0
        var traceRawCandidates: Bool = false
    }

    /// Compact runtime mask for logging namespaces. Stored as a bitset so
    /// disabled-tag checks stay allocation-free in the hot path.
    struct LoggingNamespaceMask: OptionSet, Equatable {
        let rawValue: UInt64

        static let boot        = LoggingNamespaceMask(rawValue: 1 << 0)
        static let config      = LoggingNamespaceMask(rawValue: 1 << 1)
        static let ax          = LoggingNamespaceMask(rawValue: 1 << 2)
        static let hotkey      = LoggingNamespaceMask(rawValue: 1 << 3)
        static let pickup      = LoggingNamespaceMask(rawValue: 1 << 4)
        static let clipboard   = LoggingNamespaceMask(rawValue: 1 << 5)
        static let mozc        = LoggingNamespaceMask(rawValue: 1 << 6)
        static let secureInput = LoggingNamespaceMask(rawValue: 1 << 7)
        static let undo        = LoggingNamespaceMask(rawValue: 1 << 8)
        static let cycle       = LoggingNamespaceMask(rawValue: 1 << 9)
        static let panel       = LoggingNamespaceMask(rawValue: 1 << 10)
        static let unicode     = LoggingNamespaceMask(rawValue: 1 << 11)
        static let scripting   = LoggingNamespaceMask(rawValue: 1 << 12)
        static let shell       = LoggingNamespaceMask(rawValue: 1 << 13)

        static let allKnown: LoggingNamespaceMask = [
            .boot, .config, .ax, .hotkey, .pickup, .clipboard,
            .mozc, .secureInput, .undo, .cycle, .panel, .unicode, .scripting,
            .shell,
        ]

        init(rawValue: UInt64) {
            self.rawValue = rawValue
        }

        init?(namespace token: String) {
            switch token {
            case "boot": self = .boot
            case "config": self = .config
            case "ax": self = .ax
            case "hotkey": self = .hotkey
            case "pickup": self = .pickup
            case "clipboard": self = .clipboard
            case "mozc": self = .mozc
            case "secure-input", "secure_input": self = .secureInput
            case "undo": self = .undo
            case "cycle": self = .cycle
            case "panel": self = .panel
            case "unicode": self = .unicode
            case "scripting": self = .scripting
            case "shell": self = .shell
            default: return nil
            }
        }

        var displayName: String {
            if self == [] { return "none" }
            if self == .allKnown { return "all" }
            var parts: [String] = []
            if contains(.boot)        { parts.append("boot") }
            if contains(.config)      { parts.append("config") }
            if contains(.ax)          { parts.append("ax") }
            if contains(.hotkey)      { parts.append("hotkey") }
            if contains(.pickup)      { parts.append("pickup") }
            if contains(.clipboard)   { parts.append("clipboard") }
            if contains(.mozc)        { parts.append("mozc") }
            if contains(.secureInput) { parts.append("secure-input") }
            if contains(.undo)        { parts.append("undo") }
            if contains(.cycle)       { parts.append("cycle") }
            if contains(.panel)       { parts.append("panel") }
            if contains(.unicode)     { parts.append("unicode") }
            if contains(.scripting)   { parts.append("scripting") }
            if contains(.shell)       { parts.append("shell") }
            return parts.joined(separator: ",")
        }
    }

    struct ConversionHotkey: Equatable {
        var keyCode: CGKeyCode
        var coreFlags: CGEventFlags

        /// Best-effort human-readable form of the chord, for the menu bar
        /// item and any future "current hotkey" UI. For parsed chords this is
        /// the user's original string verbatim ("Ctrl+Shift+grave"); for the
        /// built-in default it's `defaultChord`. Excluded from equality so
        /// "config changed but resolved to the same chord" still no-ops.
        var displayName: String = ""

        /// Carbon `RegisterEventHotKey` expects a modifier mask using
        /// `cmdKey` / `controlKey` / `shiftKey` / `optionKey` constants —
        /// different bit layout from `CGEventFlags`. One place to translate.
        var carbonModifierMask: UInt32 {
            var mask: UInt32 = 0
            if coreFlags.contains(.maskControl)   { mask |= UInt32(controlKey) }
            if coreFlags.contains(.maskShift)     { mask |= UInt32(shiftKey)   }
            if coreFlags.contains(.maskCommand)   { mask |= UInt32(cmdKey)     }
            if coreFlags.contains(.maskAlternate) { mask |= UInt32(optionKey)  }
            return mask
        }

        static func == (lhs: ConversionHotkey, rhs: ConversionHotkey) -> Bool {
            lhs.keyCode == rhs.keyCode && lhs.coreFlags == rhs.coreFlags
        }
    }

    /// Three-way result for both startup and reload.
    ///
    /// Startup callers map every case to a concrete hotkey (falling back to
    /// the default on `usingDefault` / `invalid`). Reload callers treat
    /// `invalid` as "keep the previous chord" instead of reverting.
    enum LoadOutcome {
        case loaded(ConversionHotkey, source: String)
        case usingDefault(reason: String)
        case invalid(reason: String)
    }

    /// Extra modifier that, when combined with the primary conversion chord,
    /// forces a katakana commit instead of the usual top-kanji candidate.
    /// Default `.none` keeps the pre-feature behavior (one chord, kanji
    /// always); set to `.shift` to bind `Shift+<primary>` as a second chord.
    ///
    /// Resolution to a concrete `CGEventFlags` bit happens via `cgFlag` so
    /// the parser and the secondary-chord builder agree.
    enum KatakanaModifier: Equatable {
        case none
        case shift

        /// CGEventFlags bit added on top of the primary chord. `nil` for
        /// `.none` so callers can branch on "is there a secondary chord?".
        var cgFlag: CGEventFlags? {
            switch self {
            case .none:  return nil
            case .shift: return .maskShift
            }
        }

        /// Human-readable form for menus / log lines.
        var displayName: String {
            switch self {
            case .none:  return "none"
            case .shift: return "Shift"
            }
        }
    }

    /// What the katakana chord does when a conversion session is active.
    /// Default `.cycleBackwards` matches the new UX: Shift+hotkey keeps
    /// katakana on a fresh conversion, but steps backward through the
    /// current candidate list while a session is live. `.katakana`
    /// preserves the old behavior.
    enum KatakanaModifierBehavior: Equatable {
        case cycleBackwards
        case katakana

        var displayName: String {
            switch self {
            case .cycleBackwards: return "cycle_backwards"
            case .katakana: return "katakana"
            }
        }
    }

    /// Extra modifier that, layered on top of the primary chord, fires the
    /// cycle-next gesture (advance to the next Mozc candidate at the same
    /// span). Same shape as `KatakanaModifier` — `.none` disables the
    /// chord. Defaults to `.alt`, so `Alt+<primary>` is the conventional
    /// "next candidate" chord on a fresh install. Validated against
    /// `KatakanaModifier` at parse time: same flag bit can't drive two
    /// gestures, so the second to load loses (logged).
    enum CycleModifier: Equatable {
        case none
        case shift
        case alt
        case control

        var cgFlag: CGEventFlags? {
            switch self {
            case .none:    return nil
            case .shift:   return .maskShift
            case .alt:     return .maskAlternate
            case .control: return .maskControl
            }
        }

        var displayName: String {
            switch self {
            case .none:    return "none"
            case .shift:   return "Shift"
            case .alt:     return "Alt"
            case .control: return "Ctrl"
            }
        }
    }

    /// What pressing the cycle chord does when the session is in the
    /// undone state (`candidateIndex = -1`, post-Esc). `redo` snaps back
    /// to `candidates[0]` — fast undo-of-undo. `pass` makes the cycle
    /// chord a no-op so the user's Esc decision sticks.
    enum CycleFromUndone: Equatable {
        case redo
        case pass

        var displayName: String {
            switch self {
            case .redo: return "redo"
            case .pass: return "pass"
            }
        }
    }

    /// When the candidate panel (the floating list of Mozc alternatives)
    /// appears. `none` = panel disabled entirely (default; pre-feature
    /// behavior). `onCycle` = stays hidden on fresh conversions, shows
    /// after the first cycle press so the user can see what they're
    /// stepping through. `onConvert` = shows on every successful
    /// conversion and stays through the session window. Hidden on
    /// session clear (focus moved, non-chord keystroke, window expired).
    enum CandidatePanelMode: Equatable {
        case none
        case onCycle
        case onConvert

        var displayName: String {
            switch self {
            case .none:      return "none"
            case .onCycle:   return "on_cycle"
            case .onConvert: return "on_convert"
            }
        }
    }

    /// Max age (ms) of the most-recent-conversion snapshot for Esc-undo to
    /// still fire. 0 disables the feature entirely (the tap callback's Esc
    /// branch short-circuits before touching any state). Clamped to
    /// [0, 30000] — anything past 30 s is well into "I forgot what I was
    /// doing" territory and Esc means something else by then.
    static let defaultUndoWindowMs: Int = 5000
    static let undoWindowRange: ClosedRange<Int> = 0...30000

    /// How long the candidate panel stays visible after each show. Reset
    /// on every `show()` call (cycle / convert / undo), so a chain of
    /// cycle presses keeps the panel alive. `0` disables auto-hide — the
    /// panel persists for the lifetime of the session (cleared the same
    /// way the session is). Clamped to [0, 30000].
    static let defaultCandidatePanelDurationMs: Int = 1500
    static let candidatePanelDurationRange: ClosedRange<Int> = 0...30000

    /// Tunable timings for the clipboard fallback path in `doClipboardPickup`.
    /// Defaults match the previously hard-coded numbers, so omitting the
    /// `[clipboard]` section reproduces pre-config behavior exactly.
    ///
    /// The 80 ms initial-peek timeout is intentionally *not* exposed — it's a
    /// heuristic ("real selection → app responds in <30 ms; if it takes longer
    /// there's no selection, fall through to force-select"), not a tuning knob.
    struct ClipboardTimings: Equatable {
        /// Pause after `Shift+Opt+Left` before issuing `Cmd+C`, so the
        /// renderer thread in Electron/Chromium apps has time to commit the
        /// new selection. Bump this if force-select copies miss intermittently.
        var preCopyDelayMs: Int = 20

        /// Max wait for the clipboard `changeCount` to advance after the
        /// force-select `Cmd+C`. Bump on slow machines / under heavy load.
        var readTimeoutMs: Int = 250

        /// Delay before writing the user's original clipboard back, so the
        /// Unicode injection that consumed the selection has fully landed.
        var restoreClipboardDelayMs: Int = 50
    }


    /// Parse `[conversion] shadow_buffer`. Wrapper that logs issues.
    /// Default `false` — the shadow buffer is opt-in; omitting the key
    /// keeps the existing clipboard/AX-only pickup chain.
    static func loadShadowBufferEnabled() -> Bool {
        let (v, issues) = parseShadowBufferEnabled()
        for issue in issues { Log.config(issue) }
        return v
    }

    static func parseShadowBufferEnabled() -> (Bool, [String]) {
        var enabled = false
        var issues: [String] = []
        let url = configFileURL()
        _ = forEachKeyValue(url) { section, key, value in
            guard section == "conversion" && key == "shadow_buffer" else { return }
            switch value.lowercased() {
            case "on", "true", "1", "yes":
                enabled = true
            case "off", "false", "0", "no", "":
                enabled = false
            default:
                issues.append("ignoring [conversion] shadow_buffer=\(value) (expected on|off)")
            }
        }
        return (enabled, issues)
    }
    /// Parse `[conversion] classifier`. Wrapper that logs issues.
    /// Default `false` — the ML classifier is opt-in; omitting the key
    /// keeps the existing heuristic pipeline (splitAcronymHead).
    static func loadClassifierEnabled() -> Bool {
        let (v, issues) = parseClassifierEnabled()
        for issue in issues { Log.config(issue) }
        return v
    }

    static func parseClassifierEnabled() -> (Bool, [String]) {
        var enabled = false
        var issues: [String] = []
        let url = configFileURL()
        _ = forEachKeyValue(url) { section, key, value in
            guard section == "conversion" && key == "classifier" else { return }
            switch value.lowercased() {
            case "on", "true", "1", "yes":
                enabled = true
            case "off", "false", "0", "no", "":
                enabled = false
            default:
                issues.append("ignoring [conversion] classifier=\(value) (expected on|off)")
            }
        }
        return (enabled, issues)
    }

    /// Parse `[bridge] mozc_backend` with `[conversion]` as a compatibility
    /// fallback. Wrapper that logs issues.
    /// Default bundled Mozc preserves the long-standing built-in behavior.
    static func loadMozcBackend() -> MozcBackend {
        let (v, issues) = parseMozcBackend()
        for issue in issues { Log.config(issue) }
        return v
    }

    /// Same parse as `loadMozcBackend()` but returns issues separately.
    /// Missing key yields bundled Mozc; malformed values yield bundled Mozc with one
    /// issue string. `[bridge] mozc_backend` wins over the legacy
    /// `[conversion]` location if both are set.
    static func parseMozcBackend() -> (MozcBackend, [String]) {
        var backend: MozcBackend = .oss
        var issues: [String] = []
        let url = configFileURL()
        _ = forEachKeyValue(url) { section, key, value in
            guard key == "mozc_backend" else { return }
            guard section == "bridge" || section == "conversion" else { return }
            switch value.lowercased() {
            case "oss", "built-in", "built_in", "":
                backend = .oss
            case "google_ime", "google-ime", "googleime":
                backend = .googleIme
            default:
                issues.append("ignoring [\(section)] mozc_backend=\(value) (expected built-in|google_ime)")
            }
        }
        return (backend, issues)
    }

    /// Parse `[bridge]` knobs that map onto env vars before bridge init.
    /// Values are only read once at boot and on config reload, so this adds
    /// no per-conversion overhead.
    static func loadBridgeRuntime() -> BridgeRuntime {
        let (runtime, issues) = parseBridgeRuntime()
        for issue in issues { Log.config(issue) }
        return runtime
    }

    /// Same parse as `loadBridgeRuntime()` but returns issues separately.
    static func parseBridgeRuntime() -> (BridgeRuntime, [String]) {
        var runtime = BridgeRuntime()
        var issues: [String] = []
        let url = configFileURL()
        _ = forEachKeyValue(url) { section, key, value in
            guard section == "bridge" else { return }
            switch key {
            case "candidate_mixing_mode":
                guard let parsed = Int(value), parsed >= 0 else {
                    issues.append("ignoring [bridge] candidate_mixing_mode=\(value) (expected non-negative integer)")
                    return
                }
                runtime.candidateMixingMode = parsed
            case "trace_raw_candidates":
                switch value.lowercased() {
                case "on", "true", "1", "yes":
                    runtime.traceRawCandidates = true
                case "off", "false", "0", "no":
                    runtime.traceRawCandidates = false
                default:
                    issues.append("ignoring [bridge] trace_raw_candidates=\(value) (expected on|off)")
                }
            case "mozc_backend":
                // Valid [bridge] key, but consumed by parseMozcBackend (it can
                // also live under [conversion]). Recognize it here so this
                // parser doesn't flag it as unknown.
                break
            default:
                issues.append("ignoring [bridge] \(key)=\(value) (unknown key)")
            }
        }
        return (runtime, issues)
    }

    /// Parse `[logging] disabled=...` into a compact runtime namespace mask.
    /// The host applies this before any other startup logs so disabled
    /// namespaces cost only a bitmask test in the hot path.
    static func loadDisabledLoggingNamespaces() -> LoggingNamespaceMask {
        let (mask, issues) = parseDisabledLoggingNamespaces()
        for issue in issues { Log.config(issue) }
        return mask
    }

    /// Same parse as `loadDisabledLoggingNamespaces()` but returns issues
    /// separately. Values are comma-separated namespace roots. `all`
    /// disables every built-in namespace; `none` is a no-op.
    static func parseDisabledLoggingNamespaces() -> (LoggingNamespaceMask, [String]) {
        var mask = LoggingNamespaceMask()
        var issues: [String] = []
        let url = configFileURL()
        _ = forEachKeyValue(url) { section, key, value in
            guard section == "logging" else { return }
            guard key == "disabled" || key == "disable" else { return }
            for rawToken in value.split(separator: ",", omittingEmptySubsequences: true) {
                let token = normalizeLoggingNamespaceToken(String(rawToken))
                if token.isEmpty || token == "none" {
                    continue
                }
                if token == "all" {
                    mask.formUnion(.allKnown)
                    continue
                }
                guard let ns = LoggingNamespaceMask(namespace: token) else {
                    issues.append("ignoring [logging] \(key)=\(value) (unknown namespace \(token))")
                    continue
                }
                mask.insert(ns)
            }
        }
        return (mask, issues)
    }

    private static func normalizeLoggingNamespaceToken(_ token: String) -> String {
        let trimmed = token.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard let colon = trimmed.firstIndex(of: ":") else { return trimmed }
        return String(trimmed[..<colon])
    }

    private static let defaultChord = "Cmd+Semicolon"

    static func defaultConversionHotkey() -> ConversionHotkey {
        if let h = parseChord(defaultChord) {
            return h
        }
        return ConversionHotkey(
            keyCode: CGKeyCode(kVK_ANSI_Semicolon),
            coreFlags: .maskCommand,
            displayName: defaultChord)
    }

    static func configFileURL() -> URL {
        if let xdg = ProcessInfo.processInfo.environment["XDG_CONFIG_HOME"], !xdg.isEmpty {
            return URL(fileURLWithPath: xdg).appendingPathComponent("modore/modore.conf")
        }
        return FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".config/modore/modore.conf")
    }

    /// Parse `~/.config/modore/modore.conf` and report the hotkey outcome.
    /// Pure — does not log, does not touch globals. Callers decide.
    static func loadConversionHotkeyOutcome() -> LoadOutcome {
        let url = configFileURL()
        var outcome: LoadOutcome? = nil
        let parsed = forEachKeyValue(url) { section, key, value in
            if section == "conversion" && key == "hotkey" {
                if let h = parseChord(value) {
                    outcome = .loaded(h, source: "[conversion] hotkey=\(value) (\(url.path))")
                } else {
                    outcome = .invalid(reason: "malformed [conversion] hotkey=\(value) in \(url.path)")
                }
            }
        }
        if !parsed {
            return .usingDefault(reason: "no config at \(url.path)")
        }
        return outcome ?? .usingDefault(reason: "[conversion] hotkey not set in \(url.path)")
    }

    /// Parse `~/.config/modore/modore.conf` for `[conversion] katakana_modifier`.
    /// Logs issues via `Log.config`; never fails. Wrapper around
    /// `parseKatakanaModifier()` for the runtime path.
    static func loadKatakanaModifier() -> KatakanaModifier {
        let (m, issues) = parseKatakanaModifier()
        for issue in issues {
            Log.config(issue)
        }
        return m
    }

    /// Same parse as `loadKatakanaModifier()` but returns the validation
    /// issues separately instead of logging them. Used by `--check-config`.
    /// Missing file / missing key / unknown value all yield `.none` with at
    /// most one issue string; the running host treats `.none` as "no
    /// secondary chord, behave exactly as before."
    static func parseKatakanaModifier() -> (KatakanaModifier, [String]) {
        var m: KatakanaModifier = .none
        var issues: [String] = []
        let url = configFileURL()
        _ = forEachKeyValue(url) { section, key, value in
            guard section == "conversion" && key == "katakana_modifier" else { return }
            switch value.lowercased() {
            case "none", "":
                m = .none
            case "shift":
                m = .shift
            default:
                issues.append("ignoring [conversion] katakana_modifier=\(value) (expected none|shift)")
            }
        }
        return (m, issues)
    }

    /// Parse `[conversion] katakana_modifier_behavior`. Wrapper that logs issues.
    static func loadKatakanaModifierBehavior() -> KatakanaModifierBehavior {
        let (m, issues) = parseKatakanaModifierBehavior()
        for issue in issues {
            Log.config(issue)
        }
        return m
    }

    /// Same parse as `loadKatakanaModifierBehavior()` but returns the
    /// validation issues separately instead of logging them.
    static func parseKatakanaModifierBehavior() -> (KatakanaModifierBehavior, [String]) {
        var m: KatakanaModifierBehavior = .cycleBackwards
        var issues: [String] = []
        let url = configFileURL()
        _ = forEachKeyValue(url) { section, key, value in
            guard section == "conversion" && key == "katakana_modifier_behavior" else { return }
            switch value.lowercased() {
            case "cycle_backwards", "":
                m = .cycleBackwards
            case "katakana":
                m = .katakana
            default:
                issues.append("ignoring [conversion] katakana_modifier_behavior=\(value) (expected katakana|cycle_backwards)")
            }
        }
        return (m, issues)
    }

    /// Parse `[conversion] cycle_modifier`. Wrapper that logs issues.
    static func loadCycleModifier() -> CycleModifier {
        let (m, issues) = parseCycleModifier()
        for issue in issues { Log.config(issue) }
        return m
    }

    /// Same parse as `loadCycleModifier()` but returns issues separately.
    /// Default `.none`: the primary chord doubles as the cycle gesture
    /// while a session is active, so a fresh install needs no extra
    /// chord. Set this to bind an *additional* dedicated cycle chord
    /// alongside the primary's same-key cycle.
    static func parseCycleModifier() -> (CycleModifier, [String]) {
        var m: CycleModifier = .none
        var issues: [String] = []
        let url = configFileURL()
        _ = forEachKeyValue(url) { section, key, value in
            guard section == "conversion" && key == "cycle_modifier" else { return }
            switch value.lowercased() {
            case "none":                 m = .none
            case "shift":                m = .shift
            case "alt", "option":        m = .alt
            case "control", "ctrl":      m = .control
            case "":
                // Empty value (e.g. `cycle_modifier =`) — treat as
                // explicit "leave default" rather than an issue. Matches
                // the behavior of katakana_modifier=.
                break
            default:
                issues.append("ignoring [conversion] cycle_modifier=\(value) (expected none|shift|alt|control)")
            }
        }
        return (m, issues)
    }

    /// Parse `[conversion] cycle_from_undone`. Wrapper that logs issues.
    static func loadCycleFromUndone() -> CycleFromUndone {
        let (m, issues) = parseCycleFromUndone()
        for issue in issues { Log.config(issue) }
        return m
    }

    /// Same parse as `loadCycleFromUndone()` but returns issues separately.
    /// Default `.redo` (cycle from undone → candidates[0]). `pass`
    /// silences cycle while in the undone state so an explicit Esc isn't
    /// trivially reversed.
    static func parseCycleFromUndone() -> (CycleFromUndone, [String]) {
        var v: CycleFromUndone = .redo
        var issues: [String] = []
        let url = configFileURL()
        _ = forEachKeyValue(url) { section, key, value in
            guard section == "conversion" && key == "cycle_from_undone" else { return }
            switch value.lowercased() {
            case "redo": v = .redo
            case "pass": v = .pass
            default:
                issues.append("ignoring [conversion] cycle_from_undone=\(value) (expected redo|pass)")
            }
        }
        return (v, issues)
    }

    /// Parse `[ui] candidate_panel`. Wrapper that logs issues.
    static func loadCandidatePanelMode() -> CandidatePanelMode {
        let (m, issues) = parseCandidatePanelMode()
        for issue in issues { Log.config(issue) }
        return m
    }

    /// Same parse as `loadCandidatePanelMode()` but returns issues
    /// separately. Default `.none` reproduces the pre-feature behavior
    /// (no panel) — omitting the `[ui]` section is always a no-op.
    static func parseCandidatePanelMode() -> (CandidatePanelMode, [String]) {
        var m: CandidatePanelMode = .none
        var issues: [String] = []
        let url = configFileURL()
        _ = forEachKeyValue(url) { section, key, value in
            guard section == "ui" && key == "candidate_panel" else { return }
            switch value.lowercased() {
            case "none", "off", "":   m = .none
            case "on_cycle":          m = .onCycle
            case "on_convert":        m = .onConvert
            default:
                issues.append("ignoring [ui] candidate_panel=\(value) (expected none|on_cycle|on_convert)")
            }
        }
        return (m, issues)
    }

    /// Parse `[ui] candidate_panel_duration_ms`. Wrapper that logs issues.
    static func loadCandidatePanelDurationMs() -> Int {
        let (n, issues) = parseCandidatePanelDurationMs()
        for issue in issues { Log.config(issue) }
        return n
    }

    /// Same parse as `loadCandidatePanelDurationMs()` but returns issues
    /// separately. Missing key yields the default (1500); malformed or
    /// out-of-range values yield the default with one issue string.
    static func parseCandidatePanelDurationMs() -> (Int, [String]) {
        var n: Int = defaultCandidatePanelDurationMs
        var issues: [String] = []
        let url = configFileURL()
        _ = forEachKeyValue(url) { section, key, value in
            guard section == "ui" && key == "candidate_panel_duration_ms" else { return }
            guard let parsed = Int(value) else {
                issues.append("ignoring [ui] candidate_panel_duration_ms=\(value) (expected integer)")
                return
            }
            guard candidatePanelDurationRange.contains(parsed) else {
                issues.append("ignoring [ui] candidate_panel_duration_ms=\(parsed) (out of range \(candidatePanelDurationRange.lowerBound)..\(candidatePanelDurationRange.upperBound))")
                return
            }
            n = parsed
        }
        return (n, issues)
    }

    /// Parse `~/.config/modore/modore.conf` for `[conversion] undo_window_ms`.
    /// Wrapper around `parseUndoWindowMs()` that logs issues via `Log.config`.
    static func loadUndoWindowMs() -> Int {
        let (n, issues) = parseUndoWindowMs()
        for issue in issues {
            Log.config(issue)
        }
        return n
    }

    /// Same parse as `loadUndoWindowMs()` but returns issues separately.
    /// Missing file / missing key yields the default (5000); malformed or
    /// out-of-range values yield the default with one issue string.
    static func parseUndoWindowMs() -> (Int, [String]) {
        var n: Int = defaultUndoWindowMs
        var issues: [String] = []
        let url = configFileURL()
        _ = forEachKeyValue(url) { section, key, value in
            guard section == "conversion" && key == "undo_window_ms" else { return }
            guard let parsed = Int(value) else {
                issues.append("ignoring [conversion] undo_window_ms=\(value) (expected integer)")
                return
            }
            guard undoWindowRange.contains(parsed) else {
                issues.append("ignoring [conversion] undo_window_ms=\(parsed) (out of range \(undoWindowRange.lowerBound)..\(undoWindowRange.upperBound))")
                return
            }
            n = parsed
        }
        return (n, issues)
    }

    /// Parse `~/.config/modore/modore.conf` for `[clipboard]` timing keys.
    /// Missing file / missing keys / malformed values all fall back to the
    /// hard-coded defaults — never fails. Malformed values get a single
    /// `[config]` log line so the user can see what was ignored.
    static func loadClipboardTimings() -> ClipboardTimings {
        let (t, issues) = parseClipboardTimings()
        for issue in issues {
            Log.config(issue)
        }
        return t
    }

    /// Same parse as `loadClipboardTimings()` but returns the validation
    /// issues separately instead of logging them. Used by `--check-config`.
    static func parseClipboardTimings() -> (ClipboardTimings, [String]) {
        var t = ClipboardTimings()
        var issues: [String] = []
        let url = configFileURL()
        _ = forEachKeyValue(url) { section, key, value in
            guard section == "clipboard" else { return }
            guard let n = Int(value), n >= 0 else {
                issues.append("ignoring [clipboard] \(key)=\(value) (expected non-negative integer)")
                return
            }
            switch key {
            case "pre_copy_delay_ms":          t.preCopyDelayMs = n
            case "read_timeout_ms":            t.readTimeoutMs = n
            case "restore_clipboard_delay_ms": t.restoreClipboardDelayMs = n
            default:
                issues.append("ignoring [clipboard] \(key)=\(value) (unknown key)")
            }
        }
        return (t, issues)
    }

    /// Convenience for startup: always returns a usable chord, logs the outcome,
    /// falls back to the default on any non-loaded case.
    static func loadConversionHotkey() -> ConversionHotkey {
        switch loadConversionHotkeyOutcome() {
        case .loaded(let h, let source):
            Log.config("loaded \(source)")
            return h
        case .usingDefault(let reason):
            Log.config("\(reason) — using default \(defaultChord)")
            return defaultConversionHotkey()
        case .invalid(let reason):
            Log.config("\(reason) — using default \(defaultChord)")
            return defaultConversionHotkey()
        }
    }

    /// Shared INI-line tokenizer. Calls `handler(section, key, value)` once per
    /// `key = value` pair (sections lowercased, keys lowercased, values
    /// preserved as-is). Returns `false` only if the file can't be read at all.
    /// Comments (`# ...`), blank lines, and malformed lines (no `=`) are skipped.
    private static func forEachKeyValue(
        _ url: URL,
        handler: (_ section: String, _ key: String, _ value: String) -> Void
    ) -> Bool {
        guard let data = try? Data(contentsOf: url),
              var text = String(data: data, encoding: .utf8) else {
            return false
        }
        // Strip a leading UTF-8 BOM (U+FEFF): Swift's UTF-8 decoder keeps it,
        // and it isn't a whitespace character, so without this the first line
        // stays "\u{FEFF}[section]", fails the hasPrefix("[") check, and the
        // opening [section] header (plus its keys) is silently dropped.
        if text.hasPrefix("\u{FEFF}") {
            text.removeFirst()
        }
        var section = ""
        for raw in text.split(whereSeparator: \.isNewline) {
            var line = String(raw)
            if let hash = line.firstIndex(of: "#") {
                line = String(line[..<hash])
            }
            line = line.trimmingCharacters(in: .whitespaces)
            if line.isEmpty { continue }

            if line.hasPrefix("[") && line.hasSuffix("]") {
                section = String(line.dropFirst().dropLast())
                    .trimmingCharacters(in: .whitespaces).lowercased()
                continue
            }
            let parts = line.split(separator: "=", maxSplits: 1)
                .map { String($0).trimmingCharacters(in: .whitespaces) }
            guard parts.count == 2 else { continue }
            handler(section, parts[0].lowercased(), parts[1])
        }
        return true
    }

    static func parseChord(_ s: String) -> ConversionHotkey? {
        let segments = s.split(separator: "+").map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
        guard !segments.isEmpty else { return nil }

        var flags: CGEventFlags = []
        if segments.count >= 2 {
            for i in 0..<(segments.count - 1) {
                switch segments[i].lowercased() {
                case "ctrl", "control":
                    flags.insert(.maskControl)
                case "shift":
                    flags.insert(.maskShift)
                case "alt", "option", "meta":
                    flags.insert(.maskAlternate)
                case "super", "win", "command", "cmd":
                    flags.insert(.maskCommand)
                default:
                    return nil
                }
            }
        }

        let keyName = segments.last!
        guard let keyCode = carbonKeyCode(named: keyName) else { return nil }
        return ConversionHotkey(keyCode: keyCode, coreFlags: flags, displayName: s)
    }

    private static func carbonKeyCode(named name: String) -> CGKeyCode? {
        let t = name.trimmingCharacters(in: .whitespaces)
        let lower = t.lowercased()

        if lower.count == 1, let ch = lower.first {
            switch ch {
            case "a": return CGKeyCode(kVK_ANSI_A)
            case "b": return CGKeyCode(kVK_ANSI_B)
            case "c": return CGKeyCode(kVK_ANSI_C)
            case "d": return CGKeyCode(kVK_ANSI_D)
            case "e": return CGKeyCode(kVK_ANSI_E)
            case "f": return CGKeyCode(kVK_ANSI_F)
            case "g": return CGKeyCode(kVK_ANSI_G)
            case "h": return CGKeyCode(kVK_ANSI_H)
            case "i": return CGKeyCode(kVK_ANSI_I)
            case "j": return CGKeyCode(kVK_ANSI_J)
            case "k": return CGKeyCode(kVK_ANSI_K)
            case "l": return CGKeyCode(kVK_ANSI_L)
            case "m": return CGKeyCode(kVK_ANSI_M)
            case "n": return CGKeyCode(kVK_ANSI_N)
            case "o": return CGKeyCode(kVK_ANSI_O)
            case "p": return CGKeyCode(kVK_ANSI_P)
            case "q": return CGKeyCode(kVK_ANSI_Q)
            case "r": return CGKeyCode(kVK_ANSI_R)
            case "s": return CGKeyCode(kVK_ANSI_S)
            case "t": return CGKeyCode(kVK_ANSI_T)
            case "u": return CGKeyCode(kVK_ANSI_U)
            case "v": return CGKeyCode(kVK_ANSI_V)
            case "w": return CGKeyCode(kVK_ANSI_W)
            case "x": return CGKeyCode(kVK_ANSI_X)
            case "y": return CGKeyCode(kVK_ANSI_Y)
            case "z": return CGKeyCode(kVK_ANSI_Z)
            default: break
            }
        }

        if lower.count == 1, let ch = lower.first, ch >= "0" && ch <= "9" {
            let map: [Character: CGKeyCode] = [
                "1": CGKeyCode(kVK_ANSI_1), "2": CGKeyCode(kVK_ANSI_2),
                "3": CGKeyCode(kVK_ANSI_3), "4": CGKeyCode(kVK_ANSI_4),
                "5": CGKeyCode(kVK_ANSI_5), "6": CGKeyCode(kVK_ANSI_6),
                "7": CGKeyCode(kVK_ANSI_7), "8": CGKeyCode(kVK_ANSI_8),
                "9": CGKeyCode(kVK_ANSI_9), "0": CGKeyCode(kVK_ANSI_0),
            ]
            return map[ch]
        }

        switch lower {
        case "slash": return CGKeyCode(kVK_ANSI_Slash)
        case "period": return CGKeyCode(kVK_ANSI_Period)
        case "comma": return CGKeyCode(kVK_ANSI_Comma)
        case "semicolon": return CGKeyCode(kVK_ANSI_Semicolon)
        case "quote", "apostrophe": return CGKeyCode(kVK_ANSI_Quote)
        case "grave", "backquote": return CGKeyCode(kVK_ANSI_Grave)
        case "minus": return CGKeyCode(kVK_ANSI_Minus)
        case "equal": return CGKeyCode(kVK_ANSI_Equal)
        case "space": return CGKeyCode(kVK_Space)
        case "return", "enter": return CGKeyCode(kVK_Return)
        case "tab": return CGKeyCode(kVK_Tab)
        case "escape", "esc": return CGKeyCode(kVK_Escape)
        case "delete": return CGKeyCode(kVK_ForwardDelete)
        case "backspace": return CGKeyCode(kVK_Delete)
        case "left": return CGKeyCode(kVK_LeftArrow)
        case "right": return CGKeyCode(kVK_RightArrow)
        case "down": return CGKeyCode(kVK_DownArrow)
        case "up": return CGKeyCode(kVK_UpArrow)
        case "home": return CGKeyCode(kVK_Home)
        case "end": return CGKeyCode(kVK_End)
        case "pageup": return CGKeyCode(kVK_PageUp)
        case "pagedown": return CGKeyCode(kVK_PageDown)
        case "bracketleft": return CGKeyCode(kVK_ANSI_LeftBracket)
        case "bracketright": return CGKeyCode(kVK_ANSI_RightBracket)
        case "backslash": return CGKeyCode(kVK_ANSI_Backslash)
        default:
            break
        }

        if lower.hasPrefix("f"), lower.count >= 2,
           let n = Int(lower.dropFirst()), n >= 1 && n <= 12 {
            let f: [CGKeyCode] = [
                CGKeyCode(kVK_F1), CGKeyCode(kVK_F2), CGKeyCode(kVK_F3), CGKeyCode(kVK_F4),
                CGKeyCode(kVK_F5), CGKeyCode(kVK_F6), CGKeyCode(kVK_F7), CGKeyCode(kVK_F8),
                CGKeyCode(kVK_F9), CGKeyCode(kVK_F10), CGKeyCode(kVK_F11), CGKeyCode(kVK_F12),
            ]
            return f[n - 1]
        }

        return nil
    }

    // MARK: - Debug overlay

    /// Parse `[debug] overlay`. Default `false` — the overlay is dev-only
    /// and must be explicitly enabled. Accepts on/yes/true/1.
    static func loadDebugOverlay() -> Bool {
        var enabled = false
        _ = forEachKeyValue(configFileURL()) { section, key, value in
            guard section == "debug" && key == "overlay" else { return }
            enabled = ["on", "yes", "true", "1"].contains(value.lowercased())
        }
        return enabled
    }
}
