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

import Carbon
import Foundation

final class CarbonHotkey {

    private let onFire: () -> Void
    private var hotkeyRef: EventHotKeyRef?
    private var handlerRef: EventHandlerRef?
    private static var nextHotkeyID: UInt32 = 1
    private var hotkeyID: UInt32 = 0

    /// FourCharCode signature for our hotkeys ('modr'). Visible in tools that
    /// list system hotkeys; helps users identify what's claimed the chord.
    private static let signature: OSType = 0x6D6F6472  // 'modr'

    init(onFire: @escaping () -> Void) {
        self.onFire = onFire
        installHandler()
    }

    deinit {
        unregister()
        if let h = handlerRef {
            RemoveEventHandler(h)
            handlerRef = nil
        }
    }

    /// Register (or re-register) the chord. Returns `true` on success.
    /// On failure, leaves no Carbon registration active; caller falls back
    /// to whatever other detector is in place.
    @discardableResult
    func register(_ chord: ModoreConfig.ConversionHotkey) -> Bool {
        unregister()

        CarbonHotkey.nextHotkeyID &+= 1
        hotkeyID = CarbonHotkey.nextHotkeyID

        var ref: EventHotKeyRef?
        let id = EventHotKeyID(signature: CarbonHotkey.signature, id: hotkeyID)
        let status = RegisterEventHotKey(
            UInt32(chord.keyCode),
            chord.carbonModifierMask,
            id,
            GetApplicationEventTarget(),
            0,
            &ref
        )
        if status == noErr, let ref = ref {
            hotkeyRef = ref
            return true
        }
        return false
    }

    func unregister() {
        if let ref = hotkeyRef {
            UnregisterEventHotKey(ref)
            hotkeyRef = nil
        }
    }

    private func installHandler() {
        var spec = EventTypeSpec(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyPressed)
        )

        let context = Unmanaged.passUnretained(self).toOpaque()
        let callback: EventHandlerUPP = { _, _, userData in
            guard let userData = userData else { return noErr }
            let me = Unmanaged<CarbonHotkey>.fromOpaque(userData).takeUnretainedValue()
            me.onFire()
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
