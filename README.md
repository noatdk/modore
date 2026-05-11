# modore

Modeless Japanese IME pickup. Type romaji into any app, hit
`Ctrl+/`, and the word at the caret gets replaced with its Mozc top-candidate
conversion.

## Status

macOS only, single hotkey, top-candidate only. Bridge is cross-platform —
Linux/Windows hosts are a future exercise.

## Build & run

```sh
make             # print the list of available targets
make build       # build the host app for the current platform
make open        # launch it
```

First build pulls ~150 MB of Mozc + protobuf + abseil source and downloads
the ~48 MB Mozc OSS dictionary. Subsequent builds are incremental and finish
in a few seconds.

On first launch, macOS prompts for **Accessibility** permission — required
for reading/writing the focused text field. Grant it in
*System Settings → Privacy & Security → Accessibility*, then re-launch.

After that, `Ctrl+/` anywhere converts the word at the caret.

## Layout

```
bridge/        Cross-platform C ABI around Mozc. CMake build.
native/macos/  Swift host: event tap + Accessibility + clipboard fallback.
third_party/   fcitx5-mozc submodule (provides CMake build of Mozc engine).
```

The bridge is a `libmozc_bridge.dylib` (~25 MB) that statically links the
Mozc engine, abseil, and protobuf. Frontends only need to consume the flat
C ABI in `bridge/include/mozc_bridge.h`.

## Requirements

- (macOS) Xcode Command Line Tools
- CMake 3.22+ (Homebrew: `brew install cmake`)
- Python 3 (Homebrew or [python.org](https://python.org/) — Required for Mozc's build

## License

MIT, see [LICENSE](LICENSE). Bundled third-party code (Mozc, fcitx5-mozc,
abseil-cpp, protobuf) is BSD-3-Clause; see [bridge/NOTICE.md](bridge/NOTICE.md).
