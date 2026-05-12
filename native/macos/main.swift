// modore — macOS native host.
//
// Runs as a menu-bar/accessory app, registers a global conversion hotkey
// (default Ctrl+/; configurable in ~/.config/modore/modore.conf), reads
// the focused text field (any app) via Accessibility, hands the picked span
// to the in-process Mozc engine, and writes the result back.
//
// Build:  make -C native/macos
// Run:    open native/macos/build/modore.app
// First run prompts for Accessibility permission. Grant it in
//   System Settings → Privacy & Security → Accessibility, then re-launch.

import Cocoa
import Carbon
import ApplicationServices

// MARK: - Config (defaults; overridden by ~/.config/modore/modore.conf — see ModoreConfig.swift)

// Where the mozc engine keeps its on-disk state (user dictionary, history).
// Bootstrapping this from a user's existing Google Japanese Input / OSS Mozc
// profile is a follow-up task.
let MOZC_PROFILE_DIR: String = {
    let home = FileManager.default.homeDirectoryForCurrentUser.path
    return "\(home)/Library/Application Support/modore"
}()

// Populated from ~/.config/modore/modore.conf before the event tap is installed.
// Read by the tap callback on the main thread; written by the main thread on
// startup and on watcher-driven reloads, so a plain swap is race-free.
private var gConversionKeyCode: CGKeyCode = CGKeyCode(kVK_ANSI_Slash)
private var gConversionCoreFlags: CGEventFlags = .maskControl

/// True when the Carbon hotkey grab is active. The tap callback consults
/// this to decide whether to also match-and-swallow the chord. If Carbon
/// succeeded, the OS consumes the keystroke before the tap sees it anyway;
/// gating the tap path defensively avoids a double-fire if that ever changes.
private var gUsingCarbonHotkey: Bool = false

/// Held for the lifetime of the process. `nil` if RegisterEventHotKey failed
/// at startup (we then rely on the tap-based detector).
private var gCarbonHotkey: CarbonHotkey?

/// Live clipboard-fallback timings. Written on the main thread (startup and
/// watcher-driven reloads); read by `doClipboardPickup` on a background queue
/// via a snapshot copy at function entry, so a plain swap is race-free.
private var gClipboardTimings = ModoreConfig.ClipboardTimings()

private func applyConversionHotkeyChord(_ chord: ModoreConfig.ConversionHotkey) {
    gConversionKeyCode = chord.keyCode
    gConversionCoreFlags = chord.coreFlags
    if let ck = gCarbonHotkey {
        let ok = ck.register(chord)
        gUsingCarbonHotkey = ok
        if !ok {
            Log.hotkey("RegisterEventHotKey failed — falling back to tap-based detection")
        }
    }
}

func applyConversionHotkeyReload() {
    let prev = ModoreConfig.ConversionHotkey(
        keyCode: gConversionKeyCode, coreFlags: gConversionCoreFlags)
    switch ModoreConfig.loadConversionHotkeyOutcome() {
    case .loaded(let next, let source):
        if next != prev {
            applyConversionHotkeyChord(next)
            Log.config("reloaded \(source)")
        }
    case .usingDefault(let reason):
        let def = ModoreConfig.defaultConversionHotkey()
        if def != prev {
            applyConversionHotkeyChord(def)
            Log.config("reload: \(reason) — reverted to default")
        }
    case .invalid(let reason):
        Log.config("reload rejected: \(reason) — keeping previous hotkey")
    }
}

/// Reload `[clipboard]` timings from disk. Logs only on actual change so a
/// no-op reload (e.g. user edited an unrelated section) stays quiet.
func applyClipboardTimingsReload() {
    let next = ModoreConfig.loadClipboardTimings()
    if next != gClipboardTimings {
        gClipboardTimings = next
        Log.config("clipboard timings: pre_copy=\(next.preCopyDelayMs)ms"
                 + " read_timeout=\(next.readTimeoutMs)ms"
                 + " restore=\(next.restoreClipboardDelayMs)ms")
    }
}

/// Single entry point for the config watcher — reloads every section.
func applyConfigReload() {
    applyConversionHotkeyReload()
    applyClipboardTimingsReload()
}

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
        Log.ax("focused-element lookup failed: \(r1.rawValue)")
        if r1 == .apiDisabled || r1 == .cannotComplete {
            Log.ax("appears disabled for this process; restart the host after granting permission")
        }
        return nil
    }
    guard let focused = focusedRef else {
        Log.ax("returned no focused element")
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
        Log.ax("value lookup failed on role=\(role): \(r2.rawValue)")
        return nil
    }
    guard let s = valueRef as? String else {
        Log.ax("value on role=\(role) is not a String (type=\(String(describing: type(of: valueRef!))))")
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
        Log.mozc("bridge error: \(String(describing: error))")
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

/// Sentinel `CGEvent.location` we stamp on every synthetic event so the same
/// process's tap callback can recognize and skip events it emitted itself
/// (otherwise a future "synth a hotkey" feature would loop).
private let kSyntheticEventMarker = CGPoint(x: -27469, y: 0)

/// Stamp the marker on a freshly created event. Call before `post(tap:)`.
private func markSynthetic(_ event: CGEvent) {
    event.location = kSyntheticEventMarker
}

/// True if `event` was emitted by this process via `markSynthetic`.
private func isSynthetic(_ event: CGEvent) -> Bool {
    let loc = event.location
    return abs(loc.x - kSyntheticEventMarker.x) < 0.001
        && abs(loc.y - kSyntheticEventMarker.y) < 0.001
}

func postKey(_ keyCode: CGKeyCode, flags: CGEventFlags = []) {
    let src = CGEventSource(stateID: .combinedSessionState)
    if let down = CGEvent(keyboardEventSource: src, virtualKey: keyCode, keyDown: true) {
        down.flags = flags
        markSynthetic(down)
        down.post(tap: kPostTap)
    }
    if let up = CGEvent(keyboardEventSource: src, virtualKey: keyCode, keyDown: false) {
        up.flags = flags
        markSynthetic(up)
        up.post(tap: kPostTap)
    }
}

/// Inject a Unicode string as keyboard input — OpenKey's technique.
/// The receiving app treats it as typed text; replaces the active selection
/// (if any) or inserts at the caret. No clipboard involved.
///
/// `CGEventKeyboardSetUnicodeString` silently truncates payloads longer than
/// 20 UTF-16 code units (undocumented macOS limit). We chunk to defeat it.
private let kUnicodeChunkMax = 20

func postUnicode(_ s: String) {
    let src = CGEventSource(stateID: .combinedSessionState)
    let utf16 = Array(s.utf16)
    guard !utf16.isEmpty else { return }

    var i = 0
    while i < utf16.count {
        let end = min(i + kUnicodeChunkMax, utf16.count)
        let chunk = Array(utf16[i..<end])
        if let down = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: true) {
            down.flags = []
            down.keyboardSetUnicodeString(stringLength: chunk.count, unicodeString: chunk)
            markSynthetic(down)
            down.post(tap: kPostTap)
        }
        if let up = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: false) {
            up.flags = []
            up.keyboardSetUnicodeString(stringLength: chunk.count, unicodeString: chunk)
            markSynthetic(up)
            up.post(tap: kPostTap)
        }
        i = end
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

/// Returns snapshot of the current clipboard and a closure that restores it after
/// `delayMs`. Intended use:
///
///     let (saved, restore) = guardClipboard()
///     defer { restore() }
///
/// `defer` guarantees the restore runs on every exit path, including
/// early-returns and thrown errors.
func guardClipboard(restoreDelayMs: Int = 50) -> (saved: [NSPasteboardItem], restore: () -> Void) {
    let saved = snapshotClipboard()
    let restore: () -> Void = {
        if restoreDelayMs > 0 {
            Thread.sleep(forTimeInterval: Double(restoreDelayMs) / 1000.0)
        }
        restoreClipboard(saved)
    }
    return (saved, restore)
}

/// Block until Cmd/Ctrl/Shift/Option are all released, or `timeoutMs` elapses.
/// Synthetic events inherit the *physical* modifier state of the user, so
/// firing Cmd+C while Ctrl is still held becomes Ctrl+Cmd+C — which most
/// apps won't honor as "copy".
///
/// Caller is expected to be on a background queue (we sleep here).
func waitForModifiersToClear(timeoutMs: Int = 3000) {
    let conflicting: CGEventFlags = [
        .maskShift, .maskControl, .maskCommand, .maskAlternate
    ]
    let deadline = Date().addingTimeInterval(Double(timeoutMs) / 1000.0)
    let start = Date()
    while Date() < deadline {
        let flags = CGEventSource.flagsState(.combinedSessionState)
        if flags.intersection(conflicting).isEmpty {
            let waitedMs = Int(Date().timeIntervalSince(start) * 1000)
            if waitedMs > 0 {
                Log.clipboard("waited \(waitedMs)ms for modifier release")
            }
            return
        }
        Thread.sleep(forTimeInterval: 0.02)
    }
    Log.clipboard("modifier-release wait hit \(timeoutMs)ms timeout; injecting anyway")
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
    // Snapshot timings once at entry so the whole pickup runs against a
    // consistent set even if the watcher fires mid-flight on the main thread.
    let timings = gClipboardTimings

    // Snapshot the user's clipboard now; `defer` guarantees we restore it on
    // every exit path. No manual cleanup at the early-returns below.
    let (_, restoreSavedClipboard) = guardClipboard(restoreDelayMs: timings.restoreClipboardDelayMs)
    defer { restoreSavedClipboard() }

    // The user is almost certainly still holding the conversion hotkey's
    // modifiers when we get here. Synthesizing Cmd+C now would land as
    // (heldModifiers)+Cmd+C and silently no-op in many apps.
    waitForModifiersToClear()

    let pb = NSPasteboard.general
    var picked: String? = nil

    // Step 1: peek at the user's current selection (if any) with Cmd+C.
    // Aggressive timeout — apps with a real selection respond in <30ms.
    // This is a heuristic threshold (no selection → fall through fast), not
    // a tuning knob, so it stays hard-coded.
    let baseline = pb.changeCount
    postKey(kVK_ANSI_C, flags: .maskCommand)
    if waitForClipboardChange(after: baseline, timeoutMs: 80),
       let s = pb.string(forType: .string), !s.isEmpty {
        if looksLikeLineCopy(s) {
            Log.clipboard("Cmd+C looks like a line-copy (no real selection); will force-select previous word")
        } else {
            picked = s
            Log.clipboard("using existing user selection: \(s)")
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
        // User-tunable via [clipboard] pre_copy_delay_ms.
        if timings.preCopyDelayMs > 0 {
            Thread.sleep(forTimeInterval: Double(timings.preCopyDelayMs) / 1000.0)
        }
        let baseline2 = pb.changeCount
        postKey(kVK_ANSI_C, flags: .maskCommand)
        // Slow apps / heavy load may need more than the default 250 ms.
        // User-tunable via [clipboard] read_timeout_ms.
        if waitForClipboardChange(after: baseline2, timeoutMs: timings.readTimeoutMs),
           let s = pb.string(forType: .string), !s.isEmpty {
            picked = s
        }
    }

    guard var pickedText = picked else {
        Log.clipboard("nothing to convert (Cmd+C didn't update clipboard in time)")
        // Don't leave the user with a stuck visible selection from our Shift+Opt+Left
        if didForceSelect {
            postKey(kVK_RightArrow)
        }
        return
    }

    if pickedText.hasSuffix("\n") { pickedText.removeLast() }
    if pickedText.hasSuffix("\r") { pickedText.removeLast() }

    Log.clipboard("pick: \(pickedText)")

    let utf16Len = pickedText.utf16.count
    guard let result = callBackend(text: pickedText, spanStart: 0, spanEnd: utf16Len) else {
        Log.clipboard("backend returned no result")
        if didForceSelect {
            postKey(kVK_RightArrow)
        }
        return
    }
    Log.clipboard("replace -> \(result.replacement)")

    // Replace the active selection by injecting the replacement as a Unicode
    // keystroke into the session event tap. No clipboard touch on the replace
    // path → no flicker, no race with the user's clipboard contents.
    postUnicode(result.replacement)
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
            Log.pickup("empty span; nothing to convert")
            return
        }

        let spanText = String(utf16CodeUnits: Array(utf16[start..<end]), count: end - start)
        Log.pickup("pick [\(start)..\(end)] \(spanText)")

        guard let result = callBackend(text: field.value, spanStart: start, spanEnd: end) else {
            Log.pickup("backend returned no result")
            return
        }
        Log.pickup("replace -> \(result.replacement)")

        if replaceRange(in: field.element, start: start, end: end, replacement: result.replacement) {
            return
        }
        Log.pickup("AX replace failed; falling back to clipboard mode")
    }

    Log.pickup("using clipboard fallback (app does not expose AX text)")
    doClipboardPickup()
}

// MARK: - Global hotkey (CGEventTap, fallback to CarbonHotkey)
//
// Primary delivery is Carbon's `RegisterEventHotKey` (see CarbonHotkey.swift)
// — it's OS-grabbed, so the focused app never sees the keystroke and the
// delivery path doesn't depend on a CGEventTap that macOS can timeout-disable
// under load.
//
// We still install this tap as:
//
//   1. A fallback hotkey detector if Carbon registration fails (chord already
//      claimed by another app, etc.).
//   2. The self-event filter for synthesized CGEvents (markSynthetic /
//      isSynthetic). The marker is cheap insurance against any future feature
//      that synthesizes the conversion chord.
//
// The callback must return promptly (~1s budget before macOS disables the
// tap), so we dispatch the slow pickup work onto a background queue.

private let kHotkeyTapQueue = DispatchQueue(label: "local.modore.pickup", qos: .userInitiated)
private var gEventTap: CFMachPort?
private var gRunLoopSource: CFRunLoopSource?

private let tapCallback: CGEventTapCallBack = { _, type, event, _ in
    // macOS can disable the tap under load or after long callbacks — re-enable.
    if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
        if let tap = gEventTap {
            CGEvent.tapEnable(tap: tap, enable: true)
            Log.hotkey("event tap re-enabled (was disabled: \(type.rawValue))")
        }
        return Unmanaged.passUnretained(event)
    }

    guard type == .keyDown else {
        return Unmanaged.passUnretained(event)
    }

    // Events we synthesized via `markSynthetic` pass straight through. Without
    // this, any future feature that synths a key matching the conversion
    // chord would re-enter doPickup and loop.
    if isSynthetic(event) {
        return Unmanaged.passUnretained(event)
    }

    // When Carbon owns the hotkey the OS consumes the keystroke before we
    // see it here, so this branch is unreachable in the common case. We
    // still gate defensively in case that ever stops being true (e.g.
    // newer macOS versions changing tap ordering).
    if !gUsingCarbonHotkey {
        let keyCode = CGKeyCode(event.getIntegerValueField(.keyboardEventKeycode))
        let coreFlags = event.flags.intersection([
            .maskCommand, .maskShift, .maskControl, .maskAlternate
        ])

        if keyCode == gConversionKeyCode && coreFlags == gConversionCoreFlags {
            kHotkeyTapQueue.async { doPickup() }
            return nil // swallow — host app must not see the "/"
        }
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
    Log.boot("pid=\(ProcessInfo.processInfo.processIdentifier)")
    Log.boot("executable=\(bundle.executablePath ?? "?")")
    Log.boot("bundle id=\(bundle.bundleIdentifier ?? "(no bundle)")")
    Log.boot("bundle path=\(bundle.bundlePath)")
}

// MARK: - --check-config (preflight)
//
// Parse-before-swap as a user-facing feature, not just a runtime invariant:
// validate the config without starting the engine. Parses both sections,
// prints each one's outcome to stdout, and exits non-zero on a hard error so
// callers like `pre-commit` hooks can fail loudly. Same code path the watcher
// uses on a live reload — what `--check-config` accepts is exactly what the
// running host would accept.

/// Run preflight config validation, print a human-readable report to stdout,
/// and exit. Exit code: 0 on a healthy load (including "no config, defaults
/// will be used"), 1 if the file contains a `[conversion] hotkey` that
/// doesn't parse, 2 if any `[clipboard]` key is rejected.
func runConfigCheck() -> Never {
    let url = ModoreConfig.configFileURL()
    print("config path: \(url.path)")

    var exit_code: Int32 = 0
    switch ModoreConfig.loadConversionHotkeyOutcome() {
    case .loaded(_, let source):
        print("  [conversion]  ok      \(source)")
    case .usingDefault(let reason):
        print("  [conversion]  default \(reason)")
    case .invalid(let reason):
        print("  [conversion]  INVALID \(reason)")
        exit_code = 1
    }

    let (timings, issues) = ModoreConfig.parseClipboardTimings()
    print("  [clipboard]   pre_copy=\(timings.preCopyDelayMs)ms"
        + " read_timeout=\(timings.readTimeoutMs)ms"
        + " restore=\(timings.restoreClipboardDelayMs)ms")
    for issue in issues {
        print("                \(issue)")
    }
    if !issues.isEmpty && exit_code == 0 {
        exit_code = 2
    }

    exit(exit_code)
}

if CommandLine.arguments.contains("--check-config") {
    runConfigCheck()
}

// MARK: - Main

let app = NSApplication.shared
app.setActivationPolicy(.accessory)

describeSelf()

let modoreHotkey = ModoreConfig.loadConversionHotkey()
gConversionKeyCode = modoreHotkey.keyCode
gConversionCoreFlags = modoreHotkey.coreFlags

gClipboardTimings = ModoreConfig.loadClipboardTimings()
Log.config("clipboard timings: pre_copy=\(gClipboardTimings.preCopyDelayMs)ms"
         + " read_timeout=\(gClipboardTimings.readTimeoutMs)ms"
         + " restore=\(gClipboardTimings.restoreClipboardDelayMs)ms")

// First, check silently. If not trusted, prompt the user. macOS will add the
// bundle to the Accessibility list at this point.
if !isTrusted(prompt: false) {
    Log.ax("not trusted yet — requesting Accessibility permission.")
    Log.ax("look for 'modore' in:")
    Log.ax("  System Settings → Privacy & Security → Accessibility")
    Log.ax("if it does NOT appear, click the '+' button and add:")
    Log.ax("  \(Bundle.main.bundlePath)")
    Log.ax("then quit and re-launch this host.")
    _ = isTrusted(prompt: true)
} else {
    Log.ax("trusted")
}

if !installEventTap() {
    Log.hotkey("failed to create event tap — Accessibility permission missing?")
    exit(1)
}

// Install Carbon hotkey grab. If it succeeds the tap's hotkey-match branch
// becomes a no-op (gated by `gUsingCarbonHotkey`); if it fails we fall back
// to the tap's existing behavior with no functional change for the user.
gCarbonHotkey = CarbonHotkey {
    kHotkeyTapQueue.async { doPickup() }
}
if gCarbonHotkey?.register(modoreHotkey) == true {
    gUsingCarbonHotkey = true
    Log.hotkey("Carbon hotkey registered — using RegisterEventHotKey for delivery")
} else {
    Log.hotkey("Carbon hotkey unavailable — using CGEventTap fallback")
}

// Held for the lifetime of the process; tears down its DispatchSource on deinit.
let gConfigWatcher = ConfigWatcher(path: ModoreConfig.configFileURL().path) {
    applyConfigReload()
}
gConfigWatcher.start()

do {
    try MozcBridge.initialize(userProfileDir: MOZC_PROFILE_DIR)
    Log.mozc("bridge initialized (profile=\(MOZC_PROFILE_DIR))")
} catch {
    Log.mozc("bridge init FAILED: \(String(describing: error))")
    exit(1)
}

Log.boot("ready: conversion hotkey installed (see ~/.config/modore/modore.conf)")
app.run()
