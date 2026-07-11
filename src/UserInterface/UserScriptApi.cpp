// ELEMENTIA-USERSCRIPT
// ===========================================================================
// Curated, sandboxed Lua API for client userscripts.
//
// EVERYTHING a userscript can touch is defined here. The security model is
// "default deny": scripts do NOT get the real Lua globals - they get a fresh
// environment table that we fill with an explicit whitelist. If a capability
// is not copied in below, a script simply cannot reach it.
//
// WHAT IS DELIBERATELY *NOT* EXPOSED (and why):
//   * os        -> os.execute / os.remove / os.getenv = arbitrary command &
//                  filesystem access. Never opened.
//   * io        -> file read/write. Never opened.
//   * package / require / loadlib -> loading native .dll/.so or arbitrary Lua
//                  files = trivial sandbox escape / RCE. Never opened.
//   * debug     -> debug.getregistry / getupvalue / setmetatable can reach
//                  internal objects and defeat the sandbox. Never opened.
//   * load / loadstring / loadfile / dofile -> would let a script compile and
//                  run new chunks (potentially with a non-sandboxed _ENV, or
//                  precompiled bytecode that can crash the VM). Not whitelisted.
//   * string.dump -> hands back bytecode; not needed by addons. Removed.
//   * NO input synthesis, NO movement/attack, NO packet send. This is the
//     anti-cheat boundary: the API is READ-ONLY for gameplay so it cannot be
//     turned into an auto-clicker / bot. Server anti-cheat stays authoritative.
// ===========================================================================

#include "StdAfx.h"
#include "UserScriptApi.h"
#include "UserScriptManager.h"

#include "PythonPlayer.h"
#include "PythonCharacterManager.h"	// target.* / nearby.* read-only getters
#include "InstanceBase.h"			// CInstanceBase read-only getters
#include "PythonChat.h"
#include "Packet.h"			// EChatType, EPointTypes
#include "GameType.h"		// TItemPos / inventory slot constants

#include <ctime>
#include <string>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Shallow-copies the table at src (an absolute stack index) into a brand-new
// table pushed on top. Used to hand every script its OWN copy of a library
// table so one script rewriting e.g. string.format cannot affect others.
static void CopyTableShallow(lua_State* L, int src)
{
	lua_newtable(L);					// [.., dst]
	lua_pushnil(L);						// first key
	while (lua_next(L, src) != 0)		// [.., dst, key, value]
	{
		// dup key so lua_next keeps working after we consume one copy
		lua_pushvalue(L, -2);			// [.., dst, key, value, key]
		lua_pushvalue(L, -2);			// [.., dst, key, value, key, value]
		lua_rawset(L, -5);				// dst[key] = value ; pops key,value copy
		lua_pop(L, 1);					// pop original value, keep key for next
	}
}

// Copies a single named global (a whitelisted base function) into the table
// currently on top of the stack. Missing names are silently skipped.
static void WhitelistGlobal(lua_State* L, const char* name)
{
	// stack: [.., env]
	lua_getglobal(L, name);				// [.., env, value]
	if (lua_isnil(L, -1))
	{
		lua_pop(L, 1);
		return;
	}
	lua_setfield(L, -2, name);			// env[name] = value
}

// Copies a whole safe library table (by global name) as a fresh per-script
// copy into env[name].
static void WhitelistLibCopy(lua_State* L, const char* name)
{
	// stack: [.., env]
	lua_getglobal(L, name);				// [.., env, lib]
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return;
	}
	int libIdx = lua_gettop(L);
	CopyTableShallow(L, libIdx);		// [.., env, lib, copy]
	lua_remove(L, libIdx);				// [.., env, copy]
	lua_setfield(L, -2, name);			// env[name] = copy
}

// ---------------------------------------------------------------------------
// log.*  - throttled logging (routed to the client chat window + trace log)
// ---------------------------------------------------------------------------
static int l_log_info(lua_State* L)
{
	const char* msg = luaL_checkstring(L, 1);
	CUserScriptManager::Instance().ApiLog(msg, false);
	return 0;
}
static int l_log_warn(lua_State* L)
{
	const char* msg = luaL_checkstring(L, 1);
	CUserScriptManager::Instance().ApiLog(msg, true);
	return 0;
}
// print(...) is remapped to log.info so legacy addon code behaves sanely.
static int l_print(lua_State* L)
{
	int n = lua_gettop(L);
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	for (int i = 1; i <= n; ++i)
	{
		luaL_tolstring(L, i, nullptr);	// pushes string form of arg i
		luaL_addvalue(&b);				// moves it into the buffer (pops it)
		if (i < n) luaL_addchar(&b, '\t');
	}
	luaL_pushresult(&b);
	CUserScriptManager::Instance().ApiLog(lua_tostring(L, -1), false);
	return 0;
}

// ---------------------------------------------------------------------------
// player.*  - READ-ONLY player state. No setters, no move/attack: on purpose.
// ---------------------------------------------------------------------------
static int PlayerStatus(lua_State* L, unsigned int point)
{
	CPythonPlayer* p = CPythonPlayer::InstancePtr();
	lua_pushinteger(L, p ? (lua_Integer)p->GetStatus(point) : 0);
	return 1;
}
static int l_player_getName(lua_State* L)
{
	CPythonPlayer* p = CPythonPlayer::InstancePtr();
	lua_pushstring(L, p ? p->GetName() : "");
	return 1;
}
static int l_player_getLevel(lua_State* L)   { return PlayerStatus(L, POINT_LEVEL); }
static int l_player_getHP(lua_State* L)      { return PlayerStatus(L, POINT_HP); }
static int l_player_getMaxHP(lua_State* L)   { return PlayerStatus(L, POINT_MAX_HP); }
static int l_player_getSP(lua_State* L)      { return PlayerStatus(L, POINT_SP); }
static int l_player_getMaxSP(lua_State* L)   { return PlayerStatus(L, POINT_MAX_SP); }
static int l_player_getGold(lua_State* L)    { return PlayerStatus(L, POINT_GOLD); }

// ---------------------------------------------------------------------------
// coord.*  - READ-ONLY own map position (pixels). Derived from the main actor.
// No setter and no way to request a move: purely informational.
// ---------------------------------------------------------------------------
static int l_coord_get(lua_State* L)
{
	CPythonPlayer* p = CPythonPlayer::InstancePtr();
	if (!p) { lua_pushnil(L); return 1; }
	TPixelPosition pos;
	p->NEW_GetMainActorPosition(&pos);
	// Metin2 map coords: divide pixels by 100 for the familiar in-game units;
	// expose both so scripts can pick. Read-only either way.
	lua_pushnumber(L, pos.x);
	lua_pushnumber(L, pos.y);
	return 2;
}
static int l_coord_getX(lua_State* L)
{
	CPythonPlayer* p = CPythonPlayer::InstancePtr();
	TPixelPosition pos;
	if (p) p->NEW_GetMainActorPosition(&pos); else pos.x = 0.0f;
	lua_pushnumber(L, p ? pos.x : 0.0);
	return 1;
}
static int l_coord_getY(lua_State* L)
{
	CPythonPlayer* p = CPythonPlayer::InstancePtr();
	TPixelPosition pos;
	if (p) p->NEW_GetMainActorPosition(&pos); else pos.y = 0.0f;
	lua_pushnumber(L, p ? pos.y : 0.0);
	return 1;
}

// ---------------------------------------------------------------------------
// time.*  - READ-ONLY clocks. Purely informational; no scheduling power beyond
// the existing timer.* API. now() is the client global time (seconds); wall()
// is the local wall clock. Neither exposes anything sensitive.
// ---------------------------------------------------------------------------
static int l_time_now(lua_State* L)
{
	lua_pushnumber(L, CUserScriptManager::Instance().GlobalTime());
	return 1;
}
static int l_time_wall(lua_State* L)
{
	std::time_t t = std::time(nullptr);
	std::tm lt;
#if defined(_WIN32)
	localtime_s(&lt, &t);
#else
	localtime_r(&t, &lt);
#endif
	lua_pushinteger(L, lt.tm_hour);
	lua_pushinteger(L, lt.tm_min);
	lua_pushinteger(L, lt.tm_sec);
	return 3;
}

// ---------------------------------------------------------------------------
// target.*  - READ-ONLY info about the currently selected target. Derived from
// CPythonPlayer::GetTargetVID + the character manager. No targeting/clearing:
// a script can observe the target the PLAYER chose, never change it.
// ---------------------------------------------------------------------------
static CInstanceBase* ResolveTarget(DWORD* pdwVID)
{
	CPythonPlayer* p = CPythonPlayer::InstancePtr();
	if (!p) { if (pdwVID) *pdwVID = 0; return nullptr; }
	DWORD vid = p->GetTargetVID();
	if (pdwVID) *pdwVID = vid;
	if (vid == 0) return nullptr;
	CPythonCharacterManager* pcm = CPythonCharacterManager::InstancePtr();
	if (!pcm) return nullptr;
	return pcm->GetInstancePtr(vid);
}
static int l_target_exists(lua_State* L)
{
	DWORD vid = 0;
	lua_pushboolean(L, ResolveTarget(&vid) != nullptr);
	return 1;
}
static int l_target_getName(lua_State* L)
{
	CInstanceBase* t = ResolveTarget(nullptr);
	const char* n = t ? t->GetNameString() : "";
	lua_pushstring(L, n ? n : "");
	return 1;
}
static int l_target_getLevel(lua_State* L)
{
	CInstanceBase* t = ResolveTarget(nullptr);
	lua_pushinteger(L, t ? (lua_Integer)t->GetLevel() : 0);
	return 1;
}
static int l_target_getDistance(lua_State* L)
{
	DWORD vid = 0;
	CInstanceBase* t = ResolveTarget(&vid);
	CPythonCharacterManager* pcm = CPythonCharacterManager::InstancePtr();
	CInstanceBase* main = pcm ? pcm->GetMainInstancePtr() : nullptr;
	if (t && main) lua_pushnumber(L, main->GetDistance(t));
	else           lua_pushnumber(L, -1.0);
	return 1;
}
static int l_target_isMonster(lua_State* L)
{
	CInstanceBase* t = ResolveTarget(nullptr);
	lua_pushboolean(L, t && !t->IsPC() && !t->IsNPC() && !t->IsStone() ? 1 : 0);
	return 1;
}
static int l_target_isPC(lua_State* L)
{
	CInstanceBase* t = ResolveTarget(nullptr);
	lua_pushboolean(L, t && t->IsPC() ? 1 : 0);
	return 1;
}
static int l_target_isStone(lua_State* L)
{
	CInstanceBase* t = ResolveTarget(nullptr);
	lua_pushboolean(L, t && t->IsStone() ? 1 : 0);
	return 1;
}
static int l_target_isNPC(lua_State* L)
{
	CInstanceBase* t = ResolveTarget(nullptr);
	lua_pushboolean(L, t && t->IsNPC() ? 1 : 0);
	return 1;
}
static int l_target_isDead(lua_State* L)
{
	CInstanceBase* t = ResolveTarget(nullptr);
	lua_pushboolean(L, t && t->IsDead() ? 1 : 0);
	return 1;
}
static int l_target_getHPPercent(lua_State* L)
{
	// Only known if the server sent a target-HP packet for this exact target.
	DWORD vid = 0;
	ResolveTarget(&vid);
	lua_pushinteger(L, (lua_Integer)CUserScriptManager::Instance().TargetHPPercent(vid));
	return 1;
}

// ---------------------------------------------------------------------------
// nearby.*  - READ-ONLY, THROTTLED info about nearby characters. Backed by a
// snapshot the manager rebuilds at most ~10x/sec (name + distance + coarse kind
// only). No pointers, no targeting - just situational awareness for a HUD.
// ---------------------------------------------------------------------------
static int NearbyKindFromString(const char* s)
{
	if (!s) return -1;					// -1 == any kind
	if (strcmp(s, "monster") == 0) return USERSCRIPT_NEARBY_MONSTER;
	if (strcmp(s, "pc") == 0)      return USERSCRIPT_NEARBY_PC;
	if (strcmp(s, "player") == 0)  return USERSCRIPT_NEARBY_PC;
	if (strcmp(s, "npc") == 0)     return USERSCRIPT_NEARBY_NPC;
	if (strcmp(s, "stone") == 0)   return USERSCRIPT_NEARBY_STONE;
	if (strcmp(s, "metin") == 0)   return USERSCRIPT_NEARBY_STONE;
	return -1;							// "all"/anything else == any
}
static int l_nearby_count(lua_State* L)
{
	int wantKind = NearbyKindFromString(luaL_optstring(L, 1, nullptr));
	double radius = luaL_optnumber(L, 2, 0.0);	// 0 == unlimited
	const std::vector<SUserScriptNearby>& snap =
		CUserScriptManager::Instance().NearbySnapshot();
	int n = 0;
	for (const SUserScriptNearby& e : snap)
	{
		if (wantKind >= 0 && e.iKind != wantKind) continue;
		if (radius > 0.0 && e.fDistance > radius) continue;
		++n;
	}
	lua_pushinteger(L, n);
	return 1;
}
static int l_nearby_names(lua_State* L)
{
	int wantKind = NearbyKindFromString(luaL_optstring(L, 1, nullptr));
	double radius = luaL_optnumber(L, 2, 0.0);
	int maxN = (int)luaL_optinteger(L, 3, 32);
	if (maxN < 0) maxN = 0;
	if (maxN > 256) maxN = 256;
	const std::vector<SUserScriptNearby>& snap =
		CUserScriptManager::Instance().NearbySnapshot();
	lua_newtable(L);
	int idx = 0;
	for (const SUserScriptNearby& e : snap)
	{
		if (idx >= maxN) break;
		if (wantKind >= 0 && e.iKind != wantKind) continue;
		if (radius > 0.0 && e.fDistance > radius) continue;
		lua_pushinteger(L, ++idx);
		lua_pushstring(L, e.strName.c_str());
		lua_settable(L, -3);
	}
	return 1;
}

// ---------------------------------------------------------------------------
// inventory.*  - READ-ONLY counts over the main bag. No move/use/drop: this
// cannot be turned into an auto-looter/auto-user, only observe what you carry.
// ---------------------------------------------------------------------------
static int l_inventory_slotCount(lua_State* L)
{
	lua_pushinteger(L, (lua_Integer)c_ItemSlot_Count);
	return 1;
}
static bool InvSlotArg(lua_State* L, int arg, TItemPos* out)
{
	lua_Integer slot = luaL_checkinteger(L, arg);
	if (slot < 0 || slot >= (lua_Integer)c_ItemSlot_Count)
		return false;
	*out = TItemPos(INVENTORY, (WORD)slot);
	return true;
}
static int l_inventory_getItemVnum(lua_State* L)
{
	TItemPos pos;
	CPythonPlayer* p = CPythonPlayer::InstancePtr();
	if (!p || !InvSlotArg(L, 1, &pos)) { lua_pushinteger(L, 0); return 1; }
	lua_pushinteger(L, (lua_Integer)p->GetItemIndex(pos));
	return 1;
}
static int l_inventory_getItemCount(lua_State* L)
{
	TItemPos pos;
	CPythonPlayer* p = CPythonPlayer::InstancePtr();
	if (!p || !InvSlotArg(L, 1, &pos)) { lua_pushinteger(L, 0); return 1; }
	lua_pushinteger(L, (lua_Integer)p->GetItemCount(pos));
	return 1;
}
static int l_inventory_isEmpty(lua_State* L)
{
	TItemPos pos;
	CPythonPlayer* p = CPythonPlayer::InstancePtr();
	if (!p || !InvSlotArg(L, 1, &pos)) { lua_pushboolean(L, 1); return 1; }
	lua_pushboolean(L, p->GetItemIndex(pos) == 0 ? 1 : 0);
	return 1;
}
static int l_inventory_countByVnum(lua_State* L)
{
	lua_Integer vnum = luaL_checkinteger(L, 1);
	CPythonPlayer* p = CPythonPlayer::InstancePtr();
	if (!p || vnum <= 0) { lua_pushinteger(L, 0); return 1; }
	lua_pushinteger(L, (lua_Integer)p->GetItemCountByVnum((DWORD)vnum));
	return 1;
}

// ---------------------------------------------------------------------------
// equipment.*  - READ-ONLY view of the OWN character's worn gear. Same shape as
// inventory.* but addresses the equip (wear) slots. No equip/unequip/move/use:
// a script can only observe what the player is wearing, never change it, and it
// can only ever see its OWN character (never another player's equipment).
//
// A slot argument is a wear-position index (equipment.slot.*); it is bounds-
// checked against [0, c_Wear_Max) and composed into an EQUIPMENT TItemPos, so a
// script can never index out of the item array.
// ---------------------------------------------------------------------------
static bool EquipSlotArg(lua_State* L, int arg, TItemPos* out)
{
	lua_Integer slot = luaL_checkinteger(L, arg);
	if (slot < 0 || slot >= (lua_Integer)c_Wear_Max)
		return false;
	// Equip cells live in the same backing array as the inventory, offset by
	// c_Equipment_Start; the EQUIPMENT window type resolves to it read-only.
	*out = TItemPos(EQUIPMENT, (WORD)(c_Equipment_Start + slot));
	return true;
}
static int l_equipment_slotCount(lua_State* L)
{
	lua_pushinteger(L, (lua_Integer)c_Wear_Max);
	return 1;
}
static int l_equipment_getItemVnum(lua_State* L)
{
	TItemPos pos;
	CPythonPlayer* p = CPythonPlayer::InstancePtr();
	if (!p || !EquipSlotArg(L, 1, &pos)) { lua_pushinteger(L, 0); return 1; }
	lua_pushinteger(L, (lua_Integer)p->GetItemIndex(pos));
	return 1;
}
static int l_equipment_getItemCount(lua_State* L)
{
	TItemPos pos;
	CPythonPlayer* p = CPythonPlayer::InstancePtr();
	if (!p || !EquipSlotArg(L, 1, &pos)) { lua_pushinteger(L, 0); return 1; }
	lua_pushinteger(L, (lua_Integer)p->GetItemCount(pos));
	return 1;
}
static int l_equipment_isEmpty(lua_State* L)
{
	TItemPos pos;
	CPythonPlayer* p = CPythonPlayer::InstancePtr();
	if (!p || !EquipSlotArg(L, 1, &pos)) { lua_pushboolean(L, 1); return 1; }
	lua_pushboolean(L, p->GetItemIndex(pos) == 0 ? 1 : 0);
	return 1;
}

// ---------------------------------------------------------------------------
// buff.*  - READ-ONLY active-affect check on the main character. Boolean state
// only (the client does not hold per-affect remaining time in C++; a HUD can
// time buffs itself from the on/off transitions - see the buff_timer example).
// buff.ids.* exposes the affect id constants.
// ---------------------------------------------------------------------------
static int l_buff_has(lua_State* L)
{
	lua_Integer affectId = luaL_checkinteger(L, 1);
	if (affectId < 0 || affectId >= CInstanceBase::AFFECT_NUM)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	CPythonCharacterManager* pcm = CPythonCharacterManager::InstancePtr();
	CInstanceBase* main = pcm ? pcm->GetMainInstancePtr() : nullptr;
	lua_pushboolean(L, (main && main->IsAffect((UINT)affectId)) ? 1 : 0);
	return 1;
}

// ---------------------------------------------------------------------------
// config.*  - sandboxed per-script key/value persistence. Values are limited to
// string/number/boolean (NEVER code/tables); the file path is fixed from the
// script name so a script cannot write anywhere else. See the manager for caps.
// ---------------------------------------------------------------------------
static int l_config_set(lua_State* L)
{
	const char* key = luaL_checkstring(L, 1);
	std::string encoded;
	int t = lua_type(L, 2);
	switch (t)
	{
		case LUA_TNIL:					// nil -> delete the key
			encoded.clear();
			break;
		case LUA_TSTRING:
			encoded = "s";
			encoded += lua_tostring(L, 2);
			break;
		case LUA_TNUMBER:
		{
			char num[40];
			_snprintf(num, sizeof(num), "%.17g", (double)lua_tonumber(L, 2));
			num[sizeof(num) - 1] = '\0';
			encoded = "n";
			encoded += num;
			break;
		}
		case LUA_TBOOLEAN:
			encoded = lua_toboolean(L, 2) ? "b1" : "b0";
			break;
		default:
			return luaL_error(L, "config.set: value must be string, number, boolean or nil");
	}
	if (!CUserScriptManager::Instance().ConfigSet(key, encoded))
		return luaL_error(L, "config.set: rejected (key/size cap or storage error)");
	return 0;
}
static int l_config_get(lua_State* L)
{
	const char* key = luaL_checkstring(L, 1);
	std::string encoded;
	if (!CUserScriptManager::Instance().ConfigGet(key, encoded) || encoded.empty())
	{
		// Optional default (arg 2) returned when the key is absent.
		if (lua_gettop(L) >= 2) lua_pushvalue(L, 2);
		else                    lua_pushnil(L);
		return 1;
	}
	char tag = encoded[0];
	const char* rest = encoded.c_str() + 1;
	switch (tag)
	{
		case 's': lua_pushstring(L, rest); break;
		case 'n': lua_pushnumber(L, (lua_Number)strtod(rest, nullptr)); break;
		case 'b': lua_pushboolean(L, rest[0] == '1' ? 1 : 0); break;
		default:  lua_pushnil(L); break;
	}
	return 1;
}

// ---------------------------------------------------------------------------
// event.on(name, fn)  - subscribe to a whitelisted client event.
// ---------------------------------------------------------------------------
static int l_event_on(lua_State* L)
{
	const char* name = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);

	int ev = -1;
	if      (strcmp(name, "update") == 0) ev = USERSCRIPT_EVENT_UPDATE;
	else if (strcmp(name, "second") == 0) ev = USERSCRIPT_EVENT_SECOND;
	else return luaL_error(L, "event.on: unknown event '%s'", name);

	lua_pushvalue(L, 2);						// dup the function
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);	// stash it, get a ref
	if (CUserScriptManager::Instance().ApiRegisterHook(ev, ref) < 0)
	{
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
		return luaL_error(L, "event.on: too many handlers");
	}
	return 0;
}

// ---------------------------------------------------------------------------
// timer.after(seconds, fn)  /  timer.every(seconds, fn)
// ---------------------------------------------------------------------------
static int TimerAdd(lua_State* L, bool repeat)
{
	double delay = luaL_checknumber(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (delay < 0.0) delay = 0.0;

	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	if (CUserScriptManager::Instance().ApiAddTimer(delay, ref, repeat) < 0)
	{
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
		return luaL_error(L, "timer: too many timers");
	}
	return 0;
}
static int l_timer_after(lua_State* L) { return TimerAdd(L, false); }
static int l_timer_every(lua_State* L) { return TimerAdd(L, true); }

// ---------------------------------------------------------------------------
// ui.*  - minimal on-screen text widgets (owned by the calling script).
// Handles are opaque integer ids; every mutator verifies ownership so one
// script cannot poke another script's widgets.
// ---------------------------------------------------------------------------
static int RequireOwnedWidget(lua_State* L, int arg, SUserScriptWidget** out)
{
	int id = (int)luaL_checkinteger(L, arg);
	CUserScriptManager& mgr = CUserScriptManager::Instance();
	if (!mgr.WidgetOwnedByCurrent(id))
		return luaL_error(L, "ui: invalid or unowned widget handle");
	*out = mgr.GetWidget(id);
	return id;
}
static int CreateWidgetOfType(lua_State* L, int type, const char* who)
{
	int id = CUserScriptManager::Instance().ApiCreateWidget(type);
	// -1 == hard limit reached (script error); -2 == font/resources not ready
	// yet (transient) -> return nil so the script can simply retry next frame.
	if (id == -1) return luaL_error(L, "%s: widget limit reached", who);
	if (id < 0) { lua_pushnil(L); return 1; }
	lua_pushinteger(L, id);
	return 1;
}
static int l_ui_createText(lua_State* L)
{
	return CreateWidgetOfType(L, USERSCRIPT_WIDGET_TEXT, "ui.createText");
}
static int l_ui_createRect(lua_State* L)
{
	return CreateWidgetOfType(L, USERSCRIPT_WIDGET_RECT, "ui.createRect");
}
static int l_ui_createBar(lua_State* L)
{
	return CreateWidgetOfType(L, USERSCRIPT_WIDGET_BAR, "ui.createBar");
}
static int l_ui_createIcon(lua_State* L)
{
	return CreateWidgetOfType(L, USERSCRIPT_WIDGET_ICON, "ui.createIcon");
}
// ui.setIcon(id, vnum) - show a client-internal ITEM icon (resolved from vnum).
// vnum 0 clears the icon. There is NO path parameter anywhere: the only way to
// choose an image is a vnum (item table) or a curated key (ui.setIconKey), so a
// script can never point the widget at an arbitrary file.
static int l_ui_setIcon(lua_State* L)
{
	SUserScriptWidget* w = nullptr;
	RequireOwnedWidget(L, 1, &w);
	lua_Integer vnum = luaL_checkinteger(L, 2);
	if (vnum < 0) vnum = 0;
	CUserScriptManager::Instance().ApiSetWidgetIconVnum(w, (unsigned int)vnum);
	return 0;
}
// ui.setIconKey(id, key) - show a CURATED, whitelisted icon by short key name.
// The key must be [a-z0-9_] (<=32); the C++ side maps it to a FIXED internal
// resource path. No directory/traversal/absolute path can be expressed.
static int l_ui_setIconKey(lua_State* L)
{
	SUserScriptWidget* w = nullptr;
	RequireOwnedWidget(L, 1, &w);
	const char* key = luaL_checkstring(L, 2);
	CUserScriptManager::Instance().ApiSetWidgetIconKey(w, key);
	return 0;
}
static int l_ui_setText(lua_State* L)
{
	SUserScriptWidget* w = nullptr;
	RequireOwnedWidget(L, 1, &w);
	const char* text = luaL_checkstring(L, 2);
	if (w && w->pInstance)
		w->pInstance->SetValue(text);
	return 0;
}
static int l_ui_setPosition(lua_State* L)
{
	SUserScriptWidget* w = nullptr;
	RequireOwnedWidget(L, 1, &w);
	float x = (float)luaL_checknumber(L, 2);
	float y = (float)luaL_checknumber(L, 3);
	if (w) { w->fX = x; w->fY = y; }
	return 0;
}
static int l_ui_setColor(lua_State* L)
{
	SUserScriptWidget* w = nullptr;
	RequireOwnedWidget(L, 1, &w);
	float r = (float)luaL_checknumber(L, 2);
	float g = (float)luaL_checknumber(L, 3);
	float b = (float)luaL_checknumber(L, 4);
	float a = (float)luaL_optnumber(L, 5, 1.0);
	if (w)
	{
		// Store on the widget (used by RECT fill / BAR foreground) and, for a
		// TEXT widget, forward to the text instance.
		w->fR = r; w->fG = g; w->fB = b; w->fA = a;
		if (w->pInstance)
			w->pInstance->SetColor(r, g, b, a);
	}
	return 0;
}
// ui.setSize(id, w, h) - RECT/BAR extent in UI pixels.
static int l_ui_setSize(lua_State* L)
{
	SUserScriptWidget* w = nullptr;
	RequireOwnedWidget(L, 1, &w);
	float ww = (float)luaL_checknumber(L, 2);
	float hh = (float)luaL_checknumber(L, 3);
	if (ww < 0.0f) ww = 0.0f;
	if (hh < 0.0f) hh = 0.0f;
	if (w) { w->fW = ww; w->fH = hh; }
	return 0;
}
// ui.setProgress(id, frac) - BAR fill fraction, clamped 0..1.
static int l_ui_setProgress(lua_State* L)
{
	SUserScriptWidget* w = nullptr;
	RequireOwnedWidget(L, 1, &w);
	float f = (float)luaL_checknumber(L, 2);
	if (f < 0.0f) f = 0.0f;
	if (f > 1.0f) f = 1.0f;
	if (w) w->fProgress = f;
	return 0;
}
// ui.setBackColor(id, r, g, b[, a]) - BAR background colour.
static int l_ui_setBackColor(lua_State* L)
{
	SUserScriptWidget* w = nullptr;
	RequireOwnedWidget(L, 1, &w);
	float r = (float)luaL_checknumber(L, 2);
	float g = (float)luaL_checknumber(L, 3);
	float b = (float)luaL_checknumber(L, 4);
	float a = (float)luaL_optnumber(L, 5, 1.0);
	if (w) { w->fBackR = r; w->fBackG = g; w->fBackB = b; w->fBackA = a; }
	return 0;
}
static int l_ui_show(lua_State* L)
{
	SUserScriptWidget* w = nullptr;
	RequireOwnedWidget(L, 1, &w);
	if (w) w->bVisible = true;
	return 0;
}
static int l_ui_hide(lua_State* L)
{
	SUserScriptWidget* w = nullptr;
	RequireOwnedWidget(L, 1, &w);
	if (w) w->bVisible = false;
	return 0;
}
static int l_ui_destroy(lua_State* L)
{
	SUserScriptWidget* w = nullptr;
	RequireOwnedWidget(L, 1, &w);
	if (w) w->bDead = true;		// reaped by the manager next frame
	return 0;
}

// ---------------------------------------------------------------------------
// setmetatable shim - MANDATORY hardening.
//
// The raw base setmetatable is NOT whitelisted because it lets a script attach
// a __gc finalizer:  setmetatable({}, { __gc = function() while true do end end })
// Lua runs __gc during garbage collection with L->allowhook cleared (see
// lgc.c GCTM), so our instruction-count deadline hook is DISABLED inside the
// finalizer - even within a protected call. A finalizer that spins forever and
// allocates nothing dodges both the time budget AND the memory cap, producing a
// permanent client freeze (also on lua_close during logout/reload). Since GC
// runs during normal play, this is a remote-ish DoS from any loaded addon.
//
// This shim behaves exactly like the standard setmetatable EXCEPT it refuses a
// metatable that carries a __gc field. Legitimate OOP (__index/__newindex/...)
// keeps working.
static int l_safe_setmetatable(lua_State* L)
{
	// Mirrors the standard luaB_setmetatable, with an added __gc rejection.
	int t = lua_type(L, 2);
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_argexpected(L, t == LUA_TNIL || t == LUA_TTABLE, 2, "nil or table");

	if (t == LUA_TTABLE)
	{
		// Reject any __gc finalizer (the unhookable-freeze vector above).
		// lua_getfield pushes the value (nil when absent); the trailing
		// lua_settop(L, 2) below discards this probe either way.
		if (lua_getfield(L, 2, "__gc") != LUA_TNIL)
			return luaL_error(L, "__gc metamethod is not allowed in userscripts");
	}

	// Preserve standard protection against changing a locked metatable.
	// (luaL_getmetafield pushes nothing when the field/metatable is absent.)
	if (luaL_getmetafield(L, 1, "__metatable") != LUA_TNIL)
		return luaL_error(L, "cannot change a protected metatable");

	lua_settop(L, 2);			// normalise stack to exactly (table, metatable)
	lua_setmetatable(L, 1);		// pops metatable, sets it on the table
	return 1;					// return the table
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
static void RegisterTable(lua_State* L, const char* name, const luaL_Reg* funcs)
{
	// stack: [.., env]
	lua_newtable(L);
	luaL_setfuncs(L, funcs, 0);
	lua_setfield(L, -2, name);		// env[name] = { ...funcs... }
}

void UserScript_OpenSafeLibs(lua_State* L)
{
	// Open ONLY the safe subset. NB: we do NOT call luaL_openlibs (which would
	// also pull in os, io, package and debug).
	static const luaL_Reg safeLibs[] = {
		{ LUA_GNAME,       luaopen_base },
		{ LUA_TABLIBNAME,  luaopen_table },
		{ LUA_STRLIBNAME,  luaopen_string },
		{ LUA_MATHLIBNAME, luaopen_math },
		{ LUA_COLIBNAME,   luaopen_coroutine },
		{ LUA_UTF8LIBNAME, luaopen_utf8 },
		{ nullptr, nullptr }
	};
	for (const luaL_Reg* lib = safeLibs; lib->func; ++lib)
	{
		luaL_requiref(L, lib->name, lib->func, 1);
		lua_pop(L, 1);	// remove the lib table left by requiref
	}
}

void UserScript_PushSandboxEnv(lua_State* L)
{
	lua_newtable(L);					// [.., env]

	// --- curated base functions (safe subset only) ---------------------
	static const char* kBaseWhitelist[] = {
		"assert", "error", "ipairs", "pairs", "next", "select",
		"tonumber", "tostring", "type", "pcall", "xpcall",
		"rawequal", "rawget", "rawset", "rawlen",
		"_VERSION",
		nullptr
	};
	for (const char** n = kBaseWhitelist; *n; ++n)
		WhitelistGlobal(L, *n);
	// Explicitly NOT copied:
	//   * load, loadfile, dofile, require -> no compiling/loading new chunks.
	//   * collectgarbage -> no manual GC control (DoS lever).
	//   * _G             -> no handle to the real globals (we set env._G = env).
	//   * print          -> replaced by the throttled log.info below.
	//   * getmetatable   -> would hand back the SHARED string metatable, whose
	//                       __index is the internal string table; a script could
	//                       poison string.* for the whole client.
	//   * setmetatable   -> replaced by l_safe_setmetatable below (rejects __gc,
	//                       which would run unhookable during GC and freeze the
	//                       client). Legit OOP metatables keep working.
	lua_pushcfunction(L, l_safe_setmetatable);
	lua_setfield(L, -2, "setmetatable");

	// --- safe library tables (fresh per-script copies) -----------------
	WhitelistLibCopy(L, LUA_TABLIBNAME);
	WhitelistLibCopy(L, LUA_MATHLIBNAME);
	WhitelistLibCopy(L, LUA_COLIBNAME);
	WhitelistLibCopy(L, LUA_UTF8LIBNAME);

	// string: copy, then strip string.dump (hands back bytecode).
	lua_getglobal(L, LUA_STRLIBNAME);
	if (lua_istable(L, -1))
	{
		int s = lua_gettop(L);
		CopyTableShallow(L, s);			// [.., env, string, copy]
		lua_pushnil(L);
		lua_setfield(L, -2, "dump");	// copy.dump = nil
		lua_remove(L, s);				// [.., env, copy]
		lua_setfield(L, -2, LUA_STRLIBNAME);
	}
	else lua_pop(L, 1);

	// --- safe replacement for print -----------------------------------
	lua_pushcfunction(L, l_print);
	lua_setfield(L, -2, "print");

	// --- ELEMENTIA curated API ----------------------------------------
	static const luaL_Reg logFuncs[] = {
		{ "info", l_log_info }, { "warn", l_log_warn }, { nullptr, nullptr }
	};
	RegisterTable(L, "log", logFuncs);

	static const luaL_Reg playerFuncs[] = {
		{ "getName",  l_player_getName },
		{ "getLevel", l_player_getLevel },
		{ "getHP",    l_player_getHP },
		{ "getMaxHP", l_player_getMaxHP },
		{ "getSP",    l_player_getSP },
		{ "getMaxSP", l_player_getMaxSP },
		{ "getGold",  l_player_getGold },
		{ nullptr, nullptr }
	};
	RegisterTable(L, "player", playerFuncs);

	static const luaL_Reg eventFuncs[] = {
		{ "on", l_event_on }, { nullptr, nullptr }
	};
	RegisterTable(L, "event", eventFuncs);

	static const luaL_Reg timerFuncs[] = {
		{ "after", l_timer_after }, { "every", l_timer_every }, { nullptr, nullptr }
	};
	RegisterTable(L, "timer", timerFuncs);

	static const luaL_Reg uiFuncs[] = {
		{ "createText",   l_ui_createText },
		{ "createRect",   l_ui_createRect },
		{ "createBar",    l_ui_createBar },
		{ "createIcon",   l_ui_createIcon },
		{ "setText",      l_ui_setText },
		{ "setIcon",      l_ui_setIcon },
		{ "setIconKey",   l_ui_setIconKey },
		{ "setPosition",  l_ui_setPosition },
		{ "setColor",     l_ui_setColor },
		{ "setSize",      l_ui_setSize },
		{ "setProgress",  l_ui_setProgress },
		{ "setBackColor", l_ui_setBackColor },
		{ "show",         l_ui_show },
		{ "hide",         l_ui_hide },
		{ "destroy",      l_ui_destroy },
		{ nullptr, nullptr }
	};
	RegisterTable(L, "ui", uiFuncs);

	static const luaL_Reg targetFuncs[] = {
		{ "exists",       l_target_exists },
		{ "getName",      l_target_getName },
		{ "getLevel",     l_target_getLevel },
		{ "getDistance",  l_target_getDistance },
		{ "getHPPercent", l_target_getHPPercent },
		{ "isMonster",    l_target_isMonster },
		{ "isPC",         l_target_isPC },
		{ "isNPC",        l_target_isNPC },
		{ "isStone",      l_target_isStone },
		{ "isDead",       l_target_isDead },
		{ nullptr, nullptr }
	};
	RegisterTable(L, "target", targetFuncs);

	static const luaL_Reg nearbyFuncs[] = {
		{ "count", l_nearby_count },
		{ "names", l_nearby_names },
		{ nullptr, nullptr }
	};
	RegisterTable(L, "nearby", nearbyFuncs);

	static const luaL_Reg inventoryFuncs[] = {
		{ "slotCount",    l_inventory_slotCount },
		{ "getItemVnum",  l_inventory_getItemVnum },
		{ "getItemCount", l_inventory_getItemCount },
		{ "isEmpty",      l_inventory_isEmpty },
		{ "countByVnum",  l_inventory_countByVnum },
		{ nullptr, nullptr }
	};
	RegisterTable(L, "inventory", inventoryFuncs);

	static const luaL_Reg equipmentFuncs[] = {
		{ "slotCount",    l_equipment_slotCount },
		{ "getItemVnum",  l_equipment_getItemVnum },
		{ "getItemCount", l_equipment_getItemCount },
		{ "isEmpty",      l_equipment_isEmpty },
		{ nullptr, nullptr }
	};
	RegisterTable(L, "equipment", equipmentFuncs);
	// equipment.slot.* - named wear-position constants matching the client's real
	// CItemData::EWearPositions. Read-only informational table.
	lua_getfield(L, -1, "equipment");		// [.., env, equipment]
	lua_newtable(L);						// [.., env, equipment, slot]
	{
		struct { const char* name; int id; } kWearSlots[] = {
			{ "BODY",    CItemData::WEAR_BODY },
			{ "HEAD",    CItemData::WEAR_HEAD },
			{ "SHOES",   CItemData::WEAR_FOOTS },
			{ "FOOTS",   CItemData::WEAR_FOOTS },
			{ "WRIST",   CItemData::WEAR_WRIST },
			{ "WEAPON",  CItemData::WEAR_WEAPON },
			{ "NECK",    CItemData::WEAR_NECK },
			{ "EAR",     CItemData::WEAR_EAR },
			{ "UNIQUE1", CItemData::WEAR_UNIQUE1 },
			{ "UNIQUE2", CItemData::WEAR_UNIQUE2 },
			{ "ARROW",   CItemData::WEAR_ARROW },
			{ "SHIELD",  CItemData::WEAR_SHIELD },
			{ "RING1",   CItemData::WEAR_RING1 },
			{ "RING2",   CItemData::WEAR_RING2 },
			{ "BELT",    CItemData::WEAR_BELT },
			{ "COSTUME_BODY",   CItemData::WEAR_COSTUME_BODY },
			{ "COSTUME_HAIR",   CItemData::WEAR_COSTUME_HAIR },
			{ "COSTUME_WEAPON", CItemData::WEAR_COSTUME_WEAPON },
		};
		for (const auto& s : kWearSlots)
		{
			lua_pushinteger(L, s.id);
			lua_setfield(L, -2, s.name);
		}
	}
	lua_setfield(L, -2, "slot");			// equipment.slot = {...}
	lua_pop(L, 1);							// [.., env]

	static const luaL_Reg buffFuncs[] = {
		{ "has", l_buff_has },
		{ nullptr, nullptr }
	};
	RegisterTable(L, "buff", buffFuncs);
	// buff.ids.* - curated affect-id constants (subset that is meaningful on the
	// local player). Read-only informational table.
	lua_getfield(L, -1, "buff");			// [.., env, buff]
	lua_newtable(L);						// [.., env, buff, ids]
	{
		struct { const char* name; int id; } kAffectIds[] = {
			{ "POISON",      CInstanceBase::AFFECT_POISON },
			{ "SLOW",        CInstanceBase::AFFECT_SLOW },
			{ "STUN",        CInstanceBase::AFFECT_STUN },
			{ "FIRE",        CInstanceBase::AFFECT_FIRE },
			{ "DASH",        CInstanceBase::AFFECT_DASH },
			{ "JEONGWI",     CInstanceBase::AFFECT_JEONGWI },
			{ "GEOMGYEONG",  CInstanceBase::AFFECT_GEOMGYEONG },
			{ "CHEONGEUN",   CInstanceBase::AFFECT_CHEONGEUN },
			{ "GYEONGGONG",  CInstanceBase::AFFECT_GYEONGGONG },
			{ "EUNHYEONG",   CInstanceBase::AFFECT_EUNHYEONG },
			{ "GWIGEOM",     CInstanceBase::AFFECT_GWIGEOM },
			{ "HOSIN",       CInstanceBase::AFFECT_HOSIN },
			{ "BOHO",        CInstanceBase::AFFECT_BOHO },
			{ "KWAESOK",     CInstanceBase::AFFECT_KWAESOK },
			{ "MUYEONG",     CInstanceBase::AFFECT_MUYEONG },
			{ "MOV_SPEED",   CInstanceBase::AFFECT_MOV_SPEED_POTION },
			{ "ATT_SPEED",   CInstanceBase::AFFECT_ATT_SPEED_POTION },
		};
		for (const auto& a : kAffectIds)
		{
			lua_pushinteger(L, a.id);
			lua_setfield(L, -2, a.name);
		}
	}
	lua_setfield(L, -2, "ids");				// buff.ids = {...} ; [.., env, buff]
	lua_pop(L, 1);							// [.., env]

	static const luaL_Reg coordFuncs[] = {
		{ "get",  l_coord_get },
		{ "getX", l_coord_getX },
		{ "getY", l_coord_getY },
		{ nullptr, nullptr }
	};
	RegisterTable(L, "coord", coordFuncs);

	static const luaL_Reg timeFuncs[] = {
		{ "now",  l_time_now },
		{ "wall", l_time_wall },
		{ nullptr, nullptr }
	};
	RegisterTable(L, "time", timeFuncs);

	static const luaL_Reg configFuncs[] = {
		{ "set", l_config_set },
		{ "get", l_config_get },
		{ nullptr, nullptr }
	};
	RegisterTable(L, "config", configFuncs);

	// elementia = { version = N, sandbox = true }  (read-only-ish info)
	// v2: added target/nearby/inventory/buff/coord/time/config + rect/bar widgets.
	// v3: added equipment.* (read-only worn gear) + ui.createIcon/setIcon/setIconKey.
	lua_newtable(L);
	lua_pushinteger(L, 3);
	lua_setfield(L, -2, "version");
	lua_pushboolean(L, 1);
	lua_setfield(L, -2, "sandbox");
	lua_setfield(L, -2, "elementia");

	// Let scripts that reference _G see their own sandbox (harmless).
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "_G");

	// leaves exactly [.., env] on the stack
}
