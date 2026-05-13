# engine

`libmodore_script.{dylib,so,dll}` — Lua scripting engine for modore.
Sibling to `libmozc_bridge`. Hosts dlopen it through `modore_script_*`
in `include/modore_script.h`.

## Build

From repo root:

```sh
make fetch-luajit   # one-time: clones LuaJIT 2.1 into third_party/luajit/
make engine         # builds build/engine/libmodore_script.{dylib,so}
make engine-test    # runs the smoke harness
```

LuaJIT is fetched lazily, not a submodule. Only users who want scripting
pay the clone (~3 MB).

## Layout

```
engine/
├── CMakeLists.txt
├── include/modore_script.h    ABI header (Phase 01 stub; pinned in Phase 02)
├── src/engine.c               init/shutdown wrapping a lua_State
├── tests/smoke.c              non-null + idempotent shutdown check
└── README.md
```

## Caveats

- macOS arm64 only this phase. Dual-arch fat lib comes later if needed.
- JIT toggleable via `-DMODORE_SCRIPT_JIT=OFF`. Default ON. Apple Silicon
  app needs `com.apple.security.cs.allow-jit` entitlement (added in
  Phase 03).
- Scripts and hooks come in Phase 02. Right now the lib only opens an
  empty Lua state with stdlib loaded.
