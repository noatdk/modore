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
├── include/modore_script.h    ABI v1 header
├── src/engine.c               init/shutdown wrapping a lua_State
├── tests/smoke.c              non-null + idempotent shutdown check
└── README.md
```

## Features (ABI v1)

Scripts live in `~/.config/modore/scripts/` with optional per-app overrides: `default.lua` (fallback) and `<app_id>.lua` (e.g., `md.obsidian.lua`). Hooks are independently optional — define any subset of `modore.on_pickup`, `modore.on_replacement`, `modore.route_for_app`, `modore.on_candidates`. Missing, undefined, or error-returning hooks silently fall through to host defaults. Logging via `modore.log.{info,warn,error}` and trampoline access via `modore.default.{pickup,replacement,route}`. LuaJIT 2.1 interpreter or JIT (toggleable; Apple Silicon needs `com.apple.security.cs.allow-jit` entitlement). Sandbox strips `io`, `os.execute`, `os.popen`, `package`, `require`, `ffi`, and `debug`. Host wiring (integration with macOS/Linux/Windows shells) is still pending.
