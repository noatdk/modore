// Carbon-level hotkey grab via RegisterEventHotKey, used in addition to the
// session CGEventTap. Carbon hotkeys are claimed at the OS level, which means:
//
//   * The focused app never sees the keystroke — no need for the tap callback
//     to swallow it.
//   * Delivery doesn't depend on the tap, which macOS can disable under load
//     or CPU pressure (tapDisabledByTimeout). Carbon hotkeys keep firing.
//   * No Accessibility-permission dependency for *hotkey delivery* (the AX
//     read/write paths still need it, so the user-facing requirement is
//     unchanged).
//
// We keep the tap around as a fallback: if RegisterEventHotKey fails (chord
// already claimed by another app, missing event target, etc.) we fall back
// to the tap's match-and-swallow path. If Carbon succeeds, the OS consumes
// the keystroke before the tap sees it, so there's no double-fire risk.
//
// Multiple chord slots are supported, keyed by `role` (e.g. "primary",
// "katakana", "cycle"). Each slot carries its own callback. They share
// one Carbon event handler — the handler reads the firing
// `EventHotKeyID` and dispatches to the matching closure.

import Carbon
import Foundation

final class CarbonHotkey {

    /// One registered chord. Held in `slots` until explicitly replaced
    /// or unregistered; the destructor releases the `EventHotKeyRef`.
    private struct Slot {
        let id: UInt32
        let ref: EventHotKeyRef
        let callback: () -> Void
    }

    private var slots: [String: Slot] = [:]
    private var handlerRef: EventHandlerRef?

    /// Monotonic counter for `EventHotKeyID.id`. Carbon uses this to tell
    /// us which chord fired in the shared event handler; new
    /// registrations always get a fresh id so a stale event from a
    /// just-replaced binding can't dispatch to the new callback.
    private static var nextHotkeyID: UInt32 = 1

    /// FourCharCode signature for our hotkeys ('modr'). Visible in tools that
    /// list system hotkeys; helps users identify what's claimed the chord.
    private static let signature: OSType = 0x6D6F6472  // 'modr'

    init() {
        installHandler()
    }

    deinit {
        // Remove the event handler first. Its callback dereferences `self`
        // through an unretained opaque pointer (see installHandler), so the
        // callback source must be gone before we tear down the rest of the
        // instance — otherwise an in-flight/queued kEventHotKeyPressed could
        // dispatch into a half-destroyed object. Then release the hotkeys.
        if let h = handlerRef {
            RemoveEventHandler(h)
            handlerRef = nil
        }
        for slot in slots.values {
            UnregisterEventHotKey(slot.ref)
        }
        slots.removeAll()
    }

    /// Register (or re-register) a chord under a given role name.
    /// Replaces any existing chord with the same role. Returns `true`
    /// on success — failure leaves the role with no active registration
    /// (callers can fall back to whatever other detector is in place).
    @discardableResult
    func register(
        role: String,
        chord: ModoreConfig.ConversionHotkey,
        onFire: @escaping () -> Void
    ) -> Bool {
        unregister(role: role)

        CarbonHotkey.nextHotkeyID &+= 1
        let id = CarbonHotkey.nextHotkeyID

        var ref: EventHotKeyRef?
        let eventID = EventHotKeyID(signature: CarbonHotkey.signature, id: id)
        let status = RegisterEventHotKey(
            UInt32(chord.keyCode),
            chord.carbonModifierMask,
            eventID,
            GetApplicationEventTarget(),
            0,
            &ref
        )
        guard status == noErr, let ref = ref else {
            return false
        }
        slots[role] = Slot(id: id, ref: ref, callback: onFire)
        return true
    }

    /// Tear down the chord under `role` (no-op if not registered).
    func unregister(role: String) {
        if let slot = slots.removeValue(forKey: role) {
            UnregisterEventHotKey(slot.ref)
        }
    }

    /// True if a chord is currently registered under `role`. Used by
    /// HotkeyState when deciding whether a re-register is needed.
    func isRegistered(role: String) -> Bool {
        return slots[role] != nil
    }

    /// Called from the Carbon event handler with the firing hotkey's id.
    /// Walks the slot table and fires the matching callback. Unknown ids
    /// (stale events from a just-replaced registration) are dropped
    /// silently — they're not actionable.
    fileprivate func dispatch(hotkeyID: UInt32) {
        for slot in slots.values {
            if slot.id == hotkeyID {
                slot.callback()
                return
            }
        }
    }

    private func installHandler() {
        var spec = EventTypeSpec(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyPressed)
        )

        let context = Unmanaged.passUnretained(self).toOpaque()
        let callback: EventHandlerUPP = { _, event, userData in
            guard let userData = userData else { return noErr }
            let me = Unmanaged<CarbonHotkey>.fromOpaque(userData).takeUnretainedValue()

            // Extract which hotkey fired so we can dispatch to the right
            // closure. `kEventParamDirectObject` of an `kEventHotKeyPressed`
            // event carries an `EventHotKeyID` — `id` field is the value we
            // stamped at register time.
            var firedID = EventHotKeyID(signature: 0, id: 0)
            let status = GetEventParameter(
                event,
                EventParamName(kEventParamDirectObject),
                EventParamType(typeEventHotKeyID),
                nil,
                MemoryLayout<EventHotKeyID>.size,
                nil,
                &firedID
            )
            if status != noErr {
                // No id available — drop. Without knowing which slot
                // fired we'd risk dispatching the wrong callback; better
                // to no-op than to misfire.
                return noErr
            }
            me.dispatch(hotkeyID: firedID.id)
            return noErr
        }

        InstallEventHandler(
            GetApplicationEventTarget(),
            callback,
            1,
            &spec,
            context,
            &handlerRef
        )
    }
}
