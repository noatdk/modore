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

private enum CycleDirection {
    case next
    case previous
}

private struct CycleTransition {
    let session: ConversionSession
    let from: String
    let to: String
}

/// Worker-queue entry point for the dedicated cycle chord (e.g. when
/// `[conversion] cycle_modifier` binds Alt on top of the conversion
/// hotkey). Always logs a reason on no-op so a triage thread can tell
/// why a press didn't cycle.
func performCycleNext() {
    if !cycleNext(verbose: true) {
        // No-op was already explained in the helper's log lines.
    }
}

/// Worker-queue entry point for the reverse cycle gesture. Used by the
/// katakana chord when the active config says it should step backward
/// through candidates while a session is live.
func performCyclePrevious() {
    if !cyclePrevious(verbose: true) {
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
    switch snap {
    case .single(let session):
        return cycleNext(single: session, verbose: verbose)
    case .batch(let session):
        return cycleBatch(session, direction: .next, verbose: verbose)
    }
}

/// Reverse cycle the active session. Mirrors `cycleNext` but steps to
/// the previous candidate. Used by the katakana chord when the config
/// opts into "cycle backwards on active session."
@discardableResult
func cyclePrevious(verbose: Bool) -> Bool {
    guard let snap = ConversionSessionStore.peek(windowMs: gUndoWindowMs) else {
        if verbose {
            Log.cycle("no session in scope; nothing to cycle backward\(FrontmostApp.logSuffix())")
        }
        return false
    }
    switch snap {
    case .single(let session):
        return cyclePrevious(single: session, verbose: verbose)
    case .batch(let session):
        return cycleBatch(session, direction: .previous, verbose: verbose)
    }
}

@discardableResult
private func cycleNext(single snap: ConversionSession, verbose: Bool) -> Bool {
    guard let transition = makeCycleTransition(snap, direction: .next, verbose: verbose) else {
        return false
    }
    guard swapCurrentText(
        backing: snap.backing,
        currentText: transition.from,
        replacement: transition.to,
        verbose: verbose
    ) else {
        return false
    }
    ConversionSessionStore.set(transition.session)
    let totalN = snap.candidates.count
    Log.cycle("'\(transition.from)' → '\(transition.to)' (\(transition.session.candidateIndex + 1)/\(totalN))")
    if gCandidatePanelMode != .none {
        CandidatePanel.shared.show(session: transition.session)
    }
    return true
}

@discardableResult
private func cyclePrevious(single snap: ConversionSession, verbose: Bool) -> Bool {
    guard let transition = makeCycleTransition(snap, direction: .previous, verbose: verbose) else {
        return false
    }
    guard swapCurrentText(
        backing: snap.backing,
        currentText: transition.from,
        replacement: transition.to,
        verbose: verbose
    ) else {
        return false
    }
    ConversionSessionStore.set(transition.session)
    let totalN = snap.candidates.count
    Log.cycle("'\(transition.from)' ← '\(transition.to)' (\(transition.session.candidateIndex + 1)/\(totalN))")
    if gCandidatePanelMode != .none {
        CandidatePanel.shared.show(session: transition.session)
    }
    return true
}

@discardableResult
private func cycleBatch(_ snap: BatchConversionSession, direction: CycleDirection, verbose: Bool) -> Bool {
    let from = snap.currentText
    var updated = snap
    var anyChanged = false
    for idx in updated.items.indices {
        guard let transition = makeCycleTransition(updated.items[idx], direction: direction, verbose: verbose) else {
            continue
        }
        Log.cycle("batch item[\(idx)] '\(transition.from)' \(direction == .next ? "→" : "←") '\(transition.to)'")
        updated.items[idx] = transition.session
        anyChanged = true
    }
    guard anyChanged else {
        Log.cycle("batch session has no candidates to cycle \(direction == .next ? "through" : "backward through")")
        return false
    }

    let to = updated.currentText
    guard swapCurrentText(
        backing: snap.backing,
        currentText: from,
        replacement: to,
        verbose: verbose
    ) else {
        return false
    }
    updated.timestamp = Date()
    ConversionSessionStore.set(updated)
    Log.cycle("batch '\(from)' \(direction == .next ? "→" : "←") '\(to)' (\(updated.items.count) items)")
    CandidatePanel.shared.hide()
    return true
}

private func makeCycleTransition(
    _ snap: ConversionSession,
    direction: CycleDirection,
    verbose: Bool
) -> CycleTransition? {
    switch direction {
    case .next:
        if snap.candidateIndex < 0 && gCycleFromUndone == .pass {
            Log.cycle("session is in undone state; cycle_from_undone=pass — no-op")
            return nil
        }
        guard let nextIndex = snap.nextCandidateIndex() else {
            Log.cycle("session has no candidates to cycle through (single-result conversion)")
            return nil
        }
        var updated = snap
        updated.candidateIndex = nextIndex
        updated.timestamp = Date()
        return CycleTransition(session: updated, from: snap.currentText, to: snap.candidates[nextIndex].value)
    case .previous:
        guard snap.candidateIndex >= 0 else {
            if verbose {
                Log.cycle("session is in undone state; katakana chord keeps katakana behavior")
            }
            return nil
        }
        guard let prevIndex = snap.previousCandidateIndex() else {
            Log.cycle("session has no candidates to cycle backward through (single-result conversion)")
            return nil
        }
        var updated = snap
        updated.candidateIndex = prevIndex
        updated.timestamp = Date()
        return CycleTransition(session: updated, from: snap.currentText, to: snap.candidates[prevIndex].value)
    }
}

func swapCurrentText(
    backing: ConversionBacking,
    currentText: String,
    replacement: String,
    verbose: Bool
) -> Bool {
    switch backing {
    case .ax(let element, let spanStart):
        return swapOnAX(
            currentText: currentText,
            element: element,
            spanStart: spanStart,
            replacement: replacement,
            verbose: verbose)
    case .clipboard(let bundleId, let pid):
        return swapOnClipboard(
            currentText: currentText,
            bundleId: bundleId,
            pid: pid,
            replacement: replacement,
            verbose: verbose)
    }
}

/// AX-backed cycle. Same validation gates as the undo path: same focused
/// element, current text at the recorded span. Refuses on any mismatch
/// rather than clobber whatever the user is now editing.
private func swapOnAX(
    currentText: String,
    element: AXUIElement,
    spanStart: Int,
    replacement next: String,
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
    let appId = FrontmostApp.describe()?.bundleID
    if field.autocomplete == "both",
       isChromiumOmnibox(field: field, appId: appId) {
        return cycleChromiumOmnibox(
            currentText: currentText,
            field: field,
            spanStart: spanStart,
            replacement: next,
            verbose: verbose)
    }
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

/// Chromium omnibox with inline autocomplete needs full-value commits on
/// cycle as well, otherwise the preserved suggestion tail keeps the search
/// model anchored to the old query until the user types another key.
///
/// spanStart is the word-start offset recorded at conversion time. The
/// converted word may not be at position 0 when the user has built a
/// multi-word omnibox query across successive conversions.
private func cycleChromiumOmnibox(
    currentText: String,
    field: FocusedField,
    spanStart: Int,
    replacement next: String,
    verbose: Bool
) -> Bool {
    let spanEnd = spanStart + currentText.utf16.count
    guard let textAtSpan = sliceUTF16(field.value, start: spanStart, end: spanEnd),
          textAtSpan == currentText else {
        if verbose {
            let actual = sliceUTF16(field.value, start: spanStart, end: min(spanEnd, field.value.utf16.count)) ?? "?"
            Log.cycle("fell through: Chromium omnibox span changed (was '\(currentText)', is '\(actual)')\(FrontmostApp.logSuffix())")
        }
        return false
    }

    let fieldPrefix = sliceUTF16(field.value, start: 0, end: spanStart) ?? ""
    let replacement = fieldPrefix + next
    let fullEnd = field.value.utf16.count
    if postUnicodeOverAXSelection(
        in: field.element,
        start: 0,
        end: fullEnd,
        replacement: replacement) {
        return true
    }
    if replaceRange(in: field.element, start: 0, end: fullEnd, replacement: replacement) {
        return true
    }
    if verbose {
        Log.cycle("Chromium omnibox: full-field cycle AX replace failed; falling back to keystroke retype\(FrontmostApp.logSuffix())")
    }
    keystrokeReplaceSpan(
        caret: (start: field.selStart, end: field.selEnd),
        spanEnd: fullEnd,
        spanLen: fullEnd,
        replacement: replacement)
    return true
}

/// Clipboard-fallback swap. Same weak gates as the clipboard-undo path
/// (frontmost app pid match) and the same backspace + retype trick.
private func swapOnClipboard(
    currentText: String,
    bundleId: String?,
    pid: pid_t,
    replacement next: String,
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
    if currentText.contains("\n") || next.contains("\n") {
        selectLeftAndPastePreservingClipboard(selecting: currentText.count, text: next)
    } else {
        replaceByBackspaceRetype(
            deleting: currentText.count,
            insert: next,
            preserveClipboard: false)
    }
    return true
}
