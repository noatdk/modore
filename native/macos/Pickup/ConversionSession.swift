// Live state of the most recent conversion, kept around for as long as
// the user might want to act on it again — Esc to undo, cycle chord to
// advance the candidate, future MRU ranking. The "session" framing
// matches the lifecycle: it begins when modore successfully writes a
// span (AX-replace or clipboard-fallback inject), persists while gates
// hold (window not elapsed, focus unchanged, expected text still in
// place where checkable), and ends when any of those gates fails.
//
// Sessions open on both the AX-fast-path and the clipboard fallback —
// the gestures (cycle, undo) work in both, with weaker gates on the
// clipboard side (no AXUIElement to compare; we can't read the text
// back). The backing enum encodes which path produced the session, and
// the gesture workers branch on it for the I/O step.
//
// Threading: written on the worker queue (`kHotkeyTapQueue`) after a
// successful pickup; read from the tap thread (peek only) and again
// from the worker before any gesture mutates the field. Behind a plain
// `NSLock` — the critical section is a single field assignment, so
// lock contention is irrelevant in practice.

import ApplicationServices
import Foundation

/// How a session was produced determines how its gestures (cycle, undo)
/// must perform I/O. The AX path can re-read and re-write the field
/// directly via the retained element ref; the clipboard fallback path
/// has no such handle and has to synthesize backspaces + Unicode injection
/// against whatever happens to have focus when the gesture fires.
enum ConversionBacking {
    /// AX-fast-path session. The element ref + the original span offset
    /// let the worker do exact, gate-validated AX replaces.
    case ax(element: AXUIElement, spanStart: Int)

    /// Clipboard-fallback session. We don't have an element ref or a
    /// stable span offset; the best we can do is verify the frontmost
    /// app's identity hasn't changed since the conversion landed, then
    /// inject backspaces + the new text. Weaker gates, best-effort
    /// undo/cycle.
    case clipboard(frontmostBundleId: String?, frontmostPid: pid_t)
}

/// One conversion's worth of state. Shared by Esc-undo and the cycle
/// gesture — both operate on the same `(span, candidates, index)`
/// triple, and Esc is just "set index to the reading-sentinel."
///
/// Index semantics:
///   `-1`  → currently showing `originalReading` (Esc-undo state).
///   `0`+  → currently showing `candidates[index]`.
///   Cycle advances `0 → 1 → … → N-1 → 0`, skipping `-1`. Esc explicitly
///   sets `-1`. Cycle from `-1` lands at `0` (so an over-eager Esc can
///   be redone by pressing the cycle chord).
struct ConversionSession {
    /// Which pickup path produced this session — determines how cycle/undo
    /// physically swap text in the focused app.
    let backing: ConversionBacking

    /// What the user typed before modore touched it. The Esc-undo target,
    /// and conceptually the `-1` position in the cycle.
    let originalReading: String

    /// Mozc's top-N candidate list. `candidates[0]` is what was first
    /// committed; cycling advances through the list. May be empty for
    /// inputs Mozc didn't offer alternatives on (rare — usually only
    /// trivial single-kana inputs), in which case cycle is a no-op.
    let candidates: [MozcBridge.Candidate]

    /// Which slot is currently committed where the conversion landed.
    /// `-1` means `originalReading` (post-Esc); `0..<candidates.count`
    /// indexes into the candidate list.
    var candidateIndex: Int

    /// When the session was last touched (initial conversion, cycle
    /// advance, or Esc-undo). Compared against `[conversion]
    /// undo_window_ms` so stale sessions auto-expire — and so cycling
    /// extends the window rather than letting the user race the clock.
    var timestamp: Date

    /// What the session believes is currently in the field at its
    /// landing position, derived from the index. The AX path's "text at
    /// span equals currentText" gate uses this; the clipboard path
    /// uses it to know how many graphemes to backspace before injecting
    /// the next candidate.
    var currentText: String {
        if candidateIndex < 0 { return originalReading }
        return candidates[candidateIndex].value
    }

    /// Index that "cycle next" should advance to. `-1` (post-Esc) snaps
    /// back to `0`; otherwise wraps `0..<count`. Returns `nil` when
    /// there's nothing to cycle to (no candidates captured).
    func nextCandidateIndex() -> Int? {
        guard !candidates.isEmpty else { return nil }
        if candidateIndex < 0 { return 0 }
        return (candidateIndex + 1) % candidates.count
    }
}

/// Single-slot store for the most recent conversion. Holds at most one
/// session at a time — every new successful conversion overwrites the
/// previous one, which is correct: cycle/undo act on "the last thing
/// modore did," not a stack of conversions.
enum ConversionSessionStore {
    private static let lock = NSLock()
    private static var current: ConversionSession? = nil

    /// Record a fresh session. Called from `doPickup` /
    /// `doClipboardPickup` after a successful write.
    static func set(_ session: ConversionSession) {
        lock.lock(); defer { lock.unlock() }
        current = session
    }

    /// Read the session if it's still within `windowMs` of its last
    /// touch. Returns `nil` and clears the slot when the window has
    /// elapsed, so callers don't have to garbage-collect expired
    /// entries. Non-destructive when still valid — Esc/cycle peek first,
    /// then `update` to mutate.
    static func peek(windowMs: Int) -> ConversionSession? {
        lock.lock(); defer { lock.unlock() }
        guard let snap = current else { return nil }
        let ageMs = Date().timeIntervalSince(snap.timestamp) * 1000
        if ageMs > Double(windowMs) {
            current = nil
            return nil
        }
        return snap
    }

    /// Forget the current session. Called explicitly by future
    /// invalidation events (focus change, etc.). Esc-undo and cycle
    /// mutate via `update` instead so the session stays alive for
    /// follow-up gestures (cycle from `-1` back to `candidates[0]`,
    /// etc.).
    static func clear() {
        lock.lock(); defer { lock.unlock() }
        current = nil
    }

    /// Mutate the session in place under the lock. Used by Esc-undo
    /// (`candidateIndex = -1`), cycle (advance `candidateIndex`), and
    /// the `timestamp` refresh both gestures do to keep the window
    /// alive across a chain of presses. No-op when no session is set.
    static func update(_ mutator: (inout ConversionSession) -> Void) {
        lock.lock(); defer { lock.unlock() }
        guard var snap = current else { return }
        mutator(&snap)
        current = snap
    }
}
