// Global hotkey via CGEventTap, kept as a backup for Carbon's
// `RegisterEventHotKey` (see CarbonHotkey.swift). Primary delivery is Carbon;
// the tap exists for two purposes:
//
//   1. A fallback hotkey detector if Carbon registration fails (chord already
//      claimed by another app, etc.).
//   2. The self-event filter for synthesized CGEvents (markSynthetic /
//      isSynthetic in SyntheticEvents.swift). The marker is cheap insurance
//      against any future feature that synthesizes the conversion chord.
//
// The callback must return promptly (~1s budget before macOS disables the
// tap), so we dispatch the slow pickup work onto a background queue.
//
// Globals read by the callback:
//   - gUsingCarbonHotkey, gConversionKeyCode, gConversionCoreFlags,
//     gKatakanaChordFlags (HotkeyState.swift) — main-thread-only writers,
//     plain swap is race-free for the tap-thread reader.

import Cocoa

let kHotkeyTapQueue = DispatchQueue(label: "local.modore.pickup", qos: .userInitiated)
var gEventTap: CFMachPort?
private var gRunLoopSource: CFRunLoopSource?

let tapCallback: CGEventTapCallBack = { _, type, event, _ in
    // macOS can disable the tap under load or after long callbacks — re-enable.
    if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
        if let tap = gEventTap {
            CGEvent.tapEnable(tap: tap, enable: true)
            Log.hotkey("event tap re-enabled (was disabled: \(type.rawValue))")
        }
        return Unmanaged.passUnretained(event)
    }

    guard type == .keyDown else {
        return Unmanaged.passUnretained(event)
    }

    // Events we synthesized via `markSynthetic` pass straight through. Without
    // this, any future feature that synths a key matching the conversion
    // chord would re-enter doPickup and loop.
    if isSynthetic(event) {
        return Unmanaged.passUnretained(event)
    }

    let keyCode = CGKeyCode(event.getIntegerValueField(.keyboardEventKeycode))
    let coreFlags = event.flags.intersection([
        .maskCommand, .maskShift, .maskControl, .maskAlternate
    ])

    // Esc undo. Independent of Carbon — Carbon only grabs the conversion
    // chord, Esc is always delivered to the tap. Gate cheaply on the
    // window-enabled bool + a non-modifier keystroke + a present snapshot.
    // The worker (`performEscUndo`) does the deeper validation and
    // re-injects Esc on any fall-through so the user never sees a
    // swallowed Esc.
    if keyCode == kVK_Escape && coreFlags.isEmpty && gUndoWindowMs > 0 {
        if ConversionSessionStore.peek(windowMs: gUndoWindowMs) != nil {
            kHotkeyTapQueue.async { performEscUndo() }
            return nil
        }
    }

    // Recognize our own chords regardless of who's *dispatching* them.
    // The session tap at `.headInsertEventTap` fires before Carbon's
    // app-target handler (verified on macOS 14), so when Carbon owns
    // the chord the tap still sees the keystroke first. If we only
    // matched chords on the !gUsingCarbonHotkey path, the session-clear
    // at the bottom of this callback would wipe the live session before
    // Carbon ever gets to cycle it — that was the "stuck on first
    // candidate" bug.
    if keyCode == gConversionKeyCode {
        if coreFlags == gConversionCoreFlags {
            if !gUsingCarbonHotkey {
                kHotkeyTapQueue.async { doPickup() }
                return nil // swallow — host app must not see the "/"
            }
            // Carbon will dispatch; we just don't clear.
            return Unmanaged.passUnretained(event)
        }
        if let secondary = gKatakanaChordFlags, coreFlags == secondary {
            if !gUsingCarbonHotkey {
                kHotkeyTapQueue.async {
                    doPickup(PickupRequest(target: .katakana))
                }
                return nil
            }
            return Unmanaged.passUnretained(event)
        }
        if let cycle = gCycleChordFlags, coreFlags == cycle {
            if !gUsingCarbonHotkey {
                kHotkeyTapQueue.async { performCycleNext() }
                return nil
            }
            return Unmanaged.passUnretained(event)
        }
    }

    // Any other keyDown that reaches this point — letters being typed,
    // backspace, arrow keys, Cmd+anything, an Esc when no session is in
    // scope — ends any active conversion session. The next press of the
    // primary chord will then do a fresh conversion at whatever caret
    // position the user is now at, rather than cycling against a stale
    // span. Pure modifier presses (Shift, Ctrl, Alt, Cmd alone) fire
    // `flagsChanged` events rather than keyDown, so this rule never
    // clears the session just because the user is *about* to press the
    // cycle chord.
    ConversionSessionStore.clear()

    return Unmanaged.passUnretained(event)
}

func installEventTap() -> Bool {
    let mask = (1 << CGEventType.keyDown.rawValue)
    guard let tap = CGEvent.tapCreate(
        tap: .cgSessionEventTap,
        place: .headInsertEventTap,
        options: .defaultTap,
        eventsOfInterest: CGEventMask(mask),
        callback: tapCallback,
        userInfo: nil
    ) else {
        return false
    }
    gEventTap = tap
    let src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
    gRunLoopSource = src
    CFRunLoopAddSource(CFRunLoopGetMain(), src, .commonModes)
    CGEvent.tapEnable(tap: tap, enable: true)
    return true
}
