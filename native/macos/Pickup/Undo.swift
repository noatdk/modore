// Esc-to-undo for the most recent AX-path conversion.
//
// The tap callback consumes Esc (returns nil) when a recent snapshot exists,
// then dispatches `performEscUndo` to `kHotkeyTapQueue` for the AX work.
// This file owns that worker side: the validation gate, the AX-replace back
// to the original reading, and the Esc re-injection used when the snapshot
// turns out to be stale by the time the worker runs.
//
// Why re-inject Esc on the fall-through path:
//   The tap has already consumed the user's keystroke by the time we get
//   here, so if we decide *not* to undo (caret moved, text edited, focus
//   changed, snapshot just expired) we have to put the Esc back into the
//   focused app or the user gets a silently-swallowed Esc — worse than no
//   feature at all. The re-injected event carries our synthetic marker, so
//   the same tap will not re-enter this path on the way out.

import ApplicationServices
import Cocoa

/// `kVK_Escape` (0x35) — Carbon constant repeated here so this file does
/// not have to import Carbon for one number.
let kVK_Escape: CGKeyCode = 0x35

/// Worker-queue entry point. The tap callback has already consumed the
/// user's Esc; this function decides whether to revert the conversion or
/// re-inject the Esc so the focused app still gets it.
func performEscUndo() {
    // Re-check the window on the worker thread. The tap saw a valid
    // snapshot when it consumed the Esc, but the worker might run after a
    // long pause (system under load), in which case the window may have
    // closed. Reading fresh keeps the worker's notion of "still valid"
    // honest.
    guard let snap = LastConversionStore.peek(windowMs: gUndoWindowMs) else {
        Log.undo("esc fell through: no snapshot or window expired between tap and worker\(FrontmostApp.logSuffix())")
        reinjectEscape()
        return
    }

    // Re-read the focused field. We compare against the snapshot's element
    // and span to make sure the user hasn't moved focus, deleted, or
    // typed over the converted text in the intervening milliseconds.
    guard let field = readFocusedField() else {
        Log.undo("esc fell through: focused field unreadable")
        reinjectEscape()
        return
    }

    // Focus check: AXUIElement supports CFEqual semantics — two refs to
    // the same logical element compare equal even when the CFTypeRef
    // pointer differs. If focus moved (different field, different app),
    // refuse to undo and pass the Esc through to whoever has focus now.
    if !CFEqual(field.element, snap.element) {
        Log.undo("esc fell through: focus changed since conversion\(FrontmostApp.logSuffix())")
        reinjectEscape()
        return
    }

    // Text check: the recorded span in the current field must still
    // contain exactly the replacement we wrote. If the user has edited
    // it (typed more, deleted, autocorrect kicked in, …) the offsets are
    // no longer reliable and we'd risk clobbering their edits.
    let replLen = snap.replacement.utf16.count
    let spanEnd = snap.spanStart + replLen
    guard let textAtSpan = sliceUTF16(field.value, start: snap.spanStart, end: spanEnd) else {
        Log.undo("esc fell through: span out of bounds (field shrunk?)")
        reinjectEscape()
        return
    }
    if textAtSpan != snap.replacement {
        Log.undo("esc fell through: text at span changed (was '\(snap.replacement)', is '\(textAtSpan)')")
        reinjectEscape()
        return
    }

    // All gates passed: replace the replacement back with the original
    // reading. Clear the snapshot so a second Esc passes through to the
    // app (rather than re-undoing what's already been undone).
    if replaceRange(in: snap.element, start: snap.spanStart, end: spanEnd, replacement: snap.originalReading) {
        let ageMs = Int(Date().timeIntervalSince(snap.timestamp) * 1000)
        Log.undo("reverted '\(snap.replacement)' → '\(snap.originalReading)' after \(ageMs)ms")
        LastConversionStore.clear()
    } else {
        Log.undo("AX replace-back failed; falling through Esc\(FrontmostApp.logSuffix())")
        reinjectEscape()
    }
}

/// Synthesize a bare Esc keystroke and post it into the session event tap.
/// The synthetic marker on the event keeps our own tap from re-entering
/// `performEscUndo`. Used on every Esc-undo fall-through path so the user
/// never sees a "swallowed Esc."
private func reinjectEscape() {
    postKey(kVK_Escape)
}
