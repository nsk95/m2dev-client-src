-- ELEMENTIA-USERSCRIPT (example addon)
-- ---------------------------------------------------------------------------
-- chat_alerts.lua - watch your OWN chat window for keywords and alert you.
--
-- Demonstrates the new chat.* read API + event.on("chat") read-hook, plus the
-- two SAFE client-local writes:
--   * chat.print(text) - echoes a note into YOUR OWN chat window (local render
--                        only; it is NEVER sent to the server or other players).
--   * sound.play(key)  - plays a curated local sound as an audible alert.
--
-- Sandbox note: this only OBSERVES chat that is already on your screen and
-- responds with a LOCAL note / LOCAL sound. There is deliberately NO way to send
-- chat, whisper, or any packet - a script can react to chat but can never speak
-- or act for you, so it cannot be an auto-responder / social bot.
-- ---------------------------------------------------------------------------

-- Keywords to watch for (lower-cased). Edit to taste.
local watch = { "help", "trade", "wts", "wtb", string.lower(player.getName() or "") }

-- Curated sound key -> a file the server owner drops in sound/userscript/<key>.mp3
-- (a missing file simply no-ops). Keep it short so it does not become annoying.
local ALERT_SOUND = "ping"

-- Small rate-limit so a burst of matching lines does not spam sound/echo.
local lastAlert = 0

event.on("chat", function(text, ctype)
    local low = string.lower(text or "")
    for _, kw in ipairs(watch) do
        if kw ~= "" and string.find(low, kw, 1, true) then
            local now = time.now()
            if now - lastAlert >= 2.0 then
                lastAlert = now
                sound.play(ALERT_SOUND)                 -- SAFE local sfx
                chat.print("[alert] matched '" .. kw .. "'")   -- SAFE local echo
            end
            return
        end
    end
end)

-- Tiny helper command demo: on load, show how many lines are already buffered.
timer.after(1.0, function()
    chat.print("chat_alerts watching " .. #watch .. " keyword(s); " ..
        chat.count() .. " line(s) in buffer")
end)

log.info("chat_alerts loaded")
