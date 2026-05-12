// Logging facade for the macOS host.
//
// Each subsystem has its own entry point (`Log.boot`, `Log.config`, …). The
// single private `write` function is the only place that decides format,
// destination, or level — touch it, not the call sites.
//
// Use `Log.tagged(_:_:)` for one-off or experimental tags; if a tagged() call
// recurs enough to look like a subsystem, promote it to a named function here.

import Foundation

enum Log {
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

    static func tagged(_ tag: String, _ message: String) { write(tag, message) }

    private static func write(_ tag: String, _ message: String) {
        NSLog("[%@] %@", tag, message)
    }
}
