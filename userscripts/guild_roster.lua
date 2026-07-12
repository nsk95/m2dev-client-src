-- ELEMENTIA-USERSCRIPT (example addon)
-- ---------------------------------------------------------------------------
-- guild_roster.lua - a compact on-screen readout of YOUR guild.
--
-- Demonstrates the new READ-ONLY guild.* API: guild name/level, member count
-- and a short list of the highest-level members with an online/offline dot.
--
-- Sandbox note: guild.* is READ-ONLY and only ever sees your OWN guild (the same
-- data the client keeps for its guild window). There is no invite/kick/withdraw/
-- grade API anywhere - a script can read the roster, never change it.
-- ---------------------------------------------------------------------------

local panel, title
local rows = {}
local MAX_ROWS = 6

local px = config.get("x", 20)
local py = config.get("y", 120)

local function ensureUi()
    if panel then return true end
    local p = ui.createPanel()
    if not p then return false end
    panel = p
    ui.setPosition(panel, px - 6, py - 6)
    ui.setSize(panel, 210, 24 + MAX_ROWS * 15 + 6)
    ui.setColor(panel, 0.5, 0.4, 0.2, 0.8)          -- gold-ish border
    ui.setBackColor(panel, 0.0, 0.0, 0.0, 0.55)
    ui.setLayer(panel, 0)

    title = ui.createText()
    if title then
        ui.setPosition(title, px, py)
        ui.setColor(title, 1.0, 0.85, 0.4)
        ui.setLayer(title, 1)
    end
    for i = 1, MAX_ROWS do
        local t = ui.createText()
        if t then
            ui.setPosition(t, px, py + 18 + (i - 1) * 15)
            ui.setColor(t, 0.85, 0.85, 0.85)
            ui.setLayer(t, 1)
            rows[i] = t
        end
    end
    return true
end

local function refresh()
    if not ensureUi() then return end

    if not guild.exists() then
        if title then ui.setText(title, "No guild") end
        for i = 1, MAX_ROWS do if rows[i] then ui.setText(rows[i], "") end end
        return
    end

    if title then
        ui.setText(title, string.format("%s  (Lv %d)  %d/%d",
            guild.getName(), guild.getLevel(),
            guild.memberCount(), guild.maxMemberCount()))
    end

    -- Collect members, sort by level (desc), show the top MAX_ROWS.
    local list = {}
    local n = guild.memberCount()
    for i = 0, n - 1 do
        list[#list + 1] = {
            name   = guild.getMemberName(i),
            level  = guild.getMemberLevel(i),
            online = guild.isMemberOnline(i),
        }
    end
    table.sort(list, function(a, b) return a.level > b.level end)

    for i = 1, MAX_ROWS do
        local r = rows[i]
        if r then
            local m = list[i]
            if m then
                local dot = m.online and "*" or "-"
                ui.setText(r, string.format("%s Lv%d  %s", dot, m.level, m.name))
                if m.online then ui.setColor(r, 0.7, 1.0, 0.7)
                else             ui.setColor(r, 0.55, 0.55, 0.55) end
            else
                ui.setText(r, "")
            end
        end
    end
end

event.on("second", refresh)
timer.after(1.0, refresh)

log.info("guild_roster loaded")
