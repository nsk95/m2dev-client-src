-- ELEMENTIA-USERSCRIPT (example addon)
-- ---------------------------------------------------------------------------
-- coords.lua - a minimal coordinate readout.
--
-- Shows the player's own map position (read-only) as a small on-screen label,
-- refreshed a few times per second. Demonstrates coord.* and config.*:
-- the label position is remembered between sessions via config storage.
--
-- Sandbox note: coord.* is READ-ONLY. There is deliberately no way to request
-- a move to a coordinate - this cannot be turned into a teleport/walk bot.
-- ---------------------------------------------------------------------------

local label = nil

-- Remember where the user last had the label (persisted per-script, sandboxed).
local px = config.get("x", 20)
local py = config.get("y", 40)

local function ensureLabel()
    if label then return true end
    local id = ui.createText()          -- nil while fonts still load: retry later
    if not id then return false end
    label = id
    ui.setPosition(label, px, py)
    ui.setColor(label, 0.6, 0.9, 1.0)   -- light blue
    ui.show(label)
    return true
end

local function refresh()
    if not ensureLabel() then return end
    local x, y = coord.get()
    if x == nil then
        ui.setText(label, "coords: (not in game)")
        return
    end
    -- Metin2 stores positions in pixels; /100 gives the familiar map units.
    ui.setText(label, string.format("X %d  Y %d", math.floor(x / 100), math.floor(y / 100)))
end

-- Update ~3x/second (smooth enough, still cheap).
event.on("update", (function()
    local acc = 0
    return function(dt)
        acc = acc + dt
        if acc >= 0.33 then acc = 0; refresh() end
    end
end)())

timer.after(0.5, refresh)

log.info("coords loaded")
