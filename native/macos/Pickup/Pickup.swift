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
/// presumably exists in other Chromium-hosted dev surfaces. Obsidian's
/// CodeMirror editor behaves the same way — and on top of that it
/// silently rejects AXValue writes, so every conversion lands here.
/// AX-capable surfaces in these apps never reach this code path (they
/// go through doPickup → readFocusedField), so blocklisting the whole
/// bundle here only affects the already-unreliable clipboard-fallback
/// regions.
private let kPeekExistingSelectionBlocklist: Set<String> = [
    "com.google.Chrome",
    "md.obsidian",
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
        Log.clipboard("nothing to convert (no trailing romaji in \(pickedText))")
        if didForceSelect {
            postKey(kVK_RightArrow)
        }
        return
    }

    let baseReplacement: String
    let baseCandidates: [String]
    if gClassifierEnabled,
       let segResult = classifierSegmentedConvert(romajiTail, request: request) {
        baseReplacement = asciiPrefix + segResult.replacement
        baseCandidates = segResult.candidates.map { asciiPrefix + $0 }
    } else {
        // Fallback: heuristic acronym split.
        let (acronymHead, mozcInput) = splitAcronymHead(romajiTail)
        let frozenPrefix = asciiPrefix + acronymHead
        guard let result = callBackend(mozcInput, request: request, wantCandidates: true) else {
            Log.clipboard("backend returned no result")
            if didForceSelect {
                postKey(kVK_RightArrow)
            }
            return
        }
        baseReplacement = frozenPrefix + result.replacement
        baseCandidates = result.candidates.isEmpty
            ? [frozenPrefix + result.replacement]
            : result.candidates.map { frozenPrefix + $0 }
    }
    // Script overrides — no AX span on the clipboard path, so span byte
    // offsets are zeroed. Scripts that care about position should branch
    // on app_id instead.
    let scriptSpan = mdr_span_t(span_start_byte: 0, span_end_byte: 0,
                                romaji: nil, romaji_len: 0)
    let replacement = ModoreScript.replacement(
        appId: frontmostBundleID, span: scriptSpan, candidates: baseCandidates) ?? baseReplacement
    let snapshotCandidates = ModoreScript.candidates(
        appId: frontmostBundleID, list: baseCandidates, currentIndex: 0) ?? baseCandidates
    Log.clipboard("replace -> \(replacement) (alts=\(snapshotCandidates.count))")

    // Replace the active selection by injecting the replacement as a Unicode
    // keystroke into the session event tap. No clipboard touch on the replace
    // path → no flicker, no race with the user's clipboard contents.
    postUnicode(replacement)

    // Open a clipboard-backed session for follow-up gestures. The frontmost
    // app's pid + bundle ID are the identity check Esc-undo / cycle use to
    // refuse to act on the wrong window.
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

// MARK: - Imperative acquisition flow
//
// Called when a script's `on_acquire` hook supplied the picked text via
// its own keystroke recipe (e.g. Cmd+Left → Cmd+Shift+Right → Cmd+C).
// We trust the script left the focused-app selection ACTIVE on the
// picked span, so postUnicode below overwrites it in place. From here
// the flow mirrors the bottom half of doClipboardPickup.
//
// Known caveat: in CodeMirror-based editors (Obsidian), the editor's
// internal selection range can extend one position past the visible
// content (typically the trailing newline) even though Cmd+C strips
// that from the *clipboard string*. postUnicode's first chunk replaces
// the full range and so eats the newline, shifting the line up a row.
// Tried Right + Backspace × N as a workaround but the synthetic events
// race: the first Backspace fires before Right collapses, deleting the
// active selection in one shot, and the remaining N-1 Backspaces walk
// backward through preceding blank lines. Reverted. Scripts that need
// pixel-perfect line replacement in CodeMirror should fall back to a
// clipboard-paste recipe driven from on_acquire itself.
func runConversionOnAcquiredText(_ raw: String, request: PickupRequest, appId: String?) {
    var pickedText = raw
    if pickedText.hasSuffix("\n") { pickedText.removeLast() }
    if pickedText.hasSuffix("\r") { pickedText.removeLast() }
    if pickedText.isEmpty {
        Log.pickup("scripted acquire: empty text — nothing to convert")
        return
    }
    Log.pickup("scripted acquire pick: \(pickedText)")

    let (asciiPrefix, romajiTail) = splitTrailingASCII(pickedText)
    guard !romajiTail.isEmpty else {
        Log.pickup("scripted acquire: no trailing romaji in \(pickedText)")
        return
    }
    let (acronymHead, mozcInput) = splitAcronymHead(romajiTail)
    let frozenPrefix = asciiPrefix + acronymHead

    guard let result = callBackend(mozcInput, request: request, wantCandidates: true) else {
        Log.pickup("scripted acquire: backend returned no result")
        return
    }
    let baseReplacement = frozenPrefix + result.replacement
    let baseCandidates: [String] = result.candidates.isEmpty
        ? [baseReplacement]
        : result.candidates.map { frozenPrefix + $0 }

    let scriptSpan = mdr_span_t(span_start_byte: 0, span_end_byte: 0,
                                romaji: nil, romaji_len: 0)
    let replacement = ModoreScript.replacement(
        appId: appId, span: scriptSpan, candidates: baseCandidates) ?? baseReplacement
    let snapshotCandidates = ModoreScript.candidates(
        appId: appId, list: baseCandidates, currentIndex: 0) ?? baseCandidates

    Log.pickup("scripted acquire replace → \(replacement) (alts=\(snapshotCandidates.count))")

    // Observability for CodeMirror one-row-drift investigation: capture the
    // AX selection range immediately before postUnicode, and again ~80ms
    // after the synthetic events have had a chance to be processed. The
    // *before* snapshot is the load-bearing one — if `sel.len` exceeds the
    // visible line length, the editor's range straddles a boundary that
    // postUnicode will replace. The *after* snapshot shows what survived.
    let pickedU16Len = pickedText.utf16.count
    let replacementU16Len = replacement.utf16.count
    axSelectionSnapshot(label: "pre-postUnicode pickedU16=\(pickedU16Len)")
    postUnicode(replacement)
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.08) {
        axSelectionSnapshot(label: "post-postUnicode replacementU16=\(replacementU16Len)")
    }

    // Frontmost-app captured at doPickup entry (passed in as `appId`) so a
    // mid-flight app switch can't tag the session against the wrong window.
    // We still need the pid for the clipboard-session identity check;
    // fetch once here and accept the small window — pid changes are rarer
    // than bundle-id changes and the session check tolerates a stale pid
    // (refuses to act on the wrong app rather than acting incorrectly).
    let session = ConversionSession(
        backing: .clipboard(
            frontmostBundleId: appId,
            frontmostPid: FrontmostApp.currentPid() ?? 0),
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

    // Frontmost-app capture once, threaded through script hooks below.
    let appId = FrontmostApp.describe()?.bundleID

    // Imperative acquisition hook. Scripts compose their own selection /
    // copy / read routine using `modore.host.*` and return the picked
    // text. The script is expected to leave the focused-app selection
    // ACTIVE so postUnicode below can overwrite it.
    // Snapshot AX state *before* on_acquire runs. Compared against the
    // pre-postUnicode snapshot inside runConversionOnAcquiredText, this
    // pinpoints whether the script's send_chord sequence ends in a
    // selection that's larger than the line content.
    axSelectionSnapshot(label: "pre-acquire")
    if let picked = ModoreScript.acquire(appId: appId) {
        Log.pickup("scripted acquire → \(picked.count) chars\(FrontmostApp.logSuffix())")
        runConversionOnAcquiredText(picked, request: request, appId: appId)
        return
    }

    // Per-app routing override. A script returning "clipboard" skips the
    // AX read entirely and goes straight to the Cmd+C fallback path —
    // useful for apps (Obsidian, Discord, …) where AX writes are unreliable
    // but the user wants the same hotkey to Just Work. "ax" and "keystroke"
    // are no-ops here today: the existing flow already tries AX first and
    // falls back to keystroke-replace on rejection.
    if let route = ModoreScript.routeFor(appId: appId), route == .clipboard {
        Log.pickup("scripted route → clipboard\(FrontmostApp.logSuffix())")
        doClipboardPickup(request)
        return
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
        } else if let scripted = ModoreScript.pickup(
            fullText: field.value,
            caretUTF16: field.selStart,
            appId: appId,
            katakana: request.target == .katakana) {
            (start, end) = scripted
            Log.pickup("scripted pickup span [\(start)..\(end)]")
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

        if gClassifierEnabled,
           let segResult = classifierSegmentedConvert(romajiTail, request: request) {
            let baseReplacement = asciiPrefix + segResult.replacement
            let baseCandidates: [String] = segResult.candidates.map { asciiPrefix + $0 }

            let scriptSpanStart = field.value.utf8ByteOffset(forUTF16Offset: start)
            let scriptSpanEnd   = field.value.utf8ByteOffset(forUTF16Offset: end)
            let scriptSpan = mdr_span_t(
                span_start_byte: size_t(scriptSpanStart),
                span_end_byte: size_t(scriptSpanEnd),
                romaji: nil, romaji_len: 0)
            let replacement = ModoreScript.replacement(
                appId: appId, span: scriptSpan, candidates: baseCandidates) ?? baseReplacement
            let snapshotCandidates = ModoreScript.candidates(
                appId: appId, list: baseCandidates, currentIndex: 0) ?? baseCandidates
            Log.pickup("classifier replace -> \(replacement) (alts=\(snapshotCandidates.count))")

            if replaceRange(in: field.element, start: start, end: end, replacement: replacement) {
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
            // AX write rejected — fall through to keystroke path below.
            // replaceValue set selection to [start, end] which may persist
            // even though the text write failed; pass that range so the
            // keystroke path collapses it before backspacing.
            Log.pickup("AX write rejected after classifier; using backspace-retype")
            keystrokeReplaceSpan(
                caret: (start: start, end: end),
                spanEnd: end,
                spanLen: end - start,
                replacement: replacement)
            let frontmost = FrontmostApp.describe()
            let session = ConversionSession(
                backing: .clipboard(
                    frontmostBundleId: frontmost?.bundleID,
                    frontmostPid: frontmost?.pid ?? 0),
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

        // Fallback: heuristic split. Carve off any leading acronym/code
        // (`R&D`, `API`, `JSON`) so Mozc only sees actual romaji.
        let (acronymHead, mozcInput) = splitAcronymHead(romajiTail)
        let frozenPrefix = asciiPrefix + acronymHead

        // If mozcInput still starts with uppercase, the acronym split
        // didn't find a romaji tail — the text is English (e.g. "Wi-fi",
        // "Hello"). Skip conversion to avoid mozc garbling it.
        guard mozcInput.first?.isLowercase == true else {
            Log.pickup("skipping: no romaji tail after acronym split (\(mozcInput))")
            return
        }

        // AX path requests candidates so the snapshot can power Esc-undo
        // and the cycle gesture. Clipboard fallback below skips this —
        // no stable span to act on.
        guard let result = callBackend(mozcInput, request: request, wantCandidates: true) else {
            Log.pickup("backend returned no result")
            return
        }
        let baseReplacement = frozenPrefix + result.replacement
        let baseCandidates: [String] = result.candidates.isEmpty
            ? [baseReplacement]
            : result.candidates.map { frozenPrefix + $0 }

        // Script overrides for replacement text and candidate list. Both
        // are independently optional. `replacement` is what gets written
        // immediately; `snapshotCandidates` is what cycle steps through.
        let scriptSpanStart = field.value.utf8ByteOffset(forUTF16Offset: start)
        let scriptSpanEnd   = field.value.utf8ByteOffset(forUTF16Offset: end)
        let scriptSpan = mdr_span_t(
            span_start_byte: size_t(scriptSpanStart),
            span_end_byte: size_t(scriptSpanEnd),
            romaji: nil, romaji_len: 0)
        let replacement = ModoreScript.replacement(
            appId: appId, span: scriptSpan, candidates: baseCandidates) ?? baseReplacement
        let snapshotCandidates = ModoreScript.candidates(
            appId: appId, list: baseCandidates, currentIndex: 0) ?? baseCandidates
        Log.pickup("replace -> \(replacement) (alts=\(snapshotCandidates.count))")

        if replaceRange(in: field.element, start: start, end: end, replacement: replacement) {
            // Open a session so Esc-undo and the cycle gesture have
            // something to act on. Candidates already include the
            // frozenPrefix from the base computation above, so cycling
            // lines up byte-for-byte.
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

        // AX read worked but the value write was silently rejected
        // (Discord, Obsidian CodeMirror, Cursor/VSCode Monaco). These
        // apps lie about AX success codes — value writes, range
        // writes, and Shift+Left selection extension all return
        // .success while doing nothing visible. The clipboard
        // fallback is wrong here too: its Shift+Opt+Left force-select
        // treats `&` / `-` / `.` as word boundaries, so `R&Diraisho`
        // arrives at the bridge as `Diraisho` — losing the acronym
        // head Phase 1 is supposed to freeze. Re-run the same span
        // edit through synthesized keystrokes via the sibling helper
        // in `SyntheticEvents.swift`.
        //
        // KNOWN LIMITATION: when the user has a *hidden* visual
        // selection that AX doesn't report (Cursor/Monaco reports
        // `[N,N]` even when text is visibly selected), the first
        // Backspace consumes the hidden selection and the rest
        // delete surrounding text. Discord/Obsidian don't hit this
        // because their AX selection reports are accurate. Tracked
        // as a follow-up — needs a per-app strategy table or a
        // selection probe that isn't ambiguous with Cmd+C line-copy.
        // replaceValue set selection to [start, end] which may persist
        // even though the text write failed; pass that range so the
        // keystroke path collapses it before backspacing.
        Log.pickup("AX write rejected; using backspace-retype for [\(start)..\(end)] \(spanText)\(FrontmostApp.logSuffix())")
        keystrokeReplaceSpan(
            caret: (start: start, end: end),
            spanEnd: end,
            spanLen: end - start,
            replacement: replacement)
        Log.pickup("keystroke replace -> \(replacement) (alts=\(result.candidates.count))")

        // Cycle/Undo via the clipboard-backed session: those use
        // backspace+retype, which works even when AX writes don't.
        // snapshotCandidates was already computed above with any script
        // overrides applied; reuse here for consistency.
        let frontmost = FrontmostApp.describe()
        let session = ConversionSession(
            backing: .clipboard(
                frontmostBundleId: frontmost?.bundleID,
                frontmostPid: frontmost?.pid ?? 0),
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

    Log.pickup("using clipboard fallback (app does not expose AX text)\(FrontmostApp.logSuffix())")
    doClipboardPickup(request)
}
