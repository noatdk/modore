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

    // When Carbon owns the hotkey the OS consumes the keystroke before we
    // see it here, so this branch is unreachable in the common case. We
    // still gate defensively in case that ever stops being true (e.g.
    // newer macOS versions changing tap ordering).
    if !gUsingCarbonHotkey {
        let keyCode = CGKeyCode(event.getIntegerValueField(.keyboardEventKeycode))
        let coreFlags = event.flags.intersection([
            .maskCommand, .maskShift, .maskControl, .maskAlternate
        ])

        if keyCode == gConversionKeyCode {
            if coreFlags == gConversionCoreFlags {
                kHotkeyTapQueue.async { doPickup() }
                return nil // swallow — host app must not see the "/"
            }
            // Same key + the configured katakana modifier (Shift). Same
            // tap-fallback shape: swallow and dispatch with the katakana
            // target set on the request.
            if let secondary = gKatakanaChordFlags, coreFlags == secondary {
                kHotkeyTapQueue.async {
                    doPickup(PickupRequest(target: .katakana))
                }
                return nil
            }
        }
    }

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
