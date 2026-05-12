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
// Two chords are supported: a `primary` chord that triggers the normal
// conversion, and an optional `secondary` chord that triggers a variant
// (currently used for the katakana-forcing modifier). Each chord has its
// own callback. They share one event handler — the handler looks up the
// firing `EventHotKeyID` and dispatches to the matching closure.

import Carbon
import Foundation

final class CarbonHotkey {

    private let onPrimary: () -> Void
    private var onSecondary: (() -> Void)?

    private var primaryRef: EventHotKeyRef?
    private var secondaryRef: EventHotKeyRef?
    private var handlerRef: EventHandlerRef?

    private static var nextHotkeyID: UInt32 = 1
    private var primaryID: UInt32 = 0
    private var secondaryID: UInt32 = 0

    /// FourCharCode signature for our hotkeys ('modr'). Visible in tools that
    /// list system hotkeys; helps users identify what's claimed the chord.
    private static let signature: OSType = 0x6D6F6472  // 'modr'

    init(onFire: @escaping () -> Void) {
        self.onPrimary = onFire
        installHandler()
    }

    deinit {
        unregister()
        unregisterSecondary()
        if let h = handlerRef {
            RemoveEventHandler(h)
            handlerRef = nil
        }
    }

    /// Register (or re-register) the primary chord. Returns `true` on success.
    /// On failure, leaves no primary registration active; caller falls back
    /// to whatever other detector is in place. The secondary chord (if any)
    /// is unaffected — they're independent registrations.
    @discardableResult
    func register(_ chord: ModoreConfig.ConversionHotkey) -> Bool {
        if let ref = primaryRef {
            UnregisterEventHotKey(ref)
            primaryRef = nil
        }

        CarbonHotkey.nextHotkeyID &+= 1
        primaryID = CarbonHotkey.nextHotkeyID

        var ref: EventHotKeyRef?
        let id = EventHotKeyID(signature: CarbonHotkey.signature, id: primaryID)
        let status = RegisterEventHotKey(
            UInt32(chord.keyCode),
            chord.carbonModifierMask,
            id,
            GetApplicationEventTarget(),
            0,
            &ref
        )
        if status == noErr, let ref = ref {
            primaryRef = ref
            return true
        }
        return false
    }

    /// Register a secondary chord with its own callback. Calling this with
    /// `chord == nil` (or before `register(_:)`) tears down any existing
    /// secondary registration. Returns `true` on successful registration.
    ///
    /// The secondary callback is held by the instance for the duration of the
    /// registration; replacing or clearing the secondary releases the prior
    /// closure.
    @discardableResult
    func registerSecondary(
        _ chord: ModoreConfig.ConversionHotkey?,
        onFire: (() -> Void)? = nil
    ) -> Bool {
        unregisterSecondary()
        guard let chord = chord, let onFire = onFire else {
            return false
        }

        CarbonHotkey.nextHotkeyID &+= 1
        secondaryID = CarbonHotkey.nextHotkeyID

        var ref: EventHotKeyRef?
        let id = EventHotKeyID(signature: CarbonHotkey.signature, id: secondaryID)
        let status = RegisterEventHotKey(
            UInt32(chord.keyCode),
            chord.carbonModifierMask,
            id,
            GetApplicationEventTarget(),
            0,
            &ref
        )
        if status == noErr, let ref = ref {
            secondaryRef = ref
            onSecondary = onFire
            return true
        }
        // Failed: leak no closure ref, leave the slot disarmed.
        secondaryID = 0
        onSecondary = nil
        return false
    }

    func unregister() {
        if let ref = primaryRef {
            UnregisterEventHotKey(ref)
            primaryRef = nil
        }
        primaryID = 0
    }

    func unregisterSecondary() {
        if let ref = secondaryRef {
            UnregisterEventHotKey(ref)
            secondaryRef = nil
        }
        secondaryID = 0
        onSecondary = nil
    }

    /// Called from the Carbon event handler with the firing hotkey's id.
    /// Dispatches to `onPrimary` or `onSecondary`. Unknown ids (which would
    /// happen if a stale event arrives after a re-register) are dropped
    /// silently — they're not actionable.
    fileprivate func dispatch(hotkeyID: UInt32) {
        if hotkeyID == primaryID {
            onPrimary()
        } else if hotkeyID == secondaryID, let cb = onSecondary {
            cb()
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
                // Without an id we can't tell primary from secondary; fall
                // back to primary so the common chord still fires.
                me.dispatch(hotkeyID: me.primaryID)
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
