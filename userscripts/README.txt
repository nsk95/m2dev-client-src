ELEMENTIA userscripts
=====================

Drop sandboxed Lua 5.4 addons (*.lua) into this folder. They load automatically
when the game client starts.

What you can use (curated, sandboxed API - ALL gameplay reads are READ-ONLY):
  log.info(text) / log.warn(text)      throttled logging -> chat + client log
  player.getName() / getLevel()        READ-ONLY player state
  player.getHP() / getMaxHP()
  player.getSP() / getMaxSP() / getGold()

  coord.get() -> x, y                  READ-ONLY own map position (pixels)
  coord.getX() / coord.getY()

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
  ui.setText(id, str) / setPosition(id,x,y) / setColor(id,r,g,b[,a])
  ui.setSize(id,w,h) / setProgress(id,0..1) / setBackColor(id,r,g,b[,a])
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
  plus safe standard libs: string, table, math, coroutine, utf8

In-game: press F10 to open the Addon Manager (enable/disable/reload addons).

What you deliberately CANNOT do (sandbox / anti-cheat boundary):
  - no os, io, package/require, debug, load/loadfile/dofile  (no RCE / file access)
  - no movement / attack / input synthesis / packet sending  (not a bot framework)
  - no access to other scripts' data or to the real Lua globals

Notes:
  - A script that errors is logged and, after repeated faults, auto-disabled.
  - A script that loops forever or allocates too much memory is aborted; it
    cannot hang or OOM the client.
  - To disable a script, move it into userscripts/disabled/ (that subfolder is
    ignored) or remove it.

The server anti-cheat remains authoritative for all gameplay regardless of what
client userscripts do.

See hud_stats.lua for a worked example, and equip_icons.lua for an
equipment.* + ui.createIcon example (a small worn-gear icon HUD).
