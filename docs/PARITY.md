# Host feature parity

What each `modore` host can do today. Update in the same commit that adds
or removes a capability.

`✓` supported · `◐` partial · `✗` not yet · `—` not applicable.

The Linux columns are the same binary on different display servers;
`--ipc-only` / `--no-ipc` only toggle the trigger surface, so they're rows.

## Trigger

| Feature                                       | macOS | Linux (X11) | Linux (Wayland) | Windows |
| --------------------------------------------- | :---: | :---------: | :-------------: | :-----: |
| In-process global hotkey grab                 |   ✓¹  |     ✓²      |       ✗³        |    ✗    |
| Configurable hotkey (`modore.conf`)           |   ✓   |     ✓       |       —³        |    ✗    |
| `modore-host --trigger` over Unix socket      |   ✗   |     ✓       |       ✓         |    ✗    |
| `--ipc-only` / `--no-ipc` / `--help` / `-V`   |   ✗   |     ✓       |       ✓         |    ✗    |

## Text I/O

| Feature                                       | macOS | Linux (X11) | Linux (Wayland) | Windows |
| --------------------------------------------- | :---: | :---------: | :-------------: | :-----: |
| Read focused field (Accessibility / AT-SPI2)  |   ✓   |     ✓       |       ◐⁴        |    ✗    |
| Clipboard fallback (force-select, copy, read) |   ✓¹³ |     ✓⁵      |       ✓⁵        |    ✗    |
| Write back: native API set-selected-text      |   ✓   |     ✓       |       ◐⁴        |    ◐    |
| Write back: synthetic keystroke injection     |   ✓⁶  |     ✓⁷      |       ✓⁸        |    ✗    |
| Save / restore user clipboard around write    |   ✓   |     ✓       |       ✓         |    ✗    |
| Chromium/Electron XWayland fast path          |   —   |     ✓       |       ✓⁹        |    ✗    |

## Conversion engine

| Feature                                       | macOS | Linux (X11) | Linux (Wayland) | Windows |
| --------------------------------------------- | :---: | :---------: | :-------------: | :-----: |
| Top-candidate Mozc conversion (in-process)    |   ✓   |     ✓       |       ✓         |    ✗    |
| Acronym/code prefix preserved (`R&Diraisho`)  |   ✓¹⁸ |     ✗       |       ✗         |    ✗    |
| Cycle through Mozc candidates (repeat hotkey) |   ✓   |     ✓       |       ✓         |    ✗    |
| Esc to undo last conversion                   |   ✓   |     ✗       |       ✗         |    ✗    |
| Katakana modifier (Shift+hotkey → カタカナ)   |   ✓   |     ✗       |       ✗         |    ◐    |
| Floating candidate panel                      |   ✓   |     ✗       |       ✗         |    ✗    |
| Bootstrap from existing Mozc / GJI profile    |   ✗   |     ✗       |       ✗         |    ✗    |

## Shell-native (terminal)

| Feature                                       | macOS | Linux (X11) | Linux (Wayland) | Windows |
| --------------------------------------------- | :---: | :---------: | :-------------: | :-----: |
| zsh / bash / fish convert + cycle (`Ctrl-X`)  |   ✓²⁴ |     ✗²⁵     |       ✗²⁵       |    ✗    |
| Inline candidate window while cycling         |   ◐²⁶ |     ✗       |       ✗         |    ✗    |
| Candidate chooser (`Ctrl-X Ctrl-L`)           |   ✓²⁷ |     ✗       |       ✗         |    ✗    |

## Scripting

| Feature                                          | macOS | Linux (X11) | Linux (Wayland) | Windows |
| ------------------------------------------------ | :---: | :---------: | :-------------: | :-----: |
| Lua scripting engine (LuaJIT 2.1 + sandbox)      |   ✓   |     ◐²⁰     |       ◐²⁰       |    ✗    |
| Hooks: pickup / replacement / route              |   ✓   |     ◐²⁰     |       ◐²⁰       |    ✗    |
| Hooks: candidates / acquire + host primitives    |   ✓   |     ✗       |       ✗         |    ✗    |
| Per-app `<app-id>.lua` lookup                    |   ✓   |     ✗²¹     |       ✗²¹       |    ✗    |
| Hot-reload on content edit + dir add/remove      |   ✓   |     ◐²²     |       ◐²²       |    ✗    |

## Config & lifecycle

| Feature                                       | macOS | Linux (X11) | Linux (Wayland) | Windows |
| --------------------------------------------- | :---: | :---------: | :-------------: | :-----: |
| `~/.config/modore/modore.conf` (XDG)          |   ✓   |     ✓       |       ✓         |    ✗    |
| Auto-reload on config change                  |   ✓¹² |     ◐²³     |       ◐²³       |    ✗    |
| Tunable clipboard-fallback timings            |   ✓¹⁴ |     ✓²³     |       ✓²³       |    ✗    |
| `--check-config` preflight (no engine start)  |   ✓¹⁵ |     ✗       |       ✗         |    ✓    |
| Menu-bar status item ("running" indicator)    |   ✓¹⁶ |     ✗       |       ✗         |    ✗    |
| SecureInput awareness (sudo/password prompts) |   ✓¹⁷ |     —       |       —         |    —    |
| Log file on disk (`modore.log`)               |   ✗¹⁰ |     ✓       |       ✓         |    ✓    |
| First-run permission prompt                   |   ✓   |     —¹¹     |       —¹¹       |    ✗    |
| systemd user unit shipped                     |   —   |     ✓       |       ✓         |    ✗    |

---

¹ Carbon `RegisterEventHotKey` (system-level grab) — `native/macos/CarbonHotkey.swift`. CGEventTap stays installed as a fallback (if Carbon registration ever fails) and as the self-event filter for synthesized CGEvents.
² `XGrabKey` with NumLock/CapsLock variants — `native/linux/main.cpp:2867`.
³ Native-Wayland windows don't see X11 grabs; Linux prefers a raw
`/dev/input/event*` hotkey monitor on Wayland and falls back to the
X11/compositor-trigger paths when raw access is unavailable.
⁴ AT-SPI only sees the field when the toolkit publishes it; native-Wayland Chromium/Electron usually doesn't, and we fall through to clipboard.
⁵ Includes line-copy / path-bar / UI-hint rejection, one-shot app-specific selection flows, and PRIMARY-vs-CLIPBOARD arbitration — `native/linux/main.cpp:1047` onward.
⁶ Unicode `keyboardSetUnicodeString` into the session tap — chunked at 20 UTF-16 units to defeat the platform's silent-truncation limit. No clipboard touch on the write path.
⁷ `XTest` synthetic `Ctrl+V` with clipboard swap.
⁸ `wtype` and/or `hyprctl sendshortcut`.
⁹ Native Wayland clients use the Hyprland bind plus clipboard-paste path; XWayland clients still use the X11 keyboard helpers when `DISPLAY` is present.
¹⁰ macOS logs via `NSLog` to `Console.app`; no on-disk log file yet.
¹¹ AT-SPI uses session D-Bus; some desktops still need the user to enable Accessibility manually, but the host does not prompt.
¹² `DispatchSourceFileSystemObject` on `modore.conf` with 300 ms debounce; survives atomic-rename editors. Malformed reloads keep the previous chord. See [`configuration.md`](configuration.md) and `native/macos/ConfigWatcher.swift`.
¹³ Polls until the trigger's modifier keys are released before synthesizing Cmd+C (otherwise the held Ctrl/Shift from the conversion hotkey poisons the synthetic copy in many apps). 3 s timeout. Self-emitted CGEvents are tagged with an off-screen `location` so the tap callback can skip them and never re-trigger pickup.

¹⁴ `[clipboard]` section: `pre_copy_delay_ms` (renderer catch-up after force-select), `read_timeout_ms` (max wait for `Cmd+C` to land on the clipboard), `restore_clipboard_delay_ms` (delay before restoring the user's clipboard). Reloads with `[conversion]`; malformed values are ignored. See [`configuration.md`](configuration.md).

¹⁵ `modore-host --check-config` parses the same file the running host would and reports each section's outcome. Exits 0 on healthy load, 1 on malformed hotkey, 2 on rejected `[clipboard]` key. Useful for pre-commit hooks and dotfiles tests.

¹⁶ `NSStatusItem` showing "ﾓﾄﾞﾚ" (half-width katakana for "modore") in the menu bar; menu lists the live hotkey, delivery path (Carbon vs CGEventTap), and shortcuts for opening / revealing the config plus Quit. Refreshes automatically on config reload. See `native/macos/StatusItem.swift`.
²³ Linux hot-reloads `[clipboard]` timing values from `modore.conf` on a short poll interval. Hotkey registration still happens at startup, so changing `[conversion] hotkey` requires a restart.

¹⁸ Leading `[A-Z][A-Z0-9&/.+\-_:@#]*` head followed by lowercase is held back from Mozc and re-attached to the result, so `R&Diraisho` → `R&D依頼書` and `APIkaitou` → `API回答`. Single-uppercase words (`Karen`) are not split. Phase 2 plans a user dict at `~/.config/modore/non-japanese.txt` for tokens this heuristic misses. See `native/macos/Pickup/SpanSplit.swift::splitAcronymHead`.

²⁰ Linux wires the basic Lua hook trio into the pickup path (`pickup` / `replacement` / `route`); the default build still ships a helpers-only `libmodore_script.so`, and `candidates` / `acquire` host primitives remain Mac-only for now.

²¹ wm-class probe not yet plumbed on Linux, so the engine receives NULL app_id and only `default.lua` ever matches. macOS reads bundle id via `NSWorkspace.frontmostApplication`.

²² Per-file content edits reload on the next hotkey press (engine's mtime poll). The macOS host also watches the scripts directory itself via `ConfigWatcher`, so adding or removing files is picked up live; the Linux host has no equivalent watcher yet, so new files only appear after a host restart.

¹⁷ Polls `IORegistry → IOConsoleUsers → kCGSSessionSecureInputPID` on a background timer (3 s idle / 1 s while held). When held by another app (sudo prompts in Terminal/iTerm, password fields in 1Password/Bitwarden/Safari, the Lock Screen, Touch ID) the menu-bar title flips to red and a `⚠ Blocked by <App>` line appears in the menu — so the user knows why the hotkey is silently failing. `modore-host --secure-input-status` is a one-shot diagnostic. See `native/macos/SecureInputMonitor.swift`. The feature is "—" on Linux/Windows because Secure Keyboard Entry is a macOS-only OS concept.

²⁴ The shell sources `scripts/modore-shell-bootstrap.{sh,fish}`, which lazily asks `modore-host --print-shell-bootstrap` for a widget snippet (generated per-shell in `bridge/src/shell_convert.cc`). The widgets talk to the running host over a Unix socket; `Ctrl-X Ctrl-J` converts/cycles, `Ctrl-X Ctrl-K` does katakana/reverse. See [`shell-integration.md`](shell-integration.md).

²⁵ The Linux host has no shell-convert socket server or `--print-shell-bootstrap`, so the bootstrap has nothing to talk to.

²⁶ zsh only: cycling draws the candidate list below the prompt via ZLE `POSTDISPLAY` (no dependency), current pick bracketed, cleared on the next edit or accept. bash (readline) and fish expose no below-buffer region, so they cycle without the inline list.

²⁷ `Ctrl-X Ctrl-L` opens a chooser; picker auto-detects `fzf` → `gum` → a built-in numbered prompt (no dependency), overridable with `MODORE_SHELL_PICKER`.
