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

Use `--ipc-only` on Wayland sessions where the X11 grab can't be relied
on, or when you'd rather drive conversion from a compositor keybind.
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
fire while an XWayland client has focus. For everything else, bind the
conversion key in your compositor and let it call `modore-host --trigger`.

Install one of `wtype` or `ydotool` so the host can synthesize keys, and
`wl-clipboard` (`wl-paste` / `wl-copy`) so the clipboard fallback path
works.

Hyprland example:

```ini
exec-once = /path/to/modore-host --ipc-only
bind = $mainMod, slash, exec, /path/to/modore-host --trigger
```

Use the real path to `modore-host` from `make build` /
`native/linux/build/`.

## Chromium / Electron quirks

When `DISPLAY` is set on a Wayland session (the typical XWayland setup
under Hyprland), `modore-host --trigger` prefers the X11 keyboard path
(`XTest` copy/paste helpers) because many Electron and Chromium builds
still attach to `DISPLAY` even on Wayland sessions. Look for this line
in `modore.log` to confirm:

```
ipc pickup: using X11 keyboard/display path
```

Pure ozone / Wayland-only browsers (i.e. `DISPLAY` unset) use the same
`wtype` / `hyprctl` + `wl-clipboard` path as any other native-Wayland
client.

## AT-SPI prerequisites

AT-SPI uses your session D-Bus, so the host must run from the same
graphical login as the apps you're typing into. Some desktops require
"Enable Accessibility" or equivalent to be turned on before AT-SPI will
expose the focused widget; the exact toggle varies by toolkit.

If AT-SPI can't reach the focused field — which is common for native
Wayland Chromium/Electron — the host falls back to the clipboard path.
Set `MODORE_ATSPI_ONLY=1` if you'd rather fail loudly than fall back.

## systemd user unit

`native/linux/systemd/user/modore-host.service` ships an `--ipc-only`
service that starts with the graphical session. Drop it in with:

```sh
make -C native/linux install-user-bin   # copy binary to ~/.local/bin
systemctl --user enable --now modore-host
```

To run with the X11 grab as well, `systemctl --user edit modore-host`
and remove `--ipc-only` from `ExecStart`.

## Environment knobs

`modore-host --help` prints the current list. The ones you're most
likely to need:

- `MODORE_IPC_SOCKET=/path` — override the socket path.
- `MODORE_E2E_TRACE=1` — verbose `[e2e]` step logs (Puppeteer, debugging).
- `MODORE_ATSPI_ONLY=1` — refuse the clipboard fallback if AT-SPI can't
  read the focused field.
- `MODORE_WL_GLYPH_ERASE_ROMANJI=1` — Wayland: per-glyph BackSpace for
  plain romaji instead of the default `Ctrl+A` clear. Use for GTK mixed
  fields; the default works better for Chromium omnibox.
