# Configuration

Both hosts read `$XDG_CONFIG_HOME/modore/modore.conf` if set, otherwise
`~/.config/modore/modore.conf` (same path on macOS).

INI-style. Sections are independent: omit any section to keep its defaults.

Each section/key below opens with an **Available**: line listing the
platforms it currently works on. The live feature matrix is in
[`PARITY.md`](PARITY.md); the labels here go stale only when a key
gains or loses a platform, which is a guaranteed line in the commit
that does so.

```ini
[conversion]
hotkey = Ctrl+Shift+grave

# Available: macOS
# Bridge-owned knobs. These values are read once before the Mozc bridge is
# initialized, so edits take effect on the next restart.
[bridge]
mozc_backend = google_ime
candidate_mixing_mode = 0
trace_raw_candidates = off

# Available: macOS
# Knobs for the clipboard fallback path. Omit for defaults.
[clipboard]
pre_copy_delay_ms = 20
read_timeout_ms = 250
restore_clipboard_delay_ms = 50
```

A copy-pasteable starter ships at [`modore.conf.example`](../modore.conf.example).

## `[conversion] hotkey`

**Available**: macOS, Linux

Format is one or more modifiers joined by `+`, then a single key name.

**Modifiers**: `Ctrl`, `Shift`, `Alt` (aliases: `Option`, `Meta`),
`Super` (aliases: `Win`; on macOS `Command` / `Cmd` map to the Command
modifier).

**Key names**: `Slash`, `Period`, `Comma`, single letters, digits,
`F1`–`F12`, `Space`, arrows, `Return`, `Tab`, etc. On Linux any name
understood by `XStringToKeysym(3)` also works.

**Default**: `Cmd+Semicolon` on macOS, `Super+Semicolon` on Linux. Applied
when the file is missing or the `hotkey` key is absent.

**Validation**: a malformed `hotkey` value logs a warning and falls back
to the default rather than refusing to start. On Linux the resolved
chord is echoed to `modore.log` after each `modore-host` start; macOS
logs the same line to `Console.app` via `NSLog`.

## `[conversion] katakana_modifier`

**Available**: macOS

Optional second chord that fires the same pickup but forces a **full-width
katakana** commit instead of Mozc's top kanji candidate. Useful for
loanwords Mozc keeps writing as kanji (e.g. "シドッチ" coming out as
"史奉行").

The modifier is layered on top of `hotkey`. With `katakana_modifier = shift`,
modore binds:

| Chord                       | Behavior                                  |
| --------------------------- | ----------------------------------------- |
| the conversion hotkey       | Convert to top kanji candidate (default). |
| Shift + the conversion hotkey | Convert to full-width katakana.         |

| Value   | Effect                                                            |
| ------- | ----------------------------------------------------------------- |
| `none`  | Default. No secondary chord; behavior matches pre-feature builds. |
| `shift` | Binds `Shift+<hotkey>` as the katakana chord.                     |

**Validation**: an unknown value logs `ignoring [conversion]
katakana_modifier=<value>` and falls back to `none`. If `hotkey`
already includes the configured modifier (e.g.
`hotkey = Ctrl+Shift+grave` + `katakana_modifier = shift`), the
secondary chord would be indistinguishable from the primary — modore
declines to bind it and logs `katakana chord cleared (collides with
primary modifiers)`. The primary chord keeps working normally.

The clipboard-fallback path (apps that don't expose Accessibility text)
honors the katakana modifier too; the only divergence between the AX
fast-path and the clipboard fallback is *how* the replacement is
written back, not what Mozc returns.

## `[bridge]`

**Available**: macOS

Launch-time knobs that the host maps to bridge env vars before
`mozc_bridge_init()`.

| Key                    | Type    | Effect                                                                 |
| ---------------------- | ------- | ---------------------------------------------------------------------- |
| `mozc_backend`         | string  | Picks the bridge backend: `oss` or `google_ime`.                      |
| `candidate_mixing_mode` | integer | Sets `MODORE_MOZC_CANDIDATE_MIXING_MODE` for the Google IME bridge.   |
| `trace_raw_candidates`  | bool    | Sets `MODORE_BRIDGE_TRACE_RAW_CANDIDATES` for debug candidate tracing. |

**Default**: deterministic values from config defaults are applied at
startup (`candidate_mixing_mode = 0`, `trace_raw_candidates = off`,
`mozc_backend = oss`). The running bridge session does not hot-swap
these values; restart modore after editing them.

`mozc_backend` accepts `oss`, `google_ime`, `google-ime`, or `googleime`.
**Validation**: `candidate_mixing_mode` must be a non-negative integer.
`trace_raw_candidates` accepts `on|off|true|false|1|0|yes|no`. Unknown
values log a `[config]` warning and are ignored.

## `[conversion] katakana_modifier_behavior`

**Available**: macOS

Controls what the katakana chord does while a conversion session is
already active.

| Value            | Effect                                                                 |
| ---------------- | ---------------------------------------------------------------------- |
| `cycle_backwards` | Shift+hotkey steps to the previous candidate when a session is active. |
| `katakana`       | Shift+hotkey always forces katakana, even during an active session.    |

**Default**: `cycle_backwards`.

This only changes the active-session path. A fresh Shift+hotkey still
does a katakana conversion when no session is in scope.

**Validation**: unknown values log `ignoring [conversion]
katakana_modifier_behavior=<value> (expected katakana|cycle_backwards)`
and fall back to `cycle_backwards`.

**Status item**: when a secondary chord is bound, the menu-bar item
shows an extra `Katakana: <chord>` line below `Hotkey:` so the binding
is visible without rechecking the config.

**Preflight**: `modore-host --check-config` reports the resolved
secondary chord (or the collision reason). A malformed
`katakana_modifier` value exits `2`, same as a malformed `[clipboard]`
key.

## `[conversion] undo_window_ms`

**Available**: macOS

How long after a successful conversion **Esc** still reverts the
replacement back to the original reading. Mozc's top kanji candidate
is sometimes wrong (homonyms like 回答/解答, 公開/後悔, 期待/機体);
without undo the user has to delete and retype in hiragana to escape
the choice. Esc within the window AX-replaces the kanji back with the
reading that was picked up.

**Default**: `5000` (5 s). **Range**: `0` – `30000`. **Unit**:
milliseconds.

`0` disables the feature entirely — Esc passes through to the focused
app as normal, no snapshot is consulted. Useful for users who hit Esc
constantly in vim/IDE modal UIs and don't want any latency added on
the off chance their last conversion is still in scope.

**Behavior**:

| Situation                                                  | What Esc does                                  |
| ---------------------------------------------------------- | ---------------------------------------------- |
| Within window, replacement still at the recorded span      | Reverts to original reading; clears snapshot.  |
| Within window, but caret/focus changed or text edited     | Passes Esc through to the app (re-injected).   |
| Window expired                                             | Passes Esc through.                            |
| No previous AX-path conversion this session                | Passes Esc through.                            |
| Last conversion went through the clipboard fallback path   | Passes Esc through — fallback path doesn't snapshot. |

The clipboard fallback exclusion is deliberate: that path injects via
`postUnicode` into the focused app's keystroke stream rather than via
AX, leaving no stable span to revert to. Undo support there would
need a different mechanism (synthesizing backspaces + retyping the
reading) with worse failure modes than just letting Esc pass through.

**Logging**: every Esc outcome logs through the `[undo]` channel —
`reverted '<kanji>' → '<reading>' after Nms` on success, `esc fell
through: <reason>` for every fall-through path so a triage thread
sees exactly which gate rejected the undo.

**Validation**: non-integer or out-of-range values are ignored with a
`[config]` log line; the previous value (or the default at startup)
stays in effect. `--check-config` reports the resolved value and
exits `2` on a rejected value.

## `[clipboard]`

**Available**: macOS

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

## `[ui] candidate_panel`

**Available**: macOS

Controls whether the floating candidate-list panel appears alongside
the conversion gesture. Default is `none` — pre-feature behavior, no
panel.

| Value        | Effect                                                                                                              |
| ------------ | ------------------------------------------------------------------------------------------------------------------- |
| `none`       | Panel disabled. Conversion + cycle + Esc-undo work exactly as before.                                               |
| `on_cycle`   | Panel stays hidden on fresh conversions and reveals on the first cycle press, so it appears only when the user signals top-1 was wrong. |
| `on_convert` | Panel appears on every successful conversion and stays for the lifetime of the session window.                      |

The panel is non-activating — it floats above the focused app without
stealing focus, so the user keeps typing normally and the cycle/undo
hotkeys continue to drive selection. Positioned near the caret on the
AX fast-path (via `kAXBoundsForRangeParameterizedAttribute`) and near
the mouse cursor on the clipboard-fallback path. Hidden automatically
on session clear (any non-chord keystroke, focus change, or expiry of
`undo_window_ms`).

**Validation**: unknown values are ignored with a `[config]` log line
and the previous setting stays in effect. `--check-config` reports the
resolved mode and exits `2` on a rejected value.

## `[ui] candidate_panel_duration_ms`

**Available**: macOS

How long the candidate panel stays visible after each refresh (cycle
press or conversion), in **milliseconds**. The timer resets on every
refresh, so a chain of cycle presses keeps the panel alive for the
full duration after the last press. Esc-undo hides the panel
immediately regardless of this setting.

Default: `1500` (1.5 seconds). Set `0` to disable auto-hide — the
panel sticks until the session clears (any non-chord keystroke, focus
change, or `undo_window_ms` expiry). Valid range: `0..30000`.

**Validation**: non-integer or out-of-range values are ignored with a
`[config]` log line; the previous value stays in effect.
`--check-config` reports the resolved value and exits `2` on a
rejected value.

## Preflight: `modore-host --check-config`

**Available**: macOS

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

## Diagnostic: `modore-host --secure-input-status`

**Available**: macOS

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

## Path printers

**Available**: macOS

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

**Available**: macOS

Edits to `modore.conf` are picked up live — no restart needed. The
watcher applies a 300 ms quiet-window debounce, so multi-event saves
(atomic-rename editors like Vim, VSCode, JetBrains) land as a single
reload. Most `[conversion]` and `[clipboard]` keys reload immediately;
bridge launch-time knobs are re-parsed for logging but only take effect
on the next bridge init.

| Edit                                    | Behavior                                                                  |
| --------------------------------------- | ------------------------------------------------------------------------- |
| Hotkey changed to a valid value         | Swap immediately. Logs `config reloaded: …`.                              |
| Hotkey re-written to the same value     | No-op. Nothing logged.                                                    |
| Hotkey changed to a malformed value     | Keep the previous chord. Logs `config reload rejected: …`.                |
| `[conversion] hotkey` line removed      | Revert to the platform default.                                           |
| File deleted                            | Keep the previous chord; resume watching for the file to reappear.        |
| File re-created with a new chord        | Swap to the new chord. Same path as the "edit" case.                      |
| `[clipboard]` value changed             | Apply on next pickup. Logs `clipboard timings: …`.                        |
| `[clipboard]` value malformed / unknown | Ignore that key. Logs `ignoring [clipboard] …`. Other keys still applied. |
| `katakana_modifier` changed             | Re-bind secondary chord. Logs `katakana modifier: …` + `katakana chord registered/cleared`. |
| `katakana_modifier` malformed           | Keep previous value. Logs `ignoring [conversion] katakana_modifier=…`.    |
| `katakana_modifier_behavior` changed    | Swap active-session behavior. Logs `katakana modifier behavior: …`.       |
| `katakana_modifier_behavior` malformed   | Keep previous value. Logs `ignoring [conversion] katakana_modifier_behavior=…`. |
| `undo_window_ms` changed                | Swap window for the next Esc check. Logs `undo window: Nms` (or `disabled`). |
| `undo_window_ms` malformed / out-of-range | Keep previous value. Logs `ignoring [conversion] undo_window_ms=…`.       |
| `[bridge]` value changed                | Log the new value and note that restart is required for the bridge.      |

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
