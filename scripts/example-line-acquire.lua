-- example-line-acquire.lua — custom selection recipe via on_acquire.
--
-- The host's built-in clipboard fallback extends selection with
-- Shift+Opt+Left (word-back). Some apps select different units on that
-- chord, or the user wants the whole current line instead. on_acquire
-- lets the script compose its own routine using modore.host.* primitives
-- and return the picked text directly.
--
-- This example targets "com.example.linecopy" — pretend that app's
-- editor needs Shift+End to select the line tail before Cmd+C. Adjust
-- the app id + chord to your needs.

modore.on_acquire = function(ctx)
  if ctx.app_id ~= "com.example.linecopy" then
    return nil  -- everything else uses the host's default acquisition
  end

  local h = modore.host

  -- Preserve the user's existing clipboard contents.
  local saved = h.clipboard_read()

  -- Extend selection to the end of the current line, then copy.
  h.send_chord("shift+end")
  h.sleep_ms(20)
  h.send_chord("cmd+c")
  h.sleep_ms(30)
  local picked = h.clipboard_read()

  -- Restore. Note: the script is expected to leave the selection ACTIVE
  -- so the host's postUnicode write lands on top of it. Restoring the
  -- clipboard doesn't touch the selection.
  h.clipboard_write(saved or "")

  return picked
end
