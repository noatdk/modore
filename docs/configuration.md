# Configuration

Both hosts read `$XDG_CONFIG_HOME/modore/modore.conf` if set, otherwise
`~/.config/modore/modore.conf` (same path on macOS).

INI-style; only the `[conversion]` section is defined today. Add new
sections and keys as features grow.

```ini
[conversion]
hotkey = Ctrl+Shift+grave
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

## Auto-reload

**macOS only (today)**: edits to `modore.conf` are picked up live — no
restart needed. The watcher applies a 300 ms quiet-window debounce, so
multi-event saves (atomic-rename editors like Vim, VSCode, JetBrains)
land as a single reload.

| Edit                                    | Behavior                                                                  |
| --------------------------------------- | ------------------------------------------------------------------------- |
| Hotkey changed to a valid value         | Swap immediately. Logs `config reloaded: …`.                              |
| Hotkey re-written to the same value     | No-op. Nothing logged.                                                    |
| Hotkey changed to a malformed value     | Keep the previous chord. Logs `config reload rejected: …`.                |
| `[conversion] hotkey` line removed      | Revert to the default (`Ctrl+Slash`).                                     |
| File deleted                            | Keep the previous chord; resume watching for the file to reappear.        |
| File re-created with a new chord        | Swap to the new chord. Same path as the "edit" case.                      |

If the config file doesn't exist at startup, the watcher polls for it
once a second and arms as soon as it appears. There's no need to restart
the host after creating the file for the first time.

Linux auto-reload is on the parity list ([`PARITY.md`](PARITY.md)) but
not implemented yet.
