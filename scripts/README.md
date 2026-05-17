# scripts

Example Lua scripts for modore's scripting engine. Drop into
`~/.config/modore/scripts/` (or `$XDG_CONFIG_HOME/modore/scripts/`) and
restart the host — modore picks them up automatically.

## File naming

- `default.lua` — applies to every app.
- `<app-id>.lua` — applies only when that app is focused (the bundle id
  on macOS, the wm-class on Linux).

`example-line-acquire.lua` is the most explicit command-by-command
sample. The other `example-*.lua` files show smaller per-app overrides
that still call host/default helpers directly. Rename any example to
`default.lua` or the matching app id before using it.

```sh
mkdir -p ~/.config/modore/scripts
cp scripts/example-obsidian.lua ~/.config/modore/scripts/md.obsidian.lua
cp scripts/example-discord.lua ~/.config/modore/scripts/com.hnc.Discord.lua
cp scripts/example-chrome.lua ~/.config/modore/scripts/com.google.Chrome.lua
```

The shipped examples are intentionally opinionated about the quirks we
have already observed:

- `example-chrome.lua` owns Chrome's acquisition choreography because
  Chromium page text fields and DevTools have different failure modes
  from the omnibox.
- `example-discord.lua` keeps Discord on the selection-sync route and
  prefers the focused selection when it exists.
- `example-obsidian.lua` keeps Obsidian on the AX route and leaves AX
  write fallback to the host baseline.

## Hooks

All hooks are independently optional. Define only the ones you care
about; everything else falls through to host defaults. Returning `nil`
from any hook is equivalent to not defining it for that call.

The stage hooks are imperative. The host passes the script's `modore`
table as a second `api` argument so hooks can call helpers directly
instead of returning a declarative plan:

- `modore.on_pickup(ctx, api)` — choose the pickup span
- `modore.on_acquire(ctx, api)` — define a custom acquisition recipe
- `modore.route_for_app(ctx, api)` — pick the delivery route
- `modore.on_replacement(span, cands, api)` — rewrite the committed text
- `modore.on_candidates(cands, i, api)` — reorder or filter candidates

| Hook                  | Purpose                                        |
| --------------------- | ---------------------------------------------- |
| `on_pickup(ctx, api)` | Override the word-boundary span around caret.  |
| `on_replacement(s,c, api)` | Rewrite Mozc's chosen replacement.       |
| `route_for_app(ctx, api)` | Force `"clipboard"` / `"ax"` / `"selection_sync"` / `"keystroke"`.  |
| `on_candidates(l,i, api)` | Reorder or filter the candidate list.     |
| `on_acquire(ctx, api)` | Compose a custom text-acquisition routine.     |

## Primitives

Available inside any hook via the explicit `api` argument:

- `modore.host.send_chord(chord)` — synthesize a key chord
- `modore.host.sleep_ms(ms)` — pause
- `modore.host.clipboard_read()` — current clipboard text or `nil`
- `modore.host.clipboard_write(s)` — set clipboard, returns boolean
- `modore.host.read_selection()` — focused accessibility selection or `nil`

Pure text helpers mirror the host baseline in UTF-8 byte offsets:

- `modore.text.word_bounds(text, caret_byte)` — `{ start_byte, end_byte }`
- `modore.text.split_trailing_ascii(s)` — `prefix, tail`
- `modore.text.split_trailing_ascii_punctuation(s)` — `core, suffix`
- `modore.text.split_acronym_head(s)` — `head, tail`
- `modore.text.normalize_pickup_suffix(s)` — maps terminal `.` / `,` / `-`
  the same way the native pickup path does

`modore.default.{pickup,replacement,route,acquire,candidates}` calls the
host baseline when the host has registered it. Scripts should treat
these as ordinary helpers: call them, inspect the result, and only then
decide whether to keep, modify, or replace it.

`on_acquire(ctx, api)` receives the base pickup context and, when the
host has it, field metadata:

- `ctx.field_role`
- `ctx.field_description`

That extra context is what lets scripts distinguish Chrome's omnibox
from ordinary page text fields, or choose AX selection reads only for the
apps that can tolerate them.

`route_for_app(ctx, api)` sees the same context table and can return
`"selection_sync"` for apps that should keep the current selection but
skip the generic AX text write path.

If you want a complete command-by-command walkthrough, start with
`example-line-acquire.lua`.

`modore.log.{info,warn,error}(msg)` interleaves with the host log.

## Caveats

- Scripts run in a sandbox with `io`, `os.execute`, `package`, `require`,
  `ffi`, `debug` stripped. Local-user trust model — not adversarial.
- Reload is mtime-based: save the file and the next hotkey press picks
  up the new behaviour.
- LuaJIT 2.1 interpreter; JIT off by default on macOS until the
  `com.apple.security.cs.allow-jit` entitlement is wired.
