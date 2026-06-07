// Helper for reconstructing the live primary chord without dropping the
// display label originally loaded from config.

import Carbon

func rememberedPrimaryChord(
    keyCode: CGKeyCode,
    coreFlags: CGEventFlags,
    displayName: String
) -> ModoreConfig.ConversionHotkey {
    let state = rememberedPrimaryHotkeyState(
        keyCode: keyCode,
        coreFlags: coreFlags,
        displayName: displayName)
    return ModoreConfig.ConversionHotkey(
        keyCode: state.keyCode,
        coreFlags: state.coreFlags,
        displayName: state.displayName)
}
