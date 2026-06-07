import Foundation

extension SpanSplitTests {
    static func runKnownAppsTests() {
        // These sets gate app-specific pickup paths. A drift here usually
        // means Chrome/Electron fixes silently stop applying in exactly the
        // places the README says macOS falls back to clipboard/AX quirks.
        expectEqual(
            KnownApps.chromiumBundleIDs.contains("com.google.Chrome"),
            true,
            "known apps: Chrome is Chromium"
        )
        expectEqual(
            isChromiumClipboardProbeBundleID("org.chromium.Chromium"),
            true,
            "known apps: Chromium uses Chrome clipboard probe"
        )
        expectEqual(
            isChromiumClipboardProbeBundleID("com.apple.Safari"),
            false,
            "known apps: Safari does not use Chrome clipboard probe"
        )
        expectEqual(
            KnownApps.peekSelectionBlocklist.contains("md.obsidian"),
            true,
            "known apps: Obsidian skips unreliable clipboard peek"
        )
        expectEqual(
            KnownApps.looksElectron(
                bundleID: "com.example.electron-helper",
                executablePath: nil),
            true,
            "known apps: generic electron bundle id"
        )
        expectEqual(
            KnownApps.looksElectron(
                bundleID: "com.tinyspeck.slackmacgap",
                executablePath: "/Applications/Slack.app/Contents/MacOS/Slack"),
            true,
            "known apps: Slack path fragment"
        )
        expectEqual(
            KnownApps.looksElectron(
                bundleID: "com.apple.TextEdit",
                executablePath: "/System/Applications/TextEdit.app/Contents/MacOS/TextEdit"),
            false,
            "known apps: TextEdit is not Electron"
        )
    }
}
