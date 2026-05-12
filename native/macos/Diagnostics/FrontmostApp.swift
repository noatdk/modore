// FrontmostApp — identify the currently focused app for diagnostics.
//
// Most pickup-pipeline failures (AX lookup returns nothing, value isn't a
// String, Cmd+C doesn't update the clipboard, AX replace silently rejects)
// are app-specific. The first triage question is always "which app?". Today
// we log the AX role but not the holder, so the user has to remember /
// reproduce. Capturing the frontmost app at log time turns every
// failure-path entry into one a maintainer can act on without a
// reproduction.
//
// This is pure observation — `NSWorkspace.shared.frontmostApplication` is a
// read-only AppKit property, safe to call from any thread that's already on
// a Cocoa runloop (we only invoke it from the main queue or from the pickup
// background queue, both of which initialize the shared workspace).
//
// Output shape is a short bracketed suffix you can paste straight into a
// `Log.ax(...)` / `Log.pickup(...)` / `Log.clipboard(...)` call. Brackets
// keep it visually distinct from message body and let `grep -F '[Cursor /'`
// pull every event from a single app.

import Cocoa

enum FrontmostApp {

    /// Returns `(localizedName, bundleIdentifier, pid)` of the current
    /// frontmost app, or `nil` if AppKit reports no frontmost (e.g.
    /// Mission Control, app-switcher transitions, or login window).
    ///
    /// `localizedName` is the user-visible name (e.g. "Cursor", "Safari").
    /// `bundleIdentifier` is the reverse-DNS identifier (e.g.
    /// "com.todesktop.230313mzl4w4u92") — necessary because two apps can
    /// share a display name (Microsoft Office variants, custom Electron
    /// shells) and the bundle id is the only reliable way to pin which.
    /// `pid` lets clipboard-fallback gestures verify that the focused app
    /// is still the same process that received the original injection,
    /// even if the user has briefly switched away and back.
    static func describe() -> (name: String, bundleID: String, pid: pid_t)? {
        guard let app = NSWorkspace.shared.frontmostApplication else {
            return nil
        }
        let name = app.localizedName
            ?? app.bundleURL?.deletingPathExtension().lastPathComponent
            ?? "?"
        let bundleID = app.bundleIdentifier ?? "?"
        return (name: name, bundleID: bundleID, pid: app.processIdentifier)
    }

    /// One-shot string suitable for direct interpolation in a log line:
    ///
    ///     Log.ax("value lookup failed on role=\(role): \(code) \(FrontmostApp.logSuffix())")
    ///
    /// renders as:
    ///
    ///     [ax] value lookup failed on role=AXTextField: -25212 [Cursor / com.todesktop.230313mzl4w4u92]
    ///
    /// Always prefixed with a leading space so the call sites don't need to
    /// remember to add one.
    static func logSuffix() -> String {
        if let info = describe() {
            return " [\(info.name) / \(info.bundleID)]"
        }
        return " [no frontmost app]"
    }

    /// Pid of the frontmost app, or `nil` when AppKit reports no
    /// frontmost. Convenience for callers (clipboard-session cycle/undo)
    /// that only need the pid for identity comparison.
    static func currentPid() -> pid_t? {
        return NSWorkspace.shared.frontmostApplication?.processIdentifier
    }
}
