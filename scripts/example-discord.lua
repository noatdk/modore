-- example-discord.lua — imperative stage hooks for Discord.
--
-- Discord's quirk profile is the one the native defaults already learned:
-- writeback should stay on the selection-sync route, but acquisition needs
-- to be explicit. The default helper reads selection first, which is the
-- wrong vector here; instead we collapse any stale selection and then copy
-- the current word directly.

local DISCORD_ID = "com.hnc.Discord"

local function is_discord(ctx)
  return ctx.app_id == DISCORD_ID
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
  modore.log.info("discord acquire: copy fallback via " .. chord)
  h.send_chord(chord)
  h.sleep_ms(45)
  h.send_chord("cmd+c")
  h.sleep_ms(70)
  local picked = h.clipboard_read()
  if saved ~= nil then
    h.clipboard_write(saved)
  end
  return picked
end

modore.route_for_app = function(ctx, api)
  modore.log.info("discord route hook entered")
  if not is_discord(ctx) then
    return nil
  end
  modore.log.info("discord route: selection_sync")
  return "selection_sync"
end

modore.on_acquire = function(ctx, api)
  modore.log.info("discord acquire hook entered")
  if not is_discord(ctx) then
    return nil
  end
  local h = api.host
  modore.log.info("discord acquire: collapsing stale selection")
  h.send_chord("right")
  h.sleep_ms(30)
  modore.log.info("discord acquire: copying current word")
  local picked = acquire_with_copy(api, "shift+alt+left")
  if needs_more_left_context(api, picked) then
    modore.log.info("discord acquire: punctuation/short tail; trying line-left copy")
    local linePicked = acquire_with_copy(api, "shift+cmd+left")
    if not is_suspicious_copy(linePicked) then
      modore.log.info("discord acquire: line-left copy accepted")
      picked = linePicked
    end
  end
  if picked == nil or #picked == 0 then
    modore.log.info("discord acquire: copy failed; abort")
    return nil
  end
  return picked
end

modore.on_pickup = function(ctx, api)
  modore.log.info("discord pickup hook entered")
  if not is_discord(ctx) then
    return nil
  end
  local span = api.default.pickup(ctx)
  if span == nil then
    modore.log.info("discord pickup: default pickup returned nil")
    return nil
  end
  modore.log.info(
    "discord pickup: default span [" ..
    tostring(span.start_byte) .. ".." ..
    tostring(span.end_byte) .. "]")
  return span
end

modore.on_replacement = function(span, cands, api)
  modore.log.info("discord replacement hook entered")
  if span == nil or cands == nil then
    return nil
  end
  return api.default.replacement(span, cands)
end
