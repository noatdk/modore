// Floating candidate-list panel for the active ConversionSession.
//
// Visibility is gated by `[ui] candidate_panel`:
//   - `none`        → never shown.
//   - `on_cycle`    → first cycle press in a session reveals the panel; fresh
//                      conversions stay silent (top-1 was probably right).
//   - `on_convert`  → every successful conversion brings the panel up too.
//
// Rendering shape: a non-activating borderless NSPanel, positioned near the
// caret on the AX path (via `kAXBoundsForRangeParameterizedAttribute`) and
// under the menu-bar status item when cursor geometry can't be recovered.
// The panel never becomes key — the user keeps typing into their app, and
// the cycle hotkey continues to drive selection. This file owns *display
// only*; gesture wiring lives in Pickup/Cycle/Undo.
//
// Threading: every public entry point is safe from any queue (the worker
// queue `kHotkeyTapQueue` is the usual caller). Anchor resolution can run
// off-main (it touches AX through the captured element, same way the rest
// of the pickup pipeline does); the AppKit work is hopped onto the main
// queue. `hide()` skips the main-queue hop when already on main so config
// reload (also main) is synchronous.

import ApplicationServices
import Cocoa

/// Singleton owner of the floating candidate list. One panel per process,
/// reused across sessions. Holds no session state of its own — every
/// `show(...)` call passes a fresh snapshot.
final class CandidatePanel {

    static let shared = CandidatePanel()
    private init() {}

    private var panel: NSPanel?
    private var stackView: NSStackView?
    private var rowViews: [RowView] = []
    /// Stable frame for one conversion's candidate list. Cleared on hide so a
    /// fresh convert can re-anchor; held across cycle presses so selection
    /// styling (semibold, highlight) never changes panel geometry.
    private var lockedLayoutKey: String?
    private var lockedPanelSize: NSSize?
    /// Pending auto-hide. Reset on every `show()` so a cycle chain keeps
    /// the panel alive. Cancelled by explicit `hide()` calls (Esc-undo,
    /// session clear). nil when no auto-hide is scheduled.
    private var autoHideWork: DispatchWorkItem?

    /// Anchor for positioning the panel. The AX path carries a screen-coord
    /// rect (top-left origin, AX-style); fallback paths carry AppKit screen
    /// coords. Resolving once at call time keeps the AppKit hop free of
    /// AX/AppKit coordinate juggling.
    enum Anchor {
        /// AX-derived span rect, in AX global coords (top-left origin from
        /// the primary screen). Converted to AppKit on the main thread.
        case axRect(CGRect)
        /// Mouse cursor in AppKit screen coords (bottom-left origin).
        case mousePoint(NSPoint)
        /// Menu-bar button frame in AppKit screen coords (bottom-left origin).
        case statusItemButtonFrame(NSRect)
    }

    /// Show the candidate list for a session. Safe from any queue.
    /// Idempotent — repeated calls with new snapshots just refresh the
    /// content and re-position. Computes the anchor from the session's
    /// backing here so the main-queue hop only does AppKit work.
    func show(session: ConversionSession) {
        let anchor = resolveAnchor(for: session)
        let snapshot = PanelSnapshot(session: session)
        DispatchQueue.main.async { [weak self] in
            self?.showOnMain(snapshot: snapshot, anchor: anchor)
        }
    }

    /// Hide the panel if visible. Safe from any queue. Same-thread fast
    /// path on main so config reload (and any other main-thread caller)
    /// observes the hide synchronously.
    func hide() {
        if Thread.isMainThread {
            hideOnMain()
        } else {
            DispatchQueue.main.async { [weak self] in self?.hideOnMain() }
        }
    }

    // MARK: - Anchor resolution

    private func resolveAnchor(for session: ConversionSession) -> Anchor {
        switch session.backing {
        case .ax(let element, let spanStart):
            let len = session.currentText.utf16.count
            let role = axStringAttr(element, kAXRoleAttribute)
                ?? "(role unknown)"
            // Best: per-range rect — pixel-accurate under the converted span.
            // Native NSTextView/NSTextField apps and most Cocoa-based editors
            // implement this.
            switch caretRect(in: element, start: spanStart, length: len) {
            case .ok(let rect):
                return .axRect(rect)
            case .fail:
                break
            }
            // Next best: the focused element's own frame. Chromium-based
            // apps (Chrome, Edge, Electron) refuse the per-range
            // parameterized attribute but expose AXPosition/AXSize on the
            // input element.
            switch elementFrame(of: element) {
            case .ok(let rect):
                if isMeaningfulCaretFrame(rect, role: role) {
                    return .axRect(rect)
                }
            case .fail:
                break
            }
            if let frame = statusItemButtonFrame() {
                return .statusItemButtonFrame(frame)
            }
            // Absolute last resort: mouse cursor. Usually nowhere near the
            // caret, but better than off-screen if the menu-bar button isn't
            // ready yet.
            let mouse = NSEvent.mouseLocation
            return .mousePoint(mouse)
        case .clipboard(_, let pid):
            // Clipboard backing means the pickup path bailed before
            // capturing an AXUIElement (Chromium, some Electron apps).
            // Try two AX entry points for positioning only — neither
            // needs to read text content, just the focused element's
            // frame:
            //   1. Per-app: AXUIElementCreateApplication(pid) →
            //      kAXFocusedUIElement → frame. Chrome answers this even
            //      when system-wide focused-element returns nil.
            //   2. System-wide: same lookup against AXUIElementCreate
            //      SystemWide(). Fallback for apps that publish focus
            //      globally but not per-app (rare).
            if pid > 0, let focused = focusedElementFromApp(pid: pid) {
                let role = axStringAttr(focused, kAXRoleAttribute) ?? "(role unknown)"
                switch elementFrame(of: focused) {
                case .ok(let rect):
                    if isMeaningfulCaretFrame(rect, role: role) {
                        return .axRect(rect)
                    }
                case .fail:
                    break
                }
            }
            // System-wide focused element: cheaper than per-app (no pid),
            // but several apps publish focus only through the per-app path,
            // so this is the secondary fallback after `focusedElementFromApp`.
            if let focused = systemWideFocusedElement() {
                let role = axStringAttr(focused, kAXRoleAttribute) ?? "(role unknown)"
                switch elementFrame(of: focused) {
                case .ok(let rect):
                    if isMeaningfulCaretFrame(rect, role: role) {
                        return .axRect(rect)
                    }
                case .fail:
                    break
                }
            }
            if let frame = statusItemButtonFrame() {
                return .statusItemButtonFrame(frame)
            }
            let mouse = NSEvent.mouseLocation
            return .mousePoint(mouse)
        }
    }

    /// Heuristic for deciding whether an AX rect is actually near a caret.
    /// Window-sized frames from AXWindow are a common failure mode and should
    /// not be used as a panel anchor.
    private func isMeaningfulCaretFrame(_ rect: CGRect, role: String) -> Bool {
        if role == "AXWindow" {
            return false
        }
        guard let screen = NSScreen.screens.first(where: { $0.frame.intersects(rect) }) ?? NSScreen.main else {
            return true
        }
        let visible = screen.visibleFrame
        let screenArea = max(1, visible.width * visible.height)
        let rectArea = rect.width * rect.height
        // Reject huge frames that are likely the entire editor/window.
        if rectArea >= screenArea * 0.20 {
            return false
        }
        // Reject frames that hug the top-left origin and span most of the
        // window, a common "focused window" result from Accessibility.
        if rect.minX <= visible.minX + 8,
           rect.minY <= visible.minY + 32,
           rect.width >= visible.width * 0.5,
           rect.height >= visible.height * 0.5 {
            return false
        }
        return rect.width > 0 && rect.height > 0
    }

    /// Current status-item button frame in AppKit screen coords. The
    /// candidate panel uses this as the fallback anchor when the active text
    /// cursor can't be recovered meaningfully.
    private func statusItemButtonFrame() -> NSRect? {
        if Thread.isMainThread {
            return gStatusItem?.buttonScreenFrame
        }
        return DispatchQueue.main.sync {
            gStatusItem?.buttonScreenFrame
        }
    }

    /// Per-application focused element. Chromium exposes its focus tree
    /// here even when the system-wide lookup answers nil, because Chrome
    /// publishes a partial AX tree only on app-specific queries (this
    /// quirk is documented in upstream Mozc's own macOS IME — see
    /// `mozc_imk_input_controller.mm:746` comment about "Emacs or Google
    /// Chrome don't return the cursor position correctly").
    private func focusedElementFromApp(pid: pid_t) -> AXUIElement? {
        let app = AXUIElementCreateApplication(pid)
        var ref: CFTypeRef?
        let err = AXUIElementCopyAttributeValue(
            app,
            kAXFocusedUIElementAttribute as CFString,
            &ref)
        guard err == .success, let any = ref else { return nil }
        return (any as! AXUIElement)
    }

    /// Tagged outcome for an AX-attribute read. Lets the resolver log a
    /// specific failure string per branch instead of collapsing every
    /// reason to "no rect."
    private enum AXResult {
        case ok(CGRect)
        case fail(String)
    }

    private func caretRect(in element: AXUIElement, start: Int, length: Int) -> AXResult {
        var range = CFRange(location: start, length: max(length, 1))
        guard let rangeValue = AXValueCreate(.cfRange, &range) else {
            return .fail("AXValueCreate(.cfRange) returned nil")
        }
        var result: AnyObject?
        let err = AXUIElementCopyParameterizedAttributeValue(
            element,
            kAXBoundsForRangeParameterizedAttribute as CFString,
            rangeValue,
            &result)
        if err != .success {
            return .fail("AXError \(axErrorName(err)) [start=\(start) len=\(length)]")
        }
        guard let any = result else {
            return .fail("attribute value was nil despite AXError.success")
        }
        let axValue = any as! AXValue
        var rect = CGRect.zero
        guard AXValueGetValue(axValue, .cgRect, &rect) else {
            return .fail("AXValueGetValue(.cgRect) returned false")
        }
        if rect.width <= 0 && rect.height <= 0 {
            return .fail("zero-size rect \(formatRect(rect))")
        }
        return .ok(rect)
    }

    /// Frame of the focused element itself, in AX coords (top-left origin,
    /// screen-global). Fallback when the per-range bounds attribute isn't
    /// implemented (Chromium, some Electron apps).
    private func elementFrame(of element: AXUIElement) -> AXResult {
        var posRef: AnyObject?
        var sizeRef: AnyObject?
        let posErr = AXUIElementCopyAttributeValue(
            element, kAXPositionAttribute as CFString, &posRef)
        let sizeErr = AXUIElementCopyAttributeValue(
            element, kAXSizeAttribute as CFString, &sizeRef)
        if posErr != .success {
            return .fail("AXPosition: \(axErrorName(posErr))")
        }
        if sizeErr != .success {
            return .fail("AXSize: \(axErrorName(sizeErr))")
        }
        guard let pAny = posRef, let sAny = sizeRef else {
            return .fail("AXPosition or AXSize was nil despite success")
        }
        var origin = CGPoint.zero
        var size = CGSize.zero
        let pv = pAny as! AXValue
        let sv = sAny as! AXValue
        guard AXValueGetValue(pv, .cgPoint, &origin) else {
            return .fail("AXValueGetValue(.cgPoint) returned false on AXPosition")
        }
        guard AXValueGetValue(sv, .cgSize, &size) else {
            return .fail("AXValueGetValue(.cgSize) returned false on AXSize")
        }
        if size.width <= 0 || size.height <= 0 {
            return .fail("zero-size element \(formatSize(size))")
        }
        return .ok(CGRect(origin: origin, size: size))
    }

    private func formatRect(_ r: CGRect) -> String {
        "(\(Int(r.minX)),\(Int(r.minY)) \(Int(r.width))x\(Int(r.height)))"
    }

    private func formatSize(_ s: CGSize) -> String {
        "\(Int(s.width))x\(Int(s.height))"
    }

    /// Symbol names for the common AXError values so log lines read better
    /// than "AXError(rawValue: -25204)." Only the cases we've actually seen
    /// fall out of AX-driven code paths are listed; everything else falls
    /// through to the raw integer.
    private func axErrorName(_ err: AXError) -> String {
        switch err {
        case .success:                     return "success"
        case .failure:                     return "failure"
        case .illegalArgument:             return "illegalArgument"
        case .invalidUIElement:            return "invalidUIElement"
        case .invalidUIElementObserver:    return "invalidUIElementObserver"
        case .cannotComplete:              return "cannotComplete"
        case .attributeUnsupported:        return "attributeUnsupported"
        case .actionUnsupported:           return "actionUnsupported"
        case .notificationUnsupported:     return "notificationUnsupported"
        case .notImplemented:              return "notImplemented"
        case .notificationAlreadyRegistered: return "notificationAlreadyRegistered"
        case .notificationNotRegistered:   return "notificationNotRegistered"
        case .apiDisabled:                 return "apiDisabled"
        case .noValue:                     return "noValue"
        case .parameterizedAttributeUnsupported: return "parameterizedAttributeUnsupported"
        case .notEnoughPrecision:          return "notEnoughPrecision"
        @unknown default:                  return "rawValue=\(err.rawValue)"
        }
    }

    // MARK: - Main-thread rendering

    private func showOnMain(snapshot: PanelSnapshot, anchor: Anchor) {
        let panel = ensurePanel()
        guard stackView != nil else { return }
        rebuildRows(snapshot: snapshot)

        let layoutKey = snapshot.layoutKey
        let contentWidth = measureStableContentWidth(snapshot: snapshot)

        panel.layoutIfNeeded()
        let fitted = stackView?.fittingSize ?? NSSize(width: contentWidth, height: 24)
        // Width from pre-measured max row (not fittingSize) so cycling selection
        // does not shrink/grow the panel; height still comes from the stack.
        let computedSize = NSSize(
            width: max(PanelMetrics.minWidth, contentWidth + PanelMetrics.panelPadH),
            height: fitted.height + PanelMetrics.panelPadV)

        let panelSize: NSSize
        if layoutKey == lockedLayoutKey,
           let locked = lockedPanelSize {
            panelSize = locked
        } else {
            panelSize = computedSize
            lockedLayoutKey = layoutKey
            lockedPanelSize = panelSize
        }
        let origin = panelOrigin(for: anchor, size: panelSize)
        panel.setFrame(NSRect(origin: origin, size: panelSize), display: false)
        panel.orderFront(nil)
        Log.panel("shown size=\(formatSize(panelSize)) origin=(\(Int(origin.x)),\(Int(origin.y))) rows=\(snapshot.candidates.count + 1) selected=\(snapshot.index)")
        rearmAutoHide()
    }

    private func hideOnMain() {
        autoHideWork?.cancel()
        autoHideWork = nil
        lockedLayoutKey = nil
        lockedPanelSize = nil
        panel?.orderOut(nil)
    }

    /// Width from the widest row (semibold value + detail), independent of
    /// which candidate is selected — avoids resize when cycling to FULLCAP etc.
    private func measureStableContentWidth(snapshot: PanelSnapshot) -> CGFloat {
        let valueFont = NSFont.systemFont(
            ofSize: PanelMetrics.candidateFontSize, weight: .semibold)
        let detailFont = NSFont.systemFont(
            ofSize: PanelMetrics.detailFontSize, weight: .regular)
        let inputFont = NSFont.systemFont(
            ofSize: PanelMetrics.inputFontSize, weight: .medium)
        let sectionFont = NSFont.systemFont(
            ofSize: PanelMetrics.sectionFontSize, weight: .semibold)

        var maxRow = PanelMetrics.minWidth
            - PanelMetrics.stackInsets.left
            - PanelMetrics.stackInsets.right
        maxRow = max(maxRow, InputRowView.measuredWidth(text: snapshot.originalReading, font: inputFont))

        var lastGroup: MozcBridge.Candidate.Group?
        for candidate in snapshot.candidates {
            if candidate.group != lastGroup {
                maxRow = max(maxRow, SectionHeaderView.measuredWidth(title: candidate.group.sectionTitle, font: sectionFont))
                lastGroup = candidate.group
            }
            maxRow = max(maxRow, CandidateRowView.measuredWidth(
                candidate: candidate, valueFont: valueFont, detailFont: detailFont))
        }
        return maxRow + PanelMetrics.stackInsets.left + PanelMetrics.stackInsets.right
    }

    /// Replace the pending auto-hide (if any) with a new one scheduled
    /// `gCandidatePanelDurationMs` from now. `0` disables auto-hide —
    /// the panel persists until something explicitly hides it (Esc-undo
    /// or session clear in the tap callback).
    private func rearmAutoHide() {
        autoHideWork?.cancel()
        autoHideWork = nil
        let durationMs = gCandidatePanelDurationMs
        guard durationMs > 0 else { return }
        let work = DispatchWorkItem { [weak self] in
            self?.panel?.orderOut(nil)
            self?.autoHideWork = nil
            Log.panel("auto-hide after \(durationMs)ms")
        }
        autoHideWork = work
        DispatchQueue.main.asyncAfter(
            deadline: .now() + .milliseconds(durationMs),
            execute: work)
    }

    private func ensurePanel() -> NSPanel {
        if let existing = panel { return existing }
        let (p, stack) = makeHUDPanel(
            level: .floating,
            cornerRadius: PanelMetrics.cornerRadius,
            stackSpacing: PanelMetrics.stackSpacing,
            stackInsets: PanelMetrics.stackInsets)
        self.panel = p
        self.stackView = stack
        return p
    }

    private func rebuildRows(snapshot: PanelSnapshot) {
        guard let stack = stackView else { return }
        // Cheap: max ~9 rows. Just clear and rebuild rather than diffing.
        for v in stack.arrangedSubviews { v.removeFromSuperview() }
        rowViews.removeAll(keepingCapacity: true)

        let inputRow = InputRowView(text: snapshot.originalReading,
                                    selected: snapshot.index < 0)
        stack.addArrangedSubview(inputRow)

        let divider = NSBox()
        divider.boxType = .separator
        divider.translatesAutoresizingMaskIntoConstraints = false
        divider.heightAnchor.constraint(equalToConstant: 1).isActive = true
        stack.addArrangedSubview(divider)

        var lastGroup: MozcBridge.Candidate.Group? = nil
        for (i, candidate) in snapshot.candidates.enumerated() {
            if candidate.group != lastGroup {
                let header = SectionHeaderView(title: candidate.group.sectionTitle)
                stack.addArrangedSubview(header)
                lastGroup = candidate.group
            }
            let row = CandidateRowView(
                candidate: candidate,
                selected: i == snapshot.index,
                prominence: prominence(for: candidate, index: i, selectedIndex: snapshot.index))
            stack.addArrangedSubview(row)
            rowViews.append(row)
        }
    }

    private func prominence(
        for candidate: MozcBridge.Candidate,
        index: Int,
        selectedIndex: Int
    ) -> CGFloat {
        if index == selectedIndex { return 1.0 }
        switch candidate.group {
        case .conversion:
            return index == 0 ? 0.86 : 0.8
        case .hiragana, .katakana, .transliteration:
            return 0.7
        case .english:
            return 0.64
        case .input, .unknown:
            return 0.58
        }
    }

    // MARK: - Position math

    private func panelOrigin(for anchor: Anchor, size: NSSize) -> NSPoint {
        let target: NSPoint
        switch anchor {
        case .axRect(let axRect):
            // AX coords: top-left origin from primary display, y grows down.
            // AppKit: bottom-left origin from primary, y grows up. Flip via
            // primary screen's frame max-y.
            let primaryMaxY = NSScreen.screens.first?.frame.maxY ?? 0
            let inputBottomAppKit = primaryMaxY - axRect.maxY
            let inputTopAppKit = primaryMaxY - axRect.minY
            let belowY = inputBottomAppKit - size.height - 4
            let aboveY = inputTopAppKit + 4
            // Flip above the input when there isn't enough room beneath it
            // (input near the bottom edge of the screen). Otherwise the
            // panel would be clamped to the bottom and cover the field.
            let probe = NSPoint(x: axRect.minX, y: inputBottomAppKit)
            let visible = screen(containing: probe)?.visibleFrame
            let y: CGFloat
            if let f = visible, belowY < f.minY, aboveY + size.height <= f.maxY {
                y = aboveY
            } else {
                y = belowY
            }
            target = NSPoint(x: axRect.minX, y: y)
        case .mousePoint(let p):
            // Place just below-and-right of the cursor so it doesn't sit
            // exactly under the pointer (which would feel like a tooltip).
            // Flip above when the cursor is too close to the bottom edge.
            let belowY = p.y - size.height - 12
            let aboveY = p.y + 12
            let visible = screen(containing: p)?.visibleFrame
            let y: CGFloat
            if let f = visible, belowY < f.minY, aboveY + size.height <= f.maxY {
                y = aboveY
            } else {
                y = belowY
            }
            target = NSPoint(x: p.x + 12, y: y)
        case .statusItemButtonFrame(let buttonFrame):
            // Menu-bar items typically drop down with a trailing-edge bias
            // when they live on the right side of the bar. Use the item's
            // actual button frame so the panel reads like a native menu.
            let panelGap: CGFloat = 6
            let visible = screen(containing: NSPoint(x: buttonFrame.midX,
                                                     y: buttonFrame.minY))?
                .visibleFrame ?? NSScreen.main?.visibleFrame
                ?? NSScreen.screens.first?.visibleFrame
                ?? .zero
            let idealX: CGFloat
            if buttonFrame.midX > visible.midX {
                idealX = buttonFrame.maxX - size.width
            } else {
                idealX = buttonFrame.minX
            }
            let x = min(max(idealX, visible.minX + 4), visible.maxX - size.width - 4)
            let y = min(max(buttonFrame.minY - size.height - panelGap,
                            visible.minY + 4),
                        visible.maxY - size.height - 4)
            target = NSPoint(x: x, y: y)
        }
        return clamp(target, size: size)
    }

    private func screen(containing point: NSPoint) -> NSScreen? {
        NSScreen.screens.first(where: { $0.frame.contains(point) })
            ?? NSScreen.main
            ?? NSScreen.screens.first
    }

    /// Keep the whole panel on-screen. If the natural anchor would push the
    /// panel off the right/bottom edge of the screen it's currently on, slide
    /// it back inside.
    private func clamp(_ origin: NSPoint, size: NSSize) -> NSPoint {
        let probe = NSPoint(x: origin.x + size.width / 2,
                            y: origin.y + size.height / 2)
        let screen = NSScreen.screens.first(where: { $0.frame.contains(probe) })
            ?? NSScreen.main
            ?? NSScreen.screens.first
        guard let f = screen?.visibleFrame else { return origin }
        var x = origin.x
        var y = origin.y
        if x + size.width > f.maxX { x = f.maxX - size.width - 4 }
        if x < f.minX { x = f.minX + 4 }
        if y < f.minY { y = f.minY + 4 }
        if y + size.height > f.maxY { y = f.maxY - size.height - 4 }
        return NSPoint(x: x, y: y)
    }
}

/// Per-show snapshot of the session, lifted onto the main queue free of
/// the session lock. Cheap to copy.
private struct PanelSnapshot {
    let originalReading: String
    let candidates: [MozcBridge.Candidate]
    let index: Int

    init(session: ConversionSession) {
        self.originalReading = session.originalReading
        self.candidates = session.candidates
        self.index = session.candidateIndex
    }

    /// Identity of list content (not selection). Used to keep panel geometry
    /// stable while the user cycles through the same conversion.
    var layoutKey: String {
        var parts: [String] = [originalReading]
        for c in candidates {
            parts.append(c.value)
            parts.append(c.detailLabel ?? "")
            parts.append(String(c.group.rawValue))
        }
        return parts.joined(separator: "\u{1e}")
    }
}

/// Layout tuned to stay at or below Google Japanese IME candidate-window footprint.
private enum PanelMetrics {
    static let cornerRadius: CGFloat = 6
    static let stackSpacing: CGFloat = 1
    static let stackInsets = NSEdgeInsets(top: 5, left: 7, bottom: 5, right: 7)
    static let panelPadH: CGFloat = 6
    static let panelPadV: CGFloat = 2
    static let minWidth: CGFloat = 140
    static let rowMinHeight: CGFloat = 22
    static let candidateFontSize: CGFloat = 13
    static let detailFontSize: CGFloat = 10
    static let inputFontSize: CGFloat = 11
    static let sectionFontSize: CGFloat = 9
}

private class RowView: NSView {}

private final class InputRowView: RowView {
    static func measuredWidth(text: String, font: NSFont) -> CGFloat {
        let iconFont = NSFont.systemFont(
            ofSize: PanelMetrics.inputFontSize - 1, weight: .semibold)
        let iconW = ("⌨" as NSString).size(withAttributes: [.font: iconFont]).width
        let textW = (text as NSString).size(withAttributes: [.font: font]).width
        return ceil(iconW) + 4 + ceil(textW)
    }

    init(text: String, selected: Bool) {
        super.init(frame: .zero)
        translatesAutoresizingMaskIntoConstraints = false
        let icon = NSTextField(labelWithString: "⌨")
        icon.font = .systemFont(ofSize: PanelMetrics.inputFontSize - 1, weight: .semibold)
        icon.textColor = NSColor.tertiaryLabelColor
        icon.translatesAutoresizingMaskIntoConstraints = false

        let label = NSTextField(labelWithString: text)
        label.font = .systemFont(ofSize: PanelMetrics.inputFontSize, weight: .medium)
        label.textColor = selected ? NSColor.secondaryLabelColor : NSColor.tertiaryLabelColor
        label.translatesAutoresizingMaskIntoConstraints = false

        addSubview(icon)
        addSubview(label)
        NSLayoutConstraint.activate([
            icon.leadingAnchor.constraint(equalTo: leadingAnchor),
            icon.centerYAnchor.constraint(equalTo: label.centerYAnchor),
            label.leadingAnchor.constraint(equalTo: icon.trailingAnchor, constant: 4),
            label.trailingAnchor.constraint(equalTo: trailingAnchor),
            label.topAnchor.constraint(equalTo: topAnchor, constant: 1),
            label.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -1),
        ])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }
}

private final class SectionHeaderView: RowView {
    static func measuredWidth(title: String, font: NSFont) -> CGFloat {
        (title.uppercased() as NSString).size(withAttributes: [.font: font]).width
    }

    init(title: String) {
        super.init(frame: .zero)
        translatesAutoresizingMaskIntoConstraints = false
        let label = NSTextField(labelWithString: title.uppercased())
        label.font = .systemFont(ofSize: PanelMetrics.sectionFontSize, weight: .semibold)
        label.textColor = NSColor.tertiaryLabelColor
        label.translatesAutoresizingMaskIntoConstraints = false
        addSubview(label)
        NSLayoutConstraint.activate([
            label.leadingAnchor.constraint(equalTo: leadingAnchor),
            label.trailingAnchor.constraint(equalTo: trailingAnchor),
            label.topAnchor.constraint(equalTo: topAnchor, constant: 3),
            label.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }
}

private final class CandidateRowView: RowView {
    /// Same horizontal chrome as Auto Layout in `init` (indicator + gaps).
    static func measuredWidth(
        candidate: MozcBridge.Candidate,
        valueFont: NSFont,
        detailFont: NSFont
    ) -> CGFloat {
        let valueW = ceil((candidate.value as NSString)
            .size(withAttributes: [.font: valueFont]).width)
        let detail = candidate.detailLabel ?? ""
        let detailW = detail.isEmpty
            ? 0
            : ceil((detail as NSString).size(withAttributes: [.font: detailFont]).width)
        let detailGap: CGFloat = detailW > 0 ? 6 : 0
        return 2 + 6 + valueW + detailGap + detailW
    }

    init(candidate: MozcBridge.Candidate, selected: Bool, prominence: CGFloat) {
        super.init(frame: .zero)
        wantsLayer = true
        translatesAutoresizingMaskIntoConstraints = false

        let indicator = NSView()
        indicator.wantsLayer = true
        indicator.layer?.cornerRadius = 1.5
        indicator.layer?.backgroundColor = (selected
            ? NSColor.controlAccentColor
            : NSColor.clear).cgColor
        indicator.translatesAutoresizingMaskIntoConstraints = false

        let valueLabel = NSTextField(labelWithString: candidate.value)
        // Keep weight constant so cycling selection does not change intrinsic
        // width; highlight is background + accent bar only.
        valueLabel.font = .systemFont(ofSize: PanelMetrics.candidateFontSize, weight: .regular)
        valueLabel.textColor = NSColor.labelColor.withAlphaComponent(prominence)
        valueLabel.translatesAutoresizingMaskIntoConstraints = false

        let detailText = candidate.detailLabel ?? ""
        let detailLabel = NSTextField(labelWithString: detailText)
        detailLabel.font = .systemFont(ofSize: PanelMetrics.detailFontSize, weight: .regular)
        detailLabel.textColor = selected
            ? NSColor.controlAccentColor
            : NSColor.tertiaryLabelColor
        detailLabel.isHidden = detailText.isEmpty
        detailLabel.translatesAutoresizingMaskIntoConstraints = false

        addSubview(indicator)
        addSubview(valueLabel)
        addSubview(detailLabel)
        NSLayoutConstraint.activate([
            heightAnchor.constraint(greaterThanOrEqualToConstant: PanelMetrics.rowMinHeight),
            indicator.leadingAnchor.constraint(equalTo: leadingAnchor),
            indicator.topAnchor.constraint(equalTo: topAnchor, constant: 2),
            indicator.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -2),
            indicator.widthAnchor.constraint(equalToConstant: 2),

            valueLabel.leadingAnchor.constraint(equalTo: indicator.trailingAnchor, constant: 6),
            valueLabel.topAnchor.constraint(equalTo: topAnchor, constant: 3),
            valueLabel.bottomAnchor.constraint(lessThanOrEqualTo: bottomAnchor, constant: -3),

            detailLabel.leadingAnchor.constraint(greaterThanOrEqualTo: valueLabel.trailingAnchor, constant: 6),
            detailLabel.trailingAnchor.constraint(equalTo: trailingAnchor),
            detailLabel.centerYAnchor.constraint(equalTo: valueLabel.centerYAnchor),
        ])

        if selected {
            layer?.backgroundColor = NSColor.controlAccentColor
                .withAlphaComponent(0.14).cgColor
            layer?.cornerRadius = 4
        }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }
}
