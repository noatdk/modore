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

    static func boot(_ message: String)      { write("boot",      message) }
    static func config(_ message: String)    { write("config",    message) }
    static func ax(_ message: String)        { write("ax",        message) }
    static func hotkey(_ message: String)    { write("hotkey",    message) }
    static func pickup(_ message: String)    { write("pickup",    message) }
    static func clipboard(_ message: String) { write("clipboard", message) }
    static func mozc(_ message: String)      { write("mozc",      message) }
    static func secureInput(_ message: String) { write("secure-input", message) }
    static func undo(_ message: String)        { write("undo",         message) }
    static func cycle(_ message: String)       { write("cycle",        message) }
    static func panel(_ message: String)        { write("panel",        message) }

    static func tagged(_ tag: String, _ message: String) { write(tag, message) }

    private static let loggersLock = NSLock()
    private static var loggers: [String: Logger] = [:]

    private static func logger(for tag: String) -> Logger {
        loggersLock.lock(); defer { loggersLock.unlock() }
        if let existing = loggers[tag] { return existing }
        let made = Logger(subsystem: subsystem, category: tag)
        loggers[tag] = made
        return made
    }

    private static func write(_ tag: String, _ message: String) {
        // %{public}@ keeps the message readable in `log show`; without it the
        // unified-logging redactor would replace dynamic strings with <private>.
        logger(for: tag).log("\(message, privacy: .public)")
    }
}
