# scripts

Example Lua scripts for modore's scripting engine. Drop into
`~/.config/modore/scripts/` (or `$XDG_CONFIG_HOME/modore/scripts/`) and
restart the host — modore picks them up automatically.

## File naming

- `default.lua` — applies to every app.
- `<app-id>.lua` — applies only when that app is focused (the bundle id
  on macOS, the wm-class on Linux).

A single repo example is named `example-*.lua` so it doesn't activate
on copy alone; rename it to `default.lua` or the matching app id.

```sh
mkdir -p ~/.config/modore/scripts
cp scripts/example-obsidian.lua ~/.config/modore/scripts/md.obsidian.lua
```

## Hooks

All hooks are independently optional. Define only the ones you care
about; everything else falls through to host defaults. Returning `nil`
from any hook is equivalent to not defining it for that call.

| Hook                  | Purpose                                        |
| --------------------- | ---------------------------------------------- |
| `on_pickup(ctx)`      | Override the word-boundary span around caret.  |
| `on_replacement(s,c)` | Rewrite Mozc's chosen replacement.             |
| `route_for_app(ctx)`  | Force `"clipboard"` / `"ax"` / `"keystroke"`.  |
| `on_candidates(l,i)`  | Reorder or filter the candidate list.          |
| `on_acquire(ctx)`     | Compose a custom text-acquisition routine.     |

## Primitives

Available inside `on_acquire` (and any hook, technically):

- `modore.host.send_chord(chord)` — synthesize a key chord
- `modore.host.sleep_ms(ms)` — pause
- `modore.host.clipboard_read()` — current clipboard text or `nil`
- `modore.host.clipboard_write(s)` — set clipboard, returns boolean
- `modore.host.read_selection()` — focused accessibility selection or `nil`

Pure text helpers mirror the host baseline in UTF-8 byte offsets:

- `modore.text.word_bounds(text, caret_byte)` — `{ start_byte, end_byte }`
- `modore.text.split_trailing_ascii(s)` — `prefix, tail`
- `modore.text.split_acronym_head(s)` — `head, tail`

`modore.default.{pickup,replacement,route}` calls the host baseline when
the host has registered it. This is the preferred way to wrap default
behaviour in app-specific scripts instead of copying it.

`modore.log.{info,warn,error}(msg)` interleaves with the host log.

## Caveats

- Scripts run in a sandbox with `io`, `os.execute`, `package`, `require`,
  `ffi`, `debug` stripped. Local-user trust model — not adversarial.
- Reload is mtime-based: save the file and the next hotkey press picks
  up the new behaviour.
- LuaJIT 2.1 interpreter; JIT off by default on macOS until the
  `com.apple.security.cs.allow-jit` entitlement is wired.
