// Accessibility (AX) read/write of the focused text element. This is the
// fast-path pickup route used by `Pickup.swift` — when it succeeds modore
// never touches the clipboard. The clipboard fallback only fires when
// `readFocusedField` returns nil or when the AX replace fails.

import Cocoa
import ApplicationServices

private let kAXAutocompleteValueAttribute = "AXAutocompleteValue"
private let gAXFocusLogQueue = DispatchQueue(label: "local.modore.ax-focus-log")
private let gAXFocusLogDelay: DispatchTimeInterval = .milliseconds(80)
private let gAXFocusLogLock = NSLock()
private var gAXFocusPendingWorkItem: DispatchWorkItem?

private struct AXFocusBurstSample {
    let role: String
    let valueLen: Int
    let selStart: Int
    let selEnd: Int
    let autocomplete: String?
}

private struct AXFocusBurst {
    var count: Int = 0
    var first: AXFocusBurstSample?
    var last: AXFocusBurstSample?
    var maxValueLen: Int = 0
    var maxSelEnd: Int = 0
}

private var gAXFocusBurst = AXFocusBurst()

struct FocusedField {
    let element: AXUIElement
    let value: String
    let selStart: Int
    let selEnd: Int
    let role: String
    let description: String
    let autocomplete: String?
}

/// Electron's AX tree is gated behind a private attribute that assistive
/// technologies are expected to set externally. Without it, even modern
/// Electron apps return -25204/-25212 on `kAXFocusedUIElementAttribute`
/// for the whole app, which is exactly why Slack/Discord/VSCode/Cursor
/// fall to the clipboard path. After we set this attribute = true,
/// subsequent AX reads start working (selection range may still be wrong
/// per electron/electron#37465, but `kAXValue` and `kAXSelectedText` come
/// through). The attribute is per-app and process-lifetime, so we cache
/// the pids we've enabled.
///
/// See: https://github.com/electron/electron/blob/main/docs/tutorial/accessibility.md
/// and the AXSwift / Hammerspoon community confirming this is the
/// documented path for AT tools.
private var gAXManualAccessibilityEnabledPids = Set<pid_t>()
private let gAXManualAccessibilityLock = NSLock()

/// Set `AXManualAccessibility = true` on the frontmost app's AXUIElement
/// if we haven't already done so for that pid. Cheap no-op on apps that
/// don't support the attribute (returns -25205 / attribute-unsupported);
/// the success/failure status is logged but not acted on. Idempotent.
func enableElectronAXIfNeeded() {
    guard let pid = FrontmostApp.describe()?.pid else { return }
    gAXManualAccessibilityLock.lock()
    let already = gAXManualAccessibilityEnabledPids.contains(pid)
    if !already { gAXManualAccessibilityEnabledPids.insert(pid) }
    gAXManualAccessibilityLock.unlock()
    if already { return }
    let appElem = AXUIElementCreateApplication(pid)
    let rc = AXUIElementSetAttributeValue(
        appElem,
        "AXManualAccessibility" as CFString,
        kCFBooleanTrue
    )
    Log.ax("AXManualAccessibility set on pid=\(pid): rc=\(rc.rawValue)")
}

func frontmostAppLooksElectron() -> Bool {
    guard let info = FrontmostApp.describe() else { return false }
    return KnownApps.looksElectron(
        bundleID: info.bundleID.lowercased(),
        executablePath: SecureInputMonitor.describeProcess(pid: info.pid)?.path)
}

func readFocusedField() -> FocusedField? {
    let systemWide = AXUIElementCreateSystemWide()
    var focusedRef: CFTypeRef?
    let r1 = AXUIElementCopyAttributeValue(
        systemWide,
        kAXFocusedUIElementAttribute as CFString,
        &focusedRef
    )
    if r1 != .success {
        Log.ax("focused-element lookup failed: \(r1.rawValue)\(FrontmostApp.logSuffix())")
        if r1 == .apiDisabled || r1 == .cannotComplete {
            Log.ax("appears disabled for this process; restart the host after granting permission")
        }
        return nil
    }
    guard let focused = focusedRef else {
        Log.ax("returned no focused element\(FrontmostApp.logSuffix())")
        return nil
    }
    let element = focused as! AXUIElement

    // Identify the element for diagnostics
    var roleRef: CFTypeRef?
    _ = AXUIElementCopyAttributeValue(element, kAXRoleAttribute as CFString, &roleRef)
    let role = (roleRef as? String) ?? "?"

    var valueRef: CFTypeRef?
    let r2 = AXUIElementCopyAttributeValue(
        element,
        kAXValueAttribute as CFString,
        &valueRef
    )
    guard r2 == .success else {
        Log.ax("value lookup failed on role=\(role): \(r2.rawValue)\(FrontmostApp.logSuffix())")
        return nil
    }
    guard let s = valueRef as? String else {
        Log.ax("value on role=\(role) is not a String (type=\(String(describing: type(of: valueRef!))))\(FrontmostApp.logSuffix())")
        return nil
    }

    var selStart = s.utf16.count
    var selEnd = s.utf16.count
    var rangeRef: CFTypeRef?
    let r3 = AXUIElementCopyAttributeValue(
        element,
        kAXSelectedTextRangeAttribute as CFString,
        &rangeRef
    )
    if r3 == .success, let rv = rangeRef {
        var cfRange = CFRange(location: 0, length: 0)
        if AXValueGetValue(rv as! AXValue, .cfRange, &cfRange) {
            selStart = cfRange.location
            selEnd = cfRange.location + cfRange.length
        }
    } else {
        Log.ax("selection range unavailable on role=\(role) (using end-of-buffer caret)")
    }
    var descRef: CFTypeRef?
    _ = AXUIElementCopyAttributeValue(element, kAXDescriptionAttribute as CFString, &descRef)
    let desc = (descRef as? String) ?? ""

    var autocompleteRef: CFTypeRef?
    let r4 = AXUIElementCopyAttributeValue(
        element,
        kAXAutocompleteValueAttribute as CFString,
        &autocompleteRef
    )
    let autocomplete = (r4 == .success) ? (autocompleteRef as? String) : nil

    let sample = AXFocusBurstSample(
        role: role,
        valueLen: s.utf16.count,
        selStart: selStart,
        selEnd: selEnd,
        autocomplete: autocomplete)
    gAXFocusLogLock.lock()
    if gAXFocusBurst.count == 0 {
        gAXFocusBurst.first = sample
        gAXFocusBurst.maxValueLen = sample.valueLen
        gAXFocusBurst.maxSelEnd = sample.selEnd
    }
    gAXFocusBurst.count += 1
    gAXFocusBurst.last = sample
    gAXFocusBurst.maxValueLen = max(gAXFocusBurst.maxValueLen, sample.valueLen)
    gAXFocusBurst.maxSelEnd = max(gAXFocusBurst.maxSelEnd, sample.selEnd)
    gAXFocusPendingWorkItem?.cancel()
    let workItem = DispatchWorkItem {
        gAXFocusLogLock.lock()
        let burst = gAXFocusBurst
        gAXFocusBurst = AXFocusBurst()
        gAXFocusPendingWorkItem = nil
        gAXFocusLogLock.unlock()

        guard let first = burst.first, let last = burst.last else { return }
        let autocomplete = last.autocomplete ?? "-"
        if burst.count == 1 {
            Log.ax("focused role=\(last.role) valueLen=\(last.valueLen) sel=[\(last.selStart),\(last.selEnd)] autocomplete=\(autocomplete)")
            return
        }
        Log.ax("focused burst x\(burst.count) role=\(first.role) valueLen \(first.valueLen)→\(last.valueLen) peak=\(burst.maxValueLen) sel [\(first.selStart),\(first.selEnd)]→[\(last.selStart),\(last.selEnd)] peakSelEnd=\(burst.maxSelEnd) autocomplete=\(autocomplete)")
    }
    gAXFocusPendingWorkItem = workItem
    gAXFocusLogLock.unlock()
    gAXFocusLogQueue.asyncAfter(deadline: .now() + gAXFocusLogDelay, execute: workItem)
    return FocusedField(
        element: element,
        value: s,
        selStart: selStart,
        selEnd: selEnd,
        role: role,
        description: desc,
        autocomplete: autocomplete)
}

func replaceRange(in element: AXUIElement, start: Int, end: Int, replacement: String) -> Bool {
    // Snapshot value AND selection range before the write so we can
    // verify afterwards and, on failure, roll the selection back.
    // Chromium textareas under --force-renderer-accessibility honor the
    // selection-range set but silently drop the subsequent
    // kAXSelectedText set; both AX calls still report .success, so the
    // only way to detect this is to compare the resulting string. If we
    // don't roll back the selection, the clipboard-fallback path below
    // starts from a contaminated [start..end] selection that its
    // Shift+Opt+Left logic doesn't know how to handle, and ends up
    // pasting at the caret without replacing the original span.
    var beforeRef: CFTypeRef?
    let rb = AXUIElementCopyAttributeValue(
        element,
        kAXValueAttribute as CFString,
        &beforeRef
    )
    guard rb == .success, let before = beforeRef as? String else { return false }
    let beforeU16 = Array(before.utf16)
    guard start >= 0, end <= beforeU16.count, start <= end else { return false }

    var origRangeRef: CFTypeRef?
    let rrc = AXUIElementCopyAttributeValue(
        element,
        kAXSelectedTextRangeAttribute as CFString,
        &origRangeRef
    )
    let origRangeValue: AXValue? = (rrc == .success) ? (origRangeRef as! AXValue?) : nil

    var range = CFRange(location: start, length: end - start)
    guard let rangeValue = AXValueCreate(.cfRange, &range) else { return false }
    let r1 = AXUIElementSetAttributeValue(
        element,
        kAXSelectedTextRangeAttribute as CFString,
        rangeValue
    )
    guard r1 == .success else { return false }
    let r2 = AXUIElementSetAttributeValue(
        element,
        kAXSelectedTextAttribute as CFString,
        replacement as CFString
    )
    guard r2 == .success else {
        if let orv = origRangeValue {
            _ = AXUIElementSetAttributeValue(
                element, kAXSelectedTextRangeAttribute as CFString, orv)
        }
        return false
    }

    var afterRef: CFTypeRef?
    let ra = AXUIElementCopyAttributeValue(
        element,
        kAXValueAttribute as CFString,
        &afterRef
    )
    guard ra == .success, let after = afterRef as? String else {
        if let orv = origRangeValue {
            _ = AXUIElementSetAttributeValue(
                element, kAXSelectedTextRangeAttribute as CFString, orv)
        }
        return false
    }
    let expected = String(utf16CodeUnits: beforeU16, count: start)
        + replacement
        + String(utf16CodeUnits: Array(beforeU16[end..<beforeU16.count]),
                 count: beforeU16.count - end)
    guard after == expected else {
        Log.ax("replace verify failed: AX write returned success but value did not match expected (got len=\(after.utf16.count), want len=\(expected.utf16.count))\(FrontmostApp.logSuffix())")
        if let orv = origRangeValue {
            _ = AXUIElementSetAttributeValue(
                element, kAXSelectedTextRangeAttribute as CFString, orv)
        }
        return false
    }
    return true
}

/// Diagnostic-only snapshot of the focused field's AX state. Reads role,
/// value length (utf16), and selection range — each soft-failing — and
/// logs a single line tagged `[ax-snap label]`. Used by the imperative
/// pickup path to disambiguate where the one-row drift comes from in
/// CodeMirror-based editors: the *length* of the selection at the moment
/// postUnicode fires tells us whether the editor's internal range
/// extends past the visible line content (trailing-newline theory) or
/// starts before line-start (leading-newline theory), or neither.
func axSelectionSnapshot(label: String) {
    let systemWide = AXUIElementCreateSystemWide()
    var focusedRef: CFTypeRef?
    let r1 = AXUIElementCopyAttributeValue(
        systemWide, kAXFocusedUIElementAttribute as CFString, &focusedRef)
    guard r1 == .success, let focused = focusedRef else {
        Log.ax("snap[\(label)] focused-element lookup failed: \(r1.rawValue)")
        return
    }
    let element = focused as! AXUIElement

    // Element identity. CFHash isn't a stable address but two AXUIElements
    // for the same underlying view hash equal, so this is enough to spot
    // "the focused node changed between snapshots."
    let eid = String(CFHash(element), radix: 16)

    func readString(_ attr: String) -> String? {
        var ref: CFTypeRef?
        if AXUIElementCopyAttributeValue(element, attr as CFString, &ref) == .success {
            return ref as? String
        }
        return nil
    }

    let role     = readString(kAXRoleAttribute as String) ?? "?"
    let subrole  = readString(kAXSubroleAttribute as String) ?? "-"
    let title    = readString(kAXTitleAttribute as String) ?? "-"
    let desc     = readString(kAXDescriptionAttribute as String) ?? "-"
    let helpStr  = readString(kAXHelpAttribute as String) ?? "-"
    let ident    = readString(kAXIdentifierAttribute as String) ?? "-"

    // Parent role helps disambiguate sibling text-area-like nodes
    // (CodeMirror's contenteditable vs. a popup vs. an offscreen label).
    var parentRole = "-"
    var parentRef: CFTypeRef?
    if AXUIElementCopyAttributeValue(
        element, kAXParentAttribute as CFString, &parentRef) == .success,
       let parent = parentRef {
        let parentEl = parent as! AXUIElement
        var pRoleRef: CFTypeRef?
        if AXUIElementCopyAttributeValue(
            parentEl, kAXRoleAttribute as CFString, &pRoleRef) == .success {
            parentRole = (pRoleRef as? String) ?? "?"
        }
    }

    var valueLen = -1
    var valueSample = "-"
    if let s = readString(kAXValueAttribute as String) {
        valueLen = s.utf16.count
        // Truncate to a single log-friendly line. Replace newlines with
        // ⏎ so a line break in the value doesn't break log parsing.
        let oneLine = s.replacingOccurrences(of: "\n", with: "⏎")
                       .replacingOccurrences(of: "\r", with: "␍")
        valueSample = oneLine.count > 120
            ? String(oneLine.prefix(120)) + "…"
            : oneLine
    }

    var selDesc = "?"
    var rangeRef: CFTypeRef?
    if AXUIElementCopyAttributeValue(
        element, kAXSelectedTextRangeAttribute as CFString, &rangeRef) == .success,
       let rv = rangeRef {
        var cfRange = CFRange(location: 0, length: 0)
        if AXValueGetValue(rv as! AXValue, .cfRange, &cfRange) {
            selDesc = "[\(cfRange.location),\(cfRange.location + cfRange.length)] len=\(cfRange.length)"
        }
    }

    var selTextLen = -1
    if let s = readString(kAXSelectedTextAttribute as String) {
        selTextLen = s.utf16.count
    }

    Log.ax("snap[\(label)] eid=\(eid) role=\(role)/\(subrole) parent=\(parentRole) "
         + "title='\(title)' desc='\(desc)' help='\(helpStr)' id='\(ident)' "
         + "valueLen=\(valueLen) sel=\(selDesc) selTextLen=\(selTextLen) "
         + "value='\(valueSample)'")
}

/// Read a string-typed AX attribute off `element`, or nil if the attribute
/// is missing or isn't a string. Best-effort: the `AXError` is swallowed
/// because every caller already has its own fallback (the value is only used
/// to enrich logs / positioning). Shared by the pickup pipeline and the
/// candidate panel.
func axStringAttr(_ element: AXUIElement, _ attr: String) -> String? {
    var ref: CFTypeRef?
    let err = AXUIElementCopyAttributeValue(element, attr as CFString, &ref)
    guard err == .success else { return nil }
    return ref as? String
}

/// The system-wide focused UI element, or nil when AX can't resolve one.
/// Non-logging — callers that want a per-failure diagnostic do the lookup
/// inline (`readFocusedField`, `axSelectionSnapshot`). Used by the
/// read-selection primitive and the candidate panel's positioning fallback,
/// which only need the element-or-nil.
func systemWideFocusedElement() -> AXUIElement? {
    let systemWide = AXUIElementCreateSystemWide()
    var focusedRef: CFTypeRef?
    let r = AXUIElementCopyAttributeValue(
        systemWide, kAXFocusedUIElementAttribute as CFString, &focusedRef)
    guard r == .success, let focused = focusedRef else { return nil }
    return (focused as! AXUIElement)
}

/// Read `kAXSelectedTextAttribute` from the system-wide focused element.
/// Returns nil if AX read fails or the attribute is absent. Used by the
/// `modore.host.read_selection` Lua primitive so scripts can pick up the
/// active selection without going through Cmd+C — in editors that apply
/// linewise-copy heuristics on Cmd+C (Obsidian's CodeMirror), the
/// clipboard path silently extends the editor's range past the visible
/// selection and corrupts the subsequent postUnicode replacement.
func readFocusedSelection() -> String? {
    guard let element = systemWideFocusedElement() else { return nil }
    var selRef: CFTypeRef?
    let r2 = AXUIElementCopyAttributeValue(
        element, kAXSelectedTextAttribute as CFString, &selRef)
    guard r2 == .success else { return nil }
    return selRef as? String
}

/// Ask the OS whether this process is allowed to use Accessibility APIs.
/// With `prompt: true`, macOS shows the "grant access" sheet on first call.
func isTrusted(prompt: Bool) -> Bool {
    let key = kAXTrustedCheckOptionPrompt.takeUnretainedValue()
    let opts: NSDictionary = [key: prompt]
    return AXIsProcessTrustedWithOptions(opts as CFDictionary)
}
