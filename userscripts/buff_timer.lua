-- ELEMENTIA-USERSCRIPT (example addon)
-- ---------------------------------------------------------------------------
-- buff_timer.lua - a small buff/affect HUD with client-side timers.
--
-- The client exposes active affects as a READ-ONLY boolean (buff.has(id)); it
-- does NOT hold each affect's server-side remaining time. So this HUD does the
-- honest, purely-visual thing: it watches the on/off transition of a curated
-- set of buffs and counts UP the elapsed time since each one appeared. That is
-- enough for "how long has my Dash/Berserk been up" without any server data.
--
-- Sandbox note: buff.* is read-only. Nothing here casts, refreshes or requests
-- a buff - it only observes and displays.
-- ---------------------------------------------------------------------------

-- Which affects to track, in display order (id -> label).
local WATCH = {
    { id = buff.ids.DASH,       label = "Dash" },
    { id = buff.ids.GEOMGYEONG, label = "Sword Aura" },
    { id = buff.ids.JEONGWI,    label = "Berserk" },
    { id = buff.ids.KWAESOK,    label = "Haste" },
    { id = buff.ids.HOSIN,      label = "Guardian" },
    { id = buff.ids.BOHO,       label = "Protection" },
    { id = buff.ids.MOV_SPEED,  label = "Move Speed" },
    { id = buff.ids.ATT_SPEED,  label = "Attack Speed" },
}

local label = nil
local activeSince = {}    -- id -> time.now() when it went active

local function ensureLabel()
    if label then return true end
    local id = ui.createText()
    if not id then return false end
    label = id
    ui.setPosition(label, 20, 140)
    ui.setColor(label, 0.7, 1.0, 0.7)   -- soft green
    ui.show(label)
    return true
end

local function refresh()
    if not ensureLabel() then return end
    local now = time.now()
    local parts = {}
    for _, b in ipairs(WATCH) do
        if buff.has(b.id) then
            if not activeSince[b.id] then activeSince[b.id] = now end
            local elapsed = now - activeSince[b.id]
            parts[#parts + 1] = string.format("%s %ds", b.label, math.floor(elapsed))
        else
            activeSince[b.id] = nil
        end
    end
    if #parts == 0 then
        ui.setText(label, "Buffs: (none)")
    else
        ui.setText(label, "Buffs: " .. table.concat(parts, "  "))
    end
end

-- Once per second is plenty for a seconds-granular timer HUD.
event.on("second", refresh)
timer.after(0.5, refresh)

log.info("buff_timer loaded")
