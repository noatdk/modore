-- example-line-acquire.lua — full imperative pickup flow.
--
-- This is the shape we want scripts to have: the hook code owns the
-- stage logic directly and composes host/default helpers command by
-- command. Nothing here returns a preset "mode"; each hook decides what
-- to do and when to fall back.
--
-- This example targets "com.example.linecopy" — pretend that app's
-- editor wants the current line tail, but sometimes already exposes a
-- useful selection. Adjust the app id and the chord sequence to your own
-- app.

local LINECOPY_APP = "com.example.linecopy"

local function is_linecopy(ctx)
  return ctx.app_id == LINECOPY_APP
end

local function copy_with_chords(api, chord)
  local h = api.host
  local saved = h.clipboard_read()

  h.send_chord(chord)
  h.sleep_ms(20)
  h.send_chord("cmd+c")
  h.sleep_ms(30)

  local picked = h.clipboard_read()
  if saved ~= nil then
    h.clipboard_write(saved)
  end
  return picked
end

modore.on_acquire = function(ctx, api)
  if not is_linecopy(ctx) then
    return nil
  end

  local h = api.host
  local selected = h.read_selection()
  if selected ~= nil and #selected > 0 then
    return selected
  end

  return copy_with_chords(api, "shift+end")
end

modore.on_pickup = function(ctx, api)
  if not is_linecopy(ctx) then
    return nil
  end

  local span = api.default.pickup(ctx)
  if span == nil then
    return nil
  end

  local text = ctx.full_text or ""
  local picked = text:sub(span.start_byte + 1, span.end_byte)
  local head, tail = api.text.split_trailing_ascii(picked)
  if tail ~= "" then
    span.start_byte = span.start_byte + #head
  end

  return span
end

modore.route_for_app = function(ctx, api)
  if not is_linecopy(ctx) then
    return nil
  end
  return api.default.route(ctx)
end

modore.on_replacement = function(span, cands, api)
  local chosen = api.default.replacement(span, cands)
  if chosen == nil then
    return nil
  end
  local core, suffix = api.text.split_trailing_ascii_punctuation(chosen)
  if suffix ~= "" then
    local normalized = api.text.normalize_pickup_suffix(suffix)
    if normalized ~= nil then
      return core .. normalized
    end
  end
  return chosen
end
