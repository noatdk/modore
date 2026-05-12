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
    /// Top-N alternatives Mozc offered, captured between SPACE and ENTER.
    /// Usually starts with `replacement` (Mozc's top candidate); may be
    /// empty when the AX path didn't request candidates or Mozc had no
    /// alternatives to offer.
    let candidates: [String]
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
    // Stop the word walk on a script transition (ASCII ↔ non-ASCII), not
    // just whitespace. Without this, typing `kaitou`→Ctrl+/→`hentai`
    // grabs `回答hentai` as one word and Mozc returns it unchanged.
    //
    // TODO: this is a coarse split — every BMP code point ≥ 0x80 reads
    // as "non-ASCII" (kana, kanji, fullwidth, latin-1 supplement, …).
    // Fine for romaji→kana flow today; revisit when boundary cycling
    // against Mozc's segment output lands, since that will subsume this logic.
    // Whitespace is handled by the outer `while` (`!isWS(utf16[start-1])`
    // / `!isWS(utf16[end])`). scriptBreak is strictly about the
    // ASCII↔non-ASCII transition — otherwise a caret sitting just
    // before a newline would break out at start = caret and collapse
    // the span.
    let isASCII: (UInt16) -> Bool = { $0 < 0x80 }
    let scriptBreak: (UInt16, UInt16) -> Bool = { a, b in
        isASCII(a) != isASCII(b)
    }
    var start = c
    while start > 0 && !isWS(utf16[start - 1]) {
        if start < utf16.count, scriptBreak(utf16[start - 1], utf16[start]) {
            break
        }
        start -= 1
    }
    var end = c
    while end < utf16.count && !isWS(utf16[end]) {
        if end > 0, scriptBreak(utf16[end - 1], utf16[end]) {
            break
        }
        end += 1
    }
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
///
/// `wantCandidates` triggers the candidate-capturing bridge path. The AX
/// path always wants candidates (snapshot powers Esc-undo + cycle); the
/// clipboard fallback path passes `false` because it can't snapshot
/// anyway (no AX element to retain).
func callBackend(
    _ span: String,
    request: PickupRequest = .init(),
    wantCandidates: Bool = false
) -> ConvertResult? {
    guard !span.isEmpty else { return nil }
    do {
        if wantCandidates {
            let r = try MozcBridge.convertWithCandidates(span, target: request.target)
            return ConvertResult(
                replacement: r.committed,
                cursorOffset: nil,
                candidates: r.candidates)
        }
        let converted = try MozcBridge.convert(span, target: request.target)
        return ConvertResult(replacement: converted, cursorOffset: nil, candidates: [])
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

    // Fetch candidates so the cycle gesture has something to work with in
    // this app too. Esc-undo and cycle on the clipboard path use a
    // backspace + retype strategy (the alternative — re-running the
    // selection + Cmd+C dance — would be much slower per cycle press).
    guard let result = callBackend(pickedText, request: request, wantCandidates: true) else {
        Log.clipboard("backend returned no result")
        if didForceSelect {
            postKey(kVK_RightArrow)
        }
        return
    }
    Log.clipboard("replace -> \(result.replacement) (alts=\(result.candidates.count))")

    // Replace the active selection by injecting the replacement as a Unicode
    // keystroke into the session event tap. No clipboard touch on the replace
    // path → no flicker, no race with the user's clipboard contents.
    postUnicode(result.replacement)

    // Open a clipboard-backed session for follow-up gestures. The frontmost
    // app's pid + bundle ID are the identity check Esc-undo / cycle use to
    // refuse to act on the wrong window. Same single-element-list fallback
    // as the AX path for inputs Mozc didn't offer alternatives on.
    let snapshotCandidates: [String] =
        result.candidates.isEmpty ? [result.replacement] : result.candidates
    let frontmost = FrontmostApp.describe()
    ConversionSessionStore.set(ConversionSession(
        backing: .clipboard(
            frontmostBundleId: frontmost?.bundleID,
            frontmostPid: frontmost?.pid ?? 0),
        originalReading: pickedText,
        candidates: snapshotCandidates,
        candidateIndex: 0,
        timestamp: Date()))
}

// MARK: - Pickup pipeline

func doPickup(_ request: PickupRequest = .init()) {
    // Single-key cycle: if an active session exists and gates hold, the
    // primary chord advances the candidate instead of doing a fresh
    // conversion. Katakana presses always fresh-convert (no cycle in
    // katakana mode). Failure to cycle (no session, expired window,
    // gates broke) falls through silently to fresh convert below — the
    // tap callback clears the session on any non-chord keystroke, so by
    // the time we get here a stale session almost always means "user is
    // legitimately starting a new conversion."
    if request.target != .katakana {
        if cycleNext(verbose: false) {
            return
        }
    }

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

        // AX path requests candidates so the snapshot can power Esc-undo
        // and the cycle gesture. Clipboard fallback below skips this —
        // no stable span to act on.
        guard let result = callBackend(spanText, request: request, wantCandidates: true) else {
            Log.pickup("backend returned no result")
            return
        }
        Log.pickup("replace -> \(result.replacement) (alts=\(result.candidates.count))")

        if replaceRange(in: field.element, start: start, end: end, replacement: result.replacement) {
            // Open a session so Esc-undo and the cycle gesture have
            // something to act on. `candidates` is the full list captured
            // from Mozc; if Mozc returned nothing (rare — only trivial
            // single-kana inputs), we still snapshot the committed string
            // as a single-element list so the index/identity math stays
            // simple. `candidateIndex = 0` means "candidates[0] is what
            // got written to the span."
            let snapshotCandidates: [String] =
                result.candidates.isEmpty ? [result.replacement] : result.candidates
            ConversionSessionStore.set(ConversionSession(
                backing: .ax(element: field.element, spanStart: start),
                originalReading: spanText,
                candidates: snapshotCandidates,
                candidateIndex: 0,
                timestamp: Date()))
            return
        }
        Log.pickup("AX replace failed; falling back to clipboard mode\(FrontmostApp.logSuffix())")
    }

    Log.pickup("using clipboard fallback (app does not expose AX text)\(FrontmostApp.logSuffix())")
    doClipboardPickup(request)
}
