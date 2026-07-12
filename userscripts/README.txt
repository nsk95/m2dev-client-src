ELEMENTIA userscripts
=====================

Drop sandboxed Lua 5.4 addons (*.lua) into this folder. They load automatically
when the game client starts.

What you can use (curated, sandboxed API - ALL gameplay reads are READ-ONLY):
  log.info(text) / log.warn(text)      throttled logging -> chat + client log
  player.getName() / getLevel()        READ-ONLY player state
  player.getHP() / getMaxHP()
  player.getSP() / getMaxSP() / getGold()
  player.getExp() / getMaxExp()        current / next-level experience
  player.getStamina() / getMaxStamina()
  player.getST() / getHT() / getDX() / getIQ()      the four base stats
  player.getAttackPower() / getDefense()

  map.getName() -> string              READ-ONLY current map name (no warp API)

  coord.get() -> x, y                  READ-ONLY own map position (pixels)
  coord.getX() / coord.getY()

  skill.slotCount() -> n               READ-ONLY view of YOUR skill slots
  skill.getIndex(slot) -> skillId      (0 = empty slot)
  skill.getLevel(slot) / getGrade(slot)
  skill.getCoolTime(slot) -> seconds   the slot's total cooldown
  skill.getElapsedCoolTime(slot)       seconds since it was last used
  skill.getRemainingCoolTime(slot)     seconds left (>= 0)
  skill.isCoolTime(slot) -> bool       currently cooling down?
  skill.isActive(slot) -> bool         (there is NO skill.use/cast - read only)

  party.count() -> n                   READ-ONLY YOUR-party roster only
  party.getName(i) -> string           i = 0 .. count-1
  party.getHPPercent(i) -> 0..100      (-1 if unknown)
  party.getState(i) -> n               (coarse member state)

  quest.count() -> n                   READ-ONLY active-quest readout
  quest.getTitle(i) -> string          i = 0 .. count-1
  quest.getCounter(i) -> name, value   quest counter (name "" if none)
  quest.getClock(i) -> name, value     quest clock/timer

  target.exists() -> bool              READ-ONLY info about YOUR selected target
  target.getName() / getLevel()        (you cannot change or clear the target)
  target.getDistance() -> pixels       (-1 if none)
  target.getHPPercent() -> 0..100      (-1 if the server has not sent it)
  target.isMonster()/isPC()/isNPC()/isStone()/isDead()

  nearby.count([kind][,radius]) -> n   THROTTLED, READ-ONLY nearby characters
  nearby.names([kind][,radius][,max])  kind = "monster"/"pc"/"npc"/"stone"/nil
                                       -> table of names (max default 32)

  inventory.slotCount() -> n           READ-ONLY bag contents (no move/use/drop)
  inventory.getItemVnum(slot) -> vnum  (0 if empty)
  inventory.getItemCount(slot) -> n
  inventory.isEmpty(slot) -> bool
  inventory.countByVnum(vnum) -> n

  equipment.slotCount() -> n           READ-ONLY view of YOUR worn gear
  equipment.getItemVnum(slot) -> vnum  (no equip/unequip/move/use)
  equipment.getItemCount(slot) -> n    slot = equipment.slot.* (see below)
  equipment.isEmpty(slot) -> bool
  equipment.slot.WEAPON / .BODY / .HEAD / .SHIELD / .SHOES / .WRIST /
    .NECK / .EAR / .UNIQUE1 / .UNIQUE2 / .ARROW / .RING1 / .RING2 / .BELT /
    .COSTUME_BODY / .COSTUME_HAIR / .COSTUME_WEAPON   wear-position constants

  inventory.getMetinSocket(slot, i)    READ-ONLY item metadata (for equip-vs-bag
  inventory.getAttribute(slot, i)        comparisons etc.). i is bounds-checked.
    -> type, value                       getAttribute returns (type, value);
  inventory.getFlags(slot) -> flags      getMetinSocket/getFlags return a number.
  equipment.getMetinSocket(slot, i)      Same three getters for worn gear.
  equipment.getAttribute(slot, i) -> type, value
  equipment.getFlags(slot) -> flags      (all READ-ONLY; no move/use/equip)

  player.getPoint(id) -> value         READ-ONLY generic status point (bounds-
                                       checked). Use player.point.* constants:
  player.point.RESIST_FIRE / .RESIST_ELEC / .RESIST_MAGIC / .RESIST_WIND /
    .RESIST_SWORD / .RESIST_TWOHAND / .RESIST_DAGGER / .RESIST_BELL / .RESIST_FAN /
    .RESIST_BOW / .ATTBONUS_HUMAN / .ATTBONUS_ANIMAL / .ATTBONUS_ORC /
    .ATTBONUS_MILGYO / .ATTBONUS_UNDEAD / .ATTBONUS_DEVIL / .ATTBONUS_INSECT /
    .ATTBONUS_FIRE / .ATTBONUS_ICE / .ATTBONUS_DESERT / .CRITICAL_PCT /
    .PENETRATE_PCT / .BLOCK / .DODGE / .STEAL_HP / .STEAL_SP / .MANA_BURN_PCT /
    .MAGIC_ATT / .MAX_HP / .MAX_SP / .EXP_BONUS / .ITEM_DROP_BONUS / .GOLD_BONUS

  guild.exists() -> bool               READ-ONLY view of YOUR OWN guild only
  guild.getName() -> string            (no invite/kick/withdraw/grade API)
  guild.getLevel() -> n
  guild.memberCount() / maxMemberCount() -> n
  guild.getMemberName(i) -> string     i = 0 .. memberCount-1
  guild.getMemberLevel(i) / getMemberGrade(i) / getMemberJob(i) -> n
  guild.isMemberOnline(i) -> bool

  shop.isOpen() -> bool                READ-ONLY listing of the shop you have OPEN
  shop.isPrivate() / isMine() -> bool  (NPC or private shop). NO buy/sell API -
  shop.tabCount() -> n                 you can read offers, never transact.
  shop.slotCount() -> n                flat slot space (tabCount * per-tab slots)
  shop.getItemVnum(slot) -> vnum       (0 for an empty slot)
  shop.getItemPrice(slot) -> gold
  shop.getItemCount(slot) -> n

  chat.count() -> n                    READ-ONLY view of your OWN chat window
  chat.getLine(i) -> text, type        i = 0 is the OLDEST retained line
  event.on("chat", fn)                 fn(text, type) for each NEW chat line.
                                       (there is NO chat SEND - read-only feed)

  buff.has(affectId) -> bool           READ-ONLY active-affect check on you
  buff.ids.DASH / .GEOMGYEONG / ...    curated affect id constants

  time.now() -> seconds                client global time (matches timers)
  time.wall() -> hour, min, sec        local wall clock

  config.set(key, value)               sandboxed per-script persistence.
  config.get(key[, default]) -> value  value is string/number/boolean ONLY
                                       (never code); stored in a fixed file
                                       userscripts/config/<scriptname>.dat that
                                       the script cannot choose. config.set(k,nil)
                                       deletes the key. Size-capped.

  event.on("update", fn)               fn(dt) each frame
  event.on("second", fn)               fn() ~once per second
  timer.after(seconds, fn)             one-shot timer
  timer.every(seconds, fn)             repeating timer

  ui.createText() -> id                on-screen text widget (returns a handle)
  ui.createRect() -> id                solid colour panel (setSize + setColor)
  ui.createBar()  -> id                progress bar (setSize + setProgress +
                                       setColor + setBackColor)
  ui.createIcon() -> id                item-icon widget (see setIcon below)
  ui.createLine() -> id                2D line (setPosition = start, setLine = end)
  ui.createPanel() -> id               framed container: translucent fill (setBackColor)
                                       + outline border (setColor); size via setSize.
                                       It is a background frame, NOT a parent that owns
                                       child widgets - place other widgets over it and
                                       use setLayer to control depth.
  ui.setText(id, str) / setPosition(id,x,y) / setColor(id,r,g,b[,a])
  ui.setSize(id,w,h) / setProgress(id,0..1) / setBackColor(id,r,g,b[,a])
  ui.setLine(id, x2, y2[, thickness])  LINE end point + thickness (1..16 px)
  ui.setLayer(id, n)                   draw order (-128..128); lower draws first
                                       (behind). Lets a panel sit behind its text.
  ui.setIcon(id, vnum)                 show a client-internal ITEM icon by vnum
                                       (0 clears it). There is NO file-path arg:
                                       the icon is resolved through the client's
                                       own item table, so a script can never
                                       load an arbitrary file.
  ui.setIconKey(id, key)               show a CURATED icon by short key name
                                       (key = [a-z0-9_], <=32). The client maps
                                       it to a fixed whitelisted resource under
                                       icon/userscript/<key>.tga - no path,
                                       directory or traversal can be expressed.
  ui.show(id) / hide(id) / destroy(id)

  --- SAFE client-local writes (cosmetic / QoL only) -----------------------
  These are the ONLY "write" APIs. Each is OUTPUT-ONLY: it renders text or plays
  a sound on YOUR machine and does NOT send anything to the server or affect
  gameplay, so it cannot become an automation or advantage.
  chat.print(text)                     append ONE line to YOUR OWN chat window
                                       (local render only; NEVER sent to the
                                       server or other players). Source-tagged
                                       with the addon name so it cannot forge a
                                       system/GM/other-player message. Throttled.
  sound.play(key) -> bool              play ONE curated local sound as an alert.
                                       key = [a-z0-9_], <=32 -> fixed resource
                                       sound/userscript/<key>.mp3 (no path/
                                       traversal). Rate-limited; returns false if
                                       throttled/rejected. Output-only: it cannot
                                       act for you.

  plus safe standard libs: string, table, math, coroutine, utf8

In-game: press F10 to open the Addon Manager. It lists every addon with its
enable/disable state, and - when an addon fails to load - the exact error
message so you can fix it. You can enable/disable addons (remembered between
sessions), reload a single addon in place, or reload them all.

What you deliberately CANNOT do (sandbox / anti-cheat boundary):
  - no os, io, package/require, debug, load/loadfile/dofile  (no RCE / file access)
  - no movement / attack / skill cast / item use-move-drop / buy-sell-trade /
    target change / packet send / input synthesis (keys/mouse)  (not a bot framework)
  - no chat/whisper SEND (chat is a read-only feed; chat.print is LOCAL echo only)
  - no camera control, no screenshot trigger, no "auto-anything"
  - no access to other scripts' data or to the real Lua globals
  The rule is glass-clear: a script may only OBSERVE client state and present it
  locally/cosmetically - it may never act for the player.

Notes:
  - A script that errors is logged and, after repeated faults, auto-disabled.
  - A script that loops forever or allocates too much memory is aborted; it
    cannot hang or OOM the client.
  - To disable a script, move it into userscripts/disabled/ (that subfolder is
    ignored) or remove it.

The server anti-cheat remains authoritative for all gameplay regardless of what
client userscripts do.

Worked examples in this folder:
  hud_stats.lua        minimal name/level/HP text overlay (start here)
  coords.lua           coordinate readout + config-persisted position
  buff_timer.lua       buff/affect HUD (counts up from on/off transitions)
  dps_meter.lua        target HP%/s estimate + HP bar (display only)
  loot_filter.lua      inventory watch/bag-full advisory (no auto-pickup)
  equip_icons.lua      equipment.* + ui.createIcon worn-gear icon HUD
  char_panel.lua       framed HUD: vitals + EXP + map + quest (player.*/map.*/
                       quest.* + ui.createPanel/setLayer)
  cooldown_tracker.lua per-skill cooldown bars (skill.* read-only cooldowns)
  party_panel.lua      party roster with HP bars (party.* + ui.createLine)
  guild_roster.lua     your-guild readout: name/level + top members (guild.*)
  chat_alerts.lua      keyword watch on your chat + local echo/sound (chat.*,
                       event.on("chat"), chat.print, sound.play)
  shop_scanner.lua     lists the open shop's items/prices (read-only shop.*)
