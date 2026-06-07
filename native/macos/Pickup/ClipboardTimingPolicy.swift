// @testable
//
// Small pure timing helpers for clipboard-backed replacement paths. Kept
// separate from the Cocoa pasteboard helpers so unit tests can cover policy
// without synthesizing events or touching the real clipboard.

/// Minimum delay before restoring the user's clipboard after a synthetic
/// Cmd+V. Paste delivery is asynchronous in several editor/rendering stacks,
/// so the normal acquisition restore delay is too short for paste-based
/// replacement paths.
private let kMinimumPasteRestoreDelayMs = 750

func clipboardPasteRestoreDelayMs(configuredMs: Int) -> Int {
    max(kMinimumPasteRestoreDelayMs, configuredMs)
}
