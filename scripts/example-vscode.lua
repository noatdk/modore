-- example-vscode.lua — route override for VS Code / Monaco editors.
--
-- VS Code's Electron/Monaco text surfaces are much happier when modore
-- skips the generic AX value-write path. The conversion itself still uses
-- the normal host acquisition and Mozc replacement logic; this script only
-- changes how the committed text is written back.

local VSCODE_IDS = {
  ["com.microsoft.VSCode"] = true,
  ["com.microsoft.VSCodeInsiders"] = true,
}

local function is_vscode(ctx)
  return ctx.app_id ~= nil and VSCODE_IDS[ctx.app_id] == true
end

modore.log.info("vscode script loaded")

modore.route_for_app = function(ctx, api)
  modore.log.info("vscode route hook entered")
  if not is_vscode(ctx) then
    return nil
  end

  modore.log.info("vscode route: keystroke")
  return "keystroke"
end
