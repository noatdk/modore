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
| Write back: native API set-selected-text      |   ✓   |     ✓       |       ◐⁴        |    ✗    |
| Write back: synthetic keystroke injection     |   ✓⁶  |     ✓⁷      |       ✓⁸        |    ✗    |
| Save / restore user clipboard around write    |   ✓   |     ✓       |       ✓         |    ✗    |
| Chromium/Electron XWayland fast path          |   —   |     ✓       |       ✓⁹        |    ✗    |

## Conversion engine

| Feature                                       | macOS | Linux (X11) | Linux (Wayland) | Windows |
| --------------------------------------------- | :---: | :---------: | :-------------: | :-----: |
| Top-candidate Mozc conversion (in-process)    |   ✓   |     ✓       |       ✓         |    ✗    |
| Candidate window / Nth candidate              |   ✗   |     ✗       |       ✗         |    ✗    |
| Bootstrap from existing Mozc / GJI profile    |   ✗   |     ✗       |       ✗         |    ✗    |

## Config & lifecycle

| Feature                                       | macOS | Linux (X11) | Linux (Wayland) | Windows |
| --------------------------------------------- | :---: | :---------: | :-------------: | :-----: |
| `~/.config/modore/modore.conf` (XDG)          |   ✓   |     ✓       |       ✓         |    ✗    |
| Auto-reload on config change                  |   ✓¹² |     ✗       |       ✗         |    ✗    |
| Tunable clipboard-fallback timings            |   ✓¹⁴ |     ✗       |       ✗         |    ✗    |
| Log file on disk (`modore.log`)               |   ✗¹⁰ |     ✓       |       ✓         |    ✗    |
| First-run permission prompt                   |   ✓   |     —¹¹     |       —¹¹       |    ✗    |
| systemd user unit shipped                     |   —   |     ✓       |       ✓         |    ✗    |

---

¹ Carbon `RegisterEventHotKey` (system-level grab) — `native/macos/CarbonHotkey.swift`. CGEventTap stays installed as a fallback (if Carbon registration ever fails) and as the self-event filter for synthesized CGEvents.
² `XGrabKey` with NumLock/CapsLock variants — `native/linux/main.cpp:2867`.
³ Native-Wayland windows don't see X11 grabs; use `--trigger` from a compositor bind.
⁴ AT-SPI only sees the field when the toolkit publishes it; native-Wayland Chromium/Electron usually doesn't, and we fall through to clipboard.
⁵ Includes line-copy / path-bar / UI-hint rejection and PRIMARY-vs-CLIPBOARD arbitration — `native/linux/main.cpp:1047` onward.
⁶ Unicode `keyboardSetUnicodeString` into the session tap — chunked at 20 UTF-16 units to defeat the platform's silent-truncation limit. No clipboard touch on the write path.
⁷ `XTest` synthetic `Ctrl+V` with clipboard swap.
⁸ `wtype` and/or `hyprctl sendshortcut`.
⁹ When `DISPLAY` is set on a Wayland session, the host prefers the X11 keyboard path because Electron is still attached to `DISPLAY`.
¹⁰ macOS logs via `NSLog` to `Console.app`; no on-disk log file yet.
¹¹ AT-SPI uses session D-Bus; some desktops still need the user to enable Accessibility manually, but the host does not prompt.
¹² `DispatchSourceFileSystemObject` on `modore.conf` with 300 ms debounce; survives atomic-rename editors. Malformed reloads keep the previous chord. See [`configuration.md`](configuration.md) and `native/macos/ConfigWatcher.swift`.
¹³ Polls until the trigger's modifier keys are released before synthesizing Cmd+C (otherwise the held Ctrl/Shift from the conversion hotkey poisons the synthetic copy in many apps). 3 s timeout. Self-emitted CGEvents are tagged with an off-screen `location` so the tap callback can skip them and never re-trigger pickup.

¹⁴ `[clipboard]` section: `pre_copy_delay_ms` (renderer catch-up after force-select), `read_timeout_ms` (max wait for `Cmd+C` to land on the clipboard), `restore_clipboard_delay_ms` (delay before restoring the user's clipboard). Reloads with `[conversion]`; malformed values are ignored. See [`configuration.md`](configuration.md).
