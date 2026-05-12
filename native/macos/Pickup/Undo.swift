// Esc-to-undo for the active ConversionSession.
//
// The tap callback consumes Esc (returns nil) when a recent session exists,
// then dispatches `performEscUndo` to `kHotkeyTapQueue` for the I/O.
// This file owns that worker side: backing-specific validation gates, the
// I/O that reverts the field to the original reading, and the Esc
// re-injection used when the session turns out to be stale by the time
// the worker runs.
//
// Undo is just a session mutation: set `candidateIndex = -1`, swap the
// committed text for `originalReading`. The session stays alive afterwards
// so the cycle gesture (cycle from -1 → 0) can redo if the user changed
// their mind.
//
// Why re-inject Esc on every fall-through:
//   The tap has already consumed the user's keystroke by the time we get
//   here, so if we decide *not* to undo we have to put the Esc back into
//   the focused app or the user gets a silently-swallowed Esc — worse
//   than no feature at all. The re-injected event carries our synthetic
//   marker, so the same tap will not re-enter this path on the way out.

import ApplicationServices
import Cocoa

/// `kVK_Escape` (0x35) — Carbon constant repeated here so this file does
/// not have to import Carbon for one number.
let kVK_Escape: CGKeyCode = 0x35

/// Worker-queue entry point. The tap callback has already consumed the
/// user's Esc; this function decides whether to revert the conversion or
/// re-inject the Esc so the focused app still gets it.
func performEscUndo() {
    guard let snap = ConversionSessionStore.peek(windowMs: gUndoWindowMs) else {
        Log.undo("esc fell through: no session or window expired between tap and worker\(FrontmostApp.logSuffix())")
        reinjectEscape()
        return
    }

    if snap.candidateIndex < 0 {
        Log.undo("esc fell through: session already in undone state")
        reinjectEscape()
        return
    }

    // Backing-specific validate + swap. AX path can read the field and
    // refuse if anything moved; clipboard path can only verify the
    // frontmost app is still the same and trust the backspace count.
    let swapped: Bool
    switch snap.backing {
    case .ax(let element, let spanStart):
        swapped = undoOnAX(snap, element: element, spanStart: spanStart)
    case .clipboard(let bundleId, let pid):
        swapped = undoOnClipboard(snap, bundleId: bundleId, pid: pid)
    }
    if !swapped {
        reinjectEscape()
        return
    }
    ConversionSessionStore.update { session in
        session.candidateIndex = -1
        session.timestamp = Date()
    }
    // Esc means "undo, I'm done with this conversion" — drop the
    // candidate panel immediately so the user gets unambiguous feedback
    // that the gesture took effect. Cycling afterwards still re-shows.
    CandidatePanel.shared.hide()
}

/// AX-backed undo. Validates exactly: same focused element, text at the
/// recorded span still equals what the session committed. If anything
/// shifted (focus moved, user edited the text), refuses and the caller
/// re-injects Esc.
private func undoOnAX(
    _ snap: ConversionSession,
    element: AXUIElement,
    spanStart: Int
) -> Bool {
    guard let field = readFocusedField() else {
        Log.undo("esc fell through: focused field unreadable")
        return false
    }
    if !CFEqual(field.element, element) {
        Log.undo("esc fell through: focus changed since conversion\(FrontmostApp.logSuffix())")
        return false
    }
    let currentText = snap.currentText
    let currentLen = currentText.utf16.count
    let spanEnd = spanStart + currentLen
    guard let textAtSpan = sliceUTF16(field.value, start: spanStart, end: spanEnd) else {
        Log.undo("esc fell through: span out of bounds (field shrunk?)")
        return false
    }
    if textAtSpan != currentText {
        Log.undo("esc fell through: text at span changed (was '\(currentText)', is '\(textAtSpan)')")
        return false
    }
    if !replaceRange(in: element, start: spanStart, end: spanEnd, replacement: snap.originalReading) {
        Log.undo("AX replace-back failed; falling through Esc\(FrontmostApp.logSuffix())")
        return false
    }
    let ageMs = Int(Date().timeIntervalSince(snap.timestamp) * 1000)
    Log.undo("reverted '\(currentText)' → '\(snap.originalReading)' after \(ageMs)ms")
    return true
}

/// Clipboard-fallback undo. No AX to read with, so the gates are weaker:
/// just "same frontmost app pid as when we injected." Synthesizes
/// `currentText.count` backspaces (grapheme count — what BackSpace eats
/// in most apps) and types the original reading back via `postUnicode`.
///
/// Best-effort: if the user typed extra characters or moved the caret
/// after the conversion, the backspaces will eat their typing. Acceptable
/// trade-off — users almost always press Esc immediately after seeing the
/// wrong conversion, and the alternative is "Esc-undo doesn't work in
/// Electron apps" which is its own confusing footgun.
private func undoOnClipboard(
    _ snap: ConversionSession,
    bundleId: String?,
    pid: pid_t
) -> Bool {
    guard let frontmost = FrontmostApp.describe() else {
        Log.undo("esc fell through: no frontmost app to undo into")
        return false
    }
    if frontmost.pid != pid {
        Log.undo("esc fell through: frontmost app changed since conversion (was pid \(pid)/\(bundleId ?? "?"), now pid \(frontmost.pid)/\(frontmost.bundleID))")
        return false
    }
    let currentText = snap.currentText
    let backspaces = currentText.count
    for _ in 0..<backspaces {
        postKey(kVK_Backspace)
    }
    postUnicode(snap.originalReading)
    let ageMs = Int(Date().timeIntervalSince(snap.timestamp) * 1000)
    Log.undo("reverted '\(currentText)' → '\(snap.originalReading)' after \(ageMs)ms (clipboard, \(backspaces) backspaces)")
    return true
}

/// Synthesize a bare Esc keystroke and post it into the session event tap.
/// The synthetic marker on the event keeps our own tap from re-entering
/// `performEscUndo`. Used on every Esc-undo fall-through path so the user
/// never sees a "swallowed Esc."
private func reinjectEscape() {
    postKey(kVK_Escape)
}
