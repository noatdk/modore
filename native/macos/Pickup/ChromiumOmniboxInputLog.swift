// Chromium omnibox typed-input log.
//
// We keep a best-effort record of what the user has actually typed while the
// Chrome omnibox is focused. On pickup, the recorded typed span tells us
// whether the current selection is inside the user's own input or is Chrome's
// autocomplete tail.

import ApplicationServices
import Cocoa

let kChromiumOmniboxBundleIDs: Set<String> = [
    "com.google.Chrome",
    "com.google.Chrome.canary",
    "org.chromium.Chromium",
]

func isChromiumOmnibox(field: FocusedField, appId: String?) -> Bool {
    guard let appId, kChromiumOmniboxBundleIDs.contains(appId) else { return false }
    return field.role == kAXTextFieldRole as String
        && field.description == "Address and search bar"
}

private let gChromiumOmniboxTypedInputLock = NSLock()
private var gChromiumOmniboxTypedInput: String = ""
private let gChromiumOmniboxFocusLock = NSLock()
private var gChromiumOmniboxFocusedElement: AXUIElement?

func chromiumOmniboxTypedInputSnapshot() -> String? {
    gChromiumOmniboxTypedInputLock.lock()
    defer { gChromiumOmniboxTypedInputLock.unlock() }
    return gChromiumOmniboxTypedInput.isEmpty ? nil : gChromiumOmniboxTypedInput
}

func chromiumOmniboxSelectionMatchesTypedInput(
    field: FocusedField,
    start: Int,
    end: Int
) -> Bool {
    guard let typed = chromiumOmniboxTypedInputSnapshot() else { return false }
    guard start >= 0, end <= field.value.utf16.count, start < end else { return false }
    guard let selectedText = sliceUTF16(field.value, start: start, end: end) else {
        return false
    }
    return typed.contains(selectedText)
}

private func chromiumOmniboxTypedInputClear() {
    gChromiumOmniboxTypedInputLock.lock()
    gChromiumOmniboxTypedInput.removeAll(keepingCapacity: true)
    gChromiumOmniboxTypedInputLock.unlock()
}

private func chromiumOmniboxHandleFocusChange(_ field: FocusedField) {
    gChromiumOmniboxFocusLock.lock()
    let sameElement = gChromiumOmniboxFocusedElement.map { CFEqual($0, field.element) } ?? false
    if sameElement {
        gChromiumOmniboxFocusLock.unlock()
        return
    }
    gChromiumOmniboxFocusedElement = field.element
    gChromiumOmniboxFocusLock.unlock()
    chromiumOmniboxTypedInputClear()
}

private func chromiumOmniboxTypedInputAppend(_ text: String) {
    guard !text.isEmpty else { return }
    gChromiumOmniboxTypedInputLock.lock()
    gChromiumOmniboxTypedInput += text
    gChromiumOmniboxTypedInputLock.unlock()
}

private func chromiumOmniboxTypedInputBackspace() {
    gChromiumOmniboxTypedInputLock.lock()
    defer { gChromiumOmniboxTypedInputLock.unlock() }
    guard !gChromiumOmniboxTypedInput.isEmpty else { return }
    gChromiumOmniboxTypedInput.removeLast()
}

private func typedString(from event: CGEvent) -> String? {
    var actualLength = 0
    event.keyboardGetUnicodeString(
        maxStringLength: 0,
        actualStringLength: &actualLength,
        unicodeString: nil)
    guard actualLength > 0 else { return nil }
    var buffer = [UniChar](repeating: 0, count: actualLength)
    var copiedLength = 0
    buffer.withUnsafeMutableBufferPointer { ptr in
        event.keyboardGetUnicodeString(
            maxStringLength: actualLength,
            actualStringLength: &copiedLength,
            unicodeString: ptr.baseAddress)
    }
    guard copiedLength > 0 else { return nil }
    return String(utf16CodeUnits: buffer, count: copiedLength)
}

func updateChromiumOmniboxTypedInputLog(for event: CGEvent) {
    guard let appId = FrontmostApp.describe()?.bundleID,
          kChromiumOmniboxBundleIDs.contains(appId) else {
        chromiumOmniboxTypedInputClear()
        return
    }
    guard let field = readFocusedField(), isChromiumOmnibox(field: field, appId: appId) else {
        chromiumOmniboxTypedInputClear()
        gChromiumOmniboxFocusLock.lock()
        gChromiumOmniboxFocusedElement = nil
        gChromiumOmniboxFocusLock.unlock()
        return
    }

    chromiumOmniboxHandleFocusChange(field)

    let coreFlags = event.flags.intersection([
        .maskCommand, .maskShift, .maskControl, .maskAlternate
    ])
    if coreFlags.contains(.maskCommand) || coreFlags.contains(.maskControl) || coreFlags.contains(.maskAlternate) {
        return
    }

    let keyCode = CGKeyCode(event.getIntegerValueField(.keyboardEventKeycode))
    if keyCode == kVK_Backspace {
        chromiumOmniboxTypedInputBackspace()
        return
    }
    if keyCode == kVK_Escape {
        return
    }

    if let typed = typedString(from: event), !typed.isEmpty {
        chromiumOmniboxTypedInputAppend(typed)
    }
}
