// Records the most recent successful conversion so that follow-up gestures
// (Esc to undo today; future repeat-hotkey to cycle candidates, MRU
// ranking for item 3) have something to act on.
//
// Only the AX-fast-path conversions record themselves here. The clipboard
// fallback writes via `postUnicode` directly into the focused app's
// keystroke stream — there is no AXUIElement to re-target later, no span
// offset that survives intermediate user input, and no way to read back
// what got injected. Trying to undo a clipboard-fallback conversion is a
// rabbit hole; skip it for v1.
//
// Threading: written on the worker queue (`kHotkeyTapQueue`) after a
// successful `replaceRange`; read from the tap thread on Esc (peek only)
// and again from the worker thread before performing the undo. Behind a
// plain `NSLock` — the critical section is a single field assignment, so
// lock contention is irrelevant in practice.

import ApplicationServices
import Foundation

/// One conversion's worth of state. The fields cover the v1 Esc-undo path;
/// the comment below each field notes which future feature would extend
/// it. Adding a `var` here is the v2/v3 path — callers that don't set the
/// optional field keep working unchanged.
struct LastConversion {
    /// The AXUIElement the replace landed in. Retained for the lifetime
    /// of this snapshot (AXUIElement is a CFTypeRef; Swift's ARC keeps
    /// it alive via this property).
    let element: AXUIElement

    /// UTF-16 offset of the replacement's start within the field at the
    /// time of the write. Used by Esc-undo to locate the span to replace
    /// back; also the anchor for any future cycle/repeat gesture.
    let spanStart: Int

    /// What the user typed before modore touched it. The Esc-undo target.
    let originalReading: String

    /// What modore wrote. Used as the "expected text at span" check —
    /// if the user has typed more or edited the converted text, we
    /// refuse to undo rather than clobbering their edits.
    let replacement: String

    /// When the conversion landed. Compared against
    /// `[conversion] undo_window_ms` so stale snapshots auto-expire.
    let timestamp: Date

    // Future extensions (items 1 / 3 in the conversion-UX backlog):
    //   var candidates: [String]? = nil   // top-N from Mozc
    //   var candidateIndex: Int = 0       // which one is currently committed
}

/// Single-slot store for the most recent conversion. Holds at most one
/// snapshot at a time — every new successful conversion overwrites the
/// previous one, which is correct: Esc undoes "the last thing modore did,"
/// not a stack of conversions.
enum LastConversionStore {
    private static let lock = NSLock()
    private static var current: LastConversion? = nil

    /// Record the snapshot. Called from `doPickup` after a successful
    /// `replaceRange`.
    static func set(_ snapshot: LastConversion) {
        lock.lock(); defer { lock.unlock() }
        current = snapshot
    }

    /// Read the snapshot if it's still within `windowMs` of being written.
    /// Returns `nil` and clears the slot when the window has elapsed, so
    /// callers don't have to remember to garbage-collect expired entries.
    /// Non-destructive when the snapshot is still valid — Esc handling
    /// peeks first, then clears explicitly only after a successful undo.
    static func peek(windowMs: Int) -> LastConversion? {
        lock.lock(); defer { lock.unlock() }
        guard let snap = current else { return nil }
        let ageMs = Date().timeIntervalSince(snap.timestamp) * 1000
        if ageMs > Double(windowMs) {
            current = nil
            return nil
        }
        return snap
    }

    /// Forget the current snapshot. Called after a successful undo so
    /// the user can't double-Esc into chaos, and could be called by
    /// future invalidation events (focus change, etc.) if needed.
    static func clear() {
        lock.lock(); defer { lock.unlock() }
        current = nil
    }
}
