// @testable
//
// Central registry of the third-party app identities modore special-cases.
//
// Before this file the same bundle-id sets lived in several places —
// the Chromium clipboard-probe set in SpanSplit.swift and an identical
// omnibox set in ChromiumOmniboxInputLog.swift, the peek blocklist in
// Pickup.swift, the shell-native terminal set in FrontmostApp.swift, and
// the Electron path fragments in AccessibilityIO.swift. Centralizing them
// keeps "which app needs which quirk" answerable from one place, and means
// adding a newly-discovered offender touches a single file.
//
// Foundation-only on purpose: SpanSplit.swift (which is `// @testable` and
// compiles without Cocoa for the unit-test driver) reads `chromiumBundleIDs`,
// so this file must stay platform-framework-free and carry the same marker.

import Foundation

enum KnownApps {
    /// Chromium-family browsers. Both the clipboard-fallback probe heuristics
    /// (SpanSplit) and the omnibox typed-input log (ChromiumOmniboxInputLog)
    /// key off this exact set — they used to define it independently.
    static let chromiumBundleIDs: Set<String> = [
        "com.google.Chrome",
        "com.google.Chrome.canary",
        "org.chromium.Chromium",
    ]

    /// Bundle IDs where the Cmd+C-peek heuristic in `doClipboardPickup`'s
    /// step 1 is unreliable because the app line-copies the caret's current
    /// line *without* a trailing newline (so `looksLikeLineCopy` misses it).
    /// Chrome's DevTools console is the discovered offender; the same
    /// Chromium behaviour presumably exists in other Chromium-hosted dev
    /// surfaces. Obsidian's CodeMirror editor behaves the same way — and on
    /// top of that it silently rejects AXValue writes, so every conversion
    /// lands here. AX-capable surfaces in these apps never reach that code
    /// path (they go through doPickup → readFocusedField), so blocklisting
    /// the whole bundle only affects the already-unreliable clipboard-
    /// fallback regions.
    static let peekSelectionBlocklist: Set<String> = [
        "com.google.Chrome",
        "md.obsidian",
    ]

    /// Terminal emulators routed through the shell-native binding path
    /// instead of the host pickup pipeline.
    static let shellNativeTerminalBundleIDs: Set<String> = [
        "net.kovidgoyal.kitty",
        "com.mitchellh.ghostty",
        "com.github.wez.wezterm",
        "io.alacritty",
    ]

    /// Substrings (matched against a lowercased executable path) that mark a
    /// process as Electron-hosted. Used to decide whether to flip the
    /// documented `AXManualAccessibility` switch before retrying the AX read.
    static let electronAppPathFragments: [String] = [
        "electron framework.framework",
        "/electron.app/",
        "/discord.app/",
        "/slack.app/",
        "/visual studio code.app/",
        "/cursor.app/",
        "/obsidian.app/",
    ]

    /// True when a lowercased bundle id or executable path looks Electron-
    /// hosted. The bundle-id check catches generic `*electron*` ids; the
    /// path check catches the major apps that ship under their own id.
    static func looksElectron(bundleID: String, executablePath: String?) -> Bool {
        if bundleID.contains("electron") { return true }
        guard let path = executablePath?.lowercased() else { return false }
        return electronAppPathFragments.contains { path.contains($0) }
    }
}
