// Cycle-next gesture for the active ConversionSession. Dedicated chord
// (configured via `[conversion] cycle_modifier`) advances the committed
// text from the current candidate to the next slot in the list, wrapping
// at the end. From the undone state (candidateIndex == -1) the gesture
// can either snap back to candidates[0] (redo, default) or pass through
// — see `[conversion] cycle_from_undone`.
//
// The chord is registered with Carbon so the focused app never sees it.
// "No session in scope" → no-op + log line; there is nothing useful to
// pass through to the app.
//
// I/O branches the same way Undo.swift does:
//   - .ax(element, spanStart) → readFocusedField + replaceRange
//   - .clipboard(bundleId, pid) → backspaces + postUnicode(next candidate)

import ApplicationServices
import Cocoa

/// Worker-queue entry point for the dedicated cycle chord (e.g. when
/// `[conversion] cycle_modifier` binds Alt on top of the conversion
/// hotkey). Always logs a reason on no-op so a triage thread can tell
/// why a press didn't cycle.
func performCycleNext() {
    if !cycleNext(verbose: true) {
        // No-op was already explained in the helper's log lines.
    }
}

/// Attempt to cycle the active session. Returns `true` when the swap
/// actually happened — used by `doPickup` to decide between cycling on
/// the primary chord (when a session is active) and falling through to
/// fresh convert (when not, or when gates fail).
///
/// `verbose=true` matches the dedicated-cycle-chord UX: every no-op
/// path logs a `[cycle]` line so the user can see *why* the press
/// didn't do anything. `verbose=false` is the primary-chord path —
/// no-ops there are normal (most presses are fresh conversions) and
/// would just be noise.
@discardableResult
func cycleNext(verbose: Bool) -> Bool {
    guard let snap = ConversionSessionStore.peek(windowMs: gUndoWindowMs) else {
        // Silent on the primary-chord path — most presses are fresh
        // conversions with no session in scope; logging here would be
        // noise. The dedicated-cycle-chord (verbose=true) does log so
        // the user can see why a press didn't cycle.
        if verbose {
            Log.cycle("no session in scope; nothing to cycle\(FrontmostApp.logSuffix())")
        }
        return false
    }

    // Past this point a session *is* in scope, so any no-op is a real
    // failure we want surfaced even on the primary-chord path. Without
    // this, single-key cycle silently degrades to fresh-convert and the
    // user only sees "stuck on first candidate."
    if snap.candidateIndex < 0 && gCycleFromUndone == .pass {
        Log.cycle("session is in undone state; cycle_from_undone=pass — no-op")
        return false
    }

    guard let nextIndex = snap.nextCandidateIndex() else {
        Log.cycle("session has no candidates to cycle through (single-result conversion)")
        return false
    }

    let from = snap.currentText
    let to = (nextIndex < snap.candidates.count) ? snap.candidates[nextIndex].value : from

    // Session-in-scope failures (focus changed, span shifted, frontmost
    // app changed) are always logged — see comment above the
    // candidate-index gates.
    let swapped: Bool
    switch snap.backing {
    case .ax(let element, let spanStart):
        swapped = cycleOnAX(snap, element: element, spanStart: spanStart, to: to, verbose: true)
    case .clipboard(let bundleId, let pid):
        swapped = cycleOnClipboard(snap, bundleId: bundleId, pid: pid, to: to, verbose: true)
    }
    if !swapped { return false }

    var refreshed: ConversionSession? = nil
    ConversionSessionStore.update { session in
        session.candidateIndex = nextIndex
        session.timestamp = Date()
        refreshed = session
    }
    let totalN = snap.candidates.count
    Log.cycle("'\(from)' → '\(to)' (\(nextIndex + 1)/\(totalN))")
    // Both panel modes show on cycle (on_cycle = "first cycle reveals";
    // on_convert was already visible from the convert path). show() is
    // idempotent — repeated calls during a cycle chain just refresh the
    // highlighted row.
    if gCandidatePanelMode != .none, let session = refreshed {
        CandidatePanel.shared.show(session: session)
    }
    return true
}

/// AX-backed cycle. Same validation gates as the undo path: same focused
/// element, current text at the recorded span. Refuses on any mismatch
/// rather than clobber whatever the user is now editing.
private func cycleOnAX(
    _ snap: ConversionSession,
    element: AXUIElement,
    spanStart: Int,
    to next: String,
    verbose: Bool
) -> Bool {
    guard let field = readFocusedField() else {
        if verbose { Log.cycle("fell through: focused field unreadable") }
        return false
    }
    if !CFEqual(field.element, element) {
        if verbose { Log.cycle("fell through: focus changed since conversion\(FrontmostApp.logSuffix())") }
        return false
    }
    let currentText = snap.currentText
    let currentLen = currentText.utf16.count
    let spanEnd = spanStart + currentLen
    guard let textAtSpan = sliceUTF16(field.value, start: spanStart, end: spanEnd) else {
        if verbose { Log.cycle("fell through: span out of bounds (field shrunk?)") }
        return false
    }
    if textAtSpan != currentText {
        if verbose { Log.cycle("fell through: text at span changed (was '\(currentText)', is '\(textAtSpan)')") }
        return false
    }
    if !replaceRange(in: element, start: spanStart, end: spanEnd, replacement: next) {
        if verbose { Log.cycle("AX replace failed\(FrontmostApp.logSuffix())") }
        return false
    }
    return true
}

/// Clipboard-fallback cycle. Same weak gates as the clipboard-undo path
/// (frontmost app pid match) and the same backspace + retype trick. The
/// risk is identical: if the user typed extra characters after the
/// conversion, our backspaces eat them. In practice the
/// "any-non-chord-keystroke clears the session" rule in the tap callback
/// rules out the typed-after case for the primary-chord cycle path.
private func cycleOnClipboard(
    _ snap: ConversionSession,
    bundleId: String?,
    pid: pid_t,
    to next: String,
    verbose: Bool
) -> Bool {
    guard let frontmost = FrontmostApp.describe() else {
        if verbose { Log.cycle("fell through: no frontmost app") }
        return false
    }
    if frontmost.pid != pid {
        if verbose { Log.cycle("fell through: frontmost app changed since conversion (was pid \(pid)/\(bundleId ?? "?"), now pid \(frontmost.pid)/\(frontmost.bundleID))") }
        return false
    }
    let currentText = snap.currentText
    let backspaces = currentText.count
    for _ in 0..<backspaces {
        postKey(kVK_Backspace)
    }
    postUnicode(next)
    return true
}
