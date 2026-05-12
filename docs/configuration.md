# Configuration

Both hosts read `$XDG_CONFIG_HOME/modore/modore.conf` if set, otherwise
`~/.config/modore/modore.conf` (same path on macOS).

INI-style. Sections are independent: omit any section to keep its defaults.

```ini
[conversion]
hotkey = Ctrl+Shift+grave

# macOS-only knobs for the clipboard fallback path. Omit for defaults.
[clipboard]
pre_copy_delay_ms = 20
read_timeout_ms = 250
restore_clipboard_delay_ms = 50
```

A copy-pasteable starter ships at [`modore.conf.example`](../modore.conf.example).

## `[conversion] hotkey`

Format is one or more modifiers joined by `+`, then a single key name.

**Modifiers**: `Ctrl`, `Shift`, `Alt` (aliases: `Option`, `Meta`),
`Super` (aliases: `Win`; on macOS `Command` / `Cmd` map to the Command
modifier).

**Key names**: `Slash`, `Period`, `Comma`, single letters, digits,
`F1`–`F12`, `Space`, arrows, `Return`, `Tab`, etc. On Linux any name
understood by `XStringToKeysym(3)` also works.

**Default**: `Ctrl+Slash`. Applied when the file is missing or the
`hotkey` key is absent.

**Validation**: a malformed `hotkey` value logs a warning and falls back
to the default rather than refusing to start. On Linux the resolved
chord is echoed to `modore.log` after each `modore-host` start; macOS
logs the same line to `Console.app` via `NSLog`.

## `[clipboard]` (macOS only)

Timings for the clipboard fallback path — the route modore takes when the
focused app doesn't expose Accessibility (Chromium/Electron browsers, some
native-Wayland windows, etc.). All values are non-negative integers
expressed in **milliseconds**. Defaults reproduce the previous hard-coded
behavior, so omitting the section is a no-op.

| Key                          | Default | What it controls                                                                                                                            |
| ---------------------------- | ------: | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `pre_copy_delay_ms`          |  `20`   | Pause after `Shift+Opt+Left` (force-select) before issuing `Cmd+C`. Bump if Electron apps (Cursor, Slack, VSCode) miss the copy intermittently. |
| `read_timeout_ms`            | `250`   | Max wait for the clipboard `changeCount` to advance after the force-select `Cmd+C`. Bump on slow machines or under heavy load.              |
| `restore_clipboard_delay_ms` |  `50`   | Delay before writing the user's original clipboard back — ensures the Unicode injection that consumed the selection has fully landed.       |

The aggressive initial-peek timeout (80 ms — "does the user already have a
selection?") is *not* exposed: it's a heuristic threshold rather than a
tuning knob, so leaving it fixed keeps the fast-path predictable.

**Validation**: each value must be a non-negative integer. Malformed
values are ignored with a `[config]` log line and the previous default
stays in effect. Unknown keys in `[clipboard]` are logged and ignored.

## Auto-reload

**macOS only (today)**: edits to `modore.conf` are picked up live — no
restart needed. The watcher applies a 300 ms quiet-window debounce, so
multi-event saves (atomic-rename editors like Vim, VSCode, JetBrains)
land as a single reload. Both `[conversion]` and `[clipboard]` reload
together; sections are independent and only the changed one logs.

| Edit                                    | Behavior                                                                  |
| --------------------------------------- | ------------------------------------------------------------------------- |
| Hotkey changed to a valid value         | Swap immediately. Logs `config reloaded: …`.                              |
| Hotkey re-written to the same value     | No-op. Nothing logged.                                                    |
| Hotkey changed to a malformed value     | Keep the previous chord. Logs `config reload rejected: …`.                |
| `[conversion] hotkey` line removed      | Revert to the default (`Ctrl+Slash`).                                     |
| File deleted                            | Keep the previous chord; resume watching for the file to reappear.        |
| File re-created with a new chord        | Swap to the new chord. Same path as the "edit" case.                      |
| `[clipboard]` value changed             | Apply on next pickup. Logs `clipboard timings: …`.                        |
| `[clipboard]` value malformed / unknown | Ignore that key. Logs `ignoring [clipboard] …`. Other keys still applied. |

If the config file doesn't exist at startup, the watcher polls for it
once a second and arms as soon as it appears. There's no need to restart
the host after creating the file for the first time.

Linux auto-reload is on the parity list ([`PARITY.md`](PARITY.md)) but
not implemented yet.
