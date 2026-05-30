// Generic debug overlay panel. Positioned below the menu-bar status item.
//
// Callers push a flat list of DebugRow values; the overlay renders them as
// a compact monospaced key-value HUD. The API is intentionally content-
// agnostic: the shadow buffer, pickup pipeline, or any other subsystem can
// push rows without this file knowing about them.
//
// Visibility is gated by `[debug] overlay = on` in modore.conf. The global
// `gDebugOverlayEnabled` is the runtime gate; callers check it before
// calling `update(...)` so the path is zero-cost when disabled.
//
// Threading: `update` and `hide` are safe from any queue — they dispatch
// to main if not already there.

import Cocoa

// MARK: - Public data type

struct DebugRow {
    let label: String
    let value: String
}

// MARK: - Overlay

final class DebugOverlay {

    static let shared = DebugOverlay()
    private init() {}

    private var panel: NSPanel?
    private var stack: NSStackView?

    // Update content and re-position below `buttonFrame` (AppKit screen coords).
    // Safe from any queue.
    func update(rows: [DebugRow], anchoredBelow buttonFrame: NSRect) {
        if Thread.isMainThread {
            updateOnMain(rows: rows, buttonFrame: buttonFrame)
        } else {
            DispatchQueue.main.async { [weak self] in
                self?.updateOnMain(rows: rows, buttonFrame: buttonFrame)
            }
        }
    }

    // Safe from any queue.
    func hide() {
        if Thread.isMainThread {
            panel?.orderOut(nil)
        } else {
            DispatchQueue.main.async { [weak self] in self?.panel?.orderOut(nil) }
        }
    }

    // MARK: - Main-thread work

    private func updateOnMain(rows: [DebugRow], buttonFrame: NSRect) {
        let p = ensurePanel()
        rebuildRows(rows: rows)
        p.layoutIfNeeded()

        let fitted = stack?.fittingSize ?? NSSize(width: 200, height: 40)
        let panelW = min(max(fitted.width + 24, 180), 420)
        let panelH = fitted.height + 12

        let screen = NSScreen.screens.first(where: { $0.frame.contains(buttonFrame.origin) })
            ?? NSScreen.main
        let screenFrame = screen?.visibleFrame ?? CGRect(x: 0, y: 0, width: 1440, height: 900)

        let idealX = buttonFrame.midX - panelW / 2
        let x = min(max(idealX, screenFrame.minX + 4), screenFrame.maxX - panelW - 4)
        let y = buttonFrame.minY - panelH

        p.setFrame(NSRect(x: x, y: y, width: panelW, height: panelH), display: false)
        p.orderFront(nil)
    }

    // MARK: - Panel lifecycle

    private func ensurePanel() -> NSPanel {
        if let existing = panel { return existing }
        let (p, s) = makeHUDPanel(
            level: .statusBar,
            cornerRadius: 8,
            stackSpacing: 1,
            stackInsets: NSEdgeInsets(top: 6, left: 10, bottom: 6, right: 10))
        self.panel = p
        self.stack = s
        return p
    }

    private func rebuildRows(rows: [DebugRow]) {
        guard let s = stack else { return }
        for v in s.arrangedSubviews { v.removeFromSuperview() }
        for row in rows {
            s.addArrangedSubview(DebugRowView(row: row))
        }
    }
}

// MARK: - Row view

private final class DebugRowView: NSView {

    init(row: DebugRow) {
        super.init(frame: .zero)

        let labelFont = NSFont.monospacedSystemFont(ofSize: 10, weight: .medium)
        let valueFont = NSFont.monospacedSystemFont(ofSize: 10, weight: .regular)

        let labelField = NSTextField(labelWithString: row.label + ":")
        labelField.font = labelFont
        labelField.textColor = .secondaryLabelColor
        labelField.translatesAutoresizingMaskIntoConstraints = false
        labelField.setContentHuggingPriority(.defaultHigh, for: .horizontal)

        let valueField = NSTextField(labelWithString: row.value)
        valueField.font = valueFont
        valueField.textColor = .labelColor
        valueField.lineBreakMode = .byTruncatingTail
        valueField.translatesAutoresizingMaskIntoConstraints = false

        let h = NSStackView(views: [labelField, valueField])
        h.orientation = .horizontal
        h.spacing = 6
        h.alignment = .firstBaseline
        h.translatesAutoresizingMaskIntoConstraints = false

        addSubview(h)
        NSLayoutConstraint.activate([
            h.leadingAnchor.constraint(equalTo: leadingAnchor),
            h.trailingAnchor.constraint(equalTo: trailingAnchor),
            h.topAnchor.constraint(equalTo: topAnchor),
            h.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
    }

    required init?(coder: NSCoder) { fatalError() }
}
