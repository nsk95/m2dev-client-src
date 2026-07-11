-- ELEMENTIA-USERSCRIPT (example addon)
-- ---------------------------------------------------------------------------
-- hud_stats.lua - a tiny QoL widget: shows the player's name / level / HP as a
-- small on-screen text overlay, refreshed once per second.
--
-- This file is a *proof* of the sandbox: it uses only the curated API
-- (ui.*, player.*, event.*, timer.*, log.*). It has NO access to the
-- filesystem, os, network, packets, or player input - that is by design.
--
-- Drop .lua files into <client>/userscripts/ to add your own addons.
-- ---------------------------------------------------------------------------

log.info("hud_stats loaded (elementia sandbox v" .. tostring(elementia.version) .. ")")

-- Create the on-screen text widget. We do this lazily on the first update so
-- the font resource is guaranteed to be ready.
local label = nil

local function ensureLabel()
    if label then return true end
    -- ui.createText returns nil while fonts/resources are still loading; just
    -- retry on a later tick instead of erroring.
    local id = ui.createText()
    if not id then return false end
    label = id
    ui.setPosition(label, 20, 20)   -- UI coordinates, top-left area
    ui.setColor(label, 1.0, 1.0, 0.4)   -- soft yellow
    ui.show(label)
    return true
end

local function refresh()
    if not ensureLabel() then return end
    local name = player.getName()
    if name == nil or name == "" then
        ui.setText(label, "ELEMENTIA - not in game yet")
        return
    end
    local hp, maxhp = player.getHP(), player.getMaxHP()
    ui.setText(label, string.format("%s  |  Lv %d  |  HP %d/%d",
        name, player.getLevel(), hp, maxhp))
end

-- Refresh once per second (cheap). A per-frame "update" hook is also available
-- if you need smoother updates.
event.on("second", refresh)

-- Also refresh immediately after a short delay so the label appears promptly.
timer.after(0.5, refresh)
