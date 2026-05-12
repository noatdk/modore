// Pickup pipeline: the user fires the hotkey, modore reads the word at the
// caret (AX fast-path, then clipboard fallback), hands it to Mozc, and
// writes the result back. This file owns:
//
//   - PickupRequest / ConvertResult: per-invocation transfer object + result.
//   - callBackend: the thin Mozc-bridge wrapper (no slicing, no policy).
//   - doPickup / doClipboardPickup: the two halves of the pipeline.
//
// Span-domain helpers (wordBounds, splitTrailingASCII, sliceUTF16) live in
// SpanSplit.swift so the test driver can compile them without Cocoa.
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

/// Bundle IDs where the Cmd+C-peek heuristic in step 1 below is unreliable
/// because the app line-copies the caret's current line *without* a
/// trailing newline (so `looksLikeLineCopy` misses it). Chrome's DevTools
/// console is the discovered offender; the same Chromium behaviour
/// presumably exists in other Chromium-hosted dev surfaces. AX-capable
/// surfaces in these apps never reach this code path (they go through
/// doPickup → readFocusedField), so blocklisting the whole bundle here
/// only affects the already-unreliable clipboard-fallback regions.
private let kPeekExistingSelectionBlocklist: Set<String> = [
    "com.google.Chrome",
]

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
    // Skipped when the frontmost app is on the blocklist (currently just
    // Chrome — its DevTools console line-copies on Cmd+C without a
    // trailing newline, defeating looksLikeLineCopy and causing the
    // pickup to wrongly treat the line as a real selection).
    let frontmostBundleID = FrontmostApp.describe()?.bundleID
    let allowPeek = !(frontmostBundleID.map(kPeekExistingSelectionBlocklist.contains) ?? false)
    if allowPeek {
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
    } else if let bid = frontmostBundleID {
        Log.clipboard("skipping existing-selection peek (\(bid) line-copies without newline)")
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
        // Drop the visible selection from our Shift+Opt+Left (if we made one)
        // so the user doesn't see a stuck highlight.
        if didForceSelect {
            postKey(kVK_RightArrow)
        }
        return
    }

    if pickedText.hasSuffix("\n") { pickedText.removeLast() }
    if pickedText.hasSuffix("\r") { pickedText.removeLast() }

    Log.clipboard("pick: \(pickedText)")

    // Mirror the AX path's script-boundary split for the clipboard path:
    // Shift+Opt+Left in Discord/Sublime grabs the whole word including any
    // non-ASCII prefix (e.g. `対人sen` arrives as one blob), and the bridge
    // sends each byte of UTF-8 to Mozc as a separate `key_code` — kanji
    // come out as Latin-1 mojibake. Strip the non-ASCII prefix here, send
    // only the trailing romaji to the bridge, then re-attach the prefix.
    let (asciiPrefix, romajiTail) = splitTrailingASCII(pickedText)
    guard !romajiTail.isEmpty else {
        // No trailing romaji to convert — bridge would error or echo input.
        // Drop the forced selection (if any) so we don't leave the user
        // with a stuck highlight.
        Log.clipboard("nothing to convert (no trailing romaji in \(pickedText))")
        if didForceSelect {
            postKey(kVK_RightArrow)
        }
        return
    }

    // Fetch candidates so the cycle gesture has something to work with in
    // this app too. Esc-undo and cycle on the clipboard path use a
    // backspace + retype strategy (the alternative — re-running the
    // selection + Cmd+C dance — would be much slower per cycle press).
    guard let result = callBackend(romajiTail, request: request, wantCandidates: true) else {
        Log.clipboard("backend returned no result")
        if didForceSelect {
            postKey(kVK_RightArrow)
        }
        return
    }
    let replacement = asciiPrefix + result.replacement
    Log.clipboard("replace -> \(replacement) (alts=\(result.candidates.count))")

    // Replace the active selection by injecting the replacement as a Unicode
    // keystroke into the session event tap. No clipboard touch on the replace
    // path → no flicker, no race with the user's clipboard contents.
    postUnicode(replacement)

    // Open a clipboard-backed session for follow-up gestures. The frontmost
    // app's pid + bundle ID are the identity check Esc-undo / cycle use to
    // refuse to act on the wrong window. Same single-element-list fallback
    // as the AX path for inputs Mozc didn't offer alternatives on.
    // Candidates get the prefix glued on too — cycle replaces the whole
    // typed run (prefix + tail) on each press, so each candidate must
    // already include the prefix for backspace-count math to line up.
    let snapshotCandidates: [String] =
        result.candidates.isEmpty
            ? [replacement]
            : result.candidates.map { asciiPrefix + $0 }
    let frontmost = FrontmostApp.describe()
    let session = ConversionSession(
        backing: .clipboard(
            frontmostBundleId: frontmost?.bundleID,
            frontmostPid: frontmost?.pid ?? 0),
        originalReading: pickedText,
        candidates: snapshotCandidates,
        candidateIndex: 0,
        timestamp: Date())
    ConversionSessionStore.set(session)
    if gCandidatePanelMode == .onConvert {
        CandidatePanel.shared.show(session: session)
    }
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
    // First AX attempt. If it fails — typically because the frontmost app
    // is Electron and hasn't been opted into accessibility — flip the
    // documented `AXManualAccessibility` switch on that app's pid and try
    // again. Once-per-pid; cheap no-op for apps that don't support it.
    var focusedField = readFocusedField()
    if focusedField == nil {
        enableElectronAXIfNeeded()
        focusedField = readFocusedField()
    }
    if let field = focusedField {
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

        // Strip any non-ASCII prefix before handing off to Mozc — same
        // reason as the clipboard path. `wordBounds` already does this on
        // the empty-caret branch (script-transition stop), but a
        // user-made selection covering `対人sen` bypasses it. Without this
        // the bridge feeds Mozc UTF-8 bytes as Latin-1 codepoints and
        // mojibake comes back.
        let (asciiPrefix, romajiTail) = splitTrailingASCII(spanText)
        guard !romajiTail.isEmpty else {
            Log.pickup("nothing to convert (no trailing romaji in \(spanText))")
            return
        }

        // AX path requests candidates so the snapshot can power Esc-undo
        // and the cycle gesture. Clipboard fallback below skips this —
        // no stable span to act on.
        guard let result = callBackend(romajiTail, request: request, wantCandidates: true) else {
            Log.pickup("backend returned no result")
            return
        }
        let replacement = asciiPrefix + result.replacement
        Log.pickup("replace -> \(replacement) (alts=\(result.candidates.count))")

        if replaceRange(in: field.element, start: start, end: end, replacement: replacement) {
            // Open a session so Esc-undo and the cycle gesture have
            // something to act on. `candidates` is the full list captured
            // from Mozc; if Mozc returned nothing (rare — only trivial
            // single-kana inputs), we still snapshot the committed string
            // as a single-element list so the index/identity math stays
            // simple. `candidateIndex = 0` means "candidates[0] is what
            // got written to the span." Each candidate carries the
            // non-ASCII prefix so cycling lines up byte-for-byte.
            let snapshotCandidates: [String] =
                result.candidates.isEmpty
                    ? [replacement]
                    : result.candidates.map { asciiPrefix + $0 }
            let session = ConversionSession(
                backing: .ax(element: field.element, spanStart: start),
                originalReading: spanText,
                candidates: snapshotCandidates,
                candidateIndex: 0,
                timestamp: Date())
            ConversionSessionStore.set(session)
            if gCandidatePanelMode == .onConvert {
                CandidatePanel.shared.show(session: session)
            }
            return
        }
        Log.pickup("AX replace failed; falling back to clipboard mode\(FrontmostApp.logSuffix())")
    }

    Log.pickup("using clipboard fallback (app does not expose AX text)\(FrontmostApp.logSuffix())")
    doClipboardPickup(request)
}
