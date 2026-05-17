-- example-obsidian.lua — imperative stage hooks for Obsidian.
--
-- Obsidian is the opposite of Discord here: keep the host on the AX
-- route, but let the script explicitly own the stage flow so the
-- baseline pickup/replacement helpers stay visible and easy to wrap.
--
-- The important quirk we preserve is that Obsidian's editor is happy
-- with AX reads, but AX writes can be rejected. The host's fallback
-- handles that already, so the script should not reintroduce clipboard
-- probing or other broad fallback policy.

local OBSIDIAN_ID = "md.obsidian"

local function is_obsidian(ctx)
  return ctx.app_id == OBSIDIAN_ID
end

local function is_suspicious_copy(s)
  if s == nil or s == "" then
    return true
  end
  if #s > 256 then
    return true
  end
  if s:find("\n", 1, true) or s:find("\r", 1, true) then
    return true
  end
  if s == "AXTextField" or s == "AXTextArea" then
    return true
  end
  if s == "Bookmark name" or s == "New Tab" then
    return true
  end
  return false
end

local function looks_like_short_romaji_tail(s)
  return s ~= nil
    and #s > 0
    and #s <= 6
    and s:match("^[a-z]+$") ~= nil
end

local function needs_more_left_context(api, picked)
  if picked == "." or picked == "," or picked == "-" then
    return true
  end
  if picked == nil then
    return false
  end
  if looks_like_short_romaji_tail(picked) then
    return true
  end
  local core, suffix = api.text.split_trailing_ascii_punctuation(picked)
  if suffix ~= "" then
    return core:match("[a-z]") ~= nil and suffix:match("^[%.,%-]+$") ~= nil
  end
  return false
end

local function acquire_with_copy(api, chord)
  local h = api.host
  local saved = h.clipboard_read()
  modore.log.info("obsidian acquire: copy fallback via " .. chord)
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

modore.route_for_app = function(ctx, api)
  modore.log.info("obsidian route hook entered")
  if not is_obsidian(ctx) then
    return nil
  end
  modore.log.info("obsidian route: ax")
  return "ax"
end

modore.on_acquire = function(ctx, api)
  modore.log.info("obsidian acquire hook entered")
  if not is_obsidian(ctx) then
    return nil
  end
  local selected = api.host.read_selection()
  if selected ~= nil and #selected > 0 then
    modore.log.info("obsidian acquire: using focused selection")
    if needs_more_left_context(api, selected) then
      modore.log.info("obsidian acquire: punctuation/short tail; trying line-left copy")
      local widened = acquire_with_copy(api, "shift+cmd+left")
      if not is_suspicious_copy(widened) then
        modore.log.info("obsidian acquire: line-left copy accepted")
        return widened
      end
    end
    return selected
  end
  modore.log.info("obsidian acquire: default AX acquire")
  local picked = api.default.acquire(ctx)
  if needs_more_left_context(api, picked) then
    modore.log.info("obsidian acquire: punctuation/short tail; trying line-left copy")
    local widened = acquire_with_copy(api, "shift+cmd+left")
    if not is_suspicious_copy(widened) then
      modore.log.info("obsidian acquire: line-left copy accepted")
      return widened
    end
  end
  if is_suspicious_copy(picked) then
    modore.log.info("obsidian acquire: default acquire was suspicious; abort")
    return nil
  end
  return picked
end

modore.on_pickup = function(ctx, api)
  modore.log.info("obsidian pickup hook entered")
  if not is_obsidian(ctx) then
    return nil
  end
  local span = api.default.pickup(ctx)
  if span == nil then
    modore.log.info("obsidian pickup: default pickup returned nil")
    return nil
  end
  modore.log.info(
    "obsidian pickup: default span [" ..
    tostring(span.start_byte) .. ".." ..
    tostring(span.end_byte) .. "]")
  return span
end

modore.on_replacement = function(span, cands, api)
  modore.log.info("obsidian replacement hook entered")
  if not span or not cands then
    return nil
  end
  return api.default.replacement(span, cands)
end
