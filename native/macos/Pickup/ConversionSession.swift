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

extension ConversionBacking {
    /// Bundle id carried by clipboard-backed sessions (the AX backing holds
    /// an element ref, not an app id — that's stamped onto the session
    /// separately). Used to key the per-app history ring / log.
    var loggingBundleId: String? {
        switch self {
        case .ax: return nil
        case .clipboard(let bundleId, _): return bundleId
        }
    }

    var loggingName: String {
        switch self {
        case .ax: return "ax"
        case .clipboard: return "clipboard"
        }
    }
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
    /// Stable per-conversion id. The history manager keys on this so a
    /// session's history entry is *overwritten* as the user cycles/undos,
    /// rather than appended — one entry per conversion, always the latest
    /// decision. Defaulted, so every construction site gets a fresh id with
    /// no signature change.
    var id = UUID()

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

    /// Frontmost app's bundle id at conversion time. Keys the per-app
    /// history ring and tags the experiment log. Stamped on the AX path
    /// after the write lands; clipboard sessions fall back to the bundle id
    /// carried in `backing`. nil when unknown.
    var appId: String? = nil

    /// Surrounding field text, for context-aware reranking experiments.
    /// Populated on the AX path (the full field value + span offsets are
    /// known there); nil on clipboard/scripted paths that lack a stable
    /// field handle. Captured for the conversion log only.
    var contextBefore: String? = nil
    var contextAfter: String? = nil

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

    /// Index that "cycle previous" should move to. Returns `nil` when
    /// there is no active candidate to move from (undone state or no
    /// candidates captured).
    func previousCandidateIndex() -> Int? {
        guard !candidates.isEmpty, candidateIndex >= 0 else { return nil }
        return (candidateIndex - 1 + candidates.count) % candidates.count
    }
}

/// Surrounding field text captured once per AX pickup (in `doPickup`, before
/// the convert branches) and consumed by `ConversionSessionStore.set` to stamp
/// the committed session — so the commit logic lives in one place, not in
/// every convert branch. nil for clipboard/scripted pickups (no field handle).
struct PendingPickupContext {
    let appId: String?
    let before: String?
    let after: String?
}

var gPendingPickupContext: PendingPickupContext? = nil

/// Single-slot store for the most recent conversion. Holds at most one
/// session at a time — every new successful conversion overwrites the
/// previous one, which is correct: cycle/undo act on "the last thing
/// modore did," not a stack of conversions.
enum ConversionSessionStore {
    private static let lock = NSLock()
    private static var current: ConversionSession? = nil

    /// Per-app history of recent conversions, keyed within each app by session
    /// id so cycling/undo *overwrites* an entry instead of appending. Holds
    /// the last `historyCap` sessions per app, each reflecting its latest
    /// decision — what a context-aware reranker reads, and what the opt-in
    /// experiment log records. Guarded by `lock`.
    private static var history: [String: [SealedConversion]] = [:]
    private static let historyCap = 5

    /// Record a fresh conversion. Stamps app id + surrounding context from the
    /// pending pickup capture, then commits it to history — one commit per
    /// conversion trigger, no per-branch wiring.
    static func set(_ session: ConversionSession) {
        var s = session
        if let pc = gPendingPickupContext {
            s.appId = s.appId ?? pc.appId
            s.contextBefore = pc.before
            s.contextAfter = pc.after
        }
        lock.lock(); defer { lock.unlock() }
        current = s
        commit(s)
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

    /// Forget the current session. The history entry was already committed at
    /// conversion time (and overwritten on each cycle/undo), so clearing the
    /// live slot doesn't lose it.
    static func clear() {
        lock.lock(); defer { lock.unlock() }
        current = nil
    }

    /// Recent conversions for an app (oldest-first), for reranker use. Returns
    /// a snapshot copy; safe to read off the lock afterward.
    static func recentHistory(appId: String?) -> [SealedConversion] {
        lock.lock(); defer { lock.unlock() }
        return history[appId ?? ""] ?? []
    }

    /// The live session without the staleness check — used right after `set`
    /// to read back the app id + context that `set` stamped on. (`peek`
    /// couples to the undo window; this doesn't.) nil if nothing is committed.
    static func currentSnapshot() -> ConversionSession? {
        lock.lock(); defer { lock.unlock() }
        return current
    }

    /// Mutate the session in place under the lock, then re-commit it — Esc-undo
    /// (`candidateIndex = -1`) and cycle (advance `candidateIndex`) change the
    /// chosen candidate, so the history entry for this session id is
    /// overwritten with the new decision. No-op when no session is set.
    static func update(_ mutator: (inout ConversionSession) -> Void) {
        lock.lock(); defer { lock.unlock() }
        guard var snap = current else { return }
        mutator(&snap)
        current = snap
        commit(snap)
    }

    /// Write the session's current decision into the per-app history ring and
    /// (when enabled) the experiment log, keyed by session id: an existing
    /// entry for this id is overwritten, otherwise it's appended and the ring
    /// trimmed to the last `historyCap`. The decided index is whatever the
    /// session holds right now — Mozc's top on first commit, the cycled/undone
    /// choice on later commits.
    ///
    /// MUST be called with `lock` held — NSLock is not reentrant, so this
    /// touches `history` directly and never re-locks. `ConversionLog.append`
    /// only enqueues onto its own queue, so calling it here doesn't block.
    private static func commit(_ session: ConversionSession) {
        let appId = session.appId ?? session.backing.loggingBundleId
        let record = SealedConversion(
            id: session.id.uuidString,
            ts: Int64(session.timestamp.timeIntervalSince1970 * 1000),
            appId: appId,
            reading: session.originalReading,
            candidates: session.candidates.map { $0.value },
            mozcTopIdx: 0,                          // candidates[0] is Mozc's top
            decidedIdx: session.candidateIndex,     // -1 = undo
            decidedValue: session.currentText,
            contextBefore: session.contextBefore,
            contextAfter: session.contextAfter,
            backing: session.backing.loggingName)
        let key = appId ?? ""
        var ring = history[key] ?? []
        if let i = ring.firstIndex(where: { $0.id == record.id }) {
            ring[i] = record
        } else {
            ring.append(record)
            if ring.count > historyCap { ring.removeFirst(ring.count - historyCap) }
        }
        history[key] = ring
        ConversionLog.append(record)
    }
}
