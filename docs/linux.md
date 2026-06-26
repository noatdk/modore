# Linux runtime guide

Setup notes that go beyond `make build && make run`: Wayland compositors,
the IPC socket, Chromium/Electron edge cases, and the systemd user unit.

## Runtime modes

The Linux host always tries to do two things on startup: grab the
configured X11 hotkey, and listen for `--trigger` on a Unix socket. Two
flags trim that surface:

| Flag           | X11 hotkey grab | Unix socket listener |
| -------------- | :-------------: | :------------------: |
| *(default)*    |        ✓        |          ✓           |
| `--ipc-only`   |        ✗        |          ✓           |
| `--no-ipc`     |        ✓        |          ✗           |

On Wayland, the host first tries to monitor `/dev/input/event*` directly
so native apps can trigger conversion without a compositor bind. If raw
input access is unavailable, it falls back to the X11 grab path and, on
Hyprland, a compositor bind.
Use `--no-ipc` if you don't want a socket on disk.

`modore-host --trigger` is a separate command that connects to the
socket and asks a running host to run conversion. It exits as soon as
the message is delivered.

## IPC socket path

Resolved in this order (the listener and `--trigger` both use the same
lookup):

1. `$MODORE_IPC_SOCKET` if set (escape hatch for tests or a second instance).
2. `$XDG_RUNTIME_DIR/modore.sock` if `XDG_RUNTIME_DIR` is set.
3. `/run/user/$UID/modore.sock` (matches systemd `%t`).
4. `/tmp/modore.<uid>.sock` (legacy, last resort).

Hyprland's `exec` sometimes drops `XDG_RUNTIME_DIR`; the `/run/user/$UID`
fallback exists for that case.

## Wayland setup

Native-Wayland windows don't see X11 grabs, so the X11 hotkey will only
fire while an XWayland client has focus. On Wayland, `modore-host`
prefers a raw `/dev/input/event*` monitor like espanso does. If that
device access is not available, the host falls back to the compositor
bind / `--trigger` path.

Install one of `wtype` or `ydotool` so the host can synthesize keys, and
`wl-clipboard` (`wl-paste` / `wl-copy`) so the clipboard path works.
The Wayland flow now mirrors espanso more closely: app-specific pickup
uses a single action queue, replacement writes the converted text to the
clipboard, pastes once, and restores the previous clipboard afterward.
The clipboard and pickup waits are configurable in `modore.conf` under
`[clipboard]`, and Linux hot-reloads those timing values without a
rebuild.
`make -C native/linux run` now tries to bootstrap raw `/dev/input/event*`
access automatically by applying `cap_dac_override+p` to the installed
binary under `~/.local/lib/modore/modore-host`. When the shell is not
root, it tries `sudo setcap` and may prompt for a password. If that still
fails, it prints a warning and the host falls back to the compositor/X11
path instead. You can still grant the capability manually in the same
spirit as espanso's Linux install notes (`setcap 'cap_dac_override+p'
~/.local/lib/modore/modore-host` is the shape they document).

Timing knobs worth tuning first:

- `pre_copy_delay_ms` / `pre_paste_delay_ms`
- `read_timeout_ms` / `paste_visibility_wait_ms`
- `restore_clipboard_delay_ms` / `short_restore_delay_ms`
- `wayland_select_settle_ms`
- `wayland_copy_poll_ms`
- `wayland_copy_poll_step_ms`
- `cycle_backspace_step_ms`

If the clipboard is still racing, increase `wayland_copy_poll_ms` and
`restore_clipboard_delay_ms` before touching the rest of the flow.

Hyprland example:

```ini
exec-once = /path/to/modore-host
```

Use the real path to `modore-host` from `make build` /
`native/linux/build/`.

## Logging

The Linux host writes tagged lines to stderr and `~/.config/modore/modore.log`.
The tags are stable and meant for filtering the same way the macOS host uses
subsystem categories: `boot`, `config`, `hotkey`, `ipc`, `pickup`, `atspi`,
`clipboard`, `mozc`, `scripting`, `e2e`, plus the generic `host` fallback for
older call sites.

`MODORE_E2E_TRACE=1` adds step-level traces for pickup and injection paths.
That is the quickest way to understand which branch the host took on a given
conversion.

## Chromium / Electron quirks

When `DISPLAY` is set on a Wayland session, XWayland clients still use
the X11 keyboard helpers. Native Wayland clients use the Hyprland bind
plus the clipboard-paste path above. The host logs the focused window and
the selected flow before pickup starts, so the active app is visible even
when AT-SPI does not expose the field.

## AT-SPI prerequisites

AT-SPI uses your session D-Bus, so the host must run from the same
graphical login as the apps you're typing into. Some desktops require
"Enable Accessibility" or equivalent to be turned on before AT-SPI will
expose the focused widget; the exact toggle varies by toolkit.

If AT-SPI can't reach the focused field — which is common for native
Wayland Chromium/Electron — the host falls back to the clipboard path.
Set `MODORE_ATSPI_ONLY=1` if you'd rather fail loudly than fall back.

## systemd user unit

`native/linux/systemd/user/modore-host.service` starts with the graphical
session and listens on the IPC socket while keeping the local hotkey path
enabled by default. Drop it in with:

```sh
make -C native/linux install-user-bin   # copy binary to ~/.local/bin
systemctl --user enable --now modore-host
```

To run in manual compositor-bind mode, `systemctl --user edit modore-host`
and add `--ipc-only` to `ExecStart`.

`make -C native/linux run` now installs the binary, reloads the user
unit, restarts `modore-host`, and tries to bootstrap evdev access in one
step. Use `make -C native/linux run-local` if you want the old behavior of
launching the freshly built binary directly.

## Conversion backend (atzc)

Default conversion is the bundled in-process Mozc. To relay to a
Wine-hosted engine via [atzc](https://github.com/noatdk/atzc) instead,
build the bridge with the backend compiled in:

```sh
make fetch-atzc                    # clone the client into third_party/atzc-server
make bridge MODORE_ENABLE_ATZC=1   # or pass MODORE_ATZC_DIR=<existing checkout>
make build                         # relink the host against the new bridge .so
```

Then select it in `~/.config/modore/modore.conf`:

```ini
[bridge]
mozc_backend = atzc                # built-in (default) | atzc
```

`atzcd` owns the engine and serves conversions; start it separately
(`atzcd --engine-dir <path>`). modore connects lazily and reconnects once
on failure, so start order doesn't matter — if `atzcd` is down a conversion
just fails and the romaji is left in place. Socket path follows atzc's
default (`$ATZC_SOCKET`, else `$XDG_RUNTIME_DIR/atzcd.sock`, else
`/tmp/atzcd-<uid>.sock`).

Caveats: a bridge built **without** `MODORE_ENABLE_ATZC` exits at startup
with `unknown mozc backend: atzc` if the config selects it. The katakana
chord falls back to the engine's top-1 kanji (atzc has no katakana mode).

## Environment knobs

`modore-host --help` prints the current list. The ones you're most
likely to need:

- `MODORE_IPC_SOCKET=/path` — override the socket path.
- `MODORE_E2E_TRACE=1` — verbose `[e2e]` step logs (Puppeteer, debugging).
- `MODORE_ATSPI_ONLY=1` — refuse the clipboard fallback if AT-SPI can't
  read the focused field.
- `MODORE_MOZC_BACKEND=atzc` — pick the conversion backend, overriding
  `[bridge] mozc_backend`. Requires an atzc-enabled bridge (see above).
