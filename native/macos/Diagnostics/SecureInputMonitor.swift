// SecureInputMonitor — watch for macOS Secure Keyboard Entry.
//
// Any process can call `EnableSecureEventInput` (Terminal/iTerm while a sudo
// or password prompt is on screen; password fields in 1Password, Bitwarden,
// Safari banking pages; the Lock Screen; Touch ID prompts; FileVault). While
// it's active, the OS routes keystrokes through a SecureInput-only HID path
// that bypasses both our CGEventTap and any synthetic CGEventPost we make
// into the session tap. From the user's perspective the hotkey just silently
// stops working — they have no way to tell if modore crashed, lost
// Accessibility, or is being blocked by a transient password field.
//
// Detection: macOS exposes the holding pid in the IORegistry under
//   root entry → "IOConsoleUsers" array → element with key
//   "kCGSSessionSecureInputPID". The constant strings are stable and have
//   worked since macOS 10.x (originally documented by the MagicKeys
//   project); both Terminal and password fields populate this key.
//
// Polling cadence: 3 s when idle (cheap insurance), 1 s while acquired so we
// can clear the menu-bar warning quickly once the user finishes typing their
// password. The IORegistry query is a handful of CFNumber lookups, so this
// is well under any reasonable budget even at 1 Hz.
//
// Transition reporting: callback fires only on edges (acquired-by-pidA →
// acquired-by-pidB also counts as an edge), never on every tick. Same pid
// repeated → no notify (de-duplication on the holder pid).

import Foundation
import IOKit
import Darwin

enum SecureInputState: Equatable {
    case clear
    case blocked(appName: String, appPath: String, pid: pid_t)
}

final class SecureInputMonitor {

    // Visible for tests / future debug CLI; never call from the timer queue.
    static func currentHolderPid() -> pid_t? {
        let root = IORegistryGetRootEntry(kIOMainPortDefault)
        guard root != 0 else { return nil }
        defer { IOObjectRelease(root) }

        // Property is "IOConsoleUsers": a CFArray of CFDictionary, one entry
        // per console session. Each dict *may* contain
        // "kCGSSessionSecureInputPID". Absent / non-numeric / ≤ 0 → nobody
        // is holding SecureInput.
        //
        // Bridging note: we deliberately use `NSArray`/`NSDictionary` rather
        // than `[[String: Any]]` here. Swift's CFArray → [Any] bridge does
        // not recurse into CFDictionary elements, so a one-shot `as? [[String:
        // Any]]` cast silently returns nil even when the array does contain
        // dicts. NSArray / case-as iteration is the documented escape hatch.
        let raw = IORegistryEntryCreateCFProperty(
            root, "IOConsoleUsers" as CFString, kCFAllocatorDefault, 0)
        guard let array = raw?.takeRetainedValue() as? NSArray else {
            return nil
        }
        for case let session as NSDictionary in array {
            if let pidNum = session["kCGSSessionSecureInputPID"] as? NSNumber {
                let pid = pid_t(truncatingIfNeeded: pidNum.intValue)
                if pid > 0 {
                    return pid
                }
            }
        }
        return nil
    }

    /// Resolve a pid to (displayName, bundlePath). Returns nil if the process
    /// has already exited or its path can't be read. The display name is the
    /// "<X>" in `/.../<X>.app/...` when present (matches what users see in
    /// Dock / Finder); otherwise the raw exec path.
    static func describeProcess(pid: pid_t) -> (name: String, path: String)? {
        // PROC_PIDPATHINFO_MAXSIZE is 4096 in the system headers; using a
        // smaller buffer makes proc_pidpath fail silently.
        var buffer = [CChar](repeating: 0, count: 4096)
        let ret = buffer.withUnsafeMutableBufferPointer { bufPtr -> Int32 in
            proc_pidpath(pid, bufPtr.baseAddress, UInt32(bufPtr.count))
        }
        guard ret > 0 else { return nil }
        let path = String(cString: buffer)
        guard !path.isEmpty else { return nil }
        // Prefer the bundle name (`/.../Terminal.app/...` → `Terminal`);
        // fall back to just the executable basename rather than the full
        // path, so an `/usr/bin/osascript` shows up as `osascript` in the
        // menu bar instead of an unsightly absolute path.
        let name = appNameFromPath(path)
            ?? URL(fileURLWithPath: path).lastPathComponent
        return (name: name, path: path)
    }

    /// Pull the bundle display name out of a path like
    /// `/Applications/Terminal.app/Contents/MacOS/Terminal`. We accept both
    /// `.app` and `.bundle` (some system frameworks ship as `.bundle`).
    private static func appNameFromPath(_ path: String) -> String? {
        for suffix in [".app", ".bundle"] {
            if let range = path.range(of: "/[^/]+\(suffix)/", options: .regularExpression) {
                var name = String(path[range])
                if name.hasPrefix("/") { name.removeFirst() }
                if name.hasSuffix("/") { name.removeLast() }
                if name.hasSuffix(suffix) { name.removeLast(suffix.count) }
                return name
            }
        }
        return nil
    }

    // MARK: - Instance

    private let queue: DispatchQueue
    private var timer: DispatchSourceTimer?
    private var lastState: SecureInputState = .clear
    private let idleInterval: DispatchTimeInterval
    private let busyInterval: DispatchTimeInterval
    /// Interval the repeating timer is currently armed with. Tracked so
    /// rescheduleIfNeeded only re-arms on an actual idle/busy change (see there).
    private var currentInterval: DispatchTimeInterval
    private let onChange: (SecureInputState) -> Void

    /// `onChange` is dispatched on the main queue.
    init(idleSeconds: Int = 3,
         busySeconds: Int = 1,
         onChange: @escaping (SecureInputState) -> Void)
    {
        self.queue = DispatchQueue(
            label: "local.modore.secure-input", qos: .utility)
        self.idleInterval = .seconds(idleSeconds)
        self.busyInterval = .seconds(busySeconds)
        // start() arms the timer at idleInterval; keep this in sync.
        self.currentInterval = .seconds(idleSeconds)
        self.onChange = onChange
    }

    deinit { stop() }

    func start() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        // First tick fires immediately so the menu bar reflects a
        // password-already-on-screen scenario at boot.
        t.schedule(deadline: .now(), repeating: idleInterval)
        t.setEventHandler { [weak self] in self?.poll() }
        timer = t
        t.resume()
    }

    func stop() {
        timer?.cancel()
        timer = nil
    }

    private func poll() {
        let next: SecureInputState
        if let pid = Self.currentHolderPid() {
            if case .blocked(_, _, let prevPid) = lastState, prevPid == pid {
                // Same holder; no transition. Caller already knows.
                rescheduleIfNeeded(busy: true)
                return
            }
            if let desc = Self.describeProcess(pid: pid) {
                next = .blocked(appName: desc.name, appPath: desc.path, pid: pid)
            } else {
                // Holder exists but its path/name is unreadable (sandboxed
                // helper, already exiting, etc.). Surface it as "unknown" so
                // the user still sees *something* in the menu bar.
                next = .blocked(
                    appName: "unknown process (pid \(pid))",
                    appPath: "",
                    pid: pid)
            }
        } else {
            next = .clear
        }

        if next != lastState {
            lastState = next
            let snapshot = next
            DispatchQueue.main.async { [weak self] in
                self?.onChange(snapshot)
            }
        }

        let busy: Bool
        if case .blocked = next { busy = true } else { busy = false }
        rescheduleIfNeeded(busy: busy)
    }

    /// Speed up the tick while a holder is active so we clear the menu-bar
    /// warning ~1 s after the user finishes the password prompt. Slow back
    /// down once idle so we're not wasting cycles.
    private func rescheduleIfNeeded(busy: Bool) {
        let want = busy ? busyInterval : idleInterval
        // Only re-arm on an actual interval change. Re-scheduling every tick
        // reset the deadline to `.now() + want` *after* the poll ran, so the
        // effective period drifted to `want + poll_duration` and the repeating
        // cadence never took effect. When the interval is unchanged, leave the
        // repeating timer to fire on its own schedule.
        guard want != currentInterval else { return }
        currentInterval = want
        timer?.schedule(deadline: .now() + want, repeating: want)
    }
}
