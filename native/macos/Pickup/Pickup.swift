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
import Dispatch

// MARK: - Cross-phase shape

struct ConvertResult {
    let replacement: String
    let cursorOffset: Int?
    /// Top-N alternatives Mozc offered, captured between SPACE and ENTER.
    /// Usually starts with `replacement` (Mozc's top candidate); may be
    /// empty when the AX path didn't request candidates or Mozc had no
    /// alternatives to offer.
    let candidates: [MozcBridge.Candidate]
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
                replacement: r.candidates.first?.value ?? r.committed,
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

private func frontmostIsShellNativeTerminal() -> Bool {
    FrontmostApp.isShellNativeTerminal(bundleID: FrontmostApp.describe()?.bundleID)
}

private func frontmostShellNativeTerminalLooksLikeEditor() -> Bool {
    FrontmostApp.focusedWindowLooksLikeEditor(pid: FrontmostApp.describe()?.pid)
}

/// Route the primary conversion hotkey into shell-native editing when the
/// frontmost app is one of the supported terminal emulators. The shell
/// binding inside the terminal will take over from there.
func handlePrimaryHotkeyTrigger() {
    if frontmostIsShellNativeTerminal() {
        if frontmostShellNativeTerminalLooksLikeEditor() {
            Log.hotkey("shell-native terminal looks like editor; falling back to pickup\(FrontmostApp.logSuffix())")
            doPickup()
            return
        }
        Log.hotkey("shell-native terminal detected; injecting shell binding\(FrontmostApp.logSuffix())")
        sendShellNativeConversionChord()
        return
    }
    doPickup()
}

func handleKatakanaHotkeyTrigger() {
    if frontmostIsShellNativeTerminal() {
        if frontmostShellNativeTerminalLooksLikeEditor() {
            Log.hotkey("shell-native terminal looks like editor; falling back to katakana pickup\(FrontmostApp.logSuffix())")
            doPickup(PickupRequest(target: .katakana))
            return
        }
        Log.hotkey("shell-native terminal detected; injecting katakana shell binding\(FrontmostApp.logSuffix())")
        sendShellNativeKatakanaChord()
        return
    }
    doPickup(PickupRequest(target: .katakana))
}

func handleCycleHotkeyTrigger() {
    if frontmostIsShellNativeTerminal() {
        if frontmostShellNativeTerminalLooksLikeEditor() {
            Log.hotkey("shell-native terminal looks like editor; falling back to cycle\(FrontmostApp.logSuffix())")
            performCycleNext()
            return
        }
        Log.hotkey("shell-native terminal detected; injecting shell binding for cycle\(FrontmostApp.logSuffix())")
        sendShellNativeConversionChord()
        return
    }
    performCycleNext()
}

private func convertTrailingASCIISuffix(
    _ suffix: String,
    request: PickupRequest
) -> String {
    guard !suffix.isEmpty else { return "" }
    var converted = ""
    converted.reserveCapacity(suffix.count)
    for scalar in suffix.unicodeScalars {
        let raw = String(scalar)
        switch scalar {
        case ".", ",", "-":
            converted += callBackend(raw, request: request)?.replacement
                ?? (raw == "." ? "。" : raw == "," ? "、" : "ー")
        default:
            converted += raw
        }
    }
    return converted
}

/// Strip leading ASCII punctuation only when the first real alnum is lowercase.
///
/// This keeps quoted HTML text like `"gion` from tripping the acronym split
/// while leaving uppercase code-ish prefixes (`.NET`, `Wi-Fi`) intact.
private func splitLeadingASCIIJunkBeforeLowercase(
    _ ascii: String
) -> (prefix: String, tail: String) {
    guard !ascii.isEmpty else { return ("", "") }
    var split = ascii.startIndex
    while split < ascii.endIndex {
        let byte = ascii[split].utf8.first ?? 0
        if (byte >= 0x61 && byte <= 0x7A) ||
            (byte >= 0x41 && byte <= 0x5A) ||
            (byte >= 0x30 && byte <= 0x39) {
            break
        }
        split = ascii.index(after: split)
    }
    guard split < ascii.endIndex else { return (ascii, "") }
    let first = ascii[split].utf8.first ?? 0
    guard first >= 0x61 && first <= 0x7A else { return ("", ascii) }
    return (String(ascii[..<split]), String(ascii[split...]))
}

/// Use the ML classifier to split an ASCII string into romaji and ASCII
/// segments, convert each romaji segment through Mozc independently, and
/// reassemble. Falls back to nil when the classifier is not loaded or the
/// string is not mixed (single-type -> let the caller use the normal path).
private func classifierSegmentedConvert(
    _ ascii: String,
    request: PickupRequest
) -> (replacement: String, candidates: [MozcBridge.Candidate])? {
    guard let segments = Classifier.segment(ascii),
          Classifier.isMixed(segments) else { return nil }

    var primaryIdx = -1
    var primaryLen = 0
    for (i, seg) in segments.enumerated() where seg.isRomaji {
        let len = seg.end - seg.start
        if len > primaryLen {
            primaryLen = len
            primaryIdx = i
        }
    }

    var segmentReplacements = Array(repeating: "", count: segments.count)
    var primaryResult: ConvertResult? = nil
    for (i, seg) in segments.enumerated() {
        let startIdx = ascii.utf8.index(ascii.utf8.startIndex, offsetBy: seg.start)
        let endIdx = ascii.utf8.index(ascii.utf8.startIndex, offsetBy: seg.end)
        let chunk = String(ascii.utf8[startIdx..<endIdx])!

        if seg.isRomaji {
            let wantCands = i == primaryIdx
            if let result = callBackend(chunk, request: request,
                                        wantCandidates: wantCands) {
                segmentReplacements[i] = result.replacement
                if wantCands { primaryResult = result }
            } else {
                segmentReplacements[i] = chunk
            }
        } else {
            segmentReplacements[i] = chunk
        }
    }

    let replacement = segmentReplacements.joined()
    var candidates: [MozcBridge.Candidate] = [candidateFromValue(replacement)]
    if let pr = primaryResult, !pr.candidates.isEmpty {
        let rebuiltCount = pr.candidates.count
        let prefix = segmentReplacements[..<primaryIdx].joined()
        let suffix = segmentReplacements[segmentReplacements.index(after: primaryIdx)...].joined()
        var rebuilt = Array<MozcBridge.Candidate>(
            repeating: candidateFromValue(replacement),
            count: rebuiltCount)
        if rebuiltCount > 1 {
            rebuilt.withUnsafeMutableBufferPointer { buffer in
                DispatchQueue.concurrentPerform(iterations: rebuiltCount) { idx in
                    let cand = pr.candidates[idx]
                    buffer[idx] = candidateByReplacingValue(
                        cand,
                        with: prefix + cand.value + suffix)
                }
            }
        } else {
            let cand = pr.candidates[0]
            rebuilt[0] = candidateByReplacingValue(
                cand,
                with: prefix + cand.value + suffix)
        }
        candidates = rebuilt
    }

    Log.pickup("classifier segmented: \(segments.count) segments -> \(replacement)")
    return (replacement, candidates)
}

private func candidateFromValue(
    _ value: String,
    group: MozcBridge.Candidate.Group = .conversion
) -> MozcBridge.Candidate {
    MozcBridge.Candidate(
        value: value,
        description: nil,
        prefix: nil,
        suffix: nil,
        id: -1,
        windowCategory: 0,
        group: group)
}

private func candidateByReplacingValue(
    _ candidate: MozcBridge.Candidate,
    with value: String
) -> MozcBridge.Candidate {
    MozcBridge.Candidate(
        value: value,
        description: candidate.description,
        prefix: candidate.prefix,
        suffix: candidate.suffix,
        id: candidate.id,
        windowCategory: candidate.windowCategory,
        group: candidate.group)
}

private func candidateValues(_ candidates: [MozcBridge.Candidate]) -> [String] {
    candidates.map(\.value)
}

private func mapCandidateValues(
    _ candidates: [MozcBridge.Candidate],
    _ transform: (MozcBridge.Candidate) -> String
) -> [MozcBridge.Candidate] {
    candidates.map { candidateByReplacingValue($0, with: transform($0)) }
}

private func candidatesFromValues(
    _ values: [String],
    group: MozcBridge.Candidate.Group = .conversion
) -> [MozcBridge.Candidate] {
    values.map { candidateFromValue($0, group: group) }
}

private func applyScriptCandidateValues(
    _ values: [String],
    onto base: [MozcBridge.Candidate]
) -> [MozcBridge.Candidate] {
    values.enumerated().map { idx, value in
        if idx < base.count {
            return candidateByReplacingValue(base[idx], with: value)
        }
        return candidateFromValue(value)
    }
}

private func normalizeCommittedCandidateState(
    replacement: String,
    candidates: [MozcBridge.Candidate]
) -> (candidates: [MozcBridge.Candidate], currentIndex: Int) {
    if let idx = candidates.firstIndex(where: { $0.value == replacement }) {
        return (candidates, idx)
    }
    var normalized = candidates
    normalized.insert(candidateFromValue(replacement), at: 0)
    return (normalized, 0)
}

// MARK: - Shadow-buffer pickup (zero-latency fallback before clipboard)

private func doShadowPickup(_ request: PickupRequest) -> Bool {
    guard let snap = gShadowBuffer.takeSnapshot() else { return false }
    let (shadowText, cursorByte) = snap

    let caretUTF16 = shadowText.utf16Offset(forUTF8Byte: cursorByte)
    guard caretUTF16 >= 0 else {
        Log.pickup("shadow: caret-offset failed (cursor \(cursorByte) mid-sequence)")
        return false
    }
    let (wordStart, wordEnd) = wordBounds(shadowText, caret: caretUTF16)
    guard wordStart < wordEnd,
          let span = sliceUTF16(shadowText, start: wordStart, end: wordEnd) else {
        Log.pickup("shadow: empty span; nothing to convert")
        return false
    }

    Log.pickup("shadow pick [\(wordStart)..\(wordEnd)] \(span)")

    let (asciiPrefix, romajiTail, preservedSuffix) = splitConvertibleASCIIWindow(span)
    let (romajiCore, romajiSuffix) = splitTrailingASCIIPunctuation(romajiTail)
    let convertedSuffix = convertTrailingASCIISuffix(romajiSuffix, request: request) + preservedSuffix
    guard !romajiCore.isEmpty else {
        Log.pickup("shadow: nothing to convert (no romaji in \(span))")
        return false
    }
    let (leadingJunk, romajiBody) = splitLeadingASCIIJunkBeforeLowercase(romajiCore)
    let (acronymHead, mozcInput) = splitAcronymHead(romajiBody)
    guard mozcInput.first?.isLowercase == true else {
        Log.pickup("shadow: skipping: no romaji after acronym split (\(mozcInput))")
        return false
    }

    let frozenPrefix = asciiPrefix + leadingJunk + acronymHead
    guard let result = callBackend(mozcInput, request: request, wantCandidates: true) else {
        Log.pickup("shadow: backend returned no result")
        return false
    }

    let baseReplacement = frozenPrefix + result.replacement + convertedSuffix
    let baseCandidates: [MozcBridge.Candidate] = result.candidates.isEmpty
        ? [candidateFromValue(baseReplacement)]
        : mapCandidateValues(result.candidates) { frozenPrefix + $0.value + convertedSuffix }
    let sessionSeed = normalizeCommittedCandidateState(
        replacement: baseReplacement,
        candidates: baseCandidates)
    Log.pickup("shadow replace -> \(baseReplacement) (alts=\(sessionSeed.candidates.count))\(FrontmostApp.logSuffix())")

    let caretInSpan = caretUTF16 - wordStart
    let leftCount  = (sliceUTF16(span, start: 0,           end: caretInSpan)      ?? "").count
    let rightCount = (sliceUTF16(span, start: caretInSpan, end: span.utf16.count) ?? "").count
    // Keystroke deletes operate on whole graphemes. For ASCII romaji (the
    // common shadow span) grapheme == scalar == UTF-16 unit, so the caret
    // always lands on a boundary and the left/right split is exact. If a
    // non-ASCII grapheme straddles the caret, both slices count the split
    // grapheme and leftCount+rightCount over-counts the span — the extra
    // Backspace would eat a neighbouring character. Defer to the
    // Unicode-safe clipboard path in that case.
    guard leftCount + rightCount == span.count else {
        Log.pickup("shadow: caret mid-grapheme in \(span); deferring to clipboard")
        return false
    }
    for _ in 0..<leftCount  { postKey(kVK_Backspace) }
    for _ in 0..<rightCount { postKey(kVK_ForwardDelete) }
    postUnicode(baseReplacement)

    let frontmost = FrontmostApp.describe()
    let session = ConversionSession(
        backing: .clipboard(
            frontmostBundleId: frontmost?.bundleID,
            frontmostPid: frontmost?.pid ?? 0),
        originalReading: span,
        candidates: sessionSeed.candidates,
        candidateIndex: sessionSeed.currentIndex,
        timestamp: Date())
    ConversionSessionStore.set(session)
    if gCandidatePanelMode == .onConvert {
        CandidatePanel.shared.show(session: session)
    }
    return true
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

private func copyCurrentSelection(
    pasteboard pb: NSPasteboard,
    timeoutMs: Int
) -> String? {
    let baseline = pb.changeCount
    postKey(kVK_ANSI_C, flags: .maskCommand)
    guard waitForClipboardChange(after: baseline, timeoutMs: timeoutMs),
          let s = pb.string(forType: .string), !s.isEmpty else {
        return nil
    }
    return s
}

private func isUsefulHyphenatedPickup(_ s: String) -> Bool {
    var hasHyphen = false
    var hasLetter = false
    for byte in s.utf8 {
        if byte == 0x2D { hasHyphen = true }
        if byte >= 0x61 && byte <= 0x7A { hasLetter = true }
    }
    return hasHyphen && hasLetter
}

private func shrinkSelectionRight(_ count: Int) {
    guard count > 0 else { return }
    for _ in 0..<count {
        postKey(kVK_RightArrow, flags: .maskShift)
    }
}

private func reselectPreviousUTF16Units(_ count: Int) {
    guard count > 0 else { return }
    postKey(kVK_RightArrow)
    for _ in 0..<count {
        postKey(kVK_LeftArrow, flags: .maskShift)
    }
}

private func trailingUsefulHyphenatedRun(_ s: String) -> String? {
    var start = s.endIndex
    while start > s.startIndex {
        let prev = s.index(before: start)
        let byte = s[prev].utf8.first ?? 0
        guard (byte >= 0x61 && byte <= 0x7A) || byte == 0x2D else { break }
        start = prev
    }
    guard start < s.endIndex else { return nil }
    let tail = String(s[start..<s.endIndex])
    return isUsefulHyphenatedPickup(tail) ? tail : nil
}

/// Chromium clipboard fallback can copy more than the target token when the
/// renderer does not preserve the exact selection. Keep any left-side text
/// intact and only convert the trailing word-sized target.
private func splitTrailingClipboardTarget(
    _ text: String,
    appBundleID: String?
) -> (prefix: String, target: String) {
    guard isChromiumClipboardProbeBundleID(appBundleID) else {
        return ("", text)
    }
    let caret = text.utf16.count
    let (start, end) = wordBounds(text, caret: caret)
    guard start > 0, end == caret,
          let prefix = sliceUTF16(text, start: 0, end: start),
          let target = sliceUTF16(text, start: start, end: end),
          !target.isEmpty else {
        return ("", text)
    }
    return (prefix, target)
}

private func maybeExpandForcedHyphenatedSelectionWindow(
    _ original: String,
    pasteboard pb: NSPasteboard,
    timeoutMs: Int
) -> String? {
    let window = 24
    for _ in 0..<window {
        postKey(kVK_LeftArrow, flags: .maskShift)
    }
    Thread.sleep(forTimeInterval: 0.02)

    guard let expanded = copyCurrentSelection(
        pasteboard: pb,
        timeoutMs: timeoutMs
    ) else {
        shrinkSelectionRight(window)
        return nil
    }

    guard let tail = trailingUsefulHyphenatedRun(expanded),
          expanded.hasSuffix(tail) else {
        shrinkSelectionRight(window)
        return nil
    }

    let shrinkCount = expanded.utf16.count - tail.utf16.count
    shrinkSelectionRight(shrinkCount)
    return tail == original ? nil : tail
}

private func maybeExpandForcedHyphenatedSelectionByWord(
    _ original: String,
    pasteboard pb: NSPasteboard,
    timeoutMs: Int
) -> String? {
    var expandedWords = 0
    var best: (text: String, selectionLen: Int, expandedWords: Int)?
    var current = original

    // Keep this intentionally shallow: if the first word-left probe does
    // not reveal a useful hyphenated tail, the wider window scan below can
    // still recover the rare Chrome cases that need it.
    for _ in 0..<2 {
        postKey(kVK_LeftArrow, flags: [.maskShift, .maskAlternate])
        expandedWords += 1
        Thread.sleep(forTimeInterval: 0.02)

        guard let probe = copyCurrentSelection(
            pasteboard: pb,
            timeoutMs: timeoutMs
        ) else {
            reselectPreviousUTF16Units(original.utf16.count)
            return nil
        }

        guard probe != current else {
            expandedWords -= 1
            break
        }

        guard let tail = trailingUsefulHyphenatedRun(probe),
              probe.hasSuffix(tail) else {
            reselectPreviousUTF16Units(original.utf16.count)
            return nil
        }

        current = probe
        best = (tail, probe.utf16.count, expandedWords)
        if tail.utf16.count < probe.utf16.count {
            break
        }
    }

    guard let best, best.text != original else {
        reselectPreviousUTF16Units(original.utf16.count)
        return nil
    }

    shrinkSelectionRight(best.selectionLen - best.text.utf16.count)
    return best.text
}

private func maybeExpandForcedHyphenatedSelection(
    _ original: String,
    pasteboard pb: NSPasteboard,
    timings: ModoreConfig.ClipboardTimings,
    appBundleID: String?
) -> String? {
    guard shouldProbeForcedHyphenatedSelectionExpansion(
        original: original,
        appBundleID: appBundleID
    ) else {
        return nil
    }
    let expansionTimeoutMs = min(timings.readTimeoutMs, 120)

    if let expanded = maybeExpandForcedHyphenatedSelectionByWord(
        original,
        pasteboard: pb,
        timeoutMs: expansionTimeoutMs
    ) {
        return expanded
    }

    if let expanded = maybeExpandForcedHyphenatedSelectionWindow(
        original,
        pasteboard: pb,
        timeoutMs: expansionTimeoutMs
    ) {
        return expanded
    }

    var current = original
    var expandedChars = 0
    var best: (text: String, expandedChars: Int)?
    if isUsefulHyphenatedPickup(original) {
        best = (original, 0)
    }

    for _ in 0..<40 {
        postKey(kVK_LeftArrow, flags: .maskShift)
        expandedChars += 1

        guard let probe = copyCurrentSelection(
            pasteboard: pb,
            timeoutMs: expansionTimeoutMs
        ) else {
            shrinkSelectionRight(expandedChars)
            return nil
        }

        guard probe != current else {
            expandedChars -= 1
            break
        }

        guard isLowerASCIIHyphenRun(probe) else {
            shrinkSelectionRight(1)
            expandedChars -= 1
            break
        }

        current = probe
        if isUsefulHyphenatedPickup(probe) {
            best = (probe, expandedChars)
        }
    }

    guard let best else {
        shrinkSelectionRight(expandedChars)
        return nil
    }
    shrinkSelectionRight(expandedChars - best.expandedChars)
    return best.text == original ? nil : best.text
}

private func maybeExpandForcedPunctuationSelection(
    _ original: String,
    pasteboard pb: NSPasteboard,
    timings: ModoreConfig.ClipboardTimings
) -> String? {
    guard isConvertiblePunctuationOnly(original) else { return nil }
    let expansionTimeoutMs = min(timings.readTimeoutMs, 120)
    var current = original
    var expandedChars = 0
    var best: (text: String, expandedChars: Int)?

    for _ in 0..<40 {
        postKey(kVK_LeftArrow, flags: .maskShift)
        expandedChars += 1

        guard let probe = copyCurrentSelection(
            pasteboard: pb,
            timeoutMs: expansionTimeoutMs
        ) else {
            shrinkSelectionRight(expandedChars)
            return nil
        }

        guard probe != current else {
            expandedChars -= 1
            break
        }

        current = probe
        if hasLowerASCIICoreBeforePunctuation(probe) {
            best = (probe, expandedChars)
        }

        let firstByte = probe.utf8.first ?? 0
        if firstByte >= 0x80 || firstByte <= 0x20 {
            break
        }
    }

    guard let best else {
        shrinkSelectionRight(expandedChars)
        return nil
    }
    shrinkSelectionRight(expandedChars - best.expandedChars)
    return best.text == original ? nil : best.text
}

private func stripLineCopySuffix(_ s: String) -> String {
    var out = s
    while out.hasSuffix("\n") || out.hasSuffix("\r") {
        out.removeLast()
    }
    return out
}

private func trailingASCIIPickupTokenFromLineCopy(_ s: String) -> String? {
    let line = stripLineCopySuffix(s)
    guard !line.isEmpty else { return nil }
    var start = line.endIndex
    while start > line.startIndex {
        let prev = line.index(before: start)
        let byte = line[prev].utf8.first ?? 0
        guard byte > 0x20 && byte < 0x7F else { break }
        start = prev
    }
    guard start < line.endIndex else { return nil }
    let tail = String(line[start..<line.endIndex])
    return tail.utf8.contains(where: { $0 >= 0x61 && $0 <= 0x7A }) ? tail : nil
}

private func postClipboardReplacement(_ replacement: String, deleteBeforeInsert: Int) {
    if deleteBeforeInsert > 0 {
        for _ in 0..<deleteBeforeInsert {
            postKey(kVK_Backspace)
        }
    }
    postUnicode(replacement)
}

private func axStringAttr(_ element: AXUIElement, _ attr: String) -> String? {
    var ref: CFTypeRef?
    let err = AXUIElementCopyAttributeValue(element, attr as CFString, &ref)
    guard err == .success else { return nil }
    return ref as? String
}

func postUnicodeOverAXSelection(
    in element: AXUIElement,
    start: Int,
    end: Int,
    replacement: String
) -> Bool {
    var range = CFRange(location: start, length: end - start)
    guard let rangeValue = AXValueCreate(.cfRange, &range) else { return false }
    let rc = AXUIElementSetAttributeValue(
        element,
        kAXSelectedTextRangeAttribute as CFString,
        rangeValue)
    guard rc == .success else { return false }
    postUnicode(replacement)
    return true
}

/// Chromium omnibox needs a stricter first attempt than the generic AX
/// path. The live suggestion model is more reliable when we drive the
/// selected text as typed input, so prefer that path first and only
/// fall back to a keystroke-driven replace if the AX selection write
/// itself fails.
private func replaceChromiumOmnibox(
    field: FocusedField,
    start: Int,
    end: Int,
    originalReading: String,
    replacement: String,
    sessionSeed: (candidates: [MozcBridge.Candidate], currentIndex: Int)
) -> Bool {
    if postUnicodeOverAXSelection(
        in: field.element,
        start: start,
        end: end,
        replacement: replacement) {
        let session = ConversionSession(
            backing: .ax(element: field.element, spanStart: start),
            originalReading: originalReading,
            candidates: sessionSeed.candidates,
            candidateIndex: sessionSeed.currentIndex,
            timestamp: Date())
        ConversionSessionStore.set(session)
        if gCandidatePanelMode == .onConvert {
            CandidatePanel.shared.show(session: session)
        }
        return true
    }
    Log.pickup("Chromium omnibox: typed-input sync failed; falling back to keystroke retype")
    keystrokeReplaceSpan(
        caret: (start: field.selStart, end: field.selEnd),
        spanEnd: end,
        spanLen: end - start,
        replacement: replacement)
    let frontmost = FrontmostApp.describe()
    let session = ConversionSession(
        backing: .clipboard(
            frontmostBundleId: frontmost?.bundleID,
            frontmostPid: frontmost?.pid ?? 0),
        originalReading: originalReading,
        candidates: sessionSeed.candidates,
        candidateIndex: sessionSeed.currentIndex,
        timestamp: Date())
    ConversionSessionStore.set(session)
    if gCandidatePanelMode == .onConvert {
        CandidatePanel.shared.show(session: session)
    }
    return true
}

private func applyFieldReplacement(
    route: ModoreScript.Route,
    field: FocusedField,
    appId: String?,
    start: Int,
    end: Int,
    originalReading: String,
    replacement: String,
    sessionSeed: (candidates: [MozcBridge.Candidate], currentIndex: Int),
    dropAutocompleteTail: Bool = false,
    request: PickupRequest = .init()
) -> Bool {
    if dropAutocompleteTail, isChromiumOmnibox(field: field, appId: appId) {
        let fullEnd = field.value.utf16.count
        switch route {
        case .selectionSync, .ax:
            if postUnicodeOverAXSelection(
                in: field.element,
                start: 0,
                end: fullEnd,
                replacement: replacement) {
                let session = ConversionSession(
                    backing: .ax(element: field.element, spanStart: 0),
                    originalReading: originalReading,
                    candidates: sessionSeed.candidates,
                    candidateIndex: sessionSeed.currentIndex,
                    timestamp: Date())
                ConversionSessionStore.set(session)
                if gCandidatePanelMode == .onConvert {
                    CandidatePanel.shared.show(session: session)
                }
                return true
            }
            Log.pickup("Chromium omnibox: full-field replace failed; falling back to keystroke retype")
            keystrokeReplaceSpan(
                caret: (start: field.selStart, end: field.selEnd),
                spanEnd: fullEnd,
                spanLen: fullEnd,
                replacement: replacement)
            let frontmost = FrontmostApp.describe()
            let session = ConversionSession(
                backing: .clipboard(
                    frontmostBundleId: frontmost?.bundleID,
                    frontmostPid: frontmost?.pid ?? 0),
                originalReading: originalReading,
                candidates: sessionSeed.candidates,
                candidateIndex: sessionSeed.currentIndex,
                timestamp: Date())
            ConversionSessionStore.set(session)
            if gCandidatePanelMode == .onConvert {
                CandidatePanel.shared.show(session: session)
            }
            return true
        case .keystroke, .clipboard:
            keystrokeReplaceSpan(
                caret: (start: field.selStart, end: field.selEnd),
                spanEnd: fullEnd,
                spanLen: fullEnd,
                replacement: replacement)
            let frontmost = FrontmostApp.describe()
            let session = ConversionSession(
                backing: .clipboard(
                    frontmostBundleId: frontmost?.bundleID,
                    frontmostPid: frontmost?.pid ?? 0),
                originalReading: originalReading,
                candidates: sessionSeed.candidates,
                candidateIndex: sessionSeed.currentIndex,
                timestamp: Date())
            ConversionSessionStore.set(session)
            if gCandidatePanelMode == .onConvert {
                CandidatePanel.shared.show(session: session)
            }
            return true
        }
    }

    switch route {
    case .selectionSync:
        if postUnicodeOverAXSelection(
            in: field.element,
            start: start,
            end: end,
            replacement: replacement) {
            let session = ConversionSession(
                backing: .ax(element: field.element, spanStart: start),
                originalReading: originalReading,
                candidates: sessionSeed.candidates,
                candidateIndex: sessionSeed.currentIndex,
                timestamp: Date())
            ConversionSessionStore.set(session)
            if gCandidatePanelMode == .onConvert {
                CandidatePanel.shared.show(session: session)
            }
            return true
        }
        Log.pickup("selection-sync route failed; falling back to keystroke-retype")
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
            originalReading: originalReading,
            candidates: sessionSeed.candidates,
            candidateIndex: sessionSeed.currentIndex,
            timestamp: Date())
        ConversionSessionStore.set(session)
        if gCandidatePanelMode == .onConvert {
            CandidatePanel.shared.show(session: session)
        }
        return true
    case .keystroke, .clipboard:
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
            originalReading: originalReading,
            candidates: sessionSeed.candidates,
            candidateIndex: sessionSeed.currentIndex,
            timestamp: Date())
        ConversionSessionStore.set(session)
        if gCandidatePanelMode == .onConvert {
            CandidatePanel.shared.show(session: session)
        }
        return true
    case .ax:
        if isChromiumOmnibox(field: field, appId: appId) {
            return replaceChromiumOmnibox(
                field: field,
                start: start,
                end: end,
                originalReading: originalReading,
                replacement: replacement,
                sessionSeed: sessionSeed)
        }
        // Chrome web fields (everything but the omnibox, handled above) silently
        // reject AX value writes, and the *failed* write scrambles the
        // caret/selection unpredictably — that scramble, not the keystroke logic,
        // is what made the fallback delete the wrong range (autocomplete combobox
        // → span left selected, first Backspace eats it; chat contenteditable →
        // caret jumps to 0, conversion prepends). So don't attempt the AX write
        // for them at all: replace by synthetic keystrokes driven by the caret we
        // read at pickup, which is still accurate because nothing perturbed it.
        // This is Espanso's browser strategy — it never writes text via AX, it
        // backspaces the trigger and types the replacement from the live caret.
        let skipAXWrite = isChromiumClipboardProbeBundleID(appId)
        if !skipAXWrite,
           replaceRange(in: field.element, start: start, end: end, replacement: replacement) {
            let session = ConversionSession(
                backing: .ax(element: field.element, spanStart: start),
                originalReading: originalReading,
                candidates: sessionSeed.candidates,
                candidateIndex: sessionSeed.currentIndex,
                timestamp: Date())
            ConversionSessionStore.set(session)
            if gCandidatePanelMode == .onConvert {
                CandidatePanel.shared.show(session: session)
            }
            return true
        }
        Log.pickup(skipAXWrite
            ? "Chromium web field: keystroke-replacing [\(start)..\(end)] \(originalReading) without AX write\(FrontmostApp.logSuffix())"
            : "AX write rejected; trying shadow pickup for [\(start)..\(end)] \(originalReading)\(FrontmostApp.logSuffix())")
        if gShadowBufferEnabled, doShadowPickup(request) { return true }
        // The pickup-time caret (field.sel*) is accurate here — for Chromium
        // because we skipped the perturbing AX write, for everyone else because
        // replaceRange restores the selection to its pre-write state on failure.
        // keystrokeReplaceSpan collapses a real selection (RightArrow → right end)
        // or skips navigation when the caret is already collapsed (avoids
        // overshooting end+1 at a Chrome contenteditable block boundary).
        keystrokeReplaceSpan(
            caret: (start: field.selStart, end: field.selEnd),
            spanEnd: end,
            spanLen: end - start,
            replacement: replacement)
        let frontmost = FrontmostApp.describe()
        let session = ConversionSession(
            backing: .clipboard(
                frontmostBundleId: frontmost?.bundleID,
                frontmostPid: frontmost?.pid ?? 0),
            originalReading: originalReading,
            candidates: sessionSeed.candidates,
            candidateIndex: sessionSeed.currentIndex,
            timestamp: Date())
        ConversionSessionStore.set(session)
        if gCandidatePanelMode == .onConvert {
            CandidatePanel.shared.show(session: session)
        }
        return true
    }
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
    if gShadowBufferEnabled, doShadowPickup(request) { return }

    // Snapshot timings once at entry so the whole pickup runs against a
    // consistent set even if the watcher fires mid-flight on the main thread.
    let timings = gClipboardTimings

    // Snapshot the user's clipboard now; `defer` guarantees we restore it on
    // every exit path. No manual cleanup at the early-returns below.
    let (_, restoreSavedClipboard) = guardClipboard(restoreDelayMs: timings.restoreClipboardDelayMs)
    defer { restoreSavedClipboard() }

    let pb = NSPasteboard.general
    var picked: String? = nil
    var deletePickedBeforeInsert = false

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
                if frontmostBundleID == "com.sublimetext.4",
                   let tail = trailingASCIIPickupTokenFromLineCopy(s) {
                    picked = tail
                    deletePickedBeforeInsert = true
                    Log.clipboard("using Sublime line-copy tail: \(tail)")
                } else {
                    Log.clipboard("Cmd+C looks like a line-copy (no real selection); will force-select previous word")
                }
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
            if let expanded = maybeExpandForcedPunctuationSelection(
                s,
                pasteboard: pb,
                timings: timings
            ) {
                picked = expanded
                Log.clipboard("expanded punctuation romaji selection: \(s) → \(expanded)")
            } else if let expanded = maybeExpandForcedHyphenatedSelection(
                s,
                pasteboard: pb,
                timings: timings,
                appBundleID: frontmostBundleID
            ) {
                picked = expanded
                Log.clipboard("expanded hyphenated romaji selection: \(s) → \(expanded)")
            } else {
                picked = s
            }
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

    let clipboardSelection = pickedText
    let (clipboardPrefix, clipboardTarget) = splitTrailingClipboardTarget(
        pickedText,
        appBundleID: frontmostBundleID
    )
    pickedText = clipboardTarget

    Log.clipboard("pick: \(pickedText)")
    // Backspace deletes one *grapheme* per press, so the count must be the
    // grapheme-cluster count — not UTF-16 units. A supplementary-plane char
    // (emoji: 1 grapheme = 2 UTF-16 units) would otherwise fire an extra
    // Backspace and eat a preceding character. Matches undoOnClipboard /
    // cycleClipboard, which already use .count.
    let deleteBeforeInsertCount = (deletePickedBeforeInsert ||
        (didForceSelect && (frontmostBundleID.map(kPeekExistingSelectionBlocklist.contains) ?? false)))
        ? pickedText.count
        : 0

    // Mirror the AX path's script-boundary split for the clipboard path:
    // Shift+Opt+Left in Discord/Sublime grabs the whole word including any
    // non-ASCII prefix (e.g. `対人sen` arrives as one blob), and the bridge
    // sends each byte of UTF-8 to Mozc as a separate `key_code`. Also handle
    // mixed selections where the acquired token includes Japanese text after
    // the caret-side ASCII run (`tesutoテスト` with the caret after `o`).
    let (asciiPrefix, romajiTail, preservedSuffix) = splitConvertibleASCIIWindow(pickedText)
    let (romajiCore, romajiSuffix) = splitTrailingASCIIPunctuation(romajiTail)
    let convertedSuffix = convertTrailingASCIISuffix(romajiSuffix, request: request) + preservedSuffix
    guard !romajiCore.isEmpty else {
        let replacement = clipboardPrefix + asciiPrefix + convertedSuffix
        guard replacement != clipboardSelection else {
            Log.clipboard("nothing to convert (no trailing romaji in \(pickedText))")
            if didForceSelect {
                postKey(kVK_RightArrow)
            }
            return
        }
        Log.clipboard("punctuation-only pickup -> \(replacement)")
        postClipboardReplacement(
            replacement,
            deleteBeforeInsert: deleteBeforeInsertCount)
        if didForceSelect {
            postKey(kVK_RightArrow)
        }
        return
    }

    let (leadingJunk, romajiBody) = splitLeadingASCIIJunkBeforeLowercase(romajiCore)
    let (acronymHead, mozcInput) = splitAcronymHead(romajiBody)
    let frozenPrefix = asciiPrefix + leadingJunk + acronymHead
    if mozcInput.first?.isLowercase != true {
        let replacement = clipboardPrefix + frozenPrefix + mozcInput + convertedSuffix
        guard replacement != clipboardSelection else {
            Log.clipboard("skipping: no romaji tail after acronym split (\(mozcInput))")
            if didForceSelect {
                postKey(kVK_RightArrow)
            }
            return
        }
        let baseCandidates = [candidateFromValue(replacement, group: .input)]
        let scriptSpan = mdr_span_t(span_start_byte: 0, span_end_byte: 0,
                                    romaji: nil, romaji_len: 0)
        let finalReplacement = ModoreScript.replacement(
            appId: frontmostBundleID, span: scriptSpan, candidates: candidateValues(baseCandidates)) ?? replacement
        let snapshotCandidateValues = ModoreScript.candidates(
            appId: frontmostBundleID, list: candidateValues(baseCandidates), currentIndex: 0)
            ?? candidateValues(baseCandidates)
        let snapshotCandidates = applyScriptCandidateValues(
            snapshotCandidateValues, onto: baseCandidates)
        Log.clipboard("replace -> \(finalReplacement) (alts=\(snapshotCandidates.count))")
        let sessionSeed = normalizeCommittedCandidateState(
            replacement: finalReplacement,
            candidates: snapshotCandidates)
        postClipboardReplacement(
            finalReplacement,
            deleteBeforeInsert: deleteBeforeInsertCount)
        let frontmost = FrontmostApp.describe()
        let session = ConversionSession(
            backing: .clipboard(
                frontmostBundleId: frontmost?.bundleID,
                frontmostPid: frontmost?.pid ?? 0),
            originalReading: clipboardSelection,
            candidates: sessionSeed.candidates,
            candidateIndex: sessionSeed.currentIndex,
            timestamp: Date())
        ConversionSessionStore.set(session)
        if gCandidatePanelMode == .onConvert {
            CandidatePanel.shared.show(session: session)
        }
        return
    }

    let baseReplacement: String
    let baseCandidates: [MozcBridge.Candidate]
    if gClassifierEnabled,
       let segResult = classifierSegmentedConvert(mozcInput, request: request) {
        baseReplacement = clipboardPrefix + frozenPrefix + segResult.replacement + convertedSuffix
        baseCandidates = mapCandidateValues(segResult.candidates) {
            clipboardPrefix + frozenPrefix + $0.value + convertedSuffix
        }
    } else {
        // Fallback: heuristic acronym split.
        guard let result = callBackend(mozcInput, request: request, wantCandidates: true) else {
            Log.clipboard("backend returned no result")
            if didForceSelect {
                postKey(kVK_RightArrow)
            }
            return
        }
        baseReplacement = clipboardPrefix + frozenPrefix + result.replacement + convertedSuffix
        baseCandidates = result.candidates.isEmpty
            ? [candidateFromValue(clipboardPrefix + frozenPrefix + result.replacement + convertedSuffix)]
            : mapCandidateValues(result.candidates) {
                clipboardPrefix + frozenPrefix + $0.value + convertedSuffix
            }
    }
    // Script overrides — no AX span on the clipboard path, so span byte
    // offsets are zeroed. Scripts that care about position should branch
    // on app_id instead.
    let scriptSpan = mdr_span_t(span_start_byte: 0, span_end_byte: 0,
                                romaji: nil, romaji_len: 0)
        let replacement = ModoreScript.replacement(
            appId: frontmostBundleID, span: scriptSpan, candidates: candidateValues(baseCandidates)) ?? baseReplacement
    let snapshotCandidateValues = ModoreScript.candidates(
        appId: frontmostBundleID, list: candidateValues(baseCandidates), currentIndex: 0)
        ?? candidateValues(baseCandidates)
    let snapshotCandidates = applyScriptCandidateValues(
        snapshotCandidateValues, onto: baseCandidates)
    Log.clipboard("replace -> \(replacement) (alts=\(snapshotCandidates.count))")
    let sessionSeed = normalizeCommittedCandidateState(
        replacement: replacement,
        candidates: snapshotCandidates)

    // Replace the active selection by injecting the replacement as a Unicode
    // keystroke into the session event tap. No clipboard touch on the replace
    // path → no flicker, no race with the user's clipboard contents.
    postClipboardReplacement(
        replacement,
        deleteBeforeInsert: deleteBeforeInsertCount)

    // Open a clipboard-backed session for follow-up gestures. The frontmost
    // app's pid + bundle ID are the identity check Esc-undo / cycle use to
    // refuse to act on the wrong window.
    let frontmost = FrontmostApp.describe()
        let session = ConversionSession(
            backing: .clipboard(
                frontmostBundleId: frontmost?.bundleID,
                frontmostPid: frontmost?.pid ?? 0),
            originalReading: clipboardSelection,
            candidates: sessionSeed.candidates,
            candidateIndex: sessionSeed.currentIndex,
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
func runConversionOnAcquiredText(
    _ raw: String,
    request: PickupRequest,
    appId: String?
) {
    var pickedText = raw
    if pickedText.hasSuffix("\n") { pickedText.removeLast() }
    if pickedText.hasSuffix("\r") { pickedText.removeLast() }
    if pickedText.isEmpty {
        Log.pickup("scripted acquire: empty text — nothing to convert")
        return
    }
    Log.pickup("scripted acquire pick: \(pickedText)")

    let (asciiPrefix, rawTail, preservedSuffix) = splitConvertibleASCIIWindow(pickedText)
    let (contextPrefix, romajiTail) = splitBeforeLastASCIIWord(rawTail)
    let (romajiCore, romajiSuffix) = splitTrailingASCIIPunctuation(romajiTail)
    let convertedSuffix = convertTrailingASCIISuffix(romajiSuffix, request: request) + preservedSuffix
    if romajiCore.isEmpty {
        let replacement = asciiPrefix + contextPrefix + convertedSuffix
        guard replacement != pickedText else {
            Log.pickup("scripted acquire: no trailing romaji in \(pickedText)")
            return
        }
        let baseCandidates = [candidateFromValue(replacement, group: .input)]
        let scriptSpan = mdr_span_t(span_start_byte: 0, span_end_byte: 0,
                                    romaji: nil, romaji_len: 0)
        let finalReplacement = ModoreScript.replacement(
            appId: appId, span: scriptSpan, candidates: candidateValues(baseCandidates)) ?? replacement
        let snapshotCandidateValues = ModoreScript.candidates(
            appId: appId, list: candidateValues(baseCandidates), currentIndex: 0)
            ?? candidateValues(baseCandidates)
        let snapshotCandidates = applyScriptCandidateValues(
            snapshotCandidateValues, onto: baseCandidates)
        Log.pickup("scripted acquire punctuation-only pickup -> \(finalReplacement) (alts=\(snapshotCandidates.count))")
        let sessionSeed = normalizeCommittedCandidateState(
            replacement: finalReplacement,
            candidates: snapshotCandidates)
        postUnicode(finalReplacement)
        let session = ConversionSession(
            backing: .clipboard(
                frontmostBundleId: appId,
                frontmostPid: FrontmostApp.currentPid() ?? 0),
            originalReading: pickedText,
            candidates: sessionSeed.candidates,
            candidateIndex: sessionSeed.currentIndex,
            timestamp: Date())
        ConversionSessionStore.set(session)
        if gCandidatePanelMode == .onConvert {
            CandidatePanel.shared.show(session: session)
        }
        return
    }
    let (leadingJunk, romajiBody) = splitLeadingASCIIJunkBeforeLowercase(romajiCore)
    let (acronymHead, mozcInput) = splitAcronymHead(romajiBody)
    let frozenPrefix = asciiPrefix + contextPrefix + leadingJunk + acronymHead

    let baseReplacement: String
    let baseCandidates: [MozcBridge.Candidate]
    if gClassifierEnabled,
       let segResult = classifierSegmentedConvert(mozcInput, request: request) {
        baseReplacement = frozenPrefix + segResult.replacement + convertedSuffix
        baseCandidates = mapCandidateValues(segResult.candidates) {
            frozenPrefix + $0.value + convertedSuffix
        }
    } else {
        guard let result = callBackend(mozcInput, request: request, wantCandidates: true) else {
            Log.pickup("scripted acquire: backend returned no result")
            return
        }
        baseReplacement = frozenPrefix + result.replacement + convertedSuffix
        baseCandidates = result.candidates.isEmpty
            ? [candidateFromValue(baseReplacement)]
            : mapCandidateValues(result.candidates) {
                frozenPrefix + $0.value + convertedSuffix
            }
    }

    let scriptSpan = mdr_span_t(span_start_byte: 0, span_end_byte: 0,
                                romaji: nil, romaji_len: 0)
    let replacement = ModoreScript.replacement(
        appId: appId, span: scriptSpan, candidates: candidateValues(baseCandidates)) ?? baseReplacement
    let snapshotCandidateValues = ModoreScript.candidates(
        appId: appId, list: candidateValues(baseCandidates), currentIndex: 0)
        ?? candidateValues(baseCandidates)
    let snapshotCandidates = applyScriptCandidateValues(
        snapshotCandidateValues, onto: baseCandidates)

    Log.pickup("scripted acquire replace → \(replacement) (alts=\(snapshotCandidates.count))")
    let sessionSeed = normalizeCommittedCandidateState(
        replacement: replacement,
        candidates: snapshotCandidates)

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
        candidates: sessionSeed.candidates,
        candidateIndex: sessionSeed.currentIndex,
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
    // conversion. Katakana presses either convert katakana or cycle
    // backwards depending on `[conversion] katakana_modifier_behavior`.
    // Failure to cycle (no session, expired window, gates broke) falls
    // through silently to fresh convert below — the tap callback clears
    // the session on any non-chord keystroke, so by the time we get here
    // a stale session almost always means "user is legitimately starting
    // a new conversion."
    if request.target != .katakana {
        if cycleNext(verbose: false) {
            return
        }
    } else if gKatakanaModifierBehavior == .cycleBackwards {
        if cyclePrevious(verbose: false) {
            return
        }
    }

    if request.target == .katakana {
        Log.pickup("katakana modifier engaged")
    }

    // Frontmost-app capture once, threaded through script hooks below.
    let appId = FrontmostApp.describe()?.bundleID

    // First AX attempt. If it fails — typically because the frontmost app
    // is Electron and hasn't been opted into accessibility — flip the
    // documented `AXManualAccessibility` switch on that app's pid and try
    // again. Once-per-pid; cheap no-op for apps that don't support it.
    var focusedField = readFocusedField()
    if focusedField == nil, frontmostAppLooksElectron() {
        enableElectronAXIfNeeded()
        focusedField = readFocusedField()
    }

    if let picked = ModoreScript.acquire(
        appId: appId,
        fullText: focusedField?.value,
        caretUTF16: focusedField?.selStart ?? 0,
        fieldRole: focusedField?.role,
        fieldDescription: focusedField?.description,
        katakana: request.target == .katakana) {
        Log.pickup("scripted acquire → \(picked.count) chars\(FrontmostApp.logSuffix())")
        runConversionOnAcquiredText(picked, request: request, appId: appId)
        return
    }

    let hasScript = appId.map(ModoreScript.hasScript(forAppId:)) ?? false
    let route = ModoreScript.routeFor(
        appId: appId,
        fullText: focusedField?.value,
        caretUTF16: focusedField?.selStart ?? 0,
        fieldRole: focusedField?.role,
        fieldDescription: focusedField?.description,
        katakana: request.target == .katakana)
        ?? (hasScript ? .ax : (focusedField == nil ? .clipboard : .ax))

    // A script returning "clipboard" skips the AX write path entirely and
    // goes straight to the Cmd+C fallback path.
    if route == .clipboard {
        Log.pickup("scripted route → clipboard\(FrontmostApp.logSuffix())")
        doClipboardPickup(request)
        return
    }
    guard let field = focusedField else {
        if hasScript {
            Log.pickup("scripted app has no AX field; skipping host clipboard fallback\(FrontmostApp.logSuffix())")
            return
        }
        doClipboardPickup(request)
        return
    }

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
        (start, end) = wordBounds(field.value, caret: field.selStart)
    }
    guard start >= 0, end <= field.value.utf16.count, start < end else {
        Log.pickup("empty span; nothing to convert")
        return
    }

    var pickupStart = start
    var pickupEnd = end
    var dropAutocompleteTail = false
    if field.selStart != field.selEnd,
       isChromiumOmnibox(field: field, appId: appId),
       let typed = chromiumOmniboxTypedInputSnapshot() {
        if typed.utf16.count > 0,
           !chromiumOmniboxSelectionMatchesTypedInput(
            field: field,
            start: start,
            end: end) {
            pickupStart = 0
            pickupEnd = start
            dropAutocompleteTail = true
            Log.pickup("Chromium omnibox typed-input log detected; using prefix [\(pickupStart)..\(pickupEnd)] instead of selected suffix [\(start)..\(end)]")
        }
    }

    guard let spanText = sliceUTF16(field.value, start: pickupStart, end: pickupEnd) else {
        Log.pickup("empty span; nothing to convert")
        return
    }
        Log.pickup("pick [\(pickupStart)..\(pickupEnd)] \(spanText)")

        // Strip any non-ASCII prefix before handing off to Mozc — same
        // reason as the clipboard path. `wordBounds` already does this on
        // the empty-caret branch (script-transition stop), but a
        // user-made selection covering `対人sen` bypasses it. Without this
        // the bridge feeds Mozc UTF-8 bytes as Latin-1 codepoints and
        // mojibake comes back.
        let (asciiPrefix, romajiTail, preservedSuffix) = splitConvertibleASCIIWindow(spanText)
        let (romajiCore, romajiSuffix) = splitTrailingASCIIPunctuation(romajiTail)
        let convertedSuffix = convertTrailingASCIISuffix(romajiSuffix, request: request) + preservedSuffix
        let (leadingJunk, romajiBody) = splitLeadingASCIIJunkBeforeLowercase(romajiCore)
        guard !romajiCore.isEmpty else {
            let replacement = asciiPrefix + convertedSuffix
            guard replacement != spanText else {
                Log.pickup("nothing to convert (no trailing romaji in \(spanText))")
                return
            }
            Log.pickup("punctuation-only pickup -> \(replacement)")
            let scriptSpanStart = field.value.utf8ByteOffset(forUTF16Offset: pickupStart)
            let scriptSpanEnd   = field.value.utf8ByteOffset(forUTF16Offset: pickupEnd)
            let scriptSpan = mdr_span_t(
                span_start_byte: size_t(scriptSpanStart),
                span_end_byte: size_t(scriptSpanEnd),
                romaji: nil, romaji_len: 0)
            let baseCandidates = [candidateFromValue(replacement, group: .input)]
            let finalReplacement = ModoreScript.replacement(
                appId: appId, span: scriptSpan, candidates: candidateValues(baseCandidates)) ?? replacement
            let snapshotCandidateValues = ModoreScript.candidates(
                appId: appId, list: candidateValues(baseCandidates), currentIndex: 0)
                ?? candidateValues(baseCandidates)
            let snapshotCandidates = applyScriptCandidateValues(
                snapshotCandidateValues, onto: baseCandidates)
            let sessionSeed = normalizeCommittedCandidateState(
                replacement: finalReplacement,
                candidates: snapshotCandidates)
            _ = applyFieldReplacement(
                route: route,
                field: field,
                appId: appId,
                start: pickupStart,
                end: pickupEnd,
                originalReading: spanText,
                replacement: finalReplacement,
                sessionSeed: sessionSeed,
                dropAutocompleteTail: dropAutocompleteTail,
                request: request)
            return
        }

        if gClassifierEnabled,
           let segResult = classifierSegmentedConvert(romajiBody, request: request) {
            let baseReplacement = asciiPrefix + leadingJunk + segResult.replacement + convertedSuffix
            let baseCandidates = mapCandidateValues(segResult.candidates) {
                asciiPrefix + leadingJunk + $0.value + convertedSuffix
            }

            let scriptSpanStart = field.value.utf8ByteOffset(forUTF16Offset: pickupStart)
            let scriptSpanEnd   = field.value.utf8ByteOffset(forUTF16Offset: pickupEnd)
            let scriptSpan = mdr_span_t(
                span_start_byte: size_t(scriptSpanStart),
                span_end_byte: size_t(scriptSpanEnd),
                romaji: nil, romaji_len: 0)
            let replacement = ModoreScript.replacement(
                appId: appId, span: scriptSpan, candidates: candidateValues(baseCandidates)) ?? baseReplacement
            let snapshotCandidateValues = ModoreScript.candidates(
                appId: appId, list: candidateValues(baseCandidates), currentIndex: 0)
                ?? candidateValues(baseCandidates)
            let snapshotCandidates = applyScriptCandidateValues(
                snapshotCandidateValues, onto: baseCandidates)
            Log.pickup("classifier replace -> \(replacement) (alts=\(snapshotCandidates.count))")
            let sessionSeed = normalizeCommittedCandidateState(
                replacement: replacement,
                candidates: snapshotCandidates)
        _ = applyFieldReplacement(
            route: route,
            field: field,
            appId: appId,
            start: pickupStart,
            end: pickupEnd,
            originalReading: spanText,
            replacement: replacement,
            sessionSeed: sessionSeed,
            dropAutocompleteTail: dropAutocompleteTail,
            request: request)
            return
        }

        // Fallback: heuristic split. Carve off any leading acronym/code
        // (`R&D`, `API`, `JSON`) so Mozc only sees actual romaji.
        let (acronymHead, mozcInput) = splitAcronymHead(romajiBody)
        let frozenPrefix = asciiPrefix + leadingJunk + acronymHead

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
        let baseReplacement = frozenPrefix + result.replacement + convertedSuffix
        let baseCandidates: [MozcBridge.Candidate] = result.candidates.isEmpty
            ? [candidateFromValue(baseReplacement)]
            : mapCandidateValues(result.candidates) {
                frozenPrefix + $0.value + convertedSuffix
            }

        // Script overrides for replacement text and candidate list. Both
        // are independently optional. `replacement` is what gets written
        // immediately; `snapshotCandidates` is what cycle steps through.
        let scriptSpanStart = field.value.utf8ByteOffset(forUTF16Offset: pickupStart)
        let scriptSpanEnd   = field.value.utf8ByteOffset(forUTF16Offset: pickupEnd)
        let scriptSpan = mdr_span_t(
            span_start_byte: size_t(scriptSpanStart),
            span_end_byte: size_t(scriptSpanEnd),
            romaji: nil, romaji_len: 0)
        let replacement = ModoreScript.replacement(
            appId: appId, span: scriptSpan, candidates: candidateValues(baseCandidates)) ?? baseReplacement
        let snapshotCandidateValues = ModoreScript.candidates(
            appId: appId, list: candidateValues(baseCandidates), currentIndex: 0)
            ?? candidateValues(baseCandidates)
        let snapshotCandidates = applyScriptCandidateValues(
            snapshotCandidateValues, onto: baseCandidates)
        Log.pickup("replace -> \(replacement) (alts=\(snapshotCandidates.count))")
        let sessionSeed = normalizeCommittedCandidateState(
            replacement: replacement,
            candidates: snapshotCandidates)

        _ = applyFieldReplacement(
            route: route,
            field: field,
            appId: appId,
            start: pickupStart,
            end: pickupEnd,
            originalReading: spanText,
            replacement: replacement,
            sessionSeed: sessionSeed,
            dropAutocompleteTail: dropAutocompleteTail,
            request: request)
        return
}
