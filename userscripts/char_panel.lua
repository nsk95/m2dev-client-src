-- ELEMENTIA-USERSCRIPT (example addon)
-- ---------------------------------------------------------------------------
-- char_panel.lua - a clean, framed character HUD.
--
-- Shows, inside a single ui.createPanel() container:
--   * name / level / current map          (player.* + map.getName, READ-ONLY)
--   * HP / SP / Stamina bars               (player.* vitals)
--   * an EXP progress bar + percentage     (player.getExp / getMaxExp)
--   * the active quest title + counter     (quest.*, if you have one)
--
-- Demonstrates the v4 API: player exp/stamina getters, map.getName(), quest.*,
-- ui.createPanel (framed background) and ui.setLayer (draw the panel BEHIND the
-- text/bars regardless of creation order).
--
-- The panel position is remembered per-script via config.* so it stays where
-- you put it between sessions. (The sandbox exposes no input synthesis, so a
-- HUD is positioned via config rather than dragged - by design.)
--
-- Sandbox note: every value here is READ-ONLY. Nothing casts, warps, spends or
-- sends anything - it only observes what the client already knows about you.
-- ---------------------------------------------------------------------------

local PANEL_W, PANEL_H = 210, 118
local px = config.get("x", 20)
local py = config.get("y", 250)

local ui_ = {}          -- created widget handles
local built = false

local function bar(x, y, w, h, r, g, b, layer)
    local id = ui.createBar()
    if not id then return nil end
    ui.setPosition(id, x, y)
    ui.setSize(id, w, h)
    ui.setColor(id, r, g, b, 0.9)
    ui.setBackColor(id, 0.08, 0.08, 0.08, 0.6)
    ui.setLayer(id, layer)
    ui.show(id)
    return id
end

local function label(x, y, r, g, b, layer)
    local id = ui.createText()
    if not id then return nil end
    ui.setPosition(id, x, y)
    ui.setColor(id, r, g, b)
    ui.setLayer(id, layer)
    ui.show(id)
    return id
end

-- Lazy build: any ui.create* may return nil while resources still load, so we
-- retry on a later tick until the whole panel exists.
local function build()
    if built then return true end

    if not ui_.panel then
        local id = ui.createPanel()
        if not id then return false end
        ui_.panel = id
        ui.setPosition(id, px, py)
        ui.setSize(id, PANEL_W, PANEL_H)
        ui.setColor(id, 0.5, 0.5, 0.6, 0.8)           -- soft border
        ui.setBackColor(id, 0.0, 0.0, 0.0, 0.55)      -- translucent fill
        ui.setLayer(id, 0)                            -- behind everything else
        ui.show(id)
    end

    ui_.title  = ui_.title  or label(px + 10, py + 8,  1.0, 0.9, 0.5, 2)
    ui_.map    = ui_.map    or label(px + 10, py + 24, 0.7, 0.85, 1.0, 2)
    ui_.hp     = ui_.hp     or bar(px + 10, py + 42, PANEL_W - 20, 8, 0.80, 0.20, 0.20, 1)
    ui_.sp     = ui_.sp     or bar(px + 10, py + 54, PANEL_W - 20, 8, 0.25, 0.45, 0.90, 1)
    ui_.st     = ui_.st     or bar(px + 10, py + 66, PANEL_W - 20, 6, 0.85, 0.75, 0.20, 1)
    ui_.exp    = ui_.exp    or bar(px + 10, py + 80, PANEL_W - 20, 8, 0.30, 0.80, 0.35, 1)
    ui_.expTxt = ui_.expTxt or label(px + 10, py + 90, 0.8, 1.0, 0.8, 2)
    ui_.quest  = ui_.quest  or label(px + 10, py + 102, 0.9, 0.85, 0.7, 2)

    for _, v in pairs({ ui_.title, ui_.map, ui_.hp, ui_.sp, ui_.st,
                        ui_.exp, ui_.expTxt, ui_.quest }) do
        if not v then return false end
    end
    built = true
    return true
end

local function frac(cur, max)
    if not max or max <= 0 then return 0 end
    local f = cur / max
    if f < 0 then f = 0 elseif f > 1 then f = 1 end
    return f
end

local function refresh()
    if not build() then return end

    local name = player.getName()
    if name == nil or name == "" then
        ui.setText(ui_.title, "ELEMENTIA - not in game yet")
        ui.setText(ui_.map, "")
        return
    end

    ui.setText(ui_.title, string.format("%s   Lv %d", name, player.getLevel()))
    ui.setText(ui_.map, "Map: " .. map.getName())

    ui.setProgress(ui_.hp, frac(player.getHP(), player.getMaxHP()))
    ui.setProgress(ui_.sp, frac(player.getSP(), player.getMaxSP()))
    ui.setProgress(ui_.st, frac(player.getStamina(), player.getMaxStamina()))

    local exp, maxExp = player.getExp(), player.getMaxExp()
    local ef = frac(exp, maxExp)
    ui.setProgress(ui_.exp, ef)
    ui.setText(ui_.expTxt, string.format("EXP %.1f%%", ef * 100.0))

    -- Show the first active quest, if any (quest.* is read-only).
    if quest.count() > 0 then
        local title = quest.getTitle(0)
        local cname, cval = quest.getCounter(0)
        if cname ~= "" then
            ui.setText(ui_.quest, string.format("Q: %s (%s %d)", title, cname, cval))
        else
            ui.setText(ui_.quest, "Q: " .. title)
        end
    else
        ui.setText(ui_.quest, "Q: (none)")
    end
end

-- HP/SP move fast; refresh a few times per second but keep it cheap.
event.on("update", (function()
    local acc = 0
    return function(dt)
        acc = acc + dt
        if acc >= 0.25 then acc = 0; refresh() end
    end
end)())

timer.after(0.5, refresh)

log.info("char_panel loaded (elementia sandbox v" .. tostring(elementia.version) .. ")")
