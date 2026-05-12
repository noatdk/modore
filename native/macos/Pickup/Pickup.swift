// Pickup pipeline: the user fires the hotkey, modore reads the word at the
// caret (AX fast-path, then clipboard fallback), hands it to Mozc, and
// writes the result back. This file owns:
//
//   - PickupRequest / ConvertResult: per-invocation transfer object + result.
//   - callBackend: the thin Mozc-bridge wrapper (no slicing, no policy).
//   - sliceUTF16 / wordBounds: span resolution in the AX/JS index domain.
//   - doPickup / doClipboardPickup: the two halves of the pipeline.
//
// Globals it reads:
//   - gClipboardTimings (HotkeyState.swift) — snapshot at function entry.

import Cocoa
import ApplicationServices

// MARK: - Cross-phase shape

struct ConvertResult {
    let replacement: String
    let cursorOffset: Int?
}

/// Per-invocation request threaded through the pickup pipeline. Built at the
/// dispatch site (Carbon / tap callback) and passed unchanged through
/// `doPickup` → `doClipboardPickup` → `callBackend` → `MozcBridge.convert`.
///
/// Per-call signals (which chord fired, whether to include surrounding-text
/// context, cycle direction, …) live here so adding one doesn't grow every
/// pipeline-stage signature. Snapshotted config (clipboard timings, the
/// current hotkey chord) stays in globals — that's long-lived state read at
/// stage entry, distinct from "what the user just asked for."
struct PickupRequest {
    /// Target conversion shape. The katakana modifier flips this to
    /// `.katakana`; everything else uses the default `.kanji`.
    var target: MozcBridge.ConvertTarget = .kanji
}

// MARK: - Span resolution (UTF-16, matches AX/JS semantics)

func wordBounds(_ utf16: [UInt16], caret: Int) -> (Int, Int) {
    if utf16.isEmpty { return (0, 0) }
    let c = min(max(caret, 0), utf16.count)
    let isWS: (UInt16) -> Bool = { ch in
        ch == 0x20 || ch == 0x09 || ch == 0x0A || ch == 0x0D
    }
    var start = c
    while start > 0 && !isWS(utf16[start - 1]) { start -= 1 }
    var end = c
    while end < utf16.count && !isWS(utf16[end]) { end += 1 }
    if start == end {
        if c < utf16.count { return (c, c + 1) }
        if c > 0 { return (c - 1, c) }
    }
    return (start, end)
}

/// UTF-16 substring helper, matching the AX/JS index domain used throughout
/// the pickup pipeline. Returns `nil` if the bounds are out of range or empty.
func sliceUTF16(_ text: String, start: Int, end: Int) -> String? {
    let utf16 = Array(text.utf16)
    guard start >= 0, end <= utf16.count, start < end else { return nil }
    return String(utf16CodeUnits: Array(utf16[start..<end]), count: end - start)
}

// MARK: - Backend call (in-process via mozc bridge)
//
// The Mozc engine is linked in-process via bridge/include/mozc_bridge.h —
// no daemon, IPC, or HTTP. The Swift façade lives in MozcBridge.swift.

/// Hand a prepared span to the Mozc bridge. Callers are responsible for
/// slicing the span text out of whatever surface they read (AX field value,
/// clipboard contents, …) and for filling any additional context fields on
/// `request` — `callBackend` is now a thin wrapper, not a slicing helper.
func callBackend(_ span: String, request: PickupRequest = .init()) -> ConvertResult? {
    guard !span.isEmpty else { return nil }
    do {
        let converted = try MozcBridge.convert(span, target: request.target)
        return ConvertResult(replacement: converted, cursorOffset: nil)
    } catch {
        Log.mozc("bridge error: \(String(describing: error))")
        return nil
    }
}

// MARK: - Clipboard-based fallback (works in any app that supports Cmd+C / Cmd+V)

/// Heuristic: a Cmd+C result that contains a newline or is huge is almost
/// certainly the editor's "copy current line on empty selection" feature
/// (Sublime, VSCode, Cursor, …) — not a real user selection. Treat as bogus.
private func looksLikeLineCopy(_ s: String) -> Bool {
    if s.contains("\n") || s.contains("\r") { return true }
    if s.count > 200 { return true }
    return false
}

func doClipboardPickup(_ request: PickupRequest = .init()) {
    // Snapshot timings once at entry so the whole pickup runs against a
    // consistent set even if the watcher fires mid-flight on the main thread.
    let timings = gClipboardTimings

    // Snapshot the user's clipboard now; `defer` guarantees we restore it on
    // every exit path. No manual cleanup at the early-returns below.
    let (_, restoreSavedClipboard) = guardClipboard(restoreDelayMs: timings.restoreClipboardDelayMs)
    defer { restoreSavedClipboard() }

    // The user is almost certainly still holding the conversion hotkey's
    // modifiers when we get here. Synthesizing Cmd+C now would land as
    // (heldModifiers)+Cmd+C and silently no-op in many apps.
    waitForModifiersToClear()

    let pb = NSPasteboard.general
    var picked: String? = nil

    // Step 1: peek at the user's current selection (if any) with Cmd+C.
    // Aggressive timeout — apps with a real selection respond in <30ms.
    // This is a heuristic threshold (no selection → fall through fast), not
    // a tuning knob, so it stays hard-coded.
    let baseline = pb.changeCount
    postKey(kVK_ANSI_C, flags: .maskCommand)
    if waitForClipboardChange(after: baseline, timeoutMs: 80),
       let s = pb.string(forType: .string), !s.isEmpty {
        if looksLikeLineCopy(s) {
            Log.clipboard("Cmd+C looks like a line-copy (no real selection); will force-select previous word")
        } else {
            picked = s
            Log.clipboard("using existing user selection: \(s)")
        }
    }

    // Step 2: no usable selection — force-select previous word, then copy.
    // The selection from Shift+Opt+Left is what we'll replace via Unicode injection.
    var didForceSelect = false
    if picked == nil {
        postKey(kVK_LeftArrow, flags: [.maskShift, .maskAlternate])
        didForceSelect = true
        // Electron apps (Cursor/Slack/VSCode) need a moment for the selection
        // to land before the next Cmd+C is processed by the renderer thread.
        // User-tunable via [clipboard] pre_copy_delay_ms.
        if timings.preCopyDelayMs > 0 {
            Thread.sleep(forTimeInterval: Double(timings.preCopyDelayMs) / 1000.0)
        }
        let baseline2 = pb.changeCount
        postKey(kVK_ANSI_C, flags: .maskCommand)
        // Slow apps / heavy load may need more than the default 250 ms.
        // User-tunable via [clipboard] read_timeout_ms.
        if waitForClipboardChange(after: baseline2, timeoutMs: timings.readTimeoutMs),
           let s = pb.string(forType: .string), !s.isEmpty {
            picked = s
        }
    }

    guard var pickedText = picked else {
        Log.clipboard("nothing to convert (Cmd+C didn't update clipboard in time)\(FrontmostApp.logSuffix())")
        // Don't leave the user with a stuck visible selection from our Shift+Opt+Left
        if didForceSelect {
            postKey(kVK_RightArrow)
        }
        return
    }

    if pickedText.hasSuffix("\n") { pickedText.removeLast() }
    if pickedText.hasSuffix("\r") { pickedText.removeLast() }

    Log.clipboard("pick: \(pickedText)")

    guard let result = callBackend(pickedText, request: request) else {
        Log.clipboard("backend returned no result")
        if didForceSelect {
            postKey(kVK_RightArrow)
        }
        return
    }
    Log.clipboard("replace -> \(result.replacement)")

    // Replace the active selection by injecting the replacement as a Unicode
    // keystroke into the session event tap. No clipboard touch on the replace
    // path → no flicker, no race with the user's clipboard contents.
    postUnicode(result.replacement)
}

// MARK: - Pickup pipeline

func doPickup(_ request: PickupRequest = .init()) {
    if request.target == .katakana {
        Log.pickup("katakana modifier engaged")
    }
    if let field = readFocusedField() {
        let utf16 = Array(field.value.utf16)
        let (start, end): (Int, Int)
        if field.selStart != field.selEnd {
            (start, end) = (field.selStart, field.selEnd)
        } else {
            (start, end) = wordBounds(utf16, caret: field.selStart)
        }
        guard start >= 0, end <= utf16.count, start < end else {
            Log.pickup("empty span; nothing to convert")
            return
        }

        guard let spanText = sliceUTF16(field.value, start: start, end: end) else {
            Log.pickup("empty span; nothing to convert")
            return
        }
        Log.pickup("pick [\(start)..\(end)] \(spanText)")

        guard let result = callBackend(spanText, request: request) else {
            Log.pickup("backend returned no result")
            return
        }
        Log.pickup("replace -> \(result.replacement)")

        if replaceRange(in: field.element, start: start, end: end, replacement: result.replacement) {
            return
        }
        Log.pickup("AX replace failed; falling back to clipboard mode\(FrontmostApp.logSuffix())")
    }

    Log.pickup("using clipboard fallback (app does not expose AX text)\(FrontmostApp.logSuffix())")
    doClipboardPickup(request)
}
