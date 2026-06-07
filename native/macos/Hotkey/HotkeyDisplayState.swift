// @testable
//
// Pure state shape for the live primary hotkey. Keeping displayName in the
// remembered state prevents modifier-only reloads from rebuilding the menu
// label from keycode/flags alone.

import Carbon

struct RememberedHotkeyState {
    let keyCode: CGKeyCode
    let coreFlags: CGEventFlags
    let displayName: String
}

func rememberedPrimaryHotkeyState(
    keyCode: CGKeyCode,
    coreFlags: CGEventFlags,
    displayName: String
) -> RememberedHotkeyState {
    RememberedHotkeyState(
        keyCode: keyCode,
        coreFlags: coreFlags,
        displayName: displayName)
}
