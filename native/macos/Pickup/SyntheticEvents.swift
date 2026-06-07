// Low-level synthetic key/Unicode injection. Used by the clipboard fallback
// in `Pickup.swift` (Cmd+C, Shift+Opt+Left, postUnicode of the replacement)
// and any future feature that needs to drive the keyboard from inside the
// host. Pure side-effect functions — no state of their own.

import Cocoa

/// The modifier bits modore treats as "core" when matching a physical
/// keystroke against a configured chord. The OS also sets device-dependent
/// bits (numeric-pad, coreGraphics flags) that we intersect away so a chord
/// comparison only sees Cmd / Shift / Ctrl / Alt.
let kCoreModifierFlags: CGEventFlags = [.maskCommand, .maskShift, .maskControl, .maskAlternate]

// MARK: - Posting location + self-event marker

/// All synthetic events are posted into the **session** event tap, not the HID
/// tap. The HID tap re-runs events through low-level transforms — which strips
/// Unicode-only events (virtualKey=0 + setUnicodeString) before they reach
/// Chromium-based apps (Cursor, VSCode, Slack, Electron). The session tap is
/// the insertion point Chromium itself reads from, so events posted there are
/// honored everywhere. This is the same posting location OpenKey ends up at
/// when calling CGEventTapPostEvent from its own tap callback.
let kPostTap: CGEventTapLocation = .cgSessionEventTap

/// Sentinel `CGEvent.location` we stamp on every synthetic event so the same
/// process's tap callback can recognize and skip events it emitted itself
/// (otherwise a future "synth a hotkey" feature would loop).
let kSyntheticEventMarker = CGPoint(x: -27469, y: 0)

/// Read the Unicode text a keyDown event carries, if any. macOS exposes
/// this through a two-call dance: probe for the length with a nil buffer,
/// then copy into a buffer of that size. Returns nil when the event carries
/// no string payload (modifier-only, dead keys mid-sequence, …). Shared by
/// the shadow buffer and the Chromium omnibox typed-input log, which both
/// reconstruct what the user typed from the raw event stream.
func unicodeString(from event: CGEvent) -> String? {
    var actualLength = 0
    event.keyboardGetUnicodeString(
        maxStringLength: 0, actualStringLength: &actualLength, unicodeString: nil)
    guard actualLength > 0 else { return nil }
    var buffer = [UniChar](repeating: 0, count: actualLength)
    var copiedLength = 0
    buffer.withUnsafeMutableBufferPointer { ptr in
        event.keyboardGetUnicodeString(
            maxStringLength: actualLength,
            actualStringLength: &copiedLength,
            unicodeString: ptr.baseAddress)
    }
    guard copiedLength > 0 else { return nil }
    return String(utf16CodeUnits: buffer, count: copiedLength)
}

/// Stamp the marker on a freshly created event. Call before `post(tap:)`.
func markSynthetic(_ event: CGEvent) {
    event.location = kSyntheticEventMarker
}

/// True if `event` was emitted by this process via `markSynthetic`.
func isSynthetic(_ event: CGEvent) -> Bool {
    let loc = event.location
    return abs(loc.x - kSyntheticEventMarker.x) < 0.001
        && abs(loc.y - kSyntheticEventMarker.y) < 0.001
}

// MARK: - Key + Unicode posting

/// Source for synthetics. `.privateState` keeps the event isolated from the
/// user's physical modifier state — a `Cmd+C` we post is delivered as
/// exactly that, even while the user is still holding Ctrl from the
/// conversion chord. With `.combinedSessionState` the event would merge
/// with the held Ctrl and arrive as Ctrl+Cmd+C, which most apps no-op.
/// Eliminates `waitForModifiersToClear`-style polling on the clipboard
/// path.
private func makeSyntheticSource() -> CGEventSource? {
    return CGEventSource(stateID: .privateState)
}

func postKey(_ keyCode: CGKeyCode, flags: CGEventFlags = []) {
    Log.tagged("scripting", "postKey: keyCode=\(keyCode) flags=\(flags.rawValue)")
    let src = makeSyntheticSource()
    if let down = CGEvent(keyboardEventSource: src, virtualKey: keyCode, keyDown: true) {
        down.flags = flags
        markSynthetic(down)
        Log.tagged("scripting", "postKey: keyDown posted")
        down.post(tap: kPostTap)
    }
    if let up = CGEvent(keyboardEventSource: src, virtualKey: keyCode, keyDown: false) {
        up.flags = flags
        markSynthetic(up)
        Log.tagged("scripting", "postKey: keyUp posted")
        up.post(tap: kPostTap)
    }
}

/// Inject a Unicode string as keyboard input — OpenKey's technique.
/// The receiving app treats it as typed text; replaces the active selection
/// (if any) or inserts at the caret. No clipboard involved.
///
/// `CGEventKeyboardSetUnicodeString` silently truncates payloads longer than
/// 20 UTF-16 code units (undocumented macOS limit). We chunk to defeat it.
let kUnicodeChunkMax = 20

func postUnicode(_ s: String) {
    Log.tagged("unicode", "postUnicode: \(s.count) chars")
    let src = makeSyntheticSource()
    let utf16 = Array(s.utf16)
    guard !utf16.isEmpty else { return }

    var i = 0
    while i < utf16.count {
        let end = min(i + kUnicodeChunkMax, utf16.count)
        let chunk = Array(utf16[i..<end])
        if let down = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: true) {
            down.flags = []
            down.keyboardSetUnicodeString(stringLength: chunk.count, unicodeString: chunk)
            markSynthetic(down)
            Log.tagged("unicode", "postUnicode: keyDown chunk=\(chunk.count) utf16")
            down.post(tap: kPostTap)
        }
        if let up = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: false) {
            up.flags = []
            up.keyboardSetUnicodeString(stringLength: chunk.count, unicodeString: chunk)
            markSynthetic(up)
            Log.tagged("unicode", "postUnicode: keyUp chunk=\(chunk.count) utf16")
            up.post(tap: kPostTap)
        }
        i = end
    }
}

/// Put `text` on the pasteboard and synthesize Cmd+V. Callers are responsible
/// for restoring the previous clipboard contents once the paste has had time to
/// land.
func postPasteFromClipboard(_ text: String) {
    let pb = NSPasteboard.general
    pb.clearContents()
    pb.setString(text, forType: .string)
    postKey(kVK_ANSI_V, flags: .maskCommand)
}

/// Paste `text` while preserving the user's clipboard contents. This is the
/// batch/multicursor path: keep the existing selection live and let the host
/// replace it with Cmd+V instead of destroying it with a backspace storm.
func pastePreservingClipboard(_ text: String) {
    let saved = snapshotClipboard()
    postPasteFromClipboard(text)
    let restoreDelayMs = clipboardPasteRestoreDelayMs(
        configuredMs: gClipboardTimings.restoreClipboardDelayMs)
    restoreClipboardAsync(saved, delayMs: restoreDelayMs)
}

/// Recreate a collapsed selection by extending it leftward, then paste over it
/// while preserving the user's clipboard.
func selectLeftAndPastePreservingClipboard(selecting graphemes: Int, text: String) {
    guard graphemes > 0 else {
        pastePreservingClipboard(text)
        return
    }
    for _ in 0..<graphemes {
        postKey(kVK_LeftArrow, flags: .maskShift)
    }
    pastePreservingClipboard(text)
}

/// Replace a known span by synthesized keystrokes, no AX writes involved.
/// Sibling to `replaceRange` in `AccessibilityIO.swift`: same operation
/// (`field[start..<end] = replacement`) against the synthetic-keystroke
/// I/O surface instead of the AX one. Used as a fallback when AX value
/// writes are silently rejected (Discord, Obsidian CodeMirror, Cursor
/// Monaco) — see the call site in `Pickup.swift` for the diagnostic
/// chain that justifies this surface.
///
/// The caret may be anywhere within the span when this is called
/// (`replaceRange`'s rollback restores the AX selection to whatever
/// it was, which is `caret` here). Move it to `spanEnd` first, then
/// `Backspace × spanLen` to delete the span, then `postUnicode` the
/// replacement. The caret/selection is given as (start, end) in
/// UTF-16 code units, matching the AX domain used by the pickup
/// pipeline.
///
/// Caveat: relies on the caller-supplied caret being accurate. Apps
/// that report wrong AX selection state (Monaco reports `[N,N]` even
/// when text is visibly selected) defeat the BS-storm — the first BS
/// consumes the hidden selection and the rest delete surrounding
/// text. The pickup call site documents this as a known limitation.
/// Delete `count` graphemes with Backspace, then type `text` in their place.
/// The clipboard-fallback swap shared by Esc-undo and the cycle gesture —
/// neither has an AX handle, so both revert/advance the committed text by
/// driving synthetic keystrokes. `count` must be a grapheme-cluster count
/// (what one Backspace eats in most apps), i.e. `String.count`, not
/// `.utf16.count`: a supplementary-plane char (emoji = 1 grapheme, 2 UTF-16
/// units) would otherwise fire an extra Backspace and eat a neighbour.
func replaceByBackspaceRetype(deleting count: Int, insert text: String, preserveClipboard: Bool = false) {
    for _ in 0..<count { postKey(kVK_Backspace) }
    if preserveClipboard {
        pastePreservingClipboard(text)
    } else {
        postUnicode(text)
    }
}

func keystrokeReplaceSpan(
    caret: (start: Int, end: Int),
    spanEnd: Int,
    spanLen: Int,
    replacement: String
) {
    if caret.start != caret.end {
        postKey(kVK_RightArrow)
    }
    let caretAfterCollapse = (caret.start != caret.end) ? caret.end : caret.start
    let toMove = spanEnd - caretAfterCollapse
    if toMove > 0 {
        for _ in 0..<toMove { postKey(kVK_RightArrow) }
    } else if toMove < 0 {
        for _ in 0..<(-toMove) { postKey(kVK_LeftArrow) }
    }
    for _ in 0..<spanLen { postKey(kVK_Backspace) }
    postUnicode(replacement)
}
