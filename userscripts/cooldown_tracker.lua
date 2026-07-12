-- ELEMENTIA-USERSCRIPT (example addon)
-- ---------------------------------------------------------------------------
-- cooldown_tracker.lua - a compact skill-cooldown HUD.
--
-- For each of your active skill slots it shows a small bar that DRAINS as the
-- skill's cooldown ticks down, plus the remaining seconds. When a skill is off
-- cooldown the bar is full and it reads "ready".
--
-- Demonstrates the v4 skill.* API, which is strictly READ-ONLY:
--   skill.getIndex/getLevel(slot)        which skill sits in a slot (0 = empty)
--   skill.getCoolTime(slot)              the slot's total cooldown (seconds)
--   skill.getRemainingCoolTime(slot)     how much is left (>= 0)
--   skill.isCoolTime(slot)               is it currently cooling down
--
-- IMPORTANT: this only OBSERVES cooldowns the client already tracks for its own
-- skill window. There is deliberately NO skill.use/cast anywhere in the sandbox,
-- so this can never be turned into a skill/rotation bot. The server anti-cheat
-- stays authoritative.
-- ---------------------------------------------------------------------------

local MAX_ROWS   = 8            -- how many skill slots to display at most
local SCAN_SLOTS = 45          -- how many slots to look through for real skills
local ORIGIN_X   = config.get("x", 20)
local ORIGIN_Y   = config.get("y", 380)
local ROW_H      = 16

local rows = {}                -- [i] = { slot=, bar=, text= }
local watch = {}               -- list of skill slot indices that hold a skill
local built = false
local rescanAcc = 0

-- Find which slots actually contain a skill (index > 0). Auto-discovers the
-- player's real skill slots without hard-coding a class layout.
local function rescan()
    watch = {}
    local n = math.min(SCAN_SLOTS, skill.slotCount())
    for slot = 0, n - 1 do
        if skill.getIndex(slot) > 0 then
            watch[#watch + 1] = slot
            if #watch >= MAX_ROWS then break end
        end
    end
end

local function buildRow(i)
    local y = ORIGIN_Y + (i - 1) * ROW_H
    local bar = ui.createBar()
    if not bar then return nil end
    ui.setPosition(bar, ORIGIN_X, y)
    ui.setSize(bar, 130, 10)
    ui.setColor(bar, 0.35, 0.75, 0.95, 0.9)
    ui.setBackColor(bar, 0.08, 0.08, 0.08, 0.6)
    ui.setLayer(bar, 1)
    ui.show(bar)

    local text = ui.createText()
    if not text then ui.destroy(bar); return nil end
    ui.setPosition(text, ORIGIN_X + 138, y - 2)
    ui.setColor(text, 0.9, 0.95, 1.0)
    ui.setLayer(text, 2)
    ui.show(text)

    return { bar = bar, text = text }
end

local function build()
    if built then return true end
    for i = 1, MAX_ROWS do
        if not rows[i] then
            local r = buildRow(i)
            if not r then return false end
            rows[i] = r
        end
    end
    built = true
    return true
end

local function refresh()
    if not build() then return end
    if #watch == 0 then rescan() end

    for i = 1, MAX_ROWS do
        local r = rows[i]
        local slot = watch[i]
        if slot == nil then
            ui.hide(r.bar); ui.hide(r.text)
        else
            ui.show(r.bar); ui.show(r.text)
            local total = skill.getCoolTime(slot)
            local level = skill.getLevel(slot)
            if skill.isCoolTime(slot) and total > 0 then
                local remain = skill.getRemainingCoolTime(slot)
                ui.setProgress(r.bar, 1.0 - (remain / total))     -- fills as it readies
                ui.setColor(r.bar, 0.90, 0.55, 0.20, 0.9)         -- amber while cooling
                ui.setText(r.text, string.format("#%d Lv%d  %.1fs",
                    skill.getIndex(slot), level, remain))
            else
                ui.setProgress(r.bar, 1.0)
                ui.setColor(r.bar, 0.35, 0.80, 0.45, 0.9)         -- green = ready
                ui.setText(r.text, string.format("#%d Lv%d  ready",
                    skill.getIndex(slot), level))
            end
        end
    end
end

event.on("update", (function()
    return function(dt)
        rescanAcc = rescanAcc + dt
        if rescanAcc >= 3.0 then rescanAcc = 0; rescan() end   -- pick up new skills
        refresh()
    end
end)())

timer.after(0.5, function() rescan(); refresh() end)

log.info("cooldown_tracker loaded")
