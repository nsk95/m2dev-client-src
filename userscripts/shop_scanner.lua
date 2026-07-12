-- ELEMENTIA-USERSCRIPT (example addon)
-- ---------------------------------------------------------------------------
-- shop_scanner.lua - list the contents of the shop you currently have OPEN.
--
-- Demonstrates the new READ-ONLY shop.* API. While an NPC or private shop is
-- open, it draws a small overlay listing each offered item's vnum, stack count
-- and price. Handy for spotting a bargain in a crowded private-shop street.
--
-- Sandbox note: shop.* is READ-ONLY and only reflects the shop window the CLIENT
-- already opened and received data for. There is NO buy/sell/withdraw API - a
-- script can read what is on offer but can never transact, so it cannot become
-- an auto-buyer / market bot.
-- ---------------------------------------------------------------------------

local panel, header
local rows = {}
local MAX_ROWS = 10

local px, py = 300, 120

local function ensureUi()
    if panel then return true end
    local p = ui.createPanel()
    if not p then return false end
    panel = p
    ui.setPosition(panel, px - 6, py - 6)
    ui.setSize(panel, 240, 22 + MAX_ROWS * 15 + 6)
    ui.setColor(panel, 0.3, 0.5, 0.6, 0.8)
    ui.setBackColor(panel, 0.0, 0.0, 0.0, 0.6)
    ui.setLayer(panel, 0)

    header = ui.createText()
    if header then
        ui.setPosition(header, px, py)
        ui.setColor(header, 0.7, 0.9, 1.0)
        ui.setLayer(header, 1)
    end
    for i = 1, MAX_ROWS do
        local t = ui.createText()
        if t then
            ui.setPosition(t, px, py + 18 + (i - 1) * 15)
            ui.setColor(t, 0.9, 0.9, 0.9)
            ui.setLayer(t, 1)
            rows[i] = t
        end
    end
    return true
end

local function setAllHidden()
    if panel then ui.hide(panel) end
    if header then ui.hide(header) end
    for i = 1, MAX_ROWS do if rows[i] then ui.hide(rows[i]) end end
end

local function setAllShown()
    if panel then ui.show(panel) end
    if header then ui.show(header) end
end

local function refresh()
    if not ensureUi() then return end

    if not shop.isOpen() then
        setAllHidden()
        return
    end
    setAllShown()

    local kind = shop.isMine() and "your shop"
        or (shop.isPrivate() and "private shop" or "NPC shop")
    if header then
        ui.setText(header, string.format("%s  (%d slots)", kind, shop.slotCount()))
    end

    local shown = 0
    local total = shop.slotCount()
    for slot = 0, total - 1 do
        if shown >= MAX_ROWS then break end
        local vnum = shop.getItemVnum(slot)
        if vnum ~= 0 then
            shown = shown + 1
            local r = rows[shown]
            if r then
                local cnt = shop.getItemCount(slot)
                local price = shop.getItemPrice(slot)
                ui.setText(r, string.format("#%d x%d  %d gold", vnum, cnt, price))
                ui.show(r)
            end
        end
    end
    for i = shown + 1, MAX_ROWS do
        if rows[i] then ui.hide(rows[i]) end
    end
end

event.on("update", (function()
    local acc = 0
    return function(dt)
        acc = acc + dt
        if acc >= 0.5 then acc = 0; refresh() end
    end
end)())

log.info("shop_scanner loaded")
