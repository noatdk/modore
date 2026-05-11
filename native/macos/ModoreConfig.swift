// Loads ~/.config/modore/modore.conf (same path on macOS/Linux via XDG layout).
// Section [conversion], key "hotkey" — same format as the Linux host.

import Carbon
import Foundation

enum ModoreConfig {

    struct ConversionHotkey {
        var keyCode: CGKeyCode
        var coreFlags: CGEventFlags
    }

    private static let defaultChord = "Ctrl+Slash"

    static func configFileURL() -> URL {
        if let xdg = ProcessInfo.processInfo.environment["XDG_CONFIG_HOME"], !xdg.isEmpty {
            return URL(fileURLWithPath: xdg).appendingPathComponent("modore/modore.conf")
        }
        return FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".config/modore/modore.conf")
    }

    /// Reads `[conversion] hotkey` or returns defaults. Logs a warning if the file specifies an invalid chord.
    static func loadConversionHotkey() -> ConversionHotkey {
        let url = configFileURL()
        guard let data = try? Data(contentsOf: url),
              let text = String(data: data, encoding: .utf8) else {
            return parseChordOrDefault(defaultChord, source: "default")
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
                section = String(line.dropFirst().dropLast()).trimmingCharacters(in: .whitespaces).lowercased()
                continue
            }
            let parts = line.split(separator: "=", maxSplits: 1).map { String($0).trimmingCharacters(in: .whitespaces) }
            guard parts.count == 2 else { continue }
            let key = parts[0].lowercased()
            let value = parts[1]
            if section == "conversion" && key == "hotkey" {
                if let h = parseChord(value) {
                    NSLog("[modore] config loaded (%@): hotkey=%@", url.path, value)
                    return h
                }
                NSLog("[modore] config invalid hotkey in %@ — using default (%@)", url.path, defaultChord)
                return parseChordOrDefault(defaultChord, source: "fallback")
            }
        }

        return parseChordOrDefault(defaultChord, source: "default (no hotkey key)")
    }

    private static func parseChordOrDefault(_ s: String, source: String) -> ConversionHotkey {
        if let h = parseChord(s) {
            NSLog("[modore] using %@ hotkey: %@", source, s)
            return h
        }
        // Last resort — must succeed for default chord.
        return ConversionHotkey(keyCode: CGKeyCode(kVK_ANSI_Slash), coreFlags: .maskControl)
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
        return ConversionHotkey(keyCode: keyCode, coreFlags: flags)
    }

    private static func carbonKeyCode(named name: String) -> CGKeyCode? {
        let t = name.trimmingCharacters(in: .whitespaces)
        let lower = t.lowercased()

        if lower.count == 1, let ch = lower.first {
            if ch >= "a" && ch <= "z" {
                let o = UInt16(ch.asciiValue! - UInt8(ascii: "a"))
                return CGKeyCode(kVK_ANSI_A + o)
            }
        }

        if lower.count == 1, let ch = lower.first, ch >= "0" && ch <= "9" {
            let map: [Character: UInt16] = [
                "1": kVK_ANSI_1, "2": kVK_ANSI_2, "3": kVK_ANSI_3, "4": kVK_ANSI_4,
                "5": kVK_ANSI_5, "6": kVK_ANSI_6, "7": kVK_ANSI_7, "8": kVK_ANSI_8,
                "9": kVK_ANSI_9, "0": kVK_ANSI_0,
            ]
            return map[ch].map { CGKeyCode($0) }
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
            let f: [Int: UInt16] = [
                1: kVK_F1, 2: kVK_F2, 3: kVK_F3, 4: kVK_F4,
                5: kVK_F5, 6: kVK_F6, 7: kVK_F7, 8: kVK_F8,
                9: kVK_F9, 10: kVK_F10, 11: kVK_F11, 12: kVK_F12,
            ]
            return f[n].map { CGKeyCode($0) }
        }

        return nil
    }
}
