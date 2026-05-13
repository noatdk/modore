// Accessibility (AX) read/write of the focused text element. This is the
// fast-path pickup route used by `Pickup.swift` — when it succeeds modore
// never touches the clipboard. The clipboard fallback only fires when
// `readFocusedField` returns nil or when the AX replace fails.

import Cocoa
import ApplicationServices

struct FocusedField {
    let element: AXUIElement
    let value: String
    let selStart: Int
    let selEnd: Int
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
    Log.ax("focused role=\(role) valueLen=\(s.utf16.count) sel=[\(selStart),\(selEnd)]")
    return FocusedField(element: element, value: s, selStart: selStart, selEnd: selEnd)
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

/// Ask the OS whether this process is allowed to use Accessibility APIs.
/// With `prompt: true`, macOS shows the "grant access" sheet on first call.
func isTrusted(prompt: Bool) -> Bool {
    let key = kAXTrustedCheckOptionPrompt.takeUnretainedValue()
    let opts: NSDictionary = [key: prompt]
    return AXIsProcessTrustedWithOptions(opts as CFDictionary)
}
