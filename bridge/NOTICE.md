# Third-party notices

This bridge vendors and links against the following third-party projects.

## fcitx-contrib/fcitx5-mozc — CMake build of mozc

- Repository: https://github.com/fcitx-contrib/fcitx5-mozc
- License: BSD 3-clause (see `third_party/fcitx5-mozc/LICENSE`)
- Used here as a git submodule under `third_party/fcitx5-mozc/`.
- We vendored `src/unix/fcitx5/mozc_direct_client.{cc,h}` into
  `bridge/src/direct_client.{cc,h}` with minor edits documented at the top
  of each file.

## google/mozc — Japanese IME engine

- Repository: https://github.com/google/mozc
- License: BSD 3-clause (see `third_party/fcitx5-mozc/mozc/LICENSE`)
- Linked statically via the `mozc-static` target produced by fcitx5-mozc's
  CMake build.

## google/protobuf, abseil-cpp

- Built and linked as part of fcitx5-mozc's CMake build.
- Licenses: see their respective LICENSE files under
  `third_party/fcitx5-mozc/`.
