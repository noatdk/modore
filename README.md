# modore

Modeless Japanese IME pickup. Type romaji into any app, hit the conversion
hotkey (default `Ctrl+/`), and the word at the caret is replaced with its Mozc
top-candidate conversion.

## Status

macOS: configurable conversion hotkey (default `Ctrl+/`), top-candidate only.

Linux (`native/linux/`): same; X11 global hotkey + AT-SPI2 (with clipboard/XTest
fallback). Same Mozc bridge as macOS.

**Wayland / no X11 grab:** the host also listens on a Unix socket (enabled by
default). Run **`modore-host --ipc-only`** when you do not want an X11 hotkey
grab, or keep the default binary and trigger conversion from the compositor
with **`modore-host --trigger`**. Socket path: **`$XDG_RUNTIME_DIR/modore.sock`**
if `XDG_RUNTIME_DIR` is set, otherwise **`/tmp/modore.<uid>.sock`**. Use
**`--no-ipc`** to disable the socket server. Optional: install **`wtype`** or
**`ydotool`** so synthetic keys work when there is no `DISPLAY` or the focused
window is native Wayland.

**Chromium / Electron (Hyprland, etc.):** when **`DISPLAY` is set** (typical
XWayland), **`modore-host --trigger`** prefers the **X11 keyboard path** (`XTest`
copy/paste helpers) because many Electron/Chromium builds still attach to **`DISPLAY`**.
Check **`modore.log`** for **`ipc pickup: using X11 keyboard/display path`**.
Pure ozone/Wayland-only browsers ( **`DISPLAY` unset** ) rely on **`wtype` /
`hyprctl`** + **`wl-clipboard`** the same way as other Wayland natives.

## Build & run

```sh
make             # print the list of available targets
make build       # build the host app for the current platform
make run         # build and launch (Linux + macOS)
make open        # macOS only: build and open the .app bundle
```

First build pulls ~150 MB of Mozc + protobuf + abseil source and downloads
the ~48 MB Mozc OSS dictionary. Subsequent builds are incremental and finish
in a few seconds.

On first launch, macOS prompts for **Accessibility** permission — required
for reading/writing the focused text field. Grant it in
*System Settings → Privacy & Security → Accessibility*, then re-launch.

On Linux, AT-SPI uses your session D-Bus (run from the same graphical login).
Install `xclip` or `wl-clipboard` if you need the clipboard fallback path.
Some desktops require **Accessibility** features enabled so AT-SPI can inspect
focused widgets; toolkits vary.

**Compositor shortcut (Hyprland-style):** start the host once (for example
`modore-host --ipc-only` in your session autostart, or the default host if X11
grab works for you). Then bind a key to **`modore-host --trigger`** — for
example in Hyprland:

```ini
bind = $mainMod, slash, exec, /path/to/modore-host --trigger
```

Use the real path to `modore-host` from `make build` / `native/linux/build/`.

After that, the configured hotkey (and/or compositor binding) converts the word
at the caret when the host can see the focused field or the clipboard path
applies.

## Configuration

Both hosts read **`$XDG_CONFIG_HOME/modore/modore.conf`** if set, otherwise
**`~/.config/modore/modore.conf`** (same path on macOS).

INI-style file; only **`[conversion]`** is defined today — add sections/keys as
features grow:

```ini
# Example chords (repository ships modore.conf.example with Ctrl+Shift+grave).
[conversion]
hotkey = Ctrl+Shift+grave
```

**Modifiers** (combine with `+`): `Ctrl`, `Shift`, `Alt` (also `Option` / `Meta`),
`Super` (also `Win`; on macOS `Command` / `Cmd` map to the Command modifier).

**Key names**: `Slash`, `Period`, `Comma`, letters, digits, `F1`–`F12`, space,
arrows, `Return`, `Tab`, etc. Linux also accepts any name understood by
`XStringToKeysym(3)`.

If the file is missing or `hotkey` is absent, the default is **`Ctrl+Slash`**.
A bad `hotkey` value logs a warning and falls back to the default.

On Linux, **`modore.log`** echoes the resolved **`[conversion] hotkey=…`** line
after each **`modore-host` start**.

## Layout

```
bridge/             Cross-platform C ABI around Mozc. CMake build.
native/macos/       Swift host: event tap + Accessibility + clipboard fallback.
native/linux/       C++ host: X11 grab + Unix socket IPC + AT-SPI2 + clipboard fallback.
third_party/        fcitx5-mozc submodule (provides CMake build of Mozc engine).
```

The bridge is a shared library (`libmozc_bridge.dylib` on macOS,
`libmozc_bridge.so` on Linux, ~25 MB) that statically links the Mozc engine,
abseil, and protobuf. Frontends only need to consume the flat C ABI in
`bridge/include/mozc_bridge.h`.

## Requirements

- CMake 3.22+
- Python 3 (Mozc’s build scripts invoke `python`)
- **macOS**: Xcode Command Line Tools
- **Linux**: GCC or Clang with C++20, X11 + XTest dev (`libX11`, `libXtst`), AT-SPI
  (`atspi-2`, GLib), and `pkg-config`. Clipboard helpers: `xclip` (X11) and/or
  `wl-clipboard` (`wl-paste` / `wl-copy`) on Wayland compositors.

## License

MIT, see [LICENSE](LICENSE). Bundled third-party code (Mozc, fcitx5-mozc,
abseil-cpp, protobuf) is BSD-3-Clause; see [bridge/NOTICE.md](bridge/NOTICE.md).
