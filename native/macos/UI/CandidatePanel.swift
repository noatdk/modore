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
// near the mouse cursor on the clipboard-fallback path (where we have no
// AX element to query). The panel never becomes key — the user keeps typing
// into their app, and the cycle hotkey continues to drive selection. This
// file owns *display only*; gesture wiring lives in Pickup/Cycle/Undo.
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
    /// Pending auto-hide. Reset on every `show()` so a cycle chain keeps
    /// the panel alive. Cancelled by explicit `hide()` calls (Esc-undo,
    /// session clear). nil when no auto-hide is scheduled.
    private var autoHideWork: DispatchWorkItem?

    /// Anchor for positioning the panel. AX path carries a screen-coord rect
    /// (top-left origin, AX-style); clipboard path carries an AppKit point
    /// (bottom-left origin). Resolving once at call time keeps the AppKit
    /// hop free of AX/AppKit coordinate juggling.
    enum Anchor {
        /// AX-derived span rect, in AX global coords (top-left origin from
        /// the primary screen). Converted to AppKit on the main thread.
        case axRect(CGRect)
        /// Mouse cursor in AppKit screen coords (bottom-left origin).
        case mousePoint(NSPoint)
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
                Log.panel("anchor=axRect via AXBoundsForRange role=\(role) rect=\(formatRect(rect))\(FrontmostApp.logSuffix())")
                return .axRect(rect)
            case .fail(let reason):
                Log.panel("AXBoundsForRange failed: \(reason) — falling through (role=\(role))")
            }
            // Next best: the focused element's own frame. Chromium-based
            // apps (Chrome, Edge, Electron) refuse the per-range
            // parameterized attribute but expose AXPosition/AXSize on the
            // input element.
            switch elementFrame(of: element) {
            case .ok(let rect):
                Log.panel("anchor=axRect via element frame role=\(role) rect=\(formatRect(rect))\(FrontmostApp.logSuffix())")
                return .axRect(rect)
            case .fail(let reason):
                Log.panel("element frame failed: \(reason) — falling through (role=\(role))")
            }
            // Last resort: mouse cursor. Usually nowhere near the caret,
            // but better than off-screen.
            let mouse = NSEvent.mouseLocation
            Log.panel("anchor=mouse fallback x=\(Int(mouse.x)) y=\(Int(mouse.y))\(FrontmostApp.logSuffix())")
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
                    Log.panel("anchor=axRect via per-app focused-element role=\(role) rect=\(formatRect(rect))\(FrontmostApp.logSuffix())")
                    return .axRect(rect)
                case .fail(let reason):
                    Log.panel("per-app focused-element frame failed: \(reason) role=\(role)")
                }
            } else if pid > 0 {
                Log.panel("per-app focused-element lookup returned nil pid=\(pid)")
            }
            if let focused = focusedElementFromSystem() {
                let role = axStringAttr(focused, kAXRoleAttribute) ?? "(role unknown)"
                switch elementFrame(of: focused) {
                case .ok(let rect):
                    Log.panel("anchor=axRect via system focused-element role=\(role) rect=\(formatRect(rect))\(FrontmostApp.logSuffix())")
                    return .axRect(rect)
                case .fail(let reason):
                    Log.panel("system focused-element frame failed: \(reason) role=\(role)")
                }
            } else {
                Log.panel("system focused-element lookup returned nil")
            }
            let mouse = NSEvent.mouseLocation
            Log.panel("anchor=mouse (clipboard backing) x=\(Int(mouse.x)) y=\(Int(mouse.y))\(FrontmostApp.logSuffix())")
            return .mousePoint(mouse)
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

    /// System-wide focused element. Cheaper than per-app (no pid required),
    /// but several apps publish focus only through the per-app path. Used
    /// as a secondary fallback after `focusedElementFromApp`.
    private func focusedElementFromSystem() -> AXUIElement? {
        let systemWide = AXUIElementCreateSystemWide()
        var ref: CFTypeRef?
        let err = AXUIElementCopyAttributeValue(
            systemWide,
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

    /// Best-effort AX string attribute read — returns nil rather than
    /// surfacing the AXError, since failure here is only used to enrich
    /// log lines (the calling code already has its own success path).
    private func axStringAttr(_ element: AXUIElement, _ attr: String) -> String? {
        var ref: AnyObject?
        let err = AXUIElementCopyAttributeValue(element, attr as CFString, &ref)
        guard err == .success else { return nil }
        return ref as? String
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
        rebuildRows(snapshot: snapshot)

        // Size to fit content, then position relative to the anchor.
        panel.layoutIfNeeded()
        let contentSize = stackView?.fittingSize ?? NSSize(width: 180, height: 24)
        let panelSize = NSSize(
            width: max(160, contentSize.width + 16),
            height: contentSize.height + 12)

        let origin = panelOrigin(for: anchor, size: panelSize)
        panel.setFrame(NSRect(origin: origin, size: panelSize), display: false)
        panel.orderFront(nil)
        Log.panel("shown size=\(formatSize(panelSize)) origin=(\(Int(origin.x)),\(Int(origin.y))) rows=\(snapshot.candidates.count + 1) selected=\(snapshot.index)")
        rearmAutoHide()
    }

    private func hideOnMain() {
        autoHideWork?.cancel()
        autoHideWork = nil
        panel?.orderOut(nil)
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
        let p = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 200, height: 40),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: true)
        p.isFloatingPanel = true
        p.level = .floating
        p.hidesOnDeactivate = false
        p.becomesKeyOnlyIfNeeded = true
        p.hasShadow = true
        p.isOpaque = false
        p.backgroundColor = .clear
        p.ignoresMouseEvents = true
        // Show on every Space so cycle still works when the user is in a
        // different Space than where the conversion started (rare; mostly
        // matters for the AX path's stale-anchor case).
        p.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary]

        let container = NSVisualEffectView()
        container.material = .hudWindow
        container.blendingMode = .behindWindow
        container.state = .active
        container.wantsLayer = true
        container.layer?.cornerRadius = 8
        container.layer?.borderWidth = 0.5
        container.layer?.borderColor = NSColor.separatorColor.cgColor
        container.translatesAutoresizingMaskIntoConstraints = false

        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 2
        stack.edgeInsets = NSEdgeInsets(top: 6, left: 8, bottom: 6, right: 8)
        stack.translatesAutoresizingMaskIntoConstraints = false

        container.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            stack.topAnchor.constraint(equalTo: container.topAnchor),
            stack.bottomAnchor.constraint(equalTo: container.bottomAnchor),
        ])

        p.contentView = container
        self.panel = p
        self.stackView = stack
        return p
    }

    private func rebuildRows(snapshot: PanelSnapshot) {
        guard let stack = stackView else { return }
        // Cheap: max ~9 rows. Just clear and rebuild rather than diffing.
        for v in stack.arrangedSubviews { v.removeFromSuperview() }
        rowViews.removeAll(keepingCapacity: true)

        // Row 0 = original reading (the post-Esc / undone state,
        // candidateIndex = -1). Dimmed so it reads as "what you typed"
        // distinct from the actual candidates underneath.
        let originalRow = RowView(text: snapshot.originalReading,
                                  selected: snapshot.index < 0,
                                  isOriginal: true)
        stack.addArrangedSubview(originalRow)
        rowViews.append(originalRow)

        for (i, c) in snapshot.candidates.enumerated() {
            let row = RowView(text: c, selected: i == snapshot.index, isOriginal: false)
            stack.addArrangedSubview(row)
            rowViews.append(row)
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
            let belowX = axRect.minX
            let belowY = primaryMaxY - axRect.maxY - size.height - 4
            target = NSPoint(x: belowX, y: belowY)
        case .mousePoint(let p):
            // Place just below-and-right of the cursor so it doesn't sit
            // exactly under the pointer (which would feel like a tooltip).
            target = NSPoint(x: p.x + 12, y: p.y - size.height - 12)
        }
        return clamp(target, size: size)
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
    let candidates: [String]
    let index: Int

    init(session: ConversionSession) {
        self.originalReading = session.originalReading
        self.candidates = session.candidates
        self.index = session.candidateIndex
    }
}

/// One row in the candidate list. Selected rows get a tinted background;
/// the original-reading row reads dimmed so the user can distinguish "what
/// I typed" from the conversion candidates without a separator widget.
private final class RowView: NSView {
    private let textField = NSTextField(labelWithString: "")

    init(text: String, selected: Bool, isOriginal: Bool) {
        super.init(frame: .zero)
        wantsLayer = true
        translatesAutoresizingMaskIntoConstraints = false

        textField.stringValue = text
        textField.font = .systemFont(
            ofSize: 13,
            weight: selected && !isOriginal ? .semibold : .regular)
        textField.textColor = isOriginal
            ? .tertiaryLabelColor
            : (selected ? .labelColor : .secondaryLabelColor)
        textField.translatesAutoresizingMaskIntoConstraints = false

        addSubview(textField)
        NSLayoutConstraint.activate([
            textField.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 8),
            textField.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
            textField.topAnchor.constraint(equalTo: topAnchor, constant: 2),
            textField.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -2),
        ])

        if selected {
            layer?.backgroundColor = NSColor.controlAccentColor
                .withAlphaComponent(0.18).cgColor
            layer?.cornerRadius = 4
        }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }
}
