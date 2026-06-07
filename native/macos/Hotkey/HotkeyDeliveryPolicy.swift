// @testable
//
// Pure policy for deciding whether a matched hotkey should be dispatched by
// the CGEventTap fallback. Carbon registration is per role, so the tap must
// not infer katakana/cycle ownership from the primary chord's state.

enum HotkeyTapRole {
    case primary
    case katakana
    case cycle
}

struct HotkeyTapDeliveryState {
    var primaryUsesCarbon: Bool
    var katakanaUsesCarbon: Bool
    var cycleUsesCarbon: Bool

    func shouldDispatchFromTap(role: HotkeyTapRole) -> Bool {
        switch role {
        case .primary:
            return !primaryUsesCarbon
        case .katakana:
            return !katakanaUsesCarbon
        case .cycle:
            return !cycleUsesCarbon
        }
    }
}
