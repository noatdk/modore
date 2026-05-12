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

## Preflight: `modore-host --check-config` (macOS)

Validate the config without starting the host. Reads the same file the
running host would, prints what it found, and exits with a status that
scripts (pre-commit hooks, dotfiles tests) can branch on:

```text
$ modore-host --check-config
config path: /Users/you/.config/modore/modore.conf
  [conversion]  ok      [conversion] hotkey=Ctrl+Shift+grave (/Users/you/.config/modore/modore.conf)
  [clipboard]   pre_copy=30ms read_timeout=400ms restore=60ms
```

| Exit code | Meaning                                                                   |
| :-------: | ------------------------------------------------------------------------- |
| `0`       | Healthy load (or no file → defaults will be used at runtime).             |
| `1`       | `[conversion] hotkey` parsed but is malformed.                            |
| `2`       | A `[clipboard]` key was rejected (`hotkey` itself was fine).              |

This is the "parse-before-swap" path made explicit: same code that runs
on a live reload, but as a one-shot you can invoke from CI.

## Diagnostic: `modore-host --secure-input-status` (macOS)

Query which (if any) app is currently holding **Secure Keyboard Entry**
(macOS's SecureInput mode). While SecureInput is active, the OS routes
keystrokes through a path that bypasses both modore's event tap and any
synthetic injection — the hotkey silently no-ops. Common holders are
Terminal/iTerm with a `sudo` or password prompt on screen, password
fields in 1Password / Bitwarden / Safari banking pages, the Lock
Screen, Touch ID prompts, and FileVault.

```text
$ modore-host --secure-input-status
secure input: clear
# exit 0

$ modore-host --secure-input-status
secure input: held by pid 30625
  app: osascript
  path: /usr/bin/osascript
# exit 1
```

| Exit code | Meaning                                                            |
| :-------: | ------------------------------------------------------------------ |
| `0`       | SecureInput is clear; modore can deliver keystrokes normally.      |
| `1`       | SecureInput is held by another app (printed). Hotkey will no-op.   |

The running host also polls for this in the background (3 s idle / 1 s
while held) and reflects the state in the menu bar: title flips to red
and a `⚠ Blocked by <App>` line appears in the menu. The `[secure-input]`
log tag records every transition.

## Path printers (macOS)

Two zero-side-effect flags for scripting and bug-report copy-paste:

```text
$ modore-host --print-config-path
/Users/you/.config/modore/modore.conf
# composes with the shell:
$ $EDITOR "$(modore-host --print-config-path)"

$ modore-host --print-paths
config:        /Users/you/.config/modore/modore.conf
mozc profile:  /Users/you/Library/Application Support/modore
bundle:        /Users/you/.../modore.app
executable:    /Users/you/.../modore.app/Contents/MacOS/modore-host
```

Both exit `0`. `--print-config-path` prints only the path (no labels,
nothing on stderr) so it slots into shell substitutions; `--print-paths`
labels every line for humans.

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

## Process supervision

modore runs as a single process (no daemon/worker split). The runtime
contains no internal supervisor; if the host process exits, it stays
exited until restarted. Two recommended options:

- **macOS — `launchd` user agent**. Drop a `~/Library/LaunchAgents/local.modore.host.plist`
  with `KeepAlive = true` and `RunAtLoad = true` pointing at
  `modore.app/Contents/MacOS/modore-host`, then `launchctl bootstrap
  gui/$UID <plist>`. `launchd` restarts the binary on crash and at login.
- **Linux — `systemd` user unit**. Already shipped; see [`linux.md`](linux.md).
  `Restart=always` gives the same behavior.

If/when we observe Mozc crashes destabilizing the hotkey listener, a
daemon/worker split becomes worth implementing internally. Until then
the platform supervisor is enough.
