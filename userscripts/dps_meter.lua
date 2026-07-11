-- ELEMENTIA-USERSCRIPT (example addon)
-- ---------------------------------------------------------------------------
-- dps_meter.lua - a client-side, target-based DPS estimate + HUD bar.
--
-- The client only *reliably* learns a target's HP as a PERCENTAGE (from the
-- server target packet, surfaced read-only via target.getHPPercent()). We turn
-- that into a rough damage-per-second estimate for the CURRENT target by
-- tracking how fast its HP% drops while it stays the same target.
--
-- This is an *estimate/among-display*, NOT a combat automation: there is no
-- attack, no targeting, no packet send anywhere in the API. It only observes
-- what the player is already doing. The server anti-cheat stays authoritative.
-- ---------------------------------------------------------------------------

local nameLabel = nil        -- target name + %/s text
local bar = nil              -- target HP bar

-- rolling DPS state
local lastName = nil
local lastPct  = nil
local lastTime = nil
local dpsPctPerSec = 0.0     -- smoothed HP% lost per second

local function ensureWidgets()
    if nameLabel and bar then return true end
    if not nameLabel then
        local id = ui.createText()
        if not id then return false end
        nameLabel = id
        ui.setPosition(nameLabel, 20, 90)
        ui.setColor(nameLabel, 1.0, 0.85, 0.4)
        ui.show(nameLabel)
    end
    if not bar then
        local b = ui.createBar()             -- BAR widget: no font needed
        if not b then return false end
        bar = b
        ui.setPosition(bar, 20, 108)
        ui.setSize(bar, 180, 10)
        ui.setColor(bar, 0.85, 0.2, 0.2)          -- red foreground
        ui.setBackColor(bar, 0.1, 0.1, 0.1, 0.6)  -- dark background
        ui.show(bar)
    end
    return true
end

local function reset()
    lastName, lastPct, lastTime = nil, nil, nil
    dpsPctPerSec = 0.0
end

local function tick(dt)
    if not ensureWidgets() then return end

    if not target.exists() or target.isPC() then
        -- Only meter monsters/metins; hide when no meterable target.
        ui.hide(nameLabel); ui.hide(bar); reset()
        return
    end
    ui.show(nameLabel); ui.show(bar)

    local name = target.getName()
    local pct  = target.getHPPercent()          -- -1 when unknown
    local now  = time.now()

    -- New target (or unknown HP): (re)start the measurement window.
    if name ~= lastName or pct < 0 then
        lastName, lastPct, lastTime = name, (pct >= 0 and pct or nil), now
        dpsPctPerSec = 0.0
    elseif lastPct ~= nil and now > lastTime then
        local drop = lastPct - pct
        if drop > 0 then
            local rate = drop / (now - lastTime)
            -- exponential smoothing so the readout is not jumpy
            dpsPctPerSec = dpsPctPerSec * 0.6 + rate * 0.4
            lastPct, lastTime = pct, now
        elseif drop < 0 then
            -- target healed / repopped: restart cleanly
            lastPct, lastTime = pct, now
        end
    end

    if pct >= 0 then
        ui.setProgress(bar, pct / 100.0)
        ui.setText(nameLabel, string.format("%s  |  %d%%  |  ~%.1f%%/s",
            name, pct, dpsPctPerSec))
    else
        ui.setProgress(bar, 1.0)
        ui.setText(nameLabel, string.format("%s  |  HP hidden", name))
    end
end

event.on("update", tick)

log.info("dps_meter loaded")
