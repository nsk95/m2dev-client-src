-- ELEMENTIA-USERSCRIPT (example addon)
-- ---------------------------------------------------------------------------
-- equip_icons.lua - a small HUD that shows the item icons of the gear you are
-- currently wearing (weapon / body / head), refreshed once per second.
--
-- It demonstrates the two v3 additions:
--   * equipment.*      READ-ONLY view of your OWN worn gear (no equip/unequip)
--   * ui.createIcon /  a client-internal item icon widget: the icon is resolved
--     ui.setIcon        from an item VNUM through the client's own item table -
--                       a script can NEVER load an arbitrary file path.
--
-- Like every userscript this uses only the curated sandbox API: no filesystem,
-- no os, no network, no input synthesis. All reads are READ-ONLY.
-- ---------------------------------------------------------------------------

log.info("equip_icons loaded (elementia sandbox v" .. tostring(elementia.version) .. ")")

-- Which equip slots to show, top to bottom. equipment.slot.* are the client's
-- real wear-position constants (WEAPON/BODY/HEAD/SHIELD/EAR/NECK/...).
local slots = {
    { label = "Weapon", slot = equipment.slot.WEAPON },
    { label = "Body",   slot = equipment.slot.BODY },
    { label = "Head",   slot = equipment.slot.HEAD },
}

local icons = {}
local built = false

-- Create the icon widgets lazily: ui.createIcon() returns nil while graphics
-- resources are still loading, so we simply retry on a later tick.
local function build()
    if built then return true end
    for i = 1, #slots do
        local id = ui.createIcon()
        if not id then return false end
        ui.setPosition(id, 20, 120 + (i - 1) * 36)  -- stacked, top-left area
        ui.show(id)
        icons[i] = id
    end
    built = true
    return true
end

local function refresh()
    if not build() then return end
    for i = 1, #slots do
        -- equipment.getItemVnum returns 0 for an empty slot; ui.setIcon(id, 0)
        -- clears the widget, so unequipped slots simply show nothing.
        local vnum = equipment.getItemVnum(slots[i].slot)
        ui.setIcon(icons[i], vnum)
    end
end

event.on("second", refresh)
timer.after(0.5, refresh)   -- show promptly after load, too
