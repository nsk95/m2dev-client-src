-- ELEMENTIA-USERSCRIPT (example addon)
-- ---------------------------------------------------------------------------
-- party_panel.lua - a small party roster with per-member HP bars.
--
-- Lists your party members (name + HP%) inside a framed panel, with a thin
-- divider line under the header. Members below ~30% HP turn their bar red so
-- you can spot who needs help at a glance.
--
-- Demonstrates the v4 party.* API (READ-ONLY, and only your OWN party):
--   party.count()               number of members
--   party.getName(i)            member name
--   party.getHPPercent(i)       server-provided HP percentage (0..100, -1 unknown)
-- plus ui.createLine (a 2D divider) and ui.createPanel / ui.setLayer.
--
-- Sandbox note: this reads only your own party's roster - the same data the
-- built-in party window shows. It is NOT an arbitrary other-player scanner, and
-- there is no invite/kick/heal action anywhere in the API. Purely informational.
-- ---------------------------------------------------------------------------

local MAX_MEMBERS = 8
local PANEL_W     = 180
local px = config.get("x", 20)
local py = config.get("y", 560)
local ROW_H = 16
local HEADER_H = 22

local panel, header, divider
local rows = {}                -- [i] = { name=, bar= }
local built = false

local function build()
    if built then return true end

    if not panel then
        local id = ui.createPanel()
        if not id then return false end
        panel = id
        ui.setPosition(id, px, py)
        ui.setSize(id, PANEL_W, HEADER_H + MAX_MEMBERS * ROW_H + 6)
        ui.setColor(id, 0.5, 0.55, 0.6, 0.8)
        ui.setBackColor(id, 0.0, 0.0, 0.0, 0.5)
        ui.setLayer(id, 0)
        ui.show(id)
    end
    if not header then
        local id = ui.createText()
        if not id then return false end
        header = id
        ui.setPosition(id, px + 10, py + 5)
        ui.setColor(id, 0.8, 0.9, 1.0)
        ui.setLayer(id, 2)
        ui.show(id)
    end
    if not divider then
        local id = ui.createLine()
        if not id then return false end
        divider = id
        ui.setPosition(id, px + 8, py + HEADER_H)
        ui.setLine(id, px + PANEL_W - 8, py + HEADER_H, 1)
        ui.setColor(id, 0.5, 0.55, 0.6, 0.7)
        ui.setLayer(id, 1)
        ui.show(id)
    end
    for i = 1, MAX_MEMBERS do
        if not rows[i] then
            local y = py + HEADER_H + 4 + (i - 1) * ROW_H
            local barId = ui.createBar()
            if not barId then return false end
            ui.setPosition(barId, px + 10, y + 2)
            ui.setSize(barId, PANEL_W - 20, 10)
            ui.setColor(barId, 0.30, 0.75, 0.35, 0.9)
            ui.setBackColor(barId, 0.08, 0.08, 0.08, 0.6)
            ui.setLayer(barId, 1)

            local nameId = ui.createText()
            if not nameId then ui.destroy(barId); return false end
            ui.setPosition(nameId, px + 12, y - 1)
            ui.setColor(nameId, 1.0, 1.0, 1.0)
            ui.setLayer(nameId, 2)

            rows[i] = { name = nameId, bar = barId }
        end
    end
    built = true
    return true
end

local function refresh()
    if not build() then return end

    local count = party.count()
    if count <= 0 then
        ui.setText(header, "Party (solo)")
        ui.hide(divider)
        for i = 1, MAX_MEMBERS do ui.hide(rows[i].name); ui.hide(rows[i].bar) end
        return
    end

    ui.setText(header, string.format("Party (%d)", count))
    ui.show(divider)

    for i = 1, MAX_MEMBERS do
        local r = rows[i]
        local mi = i - 1                     -- party.* is 0-indexed
        if mi < count then
            local name = party.getName(mi)
            local hp = party.getHPPercent(mi)
            ui.show(r.name); ui.show(r.bar)
            if hp < 0 then
                ui.setText(r.name, name)
                ui.setProgress(r.bar, 0)
            else
                ui.setText(r.name, string.format("%s  %d%%", name, hp))
                ui.setProgress(r.bar, hp / 100.0)
                if hp <= 30 then
                    ui.setColor(r.bar, 0.85, 0.20, 0.20, 0.9)     -- low HP -> red
                else
                    ui.setColor(r.bar, 0.30, 0.75, 0.35, 0.9)     -- healthy -> green
                end
            end
        else
            ui.hide(r.name); ui.hide(r.bar)
        end
    end
end

event.on("second", refresh)
event.on("update", (function()
    local acc = 0
    return function(dt)
        acc = acc + dt
        if acc >= 0.5 then acc = 0; refresh() end   -- keep HP bars responsive
    end
end)())

timer.after(0.5, refresh)

log.info("party_panel loaded")
