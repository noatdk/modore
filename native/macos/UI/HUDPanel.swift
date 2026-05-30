// Shared scaffolding for modore's floating HUD panels.
//
// Both the candidate list (CandidatePanel) and the debug overlay
// (DebugOverlay) are the same kind of window: a borderless, non-activating
// NSPanel with a blurred `hudWindow` background and a vertical NSStackView
// pinned to its edges. They differ only in window level, corner radius, and
// the stack's spacing/insets. This factory owns the common construction so
// the two call sites stay a few lines each and can't drift in the panel
// flags that make "floating, click-through, on every Space" work.

import Cocoa

/// Build a borderless, non-activating floating HUD panel plus the vertical
/// stack view pinned inside it. The caller retains both, fills the stack,
/// and positions the panel. `level`, `cornerRadius`, and the stack's
/// `spacing`/`insets` are the only per-panel knobs.
func makeHUDPanel(
    level: NSWindow.Level,
    cornerRadius: CGFloat,
    stackSpacing: CGFloat,
    stackInsets: NSEdgeInsets
) -> (panel: NSPanel, stack: NSStackView) {
    let p = NSPanel(
        contentRect: NSRect(x: 0, y: 0, width: 200, height: 40),
        styleMask: [.borderless, .nonactivatingPanel],
        backing: .buffered,
        defer: true)
    p.isFloatingPanel = true
    p.level = level
    p.hidesOnDeactivate = false
    p.becomesKeyOnlyIfNeeded = true
    p.hasShadow = true
    p.isOpaque = false
    p.backgroundColor = .clear
    p.ignoresMouseEvents = true
    // Show on every Space so the panel still appears when the user is in a
    // different Space than where it was anchored (rare; mostly matters for
    // the candidate panel's AX-path stale-anchor case).
    p.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary]

    let container = NSVisualEffectView()
    container.material = .hudWindow
    container.blendingMode = .behindWindow
    container.state = .active
    container.wantsLayer = true
    container.layer?.cornerRadius = cornerRadius
    container.layer?.borderWidth = 0.5
    container.layer?.borderColor = NSColor.separatorColor.cgColor
    container.translatesAutoresizingMaskIntoConstraints = false

    let stack = NSStackView()
    stack.orientation = .vertical
    stack.alignment = .leading
    stack.spacing = stackSpacing
    stack.edgeInsets = stackInsets
    stack.translatesAutoresizingMaskIntoConstraints = false

    container.addSubview(stack)
    NSLayoutConstraint.activate([
        stack.leadingAnchor.constraint(equalTo: container.leadingAnchor),
        stack.trailingAnchor.constraint(equalTo: container.trailingAnchor),
        stack.topAnchor.constraint(equalTo: container.topAnchor),
        stack.bottomAnchor.constraint(equalTo: container.bottomAnchor),
    ])

    p.contentView = container
    return (p, stack)
}
