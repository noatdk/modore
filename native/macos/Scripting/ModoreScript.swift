// Swift façade for the Lua scripting engine (libmodore_script.dylib).
//
// One engine instance is created at boot via `ModoreScript.boot(scriptDir:)`
// and lives for the process lifetime. Hosts call the typed wrappers
// (`routeFor`, `pickup`, `replacement`, `candidates`)
// which translate Swift values to/from the C ABI and return `nil` whenever
// a script wants the host to use its own default — undefined hook, nil
// return, or thrown error inside Lua are all surfaced as nil here. The
// caller never has to know which case fired.
//
// String marshaling: the engine ABI speaks UTF-8 byte offsets. AX speaks
// UTF-16 code units. Conversions live in this file so call sites stay in
// UTF-16 land.

import AppKit
import Foundation

enum ModoreScript {

    // Process-wide handle. Written once on the main thread during boot,
    // before `NSApplication.run()` returns control to any hotkey delivery
    // path. Subsequent reads happen on the pickup dispatch queue; the
    // dispatch_async hop inserted by Carbon hotkey delivery provides the
    // acquire/release pairing that makes the boot-time write visible.
    // If a second writer is ever introduced, this needs a lock.
    private static var engine: OpaquePointer? = nil
    private static var scriptsDir: String? = nil

    static func boot(scriptDir: String) {
        guard let h = mdr_init() else {
            Log.tagged("scripting", "engine init failed")
            return
        }
        engine = h
        scriptsDir = scriptDir
        _ = mdr_set_log_callback(h, logTrampoline, nil)
        registerDefaultCallbacks(h)
        registerHostOps(h)
        _ = mdr_load_dir(h, scriptDir)
        Log.tagged("scripting", "engine ABI v\(mdr_abi_version()) loaded (dir=\(scriptDir))")
        if gAxTraceEnabled {
            Log.tagged("scripting", "MODORE_AX_TRACE=1 → per-chord AX snapshots ENABLED")
        }
    }

    /// True iff `<appId>.lua` exists in the scripts dir. Used by the status
    /// item to surface that a per-app script will run for the frontmost app.
    /// `default.lua` does NOT count — it'd light up the indicator for every
    /// app and stop being a useful signal.
    static func hasScript(forAppId appId: String) -> Bool {
        guard let dir = scriptsDir, !appId.isEmpty else { return false }
        let path = (dir as NSString).appendingPathComponent("\(appId).lua")
        return FileManager.default.fileExists(atPath: path)
    }

    /// Wire the imperative primitives Lua scripts can compose inside the
    /// stage hooks. Each trampoline is a
    /// `@convention(c)` function
    /// pointer with no Swift captures, so it's emitted as a stable linker-
    /// level symbol; the stack-local `ops` carries only the four pointer
    /// values, and `mdr_set_host_ops` copies them into engine storage. The
    /// `ops` local can safely go out of scope after the call.
    private static func registerHostOps(_ h: OpaquePointer) {
        var ops = mdr_host_ops_t(
            send_chord: { _, chordPtr in
                guard let p = chordPtr, let chord = String(validatingUTF8: p) else { return }
                hostSendChord(chord)
            },
            sleep_ms: { _, ms in
                if ms > 0 { Thread.sleep(forTimeInterval: Double(ms) / 1000.0) }
            },
            clipboard_read: { _, outBuf, cap, outLen in
                guard let buf = outBuf, let lenOut = outLen, cap > 0 else { return 0 }
                guard let s = NSPasteboard.general.string(forType: .string) else { return 0 }
                return ModoreScript.fillCBuffer(s, buf, cap, lenOut)
            },
            clipboard_write: { _, textPtr, len in
                guard let p = textPtr else { return 0 }
                let data = Data(bytes: p, count: len)
                guard let s = String(data: data, encoding: .utf8) else { return 0 }
                let pb = NSPasteboard.general
                pb.clearContents()
                return pb.setString(s, forType: .string) ? 1 : 0
            },
            read_selection: { _, outBuf, cap, outLen in
                guard let buf = outBuf, let lenOut = outLen, cap > 0 else { return 0 }
                guard let s = readFocusedSelection(), !s.isEmpty else { return 0 }
                return ModoreScript.fillCBuffer(s, buf, cap, lenOut)
            }
        )
        _ = mdr_set_host_ops(h, &ops, nil)
    }

    /// Copy `s` as UTF-8 into a C out-buffer (NUL-terminated, truncated to
    /// `cap - 1` bytes) and report the written length. Returns the `1`
    /// success code the host-op ABI expects. Shared by the
    /// `clipboard_read` / `read_selection` trampolines, which marshal a
    /// Swift String into the engine the same way. Caller guarantees `cap > 0`.
    private static func fillCBuffer(
        _ s: String,
        _ buf: UnsafeMutablePointer<CChar>,
        _ cap: Int,
        _ outLen: UnsafeMutablePointer<size_t>
    ) -> Int32 {
        let bytes = Array(s.utf8)
        let n = min(bytes.count, cap - 1)
        bytes.withUnsafeBytes { src in
            guard let base = src.baseAddress else { return }
            UnsafeMutableRawPointer(buf).copyMemory(from: base, byteCount: n)
        }
        buf[n] = 0
        outLen.pointee = n
        return 1
    }

    /// Expose the host baseline as `modore.default.*` so scripts can wrap
    /// rather than fork it. These callbacks intentionally mirror the same
    /// helpers used by the Swift pickup pipeline.
    private static func registerDefaultCallbacks(_ h: OpaquePointer) {
        _ = mdr_set_defaults(
            h,
            nil,
            { _, ctxPtr, outPtr in
                guard let ctx = ctxPtr, let out = outPtr,
                      let textPtr = ctx.pointee.full_text else { return 0 }
                let data = Data(bytes: textPtr, count: ctx.pointee.full_text_len)
                guard let text = String(data: data, encoding: .utf8) else { return 0 }
                let caretUTF16 = text.utf16Offset(forUTF8Byte: Int(ctx.pointee.caret_byte))
                guard caretUTF16 >= 0 else { return 0 }
                let bounds = wordBounds(text, caret: caretUTF16)
                out.pointee.span_start_byte = size_t(text.utf8ByteOffset(forUTF16Offset: bounds.0))
                out.pointee.span_end_byte = size_t(text.utf8ByteOffset(forUTF16Offset: bounds.1))
                out.pointee.romaji = nil
                out.pointee.romaji_len = 0
                return 1
            },
            { _, _, _, cands, nCands, outBuf, outCap, outLen in
                guard let cands, nCands > 0, let first = cands[0],
                      let buf = outBuf, let lenOut = outLen, outCap > 0 else { return 0 }
                let n = min(strlen(first), outCap - 1)
                UnsafeMutableRawPointer(buf).copyMemory(from: first, byteCount: n)
                buf[n] = 0
                lenOut.pointee = n
                return 1
            },
            { _, _, outRoute in
                guard let out = outRoute else { return 0 }
                out.pointee = MDR_ROUTE_DEFAULT
                return 1
            })
    }

    private static func withPickupContext<R>(
        appId: String?,
        fullText: String?,
        caretUTF16: Int,
        fieldRole: String?,
        fieldDescription: String?,
        katakana: Bool,
        _ body: (mdr_pickup_ctx_t) -> R
    ) -> R {
        let text = fullText ?? ""
        let utf8 = Array(text.utf8)
        let caretByte = text.utf8ByteOffset(forUTF16Offset: caretUTF16)
        return withOptionalCString(appId) { appPtr in
            withOptionalCString(fieldRole) { rolePtr in
                withOptionalCString(fieldDescription) { descPtr in
                    utf8.withUnsafeBufferPointer { buf in
                        let cptr = buf.baseAddress.map { UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self) }
                        let ctx = mdr_pickup_ctx_t(
                            full_text: cptr,
                            full_text_len: buf.count,
                            caret_byte: caretByte,
                            app_id: appPtr,
                            field_role: rolePtr,
                            field_description: descPtr,
                            flags: katakana ? 1 : 0)
                        return body(ctx)
                    }
                }
            }
        }
    }

    static func shutdown() {
        guard let h = engine else { return }
        mdr_shutdown(h)
        engine = nil
    }

    /// Re-scan the scripts directory. Called by the host's filesystem
    /// watcher when files are added or removed; per-script content edits
    /// are picked up automatically by the engine's per-file mtime poll
    /// on every hook entry, so this is only for the add/remove case.
    static func reloadScripts(dir: String) {
        guard let h = engine else { return }
        _ = mdr_load_dir(h, dir)
        Log.tagged("scripting", "scripts dir reloaded (dir=\(dir))")
    }

    static var isLoaded: Bool { engine != nil }

    // MARK: - Hooks

    /// Run the script's `on_acquire` hook for the current pickup context.
    /// The script composes its own pickup routine via `modore.host.*` and
    /// returns the picked text (which should leave the focused-app
    /// selection ACTIVE so the host can inject the replacement on top of
    /// it). Nil = use host default acquisition.
    static func acquire(
        appId: String?,
        fullText: String? = nil,
        caretUTF16: Int = 0,
        fieldRole: String? = nil,
        fieldDescription: String? = nil,
        katakana: Bool = false
    ) -> String? {
        guard let h = engine else { return nil }
        var outBuf = [CChar](repeating: 0, count: 4096)
        var outLen: size_t = 0
        let rc = withPickupContext(
            appId: appId,
            fullText: fullText,
            caretUTF16: caretUTF16,
            fieldRole: fieldRole,
            fieldDescription: fieldDescription,
            katakana: katakana
        ) { ctx in
            outBuf.withUnsafeMutableBufferPointer { outPtr in
                withUnsafePointer(to: ctx) { ctxPtr in
                    mdr_acquire(h, ctxPtr, outPtr.baseAddress, outPtr.count, &outLen)
                }
            }
        }
        guard rc == 1, outLen > 0 else { return nil }
        return outBuf.withUnsafeBufferPointer { buf -> String? in
            guard let base = buf.baseAddress else { return nil }
            return String(cString: base)
        }
    }

    /// Ask scripts to choose a delivery route for the current pickup
    /// context. Returns nil if no script weighed in.
    static func routeFor(
        appId: String?,
        fullText: String? = nil,
        caretUTF16: Int = 0,
        fieldRole: String? = nil,
        fieldDescription: String? = nil,
        katakana: Bool = false
    ) -> Route? {
        guard let h = engine else { return nil }
        var out: mdr_route_t = MDR_ROUTE_DEFAULT
        let rc = withPickupContext(
            appId: appId,
            fullText: fullText,
            caretUTF16: caretUTF16,
            fieldRole: fieldRole,
            fieldDescription: fieldDescription,
            katakana: katakana
        ) { ctx in
            withUnsafePointer(to: ctx) { ctxPtr in
                mdr_route(h, ctxPtr, &out)
            }
        }
        guard rc == 1 else { return nil }
        return Route(out)
    }

    /// Override the pickup span. `caret` is a UTF-16 offset into `fullText`;
    /// returns UTF-16 (start, end). Returns nil if no script weighed in or
    /// the script-returned span couldn't be mapped back to UTF-16.
    static func pickup(fullText: String, caretUTF16: Int, appId: String?, katakana: Bool) -> (start: Int, end: Int)? {
        guard let h = engine else { return nil }
        let utf8 = Array(fullText.utf8)
        let caretByte = fullText.utf8ByteOffset(forUTF16Offset: caretUTF16)
        var span = mdr_span_t(span_start_byte: 0, span_end_byte: 0, romaji: nil, romaji_len: 0)
        let rc: Int32 = utf8.withUnsafeBufferPointer { buf in
            // Rebind the UInt8 buffer to CChar for the C call. The two share
            // layout; the bind is a typing concession, not a copy.
            buf.baseAddress?.withMemoryRebound(to: CChar.self, capacity: buf.count) { cptr in
                withOptionalCString(appId) { appPtr in
                    var ctx = mdr_pickup_ctx_t(
                        full_text: cptr,
                        full_text_len: buf.count,
                        caret_byte: caretByte,
                        app_id: appPtr,
                        field_role: nil,
                        field_description: nil,
                        flags: katakana ? 1 : 0)
                    return mdr_pickup(h, &ctx, &span)
                }
            } ?? -1
        }
        guard rc == 1 else { return nil }
        let s = fullText.utf16Offset(forUTF8Byte: Int(span.span_start_byte))
        let e = fullText.utf16Offset(forUTF8Byte: Int(span.span_end_byte))
        guard s >= 0, e >= s else { return nil }
        return (s, e)
    }

    /// Override the replacement text. Returns nil if no script weighed in.
    /// `cap` is the upper bound on the returned string; oversized returns
    /// are silently truncated by the engine (logged).
    static func replacement(appId: String?, span: mdr_span_t, candidates: [String], cap: Int = 4096) -> String? {
        guard let h = engine else { return nil }
        var span = span
        var outBuf = [CChar](repeating: 0, count: cap)
        var outLen: size_t = 0
        let rc = withOptionalCString(appId) { appPtr in
            candidates.withCStringArray { cands, n in
                outBuf.withUnsafeMutableBufferPointer { buf in
                    mdr_replacement(h, appPtr, &span,
                                    cands, n,
                                    buf.baseAddress, cap, &outLen)
                }
            }
        }
        guard rc == 1, outLen > 0 else { return nil }
        return outBuf.withUnsafeBufferPointer { buf -> String? in
            guard let base = buf.baseAddress else { return nil }
            return String(cString: base)
        }
    }

    /// Override the candidate list. Returns nil if no script weighed in.
    static func candidates(appId: String?, list: [String], currentIndex: Int, cap: Int = 16384) -> [String]? {
        guard let h = engine else { return nil }
        var outBuf = [CChar](repeating: 0, count: cap)
        var outCount: size_t = 0
        let rc = withOptionalCString(appId) { appPtr in
            list.withCStringArray { cands, n in
                outBuf.withUnsafeMutableBufferPointer { buf in
                    mdr_candidates(h, appPtr, cands, n,
                                   Int32(currentIndex),
                                   buf.baseAddress, cap, &outCount)
                }
            }
        }
        guard rc == 1, outCount > 0 else { return nil }
        var result: [String] = []
        result.reserveCapacity(Int(outCount))
        outBuf.withUnsafeBufferPointer { buf in
            guard let base = buf.baseAddress else { return }
            var p = base
            let end = base.advanced(by: cap)
            for _ in 0..<Int(outCount) {
                // Defensive: must find a NUL terminator before the buffer
                // ends, else engine wrote a malformed count. Bail early.
                var probe = p
                while probe < end && probe.pointee != 0 { probe = probe.advanced(by: 1) }
                if probe >= end { break }
                let s = String(cString: p)
                result.append(s)
                p = probe.advanced(by: 1)
            }
        }
        return result.isEmpty ? nil : result
    }

    // MARK: - Route mapping

    enum Route {
        case ax
        case selectionSync
        case keystroke
        case clipboard

        init?(_ r: mdr_route_t) {
            switch r {
            case MDR_ROUTE_AX:        self = .ax
            case MDR_ROUTE_SELECTION_SYNC: self = .selectionSync
            case MDR_ROUTE_KEYSTROKE: self = .keystroke
            case MDR_ROUTE_CLIPBOARD: self = .clipboard
            default:                  return nil
            }
        }
    }
}

// MARK: - Host primitive helpers (Swift-side)

/// Parse a chord string and synthesize it via postKey. Falls back to a
/// log line when the chord is unparseable so scripts surface their typos
/// instead of silently no-op'ing.
/// Set `MODORE_AX_TRACE=1` to log an AX snapshot after every host-driven
/// chord. The post-chord state is captured asynchronously after a short
/// delay so we do not block the caller while waiting for the receiving app
/// to commit the selection change.
fileprivate let gAxTraceEnabled: Bool = {
    if let v = ProcessInfo.processInfo.environment["MODORE_AX_TRACE"] {
        return v == "1" || v.lowercased() == "true"
    }
    return false
}()

fileprivate func hostSendChord(_ chord: String) {
    guard let hk = ModoreConfig.parseChord(chord) else {
        Log.tagged("scripting", "send_chord: unparseable '\(chord)'")
        return
    }
    Log.tagged("scripting", "hostSendChord: \(chord)")
    if gAxTraceEnabled {
        axSelectionSnapshot(label: "pre-chord '\(chord)'")
    }
    postKey(hk.keyCode, flags: hk.coreFlags)
    if gAxTraceEnabled {
        // Tiny delay so the receiving app's event loop has a tick to
        // commit selection state before we read it back. 20ms is enough
        // for CodeMirror's transaction batching in informal tests.
        DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(20)) {
            axSelectionSnapshot(label: "post-chord '\(chord)'")
        }
    }
}

/// Send the shell-native conversion shortcut sequence into the frontmost app.
/// This mirrors the binding emitted by the shell bootstrap and is used when
/// the host hotkey is pressed inside a shell-native terminal.
func sendShellNativeConversionChord() {
    hostSendChord("Ctrl+X")
    hostSendChord("Ctrl+J")
}

func sendShellNativeKatakanaChord() {
    hostSendChord("Ctrl+X")
    hostSendChord("Ctrl+K")
}

// MARK: - Log trampoline

/// Fires from C → routes into Log.write via a tag-dispatch. Bind as
/// `@convention(c)` so it's a plain function pointer with no Swift
/// closure context.
private let logTrampoline: mdr_log_cb = { _, level, tag, msg in
    let tagStr = tag.flatMap { String(validatingUTF8: $0) } ?? "lua"
    let msgStr = msg.flatMap { String(validatingUTF8: $0) } ?? ""
    let prefix: String
    switch level {
    case 1: prefix = "WARN "
    case 2: prefix = "ERROR "
    default: prefix = ""
    }
    Log.tagged("scripting:" + tagStr, prefix + msgStr)

    // Engine emits `reloading '<path>'` from ms_maybe_reload on every
    // content-edit pickup. The directory-level fs watcher misses these
    // (editing a file inside the dir doesn't bump dir metadata), so this
    // is the one place we can catch them. Bounce to main for the UI poke.
    if tagStr == "engine" && msgStr.hasPrefix("reloading ") {
        DispatchQueue.main.async {
            gStatusItem?.flashReload(kind: "scripts")
        }
    }
    return 0
}

// MARK: - C-string helpers

private func withOptionalCString<R>(_ s: String?, _ body: (UnsafePointer<CChar>?) -> R) -> R {
    if let s = s { return s.withCString(body) }
    return body(nil)
}

private extension Array where Element == String {
    /// Run `body` with a contiguous `const char* const*` array of the
    /// strings, plus their count. Strings are held alive for the call.
    func withCStringArray<R>(_ body: (UnsafePointer<UnsafePointer<CChar>?>?, size_t) -> R) -> R {
        if isEmpty {
            return body(nil, 0)
        }
        var ptrs: [UnsafePointer<CChar>?] = Array<UnsafePointer<CChar>?>(repeating: nil, count: count)
        return withCStringArrayHelper(self[...], ptrs: &ptrs, body: body)
    }
}

private func withCStringArrayHelper<R>(
    _ remaining: ArraySlice<String>,
    ptrs: inout [UnsafePointer<CChar>?],
    body: (UnsafePointer<UnsafePointer<CChar>?>?, size_t) -> R
) -> R {
    let idx = ptrs.count - remaining.count
    if remaining.isEmpty {
        return ptrs.withUnsafeBufferPointer { buf in
            body(buf.baseAddress, buf.count)
        }
    }
    let s = remaining.first!
    return s.withCString { cstr in
        ptrs[idx] = cstr
        return withCStringArrayHelper(remaining.dropFirst(), ptrs: &ptrs, body: body)
    }
}
