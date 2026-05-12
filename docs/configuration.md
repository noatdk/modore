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
