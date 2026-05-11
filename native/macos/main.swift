// modeless-ime macOS native host.
//
// Runs as a menu-bar/accessory app, registers a global Ctrl+/ hotkey, reads
// the focused text field (any app) via Accessibility, hands the picked span
// to the in-process Mozc engine, and writes the result back — same idea as
// OpenKey for Vietnamese.
//
// Build:  make -C native/macos
// Run:    open native/macos/build/ModelessIMEHost.app
// First run prompts for Accessibility permission. Grant it in
//   System Settings → Privacy & Security → Accessibility, then re-launch.

import Cocoa
import Carbon
import ApplicationServices

// MARK: - Config

// Default chord: Ctrl + /  (kVK_ANSI_Slash = 0x2C). Matched via CGEventTap,
// not Carbon — see installEventTap().
let HOTKEY_KEYCODE: CGKeyCode = CGKeyCode(kVK_ANSI_Slash)
// Where the mozc engine keeps its on-disk state (user dictionary, history).
// Bootstrapping this from a user's existing Google Japanese Input / OSS Mozc
// profile is a follow-up task.
let MOZC_PROFILE_DIR: String = {
    let home = FileManager.default.homeDirectoryForCurrentUser.path
    return "\(home)/Library/Application Support/ModelessIME"
}()

// MARK: - Backend call shape
//
// The Mozc engine is linked in-process via bridge/include/mozc_bridge.h —
// no daemon, IPC, or HTTP. The Swift façade lives in MozcBridge.swift.

struct ConvertResult {
    let replacement: String
    let cursorOffset: Int?
}

// MARK: - Span resolution (UTF-16, matches AX/JS semantics)

func wordBounds(_ utf16: [UInt16], caret: Int) -> (Int, Int) {
    if utf16.isEmpty { return (0, 0) }
    let c = min(max(caret, 0), utf16.count)
    let isWS: (UInt16) -> Bool = { ch in
        ch == 0x20 || ch == 0x09 || ch == 0x0A || ch == 0x0D
    }
    var start = c
    while start > 0 && !isWS(utf16[start - 1]) { start -= 1 }
    var end = c
    while end < utf16.count && !isWS(utf16[end]) { end += 1 }
    if start == end {
        if c < utf16.count { return (c, c + 1) }
        if c > 0 { return (c - 1, c) }
    }
    return (start, end)
}

// MARK: - Accessibility read/write

struct FocusedField {
    let element: AXUIElement
    let value: String
    let selStart: Int
    let selEnd: Int
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
        NSLog("[modeless-ime] AX focused-element lookup failed: %d", r1.rawValue)
        if r1 == .apiDisabled || r1 == .cannotComplete {
            NSLog("[modeless-ime] AX appears disabled for this process; restart the host after granting permission")
        }
        return nil
    }
    guard let focused = focusedRef else {
        NSLog("[modeless-ime] AX returned no focused element")
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
        NSLog("[modeless-ime] AX value lookup failed on role=%@: %d", role, r2.rawValue)
        return nil
    }
    guard let s = valueRef as? String else {
        NSLog("[modeless-ime] AX value on role=%@ is not a String (type=%@)", role, String(describing: type(of: valueRef!)))
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
        NSLog("[modeless-ime] AX selection range unavailable on role=%@ (using end-of-buffer caret)", role)
    }
    NSLog("[modeless-ime] focused role=%@ valueLen=%d sel=[%d,%d]", role, s.utf16.count, selStart, selEnd)
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

// MARK: - Backend call (in-process via mozc bridge)
//
// The text/spanStart/spanEnd shape is preserved from the old HTTP contract
// so the callers don't have to change. We just extract the span and hand it
// to the bridge, which returns the top-candidate Japanese conversion.

func callBackend(text: String, spanStart: Int, spanEnd: Int) -> ConvertResult? {
    let utf16 = Array(text.utf16)
    guard spanStart >= 0, spanEnd <= utf16.count, spanStart < spanEnd else {
        return nil
    }
    let span = String(utf16CodeUnits: Array(utf16[spanStart..<spanEnd]),
                      count: spanEnd - spanStart)
    do {
        let converted = try MozcBridge.convert(span)
        return ConvertResult(replacement: converted, cursorOffset: nil)
    } catch {
        NSLog("[modeless-ime] mozc bridge error: %@", String(describing: error))
        return nil
    }
}

// MARK: - Synthetic key events (for clipboard fallback)

private let kVK_ANSI_C: CGKeyCode = 0x08
private let kVK_ANSI_V: CGKeyCode = 0x09
private let kVK_LeftArrow: CGKeyCode = 0x7B
private let kVK_RightArrow: CGKeyCode = 0x7C

/// All synthetic events are posted into the **session** event tap, not the HID
/// tap. The HID tap re-runs events through low-level transforms — which strips
/// Unicode-only events (virtualKey=0 + setUnicodeString) before they reach
/// Chromium-based apps (Cursor, VSCode, Slack, Electron). The session tap is
/// the insertion point Chromium itself reads from, so events posted there are
/// honored everywhere. This is the same posting location OpenKey ends up at
/// when calling CGEventTapPostEvent from its own tap callback.
private let kPostTap: CGEventTapLocation = .cgSessionEventTap

func postKey(_ keyCode: CGKeyCode, flags: CGEventFlags = []) {
    let src = CGEventSource(stateID: .combinedSessionState)
    if let down = CGEvent(keyboardEventSource: src, virtualKey: keyCode, keyDown: true) {
        down.flags = flags
        down.post(tap: kPostTap)
    }
    if let up = CGEvent(keyboardEventSource: src, virtualKey: keyCode, keyDown: false) {
        up.flags = flags
        up.post(tap: kPostTap)
    }
}

/// Inject a Unicode string as keyboard input — OpenKey's technique.
/// The receiving app treats it as typed text; replaces the active selection
/// (if any) or inserts at the caret. No clipboard involved.
func postUnicode(_ s: String) {
    let src = CGEventSource(stateID: .combinedSessionState)
    let utf16 = Array(s.utf16)
    if let down = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: true) {
        down.flags = []
        down.keyboardSetUnicodeString(stringLength: utf16.count, unicodeString: utf16)
        down.post(tap: kPostTap)
    }
    if let up = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: false) {
        up.flags = []
        up.keyboardSetUnicodeString(stringLength: utf16.count, unicodeString: utf16)
        up.post(tap: kPostTap)
    }
}

func waitForClipboardChange(after baseline: Int, timeoutMs: Int) -> Bool {
    let pb = NSPasteboard.general
    let deadline = Date().addingTimeInterval(Double(timeoutMs) / 1000.0)
    while Date() < deadline {
        if pb.changeCount != baseline { return true }
        Thread.sleep(forTimeInterval: 0.01)
    }
    return false
}

func snapshotClipboard() -> [NSPasteboardItem] {
    let pb = NSPasteboard.general
    guard let items = pb.pasteboardItems else { return [] }
    return items.map { src in
        let copy = NSPasteboardItem()
        for type in src.types {
            if let data = src.data(forType: type) {
                copy.setData(data, forType: type)
            }
        }
        return copy
    }
}

func restoreClipboard(_ items: [NSPasteboardItem]) {
    let pb = NSPasteboard.general
    pb.clearContents()
    if !items.isEmpty {
        pb.writeObjects(items)
    }
}

// MARK: - Clipboard-based fallback (works in any app that supports Cmd+C / Cmd+V)

/// Heuristic: a Cmd+C result that contains a newline or is huge is almost
/// certainly the editor's "copy current line on empty selection" feature
/// (Sublime, VSCode, Cursor, …) — not a real user selection. Treat as bogus.
private func looksLikeLineCopy(_ s: String) -> Bool {
    if s.contains("\n") || s.contains("\r") { return true }
    if s.count > 200 { return true }
    return false
}

func doClipboardPickup() {
    let pb = NSPasteboard.general
    let saved = snapshotClipboard()

    var picked: String? = nil

    // Step 1: peek at the user's current selection (if any) with Cmd+C.
    // Aggressive timeout — apps with a real selection respond in <30ms.
    let baseline = pb.changeCount
    postKey(kVK_ANSI_C, flags: .maskCommand)
    if waitForClipboardChange(after: baseline, timeoutMs: 80),
       let s = pb.string(forType: .string), !s.isEmpty {
        if looksLikeLineCopy(s) {
            NSLog("[modeless-ime] Cmd+C looks like a line-copy (no real selection); will force-select previous word")
        } else {
            picked = s
            NSLog("[modeless-ime] using existing user selection: %@", s)
        }
    }

    // Step 2: no usable selection — force-select previous word, then copy.
    // The selection from Shift+Opt+Left is what we'll replace via Unicode injection.
    var didForceSelect = false
    if picked == nil {
        postKey(kVK_LeftArrow, flags: [.maskShift, .maskAlternate])
        didForceSelect = true
        // Electron apps (Cursor/Slack/VSCode) need a moment for the selection
        // to land before the next Cmd+C is processed by the renderer thread.
        Thread.sleep(forTimeInterval: 0.02)
        let baseline2 = pb.changeCount
        postKey(kVK_ANSI_C, flags: .maskCommand)
        // Slow apps may take well over 100 ms; previous 80 ms timeout occasionally lost.
        if waitForClipboardChange(after: baseline2, timeoutMs: 250),
           let s = pb.string(forType: .string), !s.isEmpty {
            picked = s
        }
    }

    guard var pickedText = picked else {
        NSLog("[modeless-ime] clipboard fallback: nothing to convert (Cmd+C didn't update clipboard in time)")
        // Don't leave the user with a stuck visible selection from our Shift+Opt+Left
        if didForceSelect {
            postKey(kVK_RightArrow)
        }
        restoreClipboard(saved)
        return
    }

    if pickedText.hasSuffix("\n") { pickedText.removeLast() }
    if pickedText.hasSuffix("\r") { pickedText.removeLast() }

    NSLog("[modeless-ime] clipboard pick: %@", pickedText)

    let utf16Len = pickedText.utf16.count
    guard let result = callBackend(text: pickedText, spanStart: 0, spanEnd: utf16Len) else {
        NSLog("[modeless-ime] clipboard fallback: backend returned no result")
        if didForceSelect {
            postKey(kVK_RightArrow)
        }
        restoreClipboard(saved)
        return
    }
    NSLog("[modeless-ime] clipboard replace -> %@", result.replacement)

    // Replace the active selection by injecting the replacement as a Unicode
    // keystroke into the session event tap. No clipboard touch on the replace
    // path → no flicker, no race with the user's clipboard contents.
    postUnicode(result.replacement)

    // Restore the user's original clipboard after the injected event lands.
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
        restoreClipboard(saved)
    }
}

// MARK: - Pickup pipeline

func doPickup() {
    if let field = readFocusedField() {
        let utf16 = Array(field.value.utf16)
        let (start, end): (Int, Int)
        if field.selStart != field.selEnd {
            (start, end) = (field.selStart, field.selEnd)
        } else {
            (start, end) = wordBounds(utf16, caret: field.selStart)
        }
        guard start >= 0, end <= utf16.count, start < end else {
            NSLog("[modeless-ime] empty span; nothing to convert")
            return
        }

        let spanText = String(utf16CodeUnits: Array(utf16[start..<end]), count: end - start)
        NSLog("[modeless-ime] pick [%d..%d] %@", start, end, spanText)

        guard let result = callBackend(text: field.value, spanStart: start, spanEnd: end) else {
            NSLog("[modeless-ime] backend returned no result")
            return
        }
        NSLog("[modeless-ime] replace -> %@", result.replacement)

        if replaceRange(in: field.element, start: start, end: end, replacement: result.replacement) {
            return
        }
        NSLog("[modeless-ime] AX replace failed; falling back to clipboard mode")
    }

    NSLog("[modeless-ime] using clipboard fallback (app does not expose AX text)")
    doClipboardPickup()
}

// MARK: - Global hotkey (CGEventTap)
//
// We use a CGEventTap at the session level instead of Carbon's
// RegisterEventHotKey. Two reasons:
//
//   1. Symmetry with our event posting: we inject through the session tap
//      (kPostTap above) so Chromium/Electron honors synthetic events, and we
//      observe at the same level so our hotkey detection is consistent with
//      what apps actually see.
//   2. We can swallow the original Ctrl+/ keystroke cleanly by returning
//      nil from the callback — apps never see it, no stray "/" inserted.
//
// The callback must return promptly (~1s budget before macOS disables the
// tap), so we dispatch the slow pickup work onto a background queue.

private let kHotkeyTapQueue = DispatchQueue(label: "local.modeless-ime.pickup", qos: .userInitiated)
private var gEventTap: CFMachPort?
private var gRunLoopSource: CFRunLoopSource?

private let tapCallback: CGEventTapCallBack = { _, type, event, _ in
    // macOS can disable the tap under load or after long callbacks — re-enable.
    if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
        if let tap = gEventTap {
            CGEvent.tapEnable(tap: tap, enable: true)
            NSLog("[modeless-ime] event tap re-enabled (was disabled: %d)", type.rawValue)
        }
        return Unmanaged.passUnretained(event)
    }

    guard type == .keyDown else {
        return Unmanaged.passUnretained(event)
    }

    let keyCode = CGKeyCode(event.getIntegerValueField(.keyboardEventKeycode))
    let coreFlags = event.flags.intersection([
        .maskCommand, .maskShift, .maskControl, .maskAlternate
    ])

    // Match Ctrl + / exactly (no Cmd, Shift, or Option).
    if keyCode == HOTKEY_KEYCODE && coreFlags == .maskControl {
        kHotkeyTapQueue.async { doPickup() }
        return nil // swallow — host app must not see the "/"
    }

    return Unmanaged.passUnretained(event)
}

func installEventTap() -> Bool {
    let mask = (1 << CGEventType.keyDown.rawValue)
    guard let tap = CGEvent.tapCreate(
        tap: .cgSessionEventTap,
        place: .headInsertEventTap,
        options: .defaultTap,
        eventsOfInterest: CGEventMask(mask),
        callback: tapCallback,
        userInfo: nil
    ) else {
        return false
    }
    gEventTap = tap
    let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
    gRunLoopSource = source
    CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
    CGEvent.tapEnable(tap: tap, enable: true)
    return true
}

// MARK: - Permissions

func isTrusted(prompt: Bool) -> Bool {
    let key = kAXTrustedCheckOptionPrompt.takeUnretainedValue()
    let opts: NSDictionary = [key: prompt]
    return AXIsProcessTrustedWithOptions(opts as CFDictionary)
}

func describeSelf() {
    let bundle = Bundle.main
    NSLog("[modeless-ime] pid=%d", ProcessInfo.processInfo.processIdentifier)
    NSLog("[modeless-ime] executable=%@", bundle.executablePath ?? "?")
    NSLog("[modeless-ime] bundle id=%@", bundle.bundleIdentifier ?? "(no bundle)")
    NSLog("[modeless-ime] bundle path=%@", bundle.bundlePath)
}

// MARK: - Main

let app = NSApplication.shared
app.setActivationPolicy(.accessory)

describeSelf()

// First, check silently. If not trusted, prompt the user. macOS will add the
// bundle to the Accessibility list at this point.
if !isTrusted(prompt: false) {
    NSLog("[modeless-ime] Not trusted yet — requesting Accessibility permission.")
    NSLog("[modeless-ime] Look for 'ModelessIMEHost' in:")
    NSLog("[modeless-ime]   System Settings → Privacy & Security → Accessibility")
    NSLog("[modeless-ime] If it does NOT appear, click the '+' button and add:")
    NSLog("[modeless-ime]   %@", Bundle.main.bundlePath)
    NSLog("[modeless-ime] Then quit and re-launch this host.")
    _ = isTrusted(prompt: true)
} else {
    NSLog("[modeless-ime] Accessibility: TRUSTED")
}

if !installEventTap() {
    NSLog("[modeless-ime] failed to create event tap — Accessibility permission missing?")
    exit(1)
}

do {
    try MozcBridge.initialize(userProfileDir: MOZC_PROFILE_DIR)
    NSLog("[modeless-ime] mozc bridge initialized (profile=%@)", MOZC_PROFILE_DIR)
} catch {
    NSLog("[modeless-ime] mozc bridge init FAILED: %@", String(describing: error))
    exit(1)
}

NSLog("[modeless-ime] ready: Ctrl+/ anywhere (via session event tap)")
app.run()
