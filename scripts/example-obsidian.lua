-- example-obsidian.lua — force the clipboard pickup path for Obsidian.
--
-- Obsidian's CodeMirror editor silently rejects AX writes, so the host's
-- default "try AX first, fall back to clipboard" routine wastes a round
-- trip on every conversion. This script tells the host to skip the AX
-- attempt and go straight to the clipboard path when the focused app is
-- Obsidian.
--
-- This is the canonical example of the per-hook opt-in pattern: it
-- defines only `route_for_app`. Pickup, replacement, candidates, and
-- acquire all stay on the host's built-in implementations, so modore
-- behaves exactly as it would without this script for every other app.
--
-- Install
-- -------
--
--     mkdir -p ~/.config/modore/scripts
--     # macOS bundle id:
--     cp scripts/example-obsidian.lua ~/.config/modore/scripts/md.obsidian.lua
--     # On Linux, Obsidian's wm-class is "obsidian" — use that instead:
--     # cp scripts/example-obsidian.lua ~/.config/modore/scripts/obsidian.lua
--
-- Both ids are accepted by the route check below, so the same file
-- works under whichever name the host expects.
--
-- Verification
-- ------------
--
-- After installing, press the conversion hotkey while focused on
-- Obsidian. The host log (Console.app on macOS, `~/.config/modore/modore.log`
-- on Linux) should show:
--
--     [pickup] scripted route → clipboard [Obsidian / md.obsidian]
--     [clipboard] skipping existing-selection peek (md.obsidian line-copies without newline)
--
-- The first line is proof the hook fired; the second is the host taking
-- the route the script asked for. Without the script you would instead
-- see an `[ax] replace verify failed` line followed by the keystroke
-- fallback — the round trip this script is meant to skip.

local OBSIDIAN_IDS = { ["md.obsidian"] = true, ["obsidian"] = true }

modore.route_for_app = function(app_id)
  if app_id and OBSIDIAN_IDS[app_id] then
    return "clipboard"
  end
  return nil  -- explicit "no opinion" — host uses its default route
end
