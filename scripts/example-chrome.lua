-- example-chrome.lua — imperative stage hooks for Chrome.
--
-- This script is intentionally command-oriented: it decides, stage by
-- stage, how Chrome text fields should be acquired and replaced. The
-- omnibox uses selection_sync so Chromium's internal suggestion state
-- stays aligned with the committed text.

local CHROME_IDS = {
  ["com.google.Chrome"] = true,
  ["com.google.Chrome.canary"] = true,
  ["org.chromium.Chromium"] = true,
}

local function is_chrome(ctx)
  return ctx.app_id ~= nil and CHROME_IDS[ctx.app_id] == true
end

local function is_omnibox(ctx)
  return ctx.field_description == "Address and search bar"
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

local function looks_like_hyphenated_romaji(s)
  return s ~= nil
    and s:find("-", 1, true) ~= nil
    and s:match("^[A-Za-z%-]+$") ~= nil
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
  modore.log.info("chrome acquire: copy fallback via " .. chord)
  h.send_chord(chord)
  for _ = 1, 4 do
    h.sleep_ms(20)
    local selected = h.read_selection()
    if not is_suspicious_copy(selected) then
      if saved ~= nil then
        h.clipboard_write(saved)
      end
      return selected
    end
  end
  h.send_chord("cmd+c")
  h.sleep_ms(30)
  local picked = h.clipboard_read()
  if saved ~= nil then
    h.clipboard_write(saved)
  end
  return picked
end

modore.log.info("chrome script loaded")

modore.on_acquire = function(ctx, api)
  modore.log.info("chrome acquire hook entered")
  if not is_chrome(ctx) or is_omnibox(ctx) then
    if is_chrome(ctx) then
      modore.log.info("chrome acquire: skipping omnibox")
    end
    return nil
  end
  local selected = api.host.read_selection()
  if selected ~= nil and #selected > 0 then
    modore.log.info("chrome acquire: using focused selection")
    if needs_more_left_context(api, selected) then
      modore.log.info("chrome acquire: focused selection needs more left context")
      local widened = acquire_with_copy(api, "shift+cmd+left")
      if not is_suspicious_copy(widened) then
        return widened
      end
    end
    return selected
  end
  if ctx.field_role == nil then
    modore.log.info("chrome acquire: no AX field; using default acquire")
    local picked = acquire_with_copy(api, "shift+alt+left")
    if needs_more_left_context(api, picked) then
      modore.log.info("chrome acquire: punctuation/short tail; trying line-left copy")
      local linePicked = acquire_with_copy(api, "shift+cmd+left")
      if not is_suspicious_copy(linePicked) then
        modore.log.info("chrome acquire: line-left copy accepted")
        return linePicked
      end
    end
    if is_suspicious_copy(picked) then
      modore.log.info("chrome acquire: default acquire was suspicious; abort")
      return nil
    end
    return picked
  end
  local picked = acquire_with_copy(api, "shift+alt+left")
  if needs_more_left_context(api, picked) then
    modore.log.info("chrome acquire: punctuation/short tail; trying line-left copy")
    local linePicked = acquire_with_copy(api, "shift+cmd+left")
    if not is_suspicious_copy(linePicked) then
      modore.log.info("chrome acquire: line-left copy accepted")
      return linePicked
    end
  end
  if needs_more_left_context(api, picked) then
    modore.log.info("chrome acquire: short tail; trying line-left copy")
    local linePicked = acquire_with_copy(api, "shift+cmd+left")
    if not is_suspicious_copy(linePicked) then
      modore.log.info("chrome acquire: line-left copy accepted")
      return linePicked
    end
  end
  if not is_suspicious_copy(picked) then
    return picked
  end
  modore.log.info("chrome acquire: no usable copy; abort")
  return nil
end

modore.route_for_app = function(ctx, api)
  modore.log.info("chrome route hook entered")
  if not is_chrome(ctx) then
    return nil
  end
  if is_omnibox(ctx) then
    modore.log.info("chrome route: omnibox → selection_sync")
    return "selection_sync"
  end
  if ctx.field_role == "AXTextField" or ctx.field_role == "AXTextArea" then
    modore.log.info("chrome route: field → selection_sync")
    return "selection_sync"
  end
  return nil
end

modore.on_replacement = function(span, cands, api)
  modore.log.info("chrome replacement hook entered")
  if span == nil or cands == nil then
    return nil
  end
  return api.default.replacement(span, cands)
end
