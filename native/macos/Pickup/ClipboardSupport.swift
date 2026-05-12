// Clipboard helpers and a modifier-release wait used by the clipboard
// fallback path in `Pickup.swift`. The AX path never touches any of this;
// these only run when modore couldn't read/write the focused field via AX
// and has to fall back to Cmd+C / Cmd+V style injection.

import Cocoa

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

