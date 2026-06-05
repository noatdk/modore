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

/// Worker-queue entry point. The tap callback has already consumed the
/// user's Esc; this function decides whether to revert the conversion or
/// re-inject the Esc so the focused app still gets it.
func performEscUndo() {
    guard let snap = ConversionSessionStore.peek(windowMs: gUndoWindowMs) else {
        Log.undo("esc fell through: no session or window expired between tap and worker\(FrontmostApp.logSuffix())")
        reinjectEscape()
        return
    }

    let swapped: Bool
    switch snap {
    case .single(let session):
        guard session.candidateIndex >= 0 else {
            Log.undo("esc fell through: session already in undone state")
            reinjectEscape()
            return
        }
        swapped = undoSingle(session)
    case .batch(let session):
        guard session.currentText != session.originalReading else {
            Log.undo("esc fell through: batch session already in undone state")
            reinjectEscape()
            return
        }
        swapped = undoBatch(session)
    }
    if !swapped {
        reinjectEscape()
        return
    }
    switch snap {
    case .single(var live):
        live.candidateIndex = -1
        live.timestamp = Date()
        ConversionSessionStore.set(live)
    case .batch(var live):
        for idx in live.items.indices {
            live.items[idx].candidateIndex = -1
            live.items[idx].timestamp = Date()
        }
        live.timestamp = Date()
        ConversionSessionStore.set(live)
    }
    // Esc means "undo, I'm done with this conversion" — drop the
    // candidate panel immediately so the user gets unambiguous feedback
    // that the gesture took effect. Cycling afterwards still re-shows.
    CandidatePanel.shared.hide()
}

private func undoSingle(_ snap: ConversionSession) -> Bool {
    // Backing-specific validate + swap. AX path can read the field and
    // refuse if anything moved; clipboard path can only verify the
    // frontmost app is still the same and trust the backspace count.
    switch snap.backing {
    case .ax(let element, let spanStart):
        return undoOnAX(snap, element: element, spanStart: spanStart)
    case .clipboard(let bundleId, let pid):
        return undoOnClipboard(snap, bundleId: bundleId, pid: pid)
    }
}

private func undoBatch(_ snap: BatchConversionSession) -> Bool {
    switch snap.backing {
    case .clipboard(let bundleId, let pid):
        guard let frontmost = FrontmostApp.describe() else {
            Log.undo("esc fell through: no frontmost app to undo into")
            return false
        }
        if frontmost.pid != pid {
            Log.undo("esc fell through: frontmost app changed since conversion (was pid \(pid)/\(bundleId ?? "?"), now pid \(frontmost.pid)/\(frontmost.bundleID))")
            return false
        }
        if snap.currentText.contains("\n") || snap.originalReading.contains("\n") {
            selectLeftAndPastePreservingClipboard(
                selecting: snap.currentText.count,
                text: snap.originalReading)
        } else {
            guard swapCurrentText(
                backing: snap.backing,
                currentText: snap.currentText,
                replacement: snap.originalReading,
                verbose: false
            ) else {
                return false
            }
        }
        let ageMs = Int(Date().timeIntervalSince(snap.timestamp) * 1000)
        let backspaces = snap.currentText.count
        Log.undo("reverted batch '\(snap.currentText)' → '\(snap.originalReading)' after \(ageMs)ms (clipboard, \(backspaces) backspaces, \(snap.items.count) items)")
        return true
    case .ax:
        Log.undo("batch session on AX backing is unsupported")
        return false
    }
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
    replaceByBackspaceRetype(
        deleting: backspaces,
        insert: snap.originalReading,
        preserveClipboard: currentText.contains("\n") || snap.originalReading.contains("\n"))
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
