// Low-level synthetic key/Unicode injection. Used by the clipboard fallback
// in `Pickup.swift` (Cmd+C, Shift+Opt+Left, postUnicode of the replacement)
// and any future feature that needs to drive the keyboard from inside the
// host. Pure side-effect functions — no state of their own.

import Cocoa

// MARK: - Virtual key codes

let kVK_ANSI_C: CGKeyCode = 0x08
let kVK_ANSI_V: CGKeyCode = 0x09
let kVK_LeftArrow: CGKeyCode = 0x7B
let kVK_RightArrow: CGKeyCode = 0x7C
/// What the user calls "Backspace" — Apple's main-keyboard Delete key.
/// (`kVK_ForwardDelete = 0x75` is the separate forward-delete key.)
let kVK_Backspace: CGKeyCode = 0x33

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
    let src = makeSyntheticSource()
    if let down = CGEvent(keyboardEventSource: src, virtualKey: keyCode, keyDown: true) {
        down.flags = flags
        markSynthetic(down)
        down.post(tap: kPostTap)
    }
    if let up = CGEvent(keyboardEventSource: src, virtualKey: keyCode, keyDown: false) {
        up.flags = flags
        markSynthetic(up)
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
            down.post(tap: kPostTap)
        }
        if let up = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: false) {
            up.flags = []
            up.keyboardSetUnicodeString(stringLength: chunk.count, unicodeString: chunk)
            markSynthetic(up)
            up.post(tap: kPostTap)
        }
        i = end
    }
}
