#pragma once
// ELEMENTIA-USERSCRIPT
// ---------------------------------------------------------------------------
// Client-side Lua *userscript* sandbox manager.
//
// This subsystem lets players/modders drop sandboxed Lua addons into
//   <client>/userscripts/*.lua
// to add QoL features, on-screen widgets and UI helpers - conceptually like
// WoW addons. It is COMPLETELY SEPARATE from the server-side quest Lua:
//   * different Lua version (client embeds a fresh, audited Lua 5.4.7;
//     the quest engine on the server is an ancient Lua 5.0.3),
//   * different process (this runs in the game client),
//   * different (much smaller) API surface, and it is heavily sandboxed.
//
// Threat model / design goals (see UserScriptApi.cpp for the full rationale):
//   1. A malicious userscript MUST NOT be able to compromise the player:
//      no filesystem, no process/OS access, no arbitrary code loading,
//      no native memory access. Only a curated, memory-safe C++ API is
//      reachable from Lua.
//   2. A userscript MUST NOT become a convenient cheat/bot framework:
//      the API is deliberately READ-ONLY for gameplay state and exposes
//      NO way to synthesize input, move/attack, or send packets. The
//      server anti-cheat stays authoritative regardless.
//   3. A broken/hostile script MUST NOT crash or hang the client:
//      every entry into Lua is pcall-isolated, bounded by a wall-clock
//      deadline (instruction-count hook) and a hard memory cap, and a
//      script that misbehaves is auto-disabled instead of taking down
//      the game.
// ---------------------------------------------------------------------------

#include "EterBase/Singleton.h"

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <chrono>

struct lua_State;
class CGraphicTextInstance;
class CGraphicImageInstance;	// ELEMENTIA-USERSCRIPT: icon widget backing instance

// ELEMENTIA-USERSCRIPT: kinds of on-screen widget a script can own.
// TEXT is the original text overlay; RECT is a solid colour panel; BAR is a
// two-layer progress bar (background + foreground scaled by a 0..1 fraction).
// ICON renders a client-internal item icon (resolved from a vnum) or a curated,
// whitelisted texture key - a script can NEVER hand in an arbitrary file path.
// RECT/BAR are drawn from the 2D Grp primitives and need NO font resource.
enum EUserScriptWidgetType
{
	USERSCRIPT_WIDGET_TEXT = 0,
	USERSCRIPT_WIDGET_RECT,
	USERSCRIPT_WIDGET_BAR,
	USERSCRIPT_WIDGET_ICON,
};

// ELEMENTIA-USERSCRIPT: one entry in the throttled "nearby characters" snapshot.
// Read-only, name + distance + coarse kind only - never a native pointer.
enum EUserScriptNearbyKind
{
	USERSCRIPT_NEARBY_OTHER = 0,
	USERSCRIPT_NEARBY_MONSTER,
	USERSCRIPT_NEARBY_PC,
	USERSCRIPT_NEARBY_NPC,
	USERSCRIPT_NEARBY_STONE,
};
struct SUserScriptNearby
{
	std::string	strName;
	float		fDistance = 0.0f;
	int			iKind = USERSCRIPT_NEARBY_OTHER;
};

// One scheduled timer callback (timer.after / timer.every).
struct SUserScriptTimer
{
	int			iOwnerScript = -1;
	int			iCallbackRef = 0;		// registry ref to the Lua function
	double		dNextFireTime = 0.0;	// seconds (global time)
	double		dInterval = 0.0;		// 0 == one-shot
	bool		bRepeat = false;
	bool		bDead = false;
};

// One on-screen widget owned by a script (ui.createText / createRect / createBar).
struct SUserScriptWidget
{
	int						iOwnerScript = -1;
	int						iType = USERSCRIPT_WIDGET_TEXT;
	CGraphicTextInstance*	pInstance = nullptr;	// TEXT only
	CGraphicImageInstance*	pImage = nullptr;		// ICON only (pooled New/Delete)
	float					fX = 0.0f;
	float					fY = 0.0f;
	float					fW = 0.0f;				// RECT/BAR size
	float					fH = 0.0f;
	float					fProgress = 1.0f;		// BAR fill fraction (0..1)
	// Primary colour: TEXT is handled by the text instance; RECT fill / BAR
	// foreground use these; BAR background uses the fBack* set.
	float					fR = 1.0f, fG = 1.0f, fB = 1.0f, fA = 1.0f;
	float					fBackR = 0.0f, fBackG = 0.0f, fBackB = 0.0f, fBackA = 0.5f;
	bool					bVisible = true;
	bool					bDead = false;
};

// One event.on subscription for a whitelisted event name.
struct SUserScriptHook
{
	int			iOwnerScript = -1;
	int			iEvent = 0;			// EUserScriptEvent
	int			iCallbackRef = 0;	// registry ref to the Lua function
	bool		bDead = false;
};

// A single loaded userscript file.
struct SUserScript
{
	std::string	strName;			// file name without extension
	std::string	strPath;			// absolute path on disk
	int			iEnvRef = 0;		// registry ref to the sandbox _ENV table
	bool		bEnabled = true;
	bool		bFaulted = false;	// disabled after a runtime fault
	int			iErrorCount = 0;
	int			iLogBudget = 0;		// remaining log lines this second
	double		dLogWindowStart = 0.0;

	// ELEMENTIA-USERSCRIPT: sandboxed per-script config.* storage.
	// Lazily loaded from <client>/userscripts/config/<name>.dat on first use;
	// values are type-tagged strings (see UserScriptApi config.*). The script
	// can NOT choose the path - it is derived from its (sanitised) file name.
	std::map<std::string, std::string>	configCache;
	bool		bConfigLoaded = false;
	// config.set only mutates the in-memory cache and raises this flag; the
	// file is written by a THROTTLED flush (>= 1s apart, and on disable/reload/
	// shutdown) so a per-frame config.set() cannot thrash the disk / hitch the
	// frame. See FlushDirtyConfigs.
	bool		bConfigDirty = false;
};

enum EUserScriptEvent
{
	USERSCRIPT_EVENT_UPDATE = 0,	// fired every game frame: fn(dtSeconds)
	USERSCRIPT_EVENT_SECOND,		// fired ~once per second: fn()
	USERSCRIPT_EVENT_MAX,
};

class CUserScriptManager : public CSingleton<CUserScriptManager>
{
	public:
		CUserScriptManager();
		virtual ~CUserScriptManager();

		// Lifecycle -------------------------------------------------------
		// Boot the sandbox VM and load every script in <client>/userscripts.
		void Initialize();
		// Tear everything down (free widgets, close the Lua state).
		void Destroy();

		bool IsInitialized() const { return m_pLuaState != nullptr; }

		// Per-frame pump: dispatches the "update"/"second" events and fires
		// due timers. Safe to call every frame; cheap when no scripts loaded.
		void Update(double dGlobalTime, double dElapsed);

		// Draw script-owned on-screen widgets (called after the UI renders).
		void Render();

		// --- Bindings called back from the curated C API (UserScriptApi) ---
		// These are public only so the free C functions in UserScriptApi.cpp
		// can reach the owning manager; they are NOT exposed to Lua.
		int   CurrentScript() const { return m_iCurrentScript; }
		lua_State* State() const { return m_pLuaState; }

		int   ApiRegisterHook(int iEvent, int iCallbackRef);
		int   ApiAddTimer(double dDelay, int iCallbackRef, bool bRepeat);
		int   ApiCreateWidget(int iType);
		SUserScriptWidget* GetWidget(int iId);
		bool  WidgetOwnedByCurrent(int iId);

		// ELEMENTIA-USERSCRIPT: icon widget resolution. BOTH paths are
		// client-internal: a script may pass ONLY an item vnum (resolved through
		// the client's own item-icon table) or a curated key that maps to a FIXED
		// whitelisted resource path. A script can never supply a raw file path, so
		// there is no filesystem escape / info-leak surface. Returns false on a
		// rejected/unknown vnum or key (the icon is then left cleared).
		bool  ApiSetWidgetIconVnum(SUserScriptWidget* pWidget, unsigned int dwVnum);
		bool  ApiSetWidgetIconKey(SUserScriptWidget* pWidget, const char* c_szKey);

		// Throttled logging helper (used by log.* bindings).
		void  ApiLog(const char* c_szText, bool bWarn);

		double GlobalTime() const { return m_dGlobalTime; }

		// --- read-only target HP% cache (fed by the target packet handler) ---
		// The client learns a target's HP percentage only from a server packet;
		// we cache the last (vid, pct) so target.getHPPercent() can return it
		// read-only. Returns -1 when the cached vid does not match dwCurrentVID.
		void  NotifyTargetHP(unsigned int dwVID, int iPercent);
		int   TargetHPPercent(unsigned int dwCurrentVID) const;

		// --- throttled "nearby characters" snapshot (nearby.* bindings) ------
		// Rebuilt at most once per kNearbyThrottleSec; read-only name/distance/
		// kind only. Never hands a native instance pointer to Lua.
		const std::vector<SUserScriptNearby>& NearbySnapshot();

		// --- sandboxed config.* storage --------------------------------------
		// Encoded values are opaque type-tagged strings owned by the API layer.
		bool  ConfigGet(const char* c_szKey, std::string& strOut);
		bool  ConfigSet(const char* c_szKey, const std::string& strEncoded);

		// --- addon-manager surface (read/enable/reload) ----------------------
		// Called from the Python `app` module bindings. Index-addressed; all
		// state changes persist the enabled/disabled set to disk.
		int   ScriptCount() const { return (int)m_scripts.size(); }
		const char* ScriptName(int iIdx) const;
		bool  ScriptEnabled(int iIdx) const;
		bool  ScriptFaulted(int iIdx) const;
		int   ScriptErrorCount(int iIdx) const;
		void  SetScriptEnabled(int iIdx, bool bEnabled);
		void  ReloadScripts();

		// Used by the instruction-count hook to abort runaway scripts.
		bool DeadlineExceeded() const
		{
			return std::chrono::steady_clock::now() > m_deadline;
		}

		// True when the script index is loaded, enabled and not faulted, i.e.
		// its hooks/timers should fire and its widgets should draw.
		bool  ScriptActive(int iIdx) const;

	private:
		void  LoadDirectory(const std::string& strDir, bool bEnabledDir);
		void  LoadScriptFile(const std::string& strPath);
		void  DispatchEvent(int iEvent, double dArg);
		void  FireTimers();
		void  FaultCurrentScript(const char* c_szWhere, const char* c_szError);

		// Userscript config/enable persistence helpers.
		void  ClearAllScripts();				// unref everything (for reload)
		void  LoadDisabledSet();				// read userscripts/config/_disabled.dat
		void  SaveDisabledSet();				// write it back
		std::string ConfigDir() const;			// <base>/config (created on demand)
		std::string SanitizeName(const std::string& strName) const;
		bool  ConfigLoadFor(int iScriptIdx);	// lazy-load a script's config file
		bool  ConfigSaveFor(int iScriptIdx);	// serialise a script's config file
		void  FlushDirtyConfigs();				// write out any dirty config caches
		void  RefreshNearbySnapshot();			// rebuild the throttled snapshot

		// Sets up the instruction-count hook deadline for the next protected
		// call and installs the hook. Returns after ProtectedEnd().
		void  ProtectedBegin();
		void  ProtectedEnd();

		lua_State*					m_pLuaState = nullptr;
		std::vector<SUserScript>	m_scripts;
		std::vector<SUserScriptHook>	m_hooks;
		std::vector<SUserScriptTimer>	m_timers;
		std::vector<SUserScriptWidget>	m_widgets;

		int		m_iCurrentScript = -1;	// index of the script currently running
		double	m_dGlobalTime = 0.0;
		double	m_dLastSecondPulse = 0.0;

		// Where userscripts (and their config subfolder) live on disk. Captured
		// at Initialize() so reloads and config writes use a fixed, known base.
		std::string	m_strBaseDir;

		// Persisted enable/disable state (set of disabled script names).
		std::vector<std::string>	m_disabledNames;

		// Read-only target HP% cache (see NotifyTargetHP).
		unsigned int	m_dwTargetHPCacheVID = 0;
		int				m_iTargetHPCachePct = -1;

		// Throttled nearby-characters snapshot.
		std::vector<SUserScriptNearby>	m_nearby;
		double	m_dNearbyStamp = -1.0;

		// Throttle for the periodic config flush (see FlushDirtyConfigs).
		double	m_dConfigFlushStamp = 0.0;

		// Deadline for the currently executing protected call (set by the
		// instruction-count hook to abort runaway scripts).
		std::chrono::steady_clock::time_point	m_deadline;
};
