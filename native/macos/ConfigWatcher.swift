// Watches modore.conf for edits and fires `onChange` on the main queue
// after a short quiet window. Designed for the editors users actually
// reach for: in-place writes (echo > file, nano), and atomic-rename
// writes (vim, VSCode, JetBrains, BBEdit) that swap the inode out from
// under us.
//
// Strategy:
//   • open(path, O_EVTONLY) + DispatchSourceFileSystemObject
//   • on any event → 300 ms debounce → re-open by path → fire onChange
//   • if the re-open fails (file genuinely removed), poll every 1 s
//     until it comes back, then fire onChange once
//
// Re-opening on every fire is what makes atomic-rename robust: the old
// fd still points at the now-orphaned inode after `mv`, so subsequent
// writes wouldn't reach us. The cheap close + open swaps onto the new
// inode without needing to watch the parent directory.

import Foundation

final class ConfigWatcher {
    private let path: String
    private let onChange: () -> Void
    private let debounceMs: Int

    private var fd: Int32 = -1
    private var source: DispatchSourceFileSystemObject?
    private var debounce: DispatchSourceTimer?
    private var retry: DispatchSourceTimer?
    private var loggedRetry = false

    private static let kEvents: DispatchSource.FileSystemEvent =
        [.write, .extend, .delete, .rename, .attrib, .link, .revoke]

    init(path: String, debounceMs: Int = 300, onChange: @escaping () -> Void) {
        self.path = path
        self.debounceMs = debounceMs
        self.onChange = onChange
    }

    deinit { teardown() }

    /// Begin watching. Safe to call from any thread; setup runs on the main
    /// queue so the DispatchSource and onChange callback share a serial
    /// context with the rest of the host.
    func start() {
        DispatchQueue.main.async { [weak self] in
            _ = self?.arm(notifyOnSuccess: false)
        }
    }

    @discardableResult
    private func arm(notifyOnSuccess: Bool) -> Bool {
        teardownSource()
        let opened = open(path, O_EVTONLY)
        guard opened >= 0 else {
            if !loggedRetry {
                Log.config("watcher: \(path) unavailable (errno=\(errno)) — will pick up when it appears")
                loggedRetry = true
            }
            scheduleRetry()
            return false
        }
        let src = DispatchSource.makeFileSystemObjectSource(
            fileDescriptor: opened,
            eventMask: ConfigWatcher.kEvents,
            queue: .main
        )
        src.setEventHandler { [weak self] in self?.scheduleDebounce() }
        src.setCancelHandler { close(opened) }
        fd = opened
        source = src
        src.resume()
        if loggedRetry {
            Log.config("watcher: \(path) now present — auto-reload armed")
        } else {
            Log.config("watcher: armed on \(path)")
        }
        loggedRetry = false
        if notifyOnSuccess { onChange() }
        return true
    }

    private func scheduleDebounce() {
        debounce?.cancel()
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now() + .milliseconds(debounceMs))
        t.setEventHandler { [weak self] in
            guard let s = self else { return }
            s.debounce = nil
            s.fire()
        }
        debounce = t
        t.resume()
    }

    private func fire() {
        // Re-arm before notifying. Atomic-rename editors swap the inode
        // during the debounce window; the next event would otherwise be
        // delivered against a stale fd.
        let armed = arm(notifyOnSuccess: false)
        if armed { onChange() }
        // If arm() failed, scheduleRetry() will fire onChange once the
        // file comes back so the user's edits aren't lost.
    }

    private func scheduleRetry() {
        retry?.cancel()
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now() + .seconds(1), repeating: .seconds(1))
        t.setEventHandler { [weak self] in
            guard let s = self else { return }
            if s.arm(notifyOnSuccess: true) {
                s.retry?.cancel()
                s.retry = nil
            }
        }
        retry = t
        t.resume()
    }

    private func teardownSource() {
        debounce?.cancel(); debounce = nil
        source?.cancel(); source = nil
        fd = -1
    }

    private func teardown() {
        retry?.cancel(); retry = nil
        teardownSource()
    }
}
