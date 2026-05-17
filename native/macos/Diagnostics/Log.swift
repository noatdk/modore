// Logging facade for the macOS host.
//
// Each subsystem has its own entry point (`Log.boot`, `Log.config`, …). The
// single private `write` function is the only place that decides format,
// destination, or level — touch it, not the call sites.
//
// Use `Log.tagged(_:_:)` for one-off or experimental tags; if a tagged() call
// recurs enough to look like a subsystem, promote it to a named function here.
//
// Backed by `os.Logger` with subsystem "com.modore.host" so `log show` /
// `log stream` can filter to just our output via
//   --predicate 'subsystem == "com.modore.host"'
// instead of dragging in AppKit / XPC / CoreAnalytics noise. Each tag becomes
// the Logger's category. Messages are passed as the public string format so
// they aren't redacted as `<private>` in unified logging.

import Foundation
import os

enum Log {
    static let subsystem = "com.modore.host"

    static func configureDisabledNamespaces(_ namespaces: ModoreConfig.LoggingNamespaceMask) {
        disabledNamespacesLock.lock()
        disabledNamespaces = namespaces
        disabledNamespacesLock.unlock()
    }

    static func boot(_ message: @autoclosure () -> String)        { write("boot",        message) }
    static func config(_ message: @autoclosure () -> String)      { write("config",      message) }
    static func ax(_ message: @autoclosure () -> String)          { write("ax",          message) }
    static func hotkey(_ message: @autoclosure () -> String)      { write("hotkey",      message) }
    static func pickup(_ message: @autoclosure () -> String)      { write("pickup",      message) }
    static func clipboard(_ message: @autoclosure () -> String)   { write("clipboard",   message) }
    static func mozc(_ message: @autoclosure () -> String)        { write("mozc",        message) }
    static func secureInput(_ message: @autoclosure () -> String) { write("secure-input", message) }
    static func undo(_ message: @autoclosure () -> String)        { write("undo",        message) }
    static func cycle(_ message: @autoclosure () -> String)       { write("cycle",       message) }
    static func panel(_ message: @autoclosure () -> String)       { write("panel",       message) }

    static func tagged(_ tag: String, _ message: @autoclosure () -> String) {
        write(tag, message)
    }

    private static let loggersLock = NSLock()
    private static var loggers: [String: Logger] = [:]
    private static let disabledNamespacesLock = NSLock()
    private static var disabledNamespaces: ModoreConfig.LoggingNamespaceMask = []

    private static func logger(for tag: String) -> Logger {
        loggersLock.lock(); defer { loggersLock.unlock() }
        if let existing = loggers[tag] { return existing }
        let made = Logger(subsystem: subsystem, category: tag)
        loggers[tag] = made
        return made
    }

    private static func write(_ tag: String, _ message: () -> String) {
        guard isEnabled(tag: tag) else { return }
        // %{public}@ keeps the message readable in `log show`; without it the
        // unified-logging redactor would replace dynamic strings with <private>.
        let rendered = message()
        logger(for: tag).log("\(rendered, privacy: .public)")
    }

    private static func isEnabled(tag: String) -> Bool {
        guard let ns = namespaceMask(for: tag) else { return true }
        disabledNamespacesLock.lock()
        let disabled = disabledNamespaces
        disabledNamespacesLock.unlock()
        return !disabled.contains(ns)
    }

    private static func namespaceMask(for tag: String) -> ModoreConfig.LoggingNamespaceMask? {
        let root: Substring
        if let colon = tag.firstIndex(of: ":") {
            root = tag[..<colon]
        } else {
            root = Substring(tag)
        }
        switch root {
        case "boot": return .boot
        case "config": return .config
        case "ax": return .ax
        case "hotkey": return .hotkey
        case "pickup": return .pickup
        case "clipboard": return .clipboard
        case "mozc": return .mozc
        case "secure-input": return .secureInput
        case "undo": return .undo
        case "cycle": return .cycle
        case "panel": return .panel
        case "unicode": return .unicode
        case "scripting": return .scripting
        default: return nil
        }
    }
}
