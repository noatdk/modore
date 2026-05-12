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
    return r2 == .success
}

/// Ask the OS whether this process is allowed to use Accessibility APIs.
/// With `prompt: true`, macOS shows the "grant access" sheet on first call.
func isTrusted(prompt: Bool) -> Bool {
    let key = kAXTrustedCheckOptionPrompt.takeUnretainedValue()
    let opts: NSDictionary = [key: prompt]
    return AXIsProcessTrustedWithOptions(opts as CFDictionary)
}
