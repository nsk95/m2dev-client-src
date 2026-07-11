-- ELEMENTIA-USERSCRIPT (example addon)
-- ---------------------------------------------------------------------------
-- loot_filter.lua - an inventory watch / highlight-style DISPLAY.
--
-- IMPORTANT: this is a *display*, not an auto-pickup bot. The sandbox exposes
-- NO way to pick up, move, use or drop items - inventory.* is strictly a
-- read-only count of what you already carry. This addon simply watches a
-- configurable watch-list of item vnums and shows a panel when your carried
-- count changes (e.g. "you now hold N of item X"), and flags when your bag is
-- getting full so you know to go sell - all purely informational.
--
-- Configure the watch-list with config.* (persisted per-script, sandboxed):
--   the default watches nothing; add vnums via config in-game or edit below.
-- ---------------------------------------------------------------------------

-- Default watch list (item vnums). Adjust to taste; these are examples only.
-- e.g. 27100 = a common upgrade stone on many servers. Empty by default so the
-- addon is inert until you opt in.
local WATCH_VNUMS = { }

-- Load any extra vnums the player saved via config (comma-separated string).
do
    local saved = config.get("watch", "")
    if type(saved) == "string" and saved ~= "" then
        for token in string.gmatch(saved, "[^,%s]+") do
            local v = tonumber(token)
            if v then WATCH_VNUMS[#WATCH_VNUMS + 1] = v end
        end
    end
end

local panel = nil       -- background RECT panel
local text = nil        -- the readout text
local lastCounts = {}   -- vnum -> last seen carried count

local function ensureWidgets()
    if panel and text then return true end
    if not panel then
        local id = ui.createRect()          -- RECT widget: no font needed
        if not id then return false end
        panel = id
        ui.setPosition(panel, 18, 168)
        ui.setSize(panel, 220, 46)
        ui.setColor(panel, 0.0, 0.0, 0.0, 0.45)   -- translucent dark panel
        ui.show(panel)
    end
    if not text then
        local id = ui.createText()
        if not id then return false end
        text = id
        ui.setPosition(text, 24, 172)
        ui.setColor(text, 1.0, 0.95, 0.6)
        ui.show(text)
    end
    return true
end

local function bagUsage()
    -- Count occupied slots in the main bag (read-only).
    local used, total = 0, inventory.slotCount()
    for slot = 0, total - 1 do
        if not inventory.isEmpty(slot) then used = used + 1 end
    end
    return used, total
end

local function refresh()
    if not ensureWidgets() then return end

    local lines = {}

    -- Watched-item counts (highlight when they change).
    for _, vnum in ipairs(WATCH_VNUMS) do
        local n = inventory.countByVnum(vnum)
        if n > 0 then
            lines[#lines + 1] = string.format("item %d x%d", vnum, n)
            if lastCounts[vnum] ~= nil and n > lastCounts[vnum] then
                log.info(string.format("loot: +%d of item %d (now %d)",
                    n - lastCounts[vnum], vnum, n))
            end
        end
        lastCounts[vnum] = n
    end

    -- Bag-full advisory.
    local used, total = bagUsage()
    lines[#lines + 1] = string.format("bag %d/%d", used, total)

    ui.setText(text, table.concat(lines, "   "))
    -- Turn the panel reddish when the bag is nearly full.
    if used >= total - 3 then
        ui.setColor(panel, 0.35, 0.0, 0.0, 0.5)
    else
        ui.setColor(panel, 0.0, 0.0, 0.0, 0.45)
    end
end

event.on("second", refresh)
timer.after(1.0, refresh)

log.info("loot_filter loaded (display only, no auto-pickup)")
