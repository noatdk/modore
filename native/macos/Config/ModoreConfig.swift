// Loads ~/.config/modore/modore.conf (same path on macOS/Linux via XDG layout).
//
// Sections:
//   [conversion] hotkey=...             — global trigger chord (same format as Linux)
//   [conversion] katakana_modifier=...  — extra modifier that forces katakana (macOS only)
//   [clipboard]  *_ms=<integer>         — fallback-path timings (macOS only)

import Carbon
import Foundation

enum ModoreConfig {

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

    private static let defaultChord = "Ctrl+Slash"

    static func defaultConversionHotkey() -> ConversionHotkey {
        if let h = parseChord(defaultChord) {
            return h
        }
        return ConversionHotkey(
            keyCode: CGKeyCode(kVK_ANSI_Slash),
            coreFlags: .maskControl,
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
              let text = String(data: data, encoding: .utf8) else {
            return false
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

    private static func parseChord(_ s: String) -> ConversionHotkey? {
        let segments = s.split(separator: "+").map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
        guard segments.count >= 2 else { return nil }

        var flags: CGEventFlags = []
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

        let keyName = segments.last!
        guard let keyCode = carbonKeyCode(named: keyName) else { return nil }
        return ConversionHotkey(keyCode: keyCode, coreFlags: flags, displayName: s)
    }

    private static func carbonKeyCode(named name: String) -> CGKeyCode? {
        let t = name.trimmingCharacters(in: .whitespaces)
        let lower = t.lowercased()

        if lower.count == 1, let ch = lower.first {
            if ch >= "a" && ch <= "z" {
                let o = Int(ch.asciiValue! - UInt8(ascii: "a"))
                return CGKeyCode(kVK_ANSI_A + o)
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
}
