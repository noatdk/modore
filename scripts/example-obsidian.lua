-- example-obsidian.lua — force the clipboard pickup path for Obsidian.
--
-- Obsidian's CodeMirror editor silently rejects AX writes, so the host's
-- default "try AX first, fall back to clipboard" routine wastes a round
-- trip on every conversion. This script tells the host to skip the AX
-- attempt and go straight to the clipboard path when the focused app is
-- Obsidian.
--
-- Drop this file in (renamed to default.lua, or copied as-is and kept
-- alongside other examples; modore loads any *.lua file in the scripts
-- dir). All other hooks (on_pickup, on_replacement, on_candidates,
-- on_acquire) stay undefined — they fall through to the host defaults,
-- and modore behaves exactly as it would without this script for every
-- other app.

modore.route_for_app = function(app_id)
  if app_id == "md.obsidian" then
    return "clipboard"
  end
  return nil  -- explicit "no opinion" — host uses its default route
end
