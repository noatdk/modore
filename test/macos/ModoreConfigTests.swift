import Carbon
import Foundation

extension SpanSplitTests {
    static func runConfigTests() {
        withTemporaryConfig("""
        [conversion]
        hotkey = Ctrl+Shift+grave
        """) {
            switch ModoreConfig.loadConversionHotkeyOutcome() {
            case .loaded(let hotkey, _):
                expectEqual(hotkey.keyCode, CGKeyCode(kVK_ANSI_Grave), "config: README hotkey key")
                expectEqual(hotkey.coreFlags.contains(.maskControl), true, "config: README hotkey ctrl")
                expectEqual(hotkey.coreFlags.contains(.maskShift), true, "config: README hotkey shift")
                expectEqual(hotkey.coreFlags.contains(.maskCommand), false, "config: README hotkey no cmd")
                expectEqual(hotkey.coreFlags.contains(.maskAlternate), false, "config: README hotkey no alt")
            default:
                expectEqual(false, true, "config: README hotkey loads")
            }
        }

        withTemporaryConfig("\u{FEFF}[conversion]\nhotkey = Cmd+Semicolon\n") {
            switch ModoreConfig.loadConversionHotkeyOutcome() {
            case .loaded(let hotkey, _):
                expectEqual(hotkey.keyCode, CGKeyCode(kVK_ANSI_Semicolon), "config: BOM hotkey key")
                expectEqual(hotkey.coreFlags.contains(.maskCommand), true, "config: BOM hotkey cmd")
            default:
                expectEqual(false, true, "config: leading UTF-8 BOM does not hide first section")
            }
        }

        withTemporaryConfig("""
        [conversion]
        mozc_backend = google-ime
        """) {
            let (backend, issues) = ModoreConfig.parseMozcBackend()
            expectEqual(backend, .googleIme, "config: legacy conversion backend remains supported")
            expectEqual(issues.isEmpty, true, "config: legacy conversion backend has no issues")
        }

        withTemporaryConfig("""
        [bridge]
        mozc_backend = google_ime

        [conversion]
        mozc_backend = built-in
        """) {
            let (backend, issues) = ModoreConfig.parseMozcBackend()
            expectEqual(backend, .googleIme, "config: bridge backend wins over legacy backend")
            expectEqual(issues.isEmpty, true, "config: bridge backend precedence has no issues")
        }

        withTemporaryConfig("""
        [conversion]
        mozc_backend = google_ime

        [bridge]
        mozc_backend = definitely-not-a-backend
        """) {
            let (backend, issues) = ModoreConfig.parseMozcBackend()
            expectEqual(backend, .oss, "config: malformed bridge backend falls back to built-in")
            expectEqual(issues.count, 1, "config: malformed bridge backend reports one issue")
        }

        withTemporaryConfig("""
        [bridge]
        mozc_backend = google_ime
        candidate_mixing_mode = 2
        trace_raw_candidates = yes
        """) {
            let (runtime, issues) = ModoreConfig.parseBridgeRuntime()
            expectEqual(runtime.candidateMixingMode, 2, "config: bridge runtime candidate mixing")
            expectEqual(runtime.traceRawCandidates, true, "config: bridge runtime trace flag")
            expectEqual(issues.isEmpty, true, "config: mozc_backend is not an unknown bridge runtime key")
        }

        withTemporaryConfig("""
        [clipboard]
        pre_copy_delay_ms = 35
        read_timeout_ms = -1
        restore_clipboard_delay_ms = 70
        unexpected_ms = 10
        """) {
            let (timings, issues) = ModoreConfig.parseClipboardTimings()
            expectEqual(timings.preCopyDelayMs, 35, "config: clipboard keeps valid pre-copy")
            expectEqual(timings.readTimeoutMs, 250, "config: clipboard rejects negative timeout")
            expectEqual(timings.restoreClipboardDelayMs, 70, "config: clipboard keeps valid restore")
            expectEqual(issues.count, 2, "config: clipboard reports invalid and unknown keys")
        }

        withTemporaryConfig("""
        [logging]
        disabled = AX, scripting:engine, none, unknown
        """) {
            let (mask, issues) = ModoreConfig.parseDisabledLoggingNamespaces()
            expectEqual(mask.contains(.ax), true, "config: logging namespace case-insensitive")
            expectEqual(mask.contains(.scripting), true, "config: logging namespace strips child tag")
            expectEqual(mask.contains(.clipboard), false, "config: logging namespace leaves omitted tags enabled")
            expectEqual(issues.count, 1, "config: logging reports unknown namespace")
        }
    }

    private static func withTemporaryConfig(_ contents: String?, run: () -> Void) {
        let fm = FileManager.default
        let root = fm.temporaryDirectory
            .appendingPathComponent("modore-config-tests-\(UUID().uuidString)", isDirectory: true)
        let modoreDir = root.appendingPathComponent("modore", isDirectory: true)
        let configURL = modoreDir.appendingPathComponent("modore.conf")
        let previous = getenv("XDG_CONFIG_HOME").map { String(cString: $0) }

        do {
            try fm.createDirectory(at: modoreDir, withIntermediateDirectories: true)
            if let contents {
                try contents.write(to: configURL, atomically: true, encoding: .utf8)
            }
            setenv("XDG_CONFIG_HOME", root.path, 1)
            run()
        } catch {
            expectEqual(String(describing: error), "", "config: temporary fixture setup")
        }

        if let previous {
            setenv("XDG_CONFIG_HOME", previous, 1)
        } else {
            unsetenv("XDG_CONFIG_HOME")
        }
        try? fm.removeItem(at: root)
    }
}
