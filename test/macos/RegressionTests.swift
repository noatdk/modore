import Carbon
import Foundation

extension SpanSplitTests {
    static func runRegressionTests() {
        expectEqual(
            clipboardPasteRestoreDelayMs(configuredMs: 50),
            750,
            "regression: paste restore clamps short clipboard delay"
        )
        expectEqual(
            clipboardPasteRestoreDelayMs(configuredMs: 1200),
            1200,
            "regression: paste restore honors longer configured delay"
        )

        let splitDelivery = HotkeyTapDeliveryState(
            primaryUsesCarbon: true,
            katakanaUsesCarbon: false,
            cycleUsesCarbon: false)
        expectEqual(
            splitDelivery.shouldDispatchFromTap(role: .primary),
            false,
            "regression: tap leaves Carbon-owned primary chord to Carbon"
        )
        expectEqual(
            splitDelivery.shouldDispatchFromTap(role: .katakana),
            true,
            "regression: tap handles failed katakana Carbon registration"
        )
        expectEqual(
            splitDelivery.shouldDispatchFromTap(role: .cycle),
            true,
            "regression: tap handles failed cycle Carbon registration"
        )

        let cfString = "not an array" as CFString
        expectEqual(
            cfTypeMatches(cfString, CFStringGetTypeID()),
            true,
            "regression: CF type guard accepts matching type"
        )
        expectEqual(
            cfTypeMatches(cfString, CFArrayGetTypeID()),
            false,
            "regression: CF type guard rejects mismatched type"
        )
        expectEqual(
            cfTypeMatches(nil, CFStringGetTypeID()),
            false,
            "regression: CF type guard rejects nil"
        )

        let remembered = rememberedPrimaryHotkeyState(
            keyCode: CGKeyCode(kVK_ANSI_Grave),
            coreFlags: [.maskControl, .maskShift],
            displayName: "Ctrl+Shift+grave")
        expectEqual(
            remembered.displayName,
            "Ctrl+Shift+grave",
            "regression: reconstructed primary chord preserves display label"
        )
        expectEqual(
            remembered.keyCode,
            CGKeyCode(kVK_ANSI_Grave),
            "regression: reconstructed primary chord preserves key code"
        )
        expectEqual(
            remembered.coreFlags.contains(.maskControl),
            true,
            "regression: reconstructed primary chord preserves flags"
        )
    }
}
