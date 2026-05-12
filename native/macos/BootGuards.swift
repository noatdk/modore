// BootGuards — refuse to start in contexts where modore will silently
// misbehave.
//
// Both checks are defensive insurance — neither has bitten a real user yet,
// but both are well-known macOS footguns and the failure modes are
// extremely confusing (the user thinks the host is broken when actually the
// OS is sandboxing or routing around it). Doing the check at boot and
// exiting loudly is much friendlier than letting the user spend an hour
// re-granting permissions.
//
// 1. **App translocation.** When macOS sees an unsigned (or first-launch
//    quarantined) bundle, it transparently runs it from a randomized
//    `/private/var/folders/.../AppTranslocation/<uuid>/d/` path instead of
//    the bundle's real location. Accessibility / Input Monitoring grants
//    are keyed to the bundle path, so granting permission to the
//    translocated path means the grant evaporates the moment the user
//    moves the bundle to `/Applications/` — they did everything right and
//    the host still doesn't work.
//
//    Heuristic: if `Bundle.main.bundlePath` is under `/private/`, we're
//    translocated. (Real `/private/var/...` paths exist, but no end-user
//    bundle should be running from one — installers don't put .app bundles
//    in /private. We're allowed false-positives only in edge cases that
//    would already be broken.)
//
// 2. **Running as root.** TCC permissions are per-uid; a process running
//    as root inherits root's TCC database, which never has the user's
//    grants. The hotkey installs (Carbon + CGEventTap) succeed but
//    Accessibility reads fail with -25204 / -25212, the user thinks the
//    grant didn't take, re-grants under their normal account, still
//    nothing works because the *running* process isn't them. Doubly
//    confusing if `sudo` is involved because that *also* puts Terminal
//    into SecureInput (see SecureInputMonitor.swift).
//
// Both guards exit non-zero so a supervisor (launchd / a wrapper script)
// can detect the failure and report it instead of restart-looping forever.

import Foundation

enum BootGuards {

    /// Runs both checks in order and `exit()`s on the first failure.
    /// Caller should invoke this very early — after the bundle is loaded
    /// (so `Bundle.main.bundlePath` is populated) but before any
    /// Accessibility / event-tap calls, so the error message gets out
    /// before a more confusing downstream error does.
    static func enforce() {
        assertNotTranslocated()
        assertNotRunningAsRoot()
    }

    private static func assertNotTranslocated() {
        let path = Bundle.main.bundlePath
        // Real /private/var/folders/... AppTranslocation paths look like
        // /private/var/folders/<hash>/<hash>/T/AppTranslocation/<uuid>/d/<Name>.app
        // — a simple "starts with /private/" catch-all is enough; no
        // legitimate .app distribution path lives under /private/.
        guard path.hasPrefix("/private/") else { return }
        Log.boot("REFUSING TO START: bundle is running from a translocated path:")
        Log.boot("  \(path)")
        Log.boot("macOS app-translocation routes first-launch bundles through this")
        Log.boot("temporary path. Any Accessibility permission granted here will be")
        Log.boot("lost the moment the bundle is moved. Move the .app to /Applications")
        Log.boot("(or anywhere outside /private/) and relaunch.")
        exit(76)
    }

    private static func assertNotRunningAsRoot() {
        guard geteuid() == 0 else { return }
        Log.boot("REFUSING TO START: running as root (euid=0).")
        Log.boot("macOS Accessibility / Input Monitoring permissions are per-uid,")
        Log.boot("so a root process can never see the regular user's grants — the")
        Log.boot("hotkey would install but every AX read would fail. Re-launch this")
        Log.boot("host as your normal user account (without sudo).")
        exit(77)
    }
}
