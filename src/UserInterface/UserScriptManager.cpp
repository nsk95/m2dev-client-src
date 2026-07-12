// ELEMENTIA-USERSCRIPT
// ===========================================================================
// Sandbox manager: owns the embedded Lua 5.4 state used for client userscripts,
// loads scripts from <client>/userscripts, and pumps their event/timer/widget
// life-cycle. See UserScriptManager.h for the threat model and UserScriptApi.cpp
// for the curated API.
//
// Isolation guarantees implemented here:
//   * Custom allocator with a hard memory cap  -> a script cannot OOM the client.
//   * Instruction-count debug hook + deadline  -> a script cannot hang the client
//                                                 with an infinite loop.
//   * Every call into Lua goes through lua_pcall (ProtectedBegin/End)  -> a Lua
//     error becomes a logged fault, never a client crash.
//   * A script that faults is auto-disabled (bFaulted) after repeated errors.
//   * Chunks are loaded in TEXT-ONLY mode ("t")  -> precompiled bytecode, which
//     could be crafted to crash the VM, is rejected.
// ===========================================================================

#include "StdAfx.h"
#include "UserScriptManager.h"
#include "UserScriptApi.h"

#include "GameType.h"				// DefaultFont_GetResource()
#include "PythonChat.h"
#include "PythonPlayer.h"			// ELEMENTIA-USERSCRIPT: player position for nearby.*
#include "PythonCharacterManager.h"	// ELEMENTIA-USERSCRIPT: nearby.* iteration
#include "InstanceBase.h"			// ELEMENTIA-USERSCRIPT: read-only instance getters

#include "EterBase/Debug.h"
#include "EterLib/GrpText.h"
#include "EterLib/GrpTextInstance.h"
#include "EterLib/GrpImageInstance.h"		// ELEMENTIA-USERSCRIPT: icon widget instance
#include "EterLib/GrpImage.h"				// ELEMENTIA-USERSCRIPT: CGraphicImage (curated key)
#include "EterLib/GrpSubImage.h"			// ELEMENTIA-USERSCRIPT: item icon CGraphicSubImage upcast
#include "EterLib/ResourceManager.h"		// ELEMENTIA-USERSCRIPT: curated icon resource load
#include "EterPythonLib/PythonGraphic.h"	// ELEMENTIA-USERSCRIPT: 2D bar/rect widgets
#include "GameLib/ItemManager.h"			// ELEMENTIA-USERSCRIPT: vnum -> item icon resolution
#include "GameLib/ItemData.h"				// ELEMENTIA-USERSCRIPT: CItemData::GetIconImage()

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

namespace
{
	// --- hard limits (tune later; conservative for the core) ---
	constexpr size_t   kMemoryCapBytes   = 16 * 1024 * 1024;	// 16 MB total VM heap
	constexpr int      kHookInstrCount   = 10000;	// check the deadline this often
	constexpr int      kCallBudgetMs     = 50;		// per protected call
	constexpr int      kMaxHooksTotal    = 256;
	constexpr int      kMaxTimersTotal   = 256;
	constexpr int      kMaxWidgetsTotal  = 256;
	constexpr int      kMaxWidgetsScript = 32;
	constexpr int      kFaultThreshold   = 5;		// auto-disable after N faults
	constexpr int      kLogLinesPerSec   = 10;		// per-script log throttle

	// --- config.* storage caps (per script) ---
	constexpr size_t   kConfigMaxKeys    = 128;
	constexpr size_t   kConfigMaxKeyLen  = 64;
	constexpr size_t   kConfigMaxValLen  = 1024;
	constexpr size_t   kConfigMaxFile    = 64 * 1024;	// total serialised bytes

	// --- nearby.* snapshot ---
	constexpr double   kNearbyThrottleSec = 0.10;	// rebuild at most every 100ms
	constexpr size_t   kNearbyMaxEntries  = 256;	// hard cap on snapshot size

	// --- ui.createIcon curated-key resolution (see ApiSetWidgetIconKey) ---
	// A curated key is a SHORT, strictly-validated leaf name ([a-z0-9_], max 32).
	// It is composed into a FIXED internal path  <prefix><key><suffix>  that lives
	// inside the client resource packs. The script supplies ONLY the leaf; it can
	// never inject a directory, a traversal ("..") or an absolute path, so there is
	// no way to read anything outside this one curated icon folder. Server owners
	// curate the set simply by dropping <key>.tga icons into that pack folder.
	const char* const  kIconKeyPrefix   = "icon/userscript/";
	const char* const  kIconKeySuffix   = ".tga";
	constexpr size_t   kIconKeyMaxLen   = 32;

	// Tracks total bytes handed out so we can enforce kMemoryCapBytes.
	struct SAllocState { size_t used = 0; };

	// Custom Lua allocator enforcing the memory cap. Denying an allocation
	// (returning NULL) makes Lua raise a catchable "not enough memory" error.
	void* SandboxAlloc(void* ud, void* ptr, size_t osize, size_t nsize)
	{
		SAllocState* st = static_cast<SAllocState*>(ud);
		if (nsize == 0)
		{
			if (ptr) { st->used -= osize; }		// osize is real size on free
			free(ptr);
			return nullptr;
		}
		size_t cur = st->used - (ptr ? osize : 0);
		if (cur + nsize > kMemoryCapBytes)
			return nullptr;						// deny -> Lua memory error
		void* np = realloc(ptr, nsize);
		if (np) st->used = cur + nsize;
		return np;
	}

	// Panic handler: Lua calls this on an unprotected error. We must NOT let it
	// call abort() (default behaviour) and take down the client. Since every
	// real entry point is protected, reaching here means a bug in our glue; log
	// and unwind by longjmp is unsafe, so we just log and let Lua's protected
	// boundary handle it. Installed mostly as a safety net for early boot.
	int SandboxPanic(lua_State* L)
	{
		const char* msg = lua_tostring(L, -1);
		TraceError("[USERSCRIPT] PANIC: %s", msg ? msg : "(unknown)");
		return 0;	// returning makes lua_error re-raise; protected calls catch it
	}

	// Instruction-count hook: aborts a script that blows its time budget.
	void CountHook(lua_State* L, lua_Debug* /*ar*/)
	{
		if (CUserScriptManager::InstancePtr() &&
			CUserScriptManager::Instance().DeadlineExceeded())
		{
			luaL_error(L, "userscript exceeded time budget (possible infinite loop)");
		}
	}

	SAllocState g_allocState;
}

CUserScriptManager::CUserScriptManager() = default;

CUserScriptManager::~CUserScriptManager()
{
	Destroy();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void CUserScriptManager::Initialize()
{
	if (m_pLuaState)
		return;

	g_allocState.used = 0;
	m_pLuaState = lua_newstate(SandboxAlloc, &g_allocState);
	if (!m_pLuaState)
	{
		TraceError("[USERSCRIPT] failed to create Lua state");
		return;
	}
	lua_atpanic(m_pLuaState, SandboxPanic);

	// Only the memory-safe subset of the stdlib - never os/io/package/debug.
	UserScript_OpenSafeLibs(m_pLuaState);

	// Resolve <client>/userscripts (relative to the working dir the client
	// runs from). Enabled scripts live directly in it; anything under
	// userscripts/disabled/ is intentionally skipped.
	std::error_code ec;
	std::filesystem::path base = std::filesystem::current_path(ec) / "userscripts";
	if (!std::filesystem::exists(base, ec))
	{
		// Create the folder so players have an obvious place to drop addons.
		std::filesystem::create_directories(base, ec);
		Tracef("[USERSCRIPT] created userscripts folder: %s\n", base.string().c_str());
	}
	m_strBaseDir = base.string();

	// Read the persisted enable/disable set BEFORE loading so disabled scripts
	// still load (their name shows in the manager) but stay dormant.
	LoadDisabledSet();

	LoadDirectory(m_strBaseDir, true);

	Tracef("[USERSCRIPT] initialized: %zu script(s) loaded\n", m_scripts.size());
}

void CUserScriptManager::Destroy()
{
	// Persist any pending config.set() changes before we tear the state down.
	FlushDirtyConfigs();

	for (SUserScriptWidget& w : m_widgets)
	{
		if (w.pInstance) { delete w.pInstance; w.pInstance = nullptr; }
		if (w.pImage) { CGraphicImageInstance::Delete(w.pImage); w.pImage = nullptr; }
	}
	m_widgets.clear();
	m_hooks.clear();
	m_timers.clear();
	m_scripts.clear();

	if (m_pLuaState)
	{
		lua_close(m_pLuaState);		// frees registry refs, envs, everything
		m_pLuaState = nullptr;
	}
	m_iCurrentScript = -1;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------
void CUserScriptManager::LoadDirectory(const std::string& strDir, bool /*bEnabledDir*/)
{
	std::error_code ec;
	std::filesystem::directory_iterator it(strDir, ec), end;
	if (ec)
		return;

	for (; it != end; it.increment(ec))
	{
		if (ec) break;
		const std::filesystem::directory_entry& de = *it;
		if (!de.is_regular_file(ec))
			continue;
		std::filesystem::path p = de.path();
		if (p.extension() != ".lua")
			continue;
		LoadScriptFile(p.string());
	}
}

void CUserScriptManager::LoadScriptFile(const std::string& strPath)
{
	lua_State* L = m_pLuaState;
	if (!L) return;

	std::filesystem::path pp(strPath);
	std::string name = pp.stem().string();

	// Register the script record first so bindings invoked during the
	// top-level run (event.on / timer.* / ui.*) attribute correctly.
	SUserScript sc;
	sc.strName = name;
	sc.strPath = strPath;
	// Honour the persisted disable set: a disabled script is still recorded (so
	// the addon manager can list and re-enable it) but its hooks/timers never
	// fire and its widgets never draw (see ScriptActive).
	sc.bEnabled = (std::find(m_disabledNames.begin(), m_disabledNames.end(), name)
					== m_disabledNames.end());
	m_scripts.push_back(sc);
	int scriptIdx = (int)m_scripts.size() - 1;

	LoadScriptChunk(scriptIdx);
}

// Store a length-capped copy of the last error and log it. Also used so the F10
// addon manager can display WHY a script failed to load.
void CUserScriptManager::SetScriptError(int iScriptIdx, const char* c_szError)
{
	if (iScriptIdx < 0 || iScriptIdx >= (int)m_scripts.size())
		return;
	std::string msg = c_szError ? c_szError : "(unknown error)";
	if (msg.size() > 256) msg.resize(256);		// keep the record bounded
	m_scripts[iScriptIdx].strLastError = msg;
}

// Compile + run the top-level chunk for an already-registered record. Shared by
// the initial directory load and single-script reload.
void CUserScriptManager::LoadScriptChunk(int iScriptIdx)
{
	lua_State* L = m_pLuaState;
	if (!L || iScriptIdx < 0 || iScriptIdx >= (int)m_scripts.size())
		return;

	const std::string strPath = m_scripts[iScriptIdx].strPath;
	const std::string name    = m_scripts[iScriptIdx].strName;

	// Read file bytes (plain disk file; NOT via the eterpack VFS on purpose -
	// userscripts are meant to be hand-editable by the player).
	std::ifstream f(strPath, std::ios::binary);
	if (!f)
	{
		TraceError("[USERSCRIPT] cannot open %s", strPath.c_str());
		m_scripts[iScriptIdx].bFaulted = true;
		SetScriptError(iScriptIdx, "cannot open file");
		return;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	std::string src = ss.str();

	// Build this script's private sandbox environment and stash it.
	UserScript_PushSandboxEnv(L);				// [env]
	int envRef = luaL_ref(L, LUA_REGISTRYINDEX);
	m_scripts[iScriptIdx].iEnvRef = envRef;

	// Compile in TEXT-ONLY mode ("t"): reject precompiled bytecode chunks.
	std::string chunkName = "@" + name;
	int rc = luaL_loadbufferx(L, src.data(), src.size(), chunkName.c_str(), "t");
	if (rc != LUA_OK)
	{
		const char* err = lua_tostring(L, -1);
		TraceError("[USERSCRIPT] compile error in %s: %s", name.c_str(), err);
		SetScriptError(iScriptIdx, err);
		lua_pop(L, 1);
		m_scripts[iScriptIdx].bFaulted = true;
		return;
	}

	// Set the chunk's _ENV (upvalue 1) to the sandbox env, so ALL globals the
	// script sees resolve into the curated table - it can never reach real _G.
	lua_rawgeti(L, LUA_REGISTRYINDEX, envRef);	// [chunk, env]
	if (lua_setupvalue(L, -2, 1) == nullptr)	// chunk._ENV = env ; pops env
		lua_pop(L, 1);							// (no upvalue: e.g. empty file)

	// Run the top-level chunk under the full isolation harness.
	m_iCurrentScript = iScriptIdx;
	ProtectedBegin();
	int callrc = lua_pcall(L, 0, 0, 0);			// [ (err?) ]
	ProtectedEnd();
	m_iCurrentScript = -1;

	if (callrc != LUA_OK)
	{
		const char* err = lua_tostring(L, -1);
		TraceError("[USERSCRIPT] runtime error loading %s: %s", name.c_str(), err);
		SetScriptError(iScriptIdx, err);
		lua_pop(L, 1);
		m_scripts[iScriptIdx].bFaulted = true;
	}
	else
	{
		Tracef("[USERSCRIPT] loaded '%s'\n", name.c_str());
	}
}

// ---------------------------------------------------------------------------
// Protected-call harness (memory cap is always on via the allocator; here we
// add the wall-clock deadline + instruction hook).
// ---------------------------------------------------------------------------
void CUserScriptManager::ProtectedBegin()
{
	m_deadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(kCallBudgetMs);
	lua_sethook(m_pLuaState, CountHook, LUA_MASKCOUNT, kHookInstrCount);
}

void CUserScriptManager::ProtectedEnd()
{
	lua_sethook(m_pLuaState, nullptr, 0, 0);
}

void CUserScriptManager::FaultCurrentScript(const char* c_szWhere, const char* c_szError)
{
	if (m_iCurrentScript < 0 || m_iCurrentScript >= (int)m_scripts.size())
		return;
	SUserScript& sc = m_scripts[m_iCurrentScript];
	sc.iErrorCount++;
	SetScriptError(m_iCurrentScript, c_szError);
	TraceError("[USERSCRIPT] '%s' error in %s: %s",
		sc.strName.c_str(), c_szWhere, c_szError ? c_szError : "(nil)");
	if (sc.iErrorCount >= kFaultThreshold && !sc.bFaulted)
	{
		sc.bFaulted = true;
		TraceError("[USERSCRIPT] '%s' auto-disabled after %d errors",
			sc.strName.c_str(), sc.iErrorCount);
	}
}

// ---------------------------------------------------------------------------
// Per-frame pump
// ---------------------------------------------------------------------------
void CUserScriptManager::Update(double dGlobalTime, double dElapsed)
{
	if (!m_pLuaState || m_scripts.empty())
		return;

	m_dGlobalTime = dGlobalTime;

	DispatchEvent(USERSCRIPT_EVENT_UPDATE, dElapsed);

	if (dGlobalTime - m_dLastSecondPulse >= 1.0)
	{
		m_dLastSecondPulse = dGlobalTime;
		DispatchEvent(USERSCRIPT_EVENT_SECOND, 0.0);
	}

	FireTimers();

	// Persist any config.set() changes at most once per second (see ConfigSet).
	if (dGlobalTime - m_dConfigFlushStamp >= 1.0)
	{
		m_dConfigFlushStamp = dGlobalTime;
		FlushDirtyConfigs();
	}
}

void CUserScriptManager::DispatchEvent(int iEvent, double dArg)
{
	lua_State* L = m_pLuaState;
	// Iterate by index; the vector may be appended to during dispatch, but we
	// only need to cover hooks that existed at frame start.
	// NB: a handler may call event.on/timer.* and reallocate these vectors, so
	// we must NOT hold references across the pcall - capture primitives first,
	// then re-index by position afterwards.
	size_t count = m_hooks.size();
	for (size_t i = 0; i < count; ++i)
	{
		if (m_hooks[i].bDead || m_hooks[i].iEvent != iEvent)
			continue;
		int owner = m_hooks[i].iOwnerScript;
		int cbRef = m_hooks[i].iCallbackRef;
		if (!ScriptActive(owner))	// skips faulted AND disabled scripts
			continue;

		lua_rawgeti(L, LUA_REGISTRYINDEX, cbRef);	// [fn]
		if (iEvent == USERSCRIPT_EVENT_UPDATE)
			lua_pushnumber(L, dArg);

		m_iCurrentScript = owner;
		ProtectedBegin();
		int rc = lua_pcall(L, iEvent == USERSCRIPT_EVENT_UPDATE ? 1 : 0, 0, 0);
		ProtectedEnd();

		if (rc != LUA_OK)
		{
			FaultCurrentScript("event handler", lua_tostring(L, -1));
			lua_pop(L, 1);
		}
		m_iCurrentScript = -1;
	}
}

void CUserScriptManager::FireTimers()
{
	lua_State* L = m_pLuaState;
	// Same reallocation hazard as DispatchEvent: don't hold a reference across
	// the pcall; capture what we need, re-index afterwards.
	size_t count = m_timers.size();
	for (size_t i = 0; i < count; ++i)
	{
		if (m_timers[i].bDead)
			continue;
		int owner = m_timers[i].iOwnerScript;
		if (owner < 0 || owner >= (int)m_scripts.size())
			continue;
		if (m_scripts[owner].bFaulted) { m_timers[i].bDead = true; continue; }
		// Disabled (but not faulted): stay dormant, keep the timer so it resumes
		// when the script is re-enabled from the addon manager.
		if (!m_scripts[owner].bEnabled)
			continue;
		if (m_dGlobalTime < m_timers[i].dNextFireTime)
			continue;

		int cbRef = m_timers[i].iCallbackRef;
		lua_rawgeti(L, LUA_REGISTRYINDEX, cbRef);	// [fn]
		m_iCurrentScript = owner;
		ProtectedBegin();
		int rc = lua_pcall(L, 0, 0, 0);
		ProtectedEnd();

		bool killed = false;
		if (rc != LUA_OK)
		{
			FaultCurrentScript("timer", lua_tostring(L, -1));
			lua_pop(L, 1);
			killed = true;
		}
		m_iCurrentScript = -1;

		// Re-index: m_timers may have been reallocated by the callback.
		SUserScriptTimer& t = m_timers[i];
		if (!killed && t.bRepeat)
		{
			t.dNextFireTime = m_dGlobalTime + t.dInterval;
		}
		else
		{
			t.bDead = true;
			luaL_unref(L, LUA_REGISTRYINDEX, cbRef);
		}
	}
}

// ---------------------------------------------------------------------------
// Rendering (called after the UI so widgets draw on top)
// ---------------------------------------------------------------------------
void CUserScriptManager::Render()
{
	// Draw in ascending layer order so a script can place e.g. a background PANEL
	// behind its TEXT regardless of creation order. We sort a scratch index list
	// (NOT m_widgets itself - widget handles are indices into it and must stay
	// stable). std::stable_sort keeps creation order within the same layer.
	m_drawOrder.clear();
	m_drawOrder.reserve(m_widgets.size());
	for (int i = 0; i < (int)m_widgets.size(); ++i)
	{
		const SUserScriptWidget& w = m_widgets[i];
		if (w.bDead || !w.bVisible) continue;
		if (!ScriptActive(w.iOwnerScript)) continue;
		m_drawOrder.push_back(i);
	}
	std::stable_sort(m_drawOrder.begin(), m_drawOrder.end(),
		[this](int a, int b) { return m_widgets[a].iLayer < m_widgets[b].iLayer; });

	for (int idx : m_drawOrder)
	{
		SUserScriptWidget& w = m_widgets[idx];

		// Every 2D-primitive widget needs the graphics singleton; skip if not up.
		if ((w.iType == USERSCRIPT_WIDGET_RECT || w.iType == USERSCRIPT_WIDGET_BAR ||
			 w.iType == USERSCRIPT_WIDGET_LINE || w.iType == USERSCRIPT_WIDGET_PANEL)
			&& !CPythonGraphic::InstancePtr())
			continue;

		switch (w.iType)
		{
			case USERSCRIPT_WIDGET_LINE:
			{
				CPythonGraphic& g = CPythonGraphic::Instance();
				g.SetDiffuseColor(w.fR, w.fG, w.fB, w.fA);
				float th = w.fThickness;
				if (th <= 1.0f)
				{
					g.RenderLine2d(w.fX, w.fY, w.fX2, w.fY2);
				}
				else
				{
					// Approximate a thick line by stacking thin lines along the
					// minor axis (cheap; no new primitive needed).
					float dx = w.fX2 - w.fX, dy = w.fY2 - w.fY;
					bool horiz = (dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy);
					int n = (int)th; if (n > 16) n = 16;
					float start = -(float)(n - 1) * 0.5f;
					for (int k = 0; k < n; ++k)
					{
						float off = start + (float)k;
						if (horiz) g.RenderLine2d(w.fX, w.fY + off, w.fX2, w.fY2 + off);
						else       g.RenderLine2d(w.fX + off, w.fY, w.fX2 + off, w.fY2);
					}
				}
				break;
			}
			case USERSCRIPT_WIDGET_PANEL:
			{
				// Framed container: translucent fill (fBack*) + outline (fR/fG/fB/fA).
				CPythonGraphic& g = CPythonGraphic::Instance();
				if (w.fBackA > 0.0f)
				{
					g.SetDiffuseColor(w.fBackR, w.fBackG, w.fBackB, w.fBackA);
					g.RenderBar2d(w.fX, w.fY, w.fX + w.fW, w.fY + w.fH);
				}
				if (w.fA > 0.0f)
				{
					g.SetDiffuseColor(w.fR, w.fG, w.fB, w.fA);
					g.RenderBox2d(w.fX, w.fY, w.fX + w.fW, w.fY + w.fH);
				}
				break;
			}
			case USERSCRIPT_WIDGET_RECT:
			{
				// Solid colour panel drawn from the 2D Grp primitive.
				CPythonGraphic& g = CPythonGraphic::Instance();
				g.SetDiffuseColor(w.fR, w.fG, w.fB, w.fA);
				g.RenderBar2d(w.fX, w.fY, w.fX + w.fW, w.fY + w.fH);
				break;
			}
			case USERSCRIPT_WIDGET_BAR:
			{
				// Background then foreground scaled by the fill fraction.
				CPythonGraphic& g = CPythonGraphic::Instance();
				float f = w.fProgress;
				if (f < 0.0f) f = 0.0f;
				if (f > 1.0f) f = 1.0f;
				if (w.fBackA > 0.0f)
				{
					g.SetDiffuseColor(w.fBackR, w.fBackG, w.fBackB, w.fBackA);
					g.RenderBar2d(w.fX, w.fY, w.fX + w.fW, w.fY + w.fH);
				}
				if (f > 0.0f)
				{
					g.SetDiffuseColor(w.fR, w.fG, w.fB, w.fA);
					g.RenderBar2d(w.fX, w.fY, w.fX + (w.fW * f), w.fY + w.fH);
				}
				break;
			}
			case USERSCRIPT_WIDGET_ICON:
			{
				// Client-internal item/curated icon. Skip until an icon has been
				// assigned (empty instance) so nothing spurious draws.
				if (!w.pImage || w.pImage->IsEmpty())
					continue;
				w.pImage->SetPosition(w.fX, w.fY);
				// fR/fG/fB/fA double as an optional tint (default white/opaque).
				w.pImage->SetDiffuseColor(w.fR, w.fG, w.fB, w.fA);
				w.pImage->Render();
				break;
			}
			case USERSCRIPT_WIDGET_TEXT:
			default:
			{
				if (!w.pInstance)
					continue;
				w.pInstance->SetPosition(w.fX, w.fY);
				w.pInstance->Update();
				w.pInstance->Render();
				break;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// API callbacks (invoked from UserScriptApi.cpp bindings)
// ---------------------------------------------------------------------------
int CUserScriptManager::ApiRegisterHook(int iEvent, int iCallbackRef)
{
	if (m_iCurrentScript < 0) return -1;
	if ((int)m_hooks.size() >= kMaxHooksTotal) return -1;
	SUserScriptHook h;
	h.iOwnerScript = m_iCurrentScript;
	h.iEvent = iEvent;
	h.iCallbackRef = iCallbackRef;
	m_hooks.push_back(h);
	return (int)m_hooks.size() - 1;
}

int CUserScriptManager::ApiAddTimer(double dDelay, int iCallbackRef, bool bRepeat)
{
	if (m_iCurrentScript < 0) return -1;
	if ((int)m_timers.size() >= kMaxTimersTotal) return -1;
	SUserScriptTimer t;
	t.iOwnerScript = m_iCurrentScript;
	t.iCallbackRef = iCallbackRef;
	t.dInterval = dDelay;
	t.bRepeat = bRepeat;
	t.dNextFireTime = m_dGlobalTime + dDelay;
	m_timers.push_back(t);
	return (int)m_timers.size() - 1;
}

int CUserScriptManager::ApiCreateWidget(int iType)
{
	if (m_iCurrentScript < 0) return -1;
	if ((int)m_widgets.size() >= kMaxWidgetsTotal) return -1;

	// Per-script widget cap.
	int owned = 0;
	for (const SUserScriptWidget& w : m_widgets)
		if (!w.bDead && w.iOwnerScript == m_iCurrentScript) ++owned;
	if (owned >= kMaxWidgetsScript) return -1;

	SUserScriptWidget w;
	w.iOwnerScript = m_iCurrentScript;
	w.iType = iType;

	// Only TEXT widgets need a font resource; RECT/BAR are pure 2D primitives.
	if (iType == USERSCRIPT_WIDGET_TEXT)
	{
		CGraphicText* pFont = static_cast<CGraphicText*>(DefaultFont_GetResource());
		if (!pFont)
			return -2;	// font/resources not ready yet: transient, script may retry
		w.pInstance = new CGraphicTextInstance();
		w.pInstance->SetTextPointer(pFont);
		w.pInstance->SetValue("");
	}
	else if (iType == USERSCRIPT_WIDGET_ICON)
	{
		// Pooled image instance (same lifecycle the client uses for CImageBox).
		w.pImage = CGraphicImageInstance::New();
		if (!w.pImage)
			return -2;	// pool not ready yet: transient, script may retry
	}

	m_widgets.push_back(w);
	return (int)m_widgets.size() - 1;
}

SUserScriptWidget* CUserScriptManager::GetWidget(int iId)
{
	if (iId < 0 || iId >= (int)m_widgets.size()) return nullptr;
	SUserScriptWidget& w = m_widgets[iId];
	return w.bDead ? nullptr : &w;
}

bool CUserScriptManager::WidgetOwnedByCurrent(int iId)
{
	SUserScriptWidget* w = GetWidget(iId);
	return w && w->iOwnerScript == m_iCurrentScript;
}

// ELEMENTIA-USERSCRIPT: point an ICON widget at a client-internal item icon.
// The ONLY input from the script is an integer vnum; the icon image is looked up
// through the client's own item table (CItemManager -> CItemData::GetIconImage),
// exactly like the inventory UI does. There is no path, so nothing outside the
// client's item packs can ever be addressed. vnum 0 clears the icon.
bool CUserScriptManager::ApiSetWidgetIconVnum(SUserScriptWidget* pWidget, unsigned int dwVnum)
{
	if (!pWidget || pWidget->iType != USERSCRIPT_WIDGET_ICON || !pWidget->pImage)
		return false;

	if (dwVnum == 0)
	{
		pWidget->pImage->SetImagePointer(nullptr);	// clear -> renders as empty
		return true;
	}

	CItemManager* pItemMgr = CItemManager::InstancePtr();
	if (!pItemMgr)
		return false;
	CItemData* pItemData = nullptr;
	if (!pItemMgr->GetItemDataPointer(dwVnum, &pItemData) || !pItemData)
		return false;
	CGraphicSubImage* pIcon = pItemData->GetIconImage();
	if (!pIcon)
		return false;

	// CGraphicSubImage IS-A CGraphicImage; this is the same call the item UI uses.
	pWidget->pImage->SetImagePointer(pIcon);
	return true;
}

// ELEMENTIA-USERSCRIPT: point an ICON widget at a CURATED, whitelisted texture.
// The script supplies only a short leaf key ([a-z0-9_], <=32). We compose a FIXED
// internal path (kIconKeyPrefix + key + kIconKeySuffix) and load it through the
// client resource manager (the eterpack VFS). Any character outside the strict
// class - including '/', '\\', '.', ':' - is rejected, so no directory, no
// traversal and no absolute/UNC path can be injected. A missing curated file
// fails cleanly (returns false); it can never reach outside the curated folder.
bool CUserScriptManager::ApiSetWidgetIconKey(SUserScriptWidget* pWidget, const char* c_szKey)
{
	if (!pWidget || pWidget->iType != USERSCRIPT_WIDGET_ICON || !pWidget->pImage)
		return false;
	if (!c_szKey)
		return false;

	// Strict leaf validation (defence-in-depth: reject BEFORE building any path).
	size_t len = strlen(c_szKey);
	if (len == 0 || len > kIconKeyMaxLen)
		return false;
	for (size_t i = 0; i < len; ++i)
	{
		char c = c_szKey[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
		if (!ok)
			return false;	// anything else (/, \, ., :, .., spaces) is rejected
	}

	std::string path = kIconKeyPrefix;
	path += c_szKey;
	path += kIconKeySuffix;

	// Must exist inside the packs and actually be an image resource.
	if (!CResourceManager::Instance().IsFileExist(path.c_str()))
		return false;
	CResource* pResource = CResourceManager::Instance().GetResourcePointer(path.c_str());
	if (!pResource || !pResource->IsType(CGraphicImage::Type()))
		return false;

	pWidget->pImage->SetImagePointer(static_cast<CGraphicImage*>(pResource));
	return true;
}

void CUserScriptManager::ApiLog(const char* c_szText, bool bWarn)
{
	if (!c_szText) return;

	// Per-script rate limit so a runaway loop can't spam the chat/log.
	if (m_iCurrentScript >= 0 && m_iCurrentScript < (int)m_scripts.size())
	{
		SUserScript& sc = m_scripts[m_iCurrentScript];
		if (m_dGlobalTime - sc.dLogWindowStart >= 1.0)
		{
			sc.dLogWindowStart = m_dGlobalTime;
			sc.iLogBudget = kLogLinesPerSec;
		}
		if (sc.iLogBudget <= 0)
			return;
		sc.iLogBudget--;
	}

	const char* scriptName =
		(m_iCurrentScript >= 0 && m_iCurrentScript < (int)m_scripts.size())
		? m_scripts[m_iCurrentScript].strName.c_str() : "?";

	char line[1024];
	_snprintf(line, sizeof(line), "[addon:%s] %s", scriptName, c_szText);
	line[sizeof(line) - 1] = '\0';

	if (bWarn) TraceError("%s", line);
	else       Tracef("%s\n", line);

	// Mirror into the in-game chat window (INFO channel) when it exists.
	if (CPythonChat::InstancePtr())
		CPythonChat::Instance().AppendChat(CHAT_TYPE_INFO, line);
}

// ---------------------------------------------------------------------------
// Script activation / addon-manager surface
// ---------------------------------------------------------------------------
bool CUserScriptManager::ScriptActive(int iIdx) const
{
	if (iIdx < 0 || iIdx >= (int)m_scripts.size())
		return false;
	const SUserScript& sc = m_scripts[iIdx];
	return sc.bEnabled && !sc.bFaulted;
}

const char* CUserScriptManager::ScriptName(int iIdx) const
{
	if (iIdx < 0 || iIdx >= (int)m_scripts.size()) return "";
	return m_scripts[iIdx].strName.c_str();
}
bool CUserScriptManager::ScriptEnabled(int iIdx) const
{
	if (iIdx < 0 || iIdx >= (int)m_scripts.size()) return false;
	return m_scripts[iIdx].bEnabled;
}
bool CUserScriptManager::ScriptFaulted(int iIdx) const
{
	if (iIdx < 0 || iIdx >= (int)m_scripts.size()) return false;
	return m_scripts[iIdx].bFaulted;
}
int CUserScriptManager::ScriptErrorCount(int iIdx) const
{
	if (iIdx < 0 || iIdx >= (int)m_scripts.size()) return 0;
	return m_scripts[iIdx].iErrorCount;
}
const char* CUserScriptManager::ScriptError(int iIdx) const
{
	if (iIdx < 0 || iIdx >= (int)m_scripts.size()) return "";
	return m_scripts[iIdx].strLastError.c_str();
}

void CUserScriptManager::SetScriptEnabled(int iIdx, bool bEnabled)
{
	if (iIdx < 0 || iIdx >= (int)m_scripts.size())
		return;
	// Flush pending config now so a script being disabled keeps its saved state.
	FlushDirtyConfigs();
	SUserScript& sc = m_scripts[iIdx];
	sc.bEnabled = bEnabled;

	// Maintain the persisted disable set (by script name).
	auto it = std::find(m_disabledNames.begin(), m_disabledNames.end(), sc.strName);
	if (bEnabled)
	{
		if (it != m_disabledNames.end())
			m_disabledNames.erase(it);
	}
	else
	{
		if (it == m_disabledNames.end())
			m_disabledNames.push_back(sc.strName);
	}
	SaveDisabledSet();
}

// ---------------------------------------------------------------------------
// Reload: tear down every loaded script (freeing all Lua registry refs and
// widget instances) but keep the same Lua state, then load the directory again.
// ---------------------------------------------------------------------------
void CUserScriptManager::ClearAllScripts()
{
	lua_State* L = m_pLuaState;

	if (L)
	{
		for (SUserScriptHook& h : m_hooks)
			if (!h.bDead) luaL_unref(L, LUA_REGISTRYINDEX, h.iCallbackRef);
		for (SUserScriptTimer& t : m_timers)
			if (!t.bDead) luaL_unref(L, LUA_REGISTRYINDEX, t.iCallbackRef);
		for (SUserScript& sc : m_scripts)
			if (sc.iEnvRef) luaL_unref(L, LUA_REGISTRYINDEX, sc.iEnvRef);
	}

	for (SUserScriptWidget& w : m_widgets)
	{
		if (w.pInstance) { delete w.pInstance; w.pInstance = nullptr; }
		if (w.pImage) { CGraphicImageInstance::Delete(w.pImage); w.pImage = nullptr; }
	}

	m_widgets.clear();
	m_hooks.clear();
	m_timers.clear();
	m_scripts.clear();
	m_iCurrentScript = -1;
	m_nearby.clear();
	m_dNearbyStamp = -1.0;
}

void CUserScriptManager::ReloadScripts()
{
	if (!m_pLuaState || m_strBaseDir.empty())
		return;
	// Persist pending config before discarding the current script state.
	FlushDirtyConfigs();
	ClearAllScripts();
	LoadDisabledSet();
	LoadDirectory(m_strBaseDir, true);
	Tracef("[USERSCRIPT] reloaded: %zu script(s)\n", m_scripts.size());
}

// Free every Lua registry ref and native widget owned by ONE script, without
// touching any other script's records or widget handles (handles are indices
// into m_widgets, so entries are marked dead in place, never erased/reordered).
void CUserScriptManager::FreeScriptResources(int iScriptIdx)
{
	lua_State* L = m_pLuaState;
	if (!L || iScriptIdx < 0 || iScriptIdx >= (int)m_scripts.size())
		return;

	for (SUserScriptHook& h : m_hooks)
		if (!h.bDead && h.iOwnerScript == iScriptIdx)
		{
			luaL_unref(L, LUA_REGISTRYINDEX, h.iCallbackRef);
			h.bDead = true;
		}
	for (SUserScriptTimer& t : m_timers)
		if (!t.bDead && t.iOwnerScript == iScriptIdx)
		{
			luaL_unref(L, LUA_REGISTRYINDEX, t.iCallbackRef);
			t.bDead = true;
		}
	for (SUserScriptWidget& w : m_widgets)
		if (!w.bDead && w.iOwnerScript == iScriptIdx)
		{
			if (w.pInstance) { delete w.pInstance; w.pInstance = nullptr; }
			if (w.pImage) { CGraphicImageInstance::Delete(w.pImage); w.pImage = nullptr; }
			w.bDead = true;
		}
	if (m_scripts[iScriptIdx].iEnvRef)
	{
		luaL_unref(L, LUA_REGISTRYINDEX, m_scripts[iScriptIdx].iEnvRef);
		m_scripts[iScriptIdx].iEnvRef = 0;
	}
}

// Reload a SINGLE script in place: free its resources, reset its record (keeping
// its index/identity and persisted enable state), and re-run its file. Other
// scripts and their widget handles are untouched.
bool CUserScriptManager::ReloadScript(int iIdx)
{
	if (!m_pLuaState || iIdx < 0 || iIdx >= (int)m_scripts.size())
		return false;

	// Persist this script's pending config so a reload does not lose saved state.
	FlushDirtyConfigs();
	FreeScriptResources(iIdx);

	SUserScript& sc = m_scripts[iIdx];
	// Re-evaluate the persisted enable/disable set (it may have changed on disk).
	sc.bEnabled = (std::find(m_disabledNames.begin(), m_disabledNames.end(), sc.strName)
					== m_disabledNames.end());
	sc.bFaulted     = false;
	sc.iErrorCount  = 0;
	sc.iLogBudget   = 0;
	sc.dLogWindowStart = 0.0;
	sc.strLastError.clear();
	sc.configCache.clear();		// drop the in-memory cache; reloaded lazily
	sc.bConfigLoaded = false;
	sc.bConfigDirty  = false;

	LoadScriptChunk(iIdx);
	Tracef("[USERSCRIPT] single-reloaded '%s'\n", sc.strName.c_str());
	return true;
}

// ---------------------------------------------------------------------------
// Enable/disable persistence (userscripts/config/_disabled.dat)
// ---------------------------------------------------------------------------
std::string CUserScriptManager::ConfigDir() const
{
	if (m_strBaseDir.empty()) return std::string();
	return (std::filesystem::path(m_strBaseDir) / "config").string();
}

void CUserScriptManager::LoadDisabledSet()
{
	m_disabledNames.clear();
	std::string dir = ConfigDir();
	if (dir.empty()) return;
	std::ifstream f((std::filesystem::path(dir) / "_disabled.dat").string());
	if (!f) return;
	std::string name;
	while (std::getline(f, name))
	{
		// strip trailing CR / whitespace
		while (!name.empty() && (name.back() == '\r' || name.back() == '\n' ||
			name.back() == ' ' || name.back() == '\t'))
			name.pop_back();
		if (!name.empty())
			m_disabledNames.push_back(name);
	}
}

void CUserScriptManager::SaveDisabledSet()
{
	std::string dir = ConfigDir();
	if (dir.empty()) return;
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	std::ofstream f((std::filesystem::path(dir) / "_disabled.dat").string(),
		std::ios::binary | std::ios::trunc);
	if (!f) return;
	for (const std::string& n : m_disabledNames)
		f << n << "\n";
}

// ---------------------------------------------------------------------------
// Read-only target HP% cache
// ---------------------------------------------------------------------------
void CUserScriptManager::NotifyTargetHP(unsigned int dwVID, int iPercent)
{
	if (iPercent < 0)   iPercent = 0;
	if (iPercent > 100) iPercent = 100;
	m_dwTargetHPCacheVID = dwVID;
	m_iTargetHPCachePct  = iPercent;
}

int CUserScriptManager::TargetHPPercent(unsigned int dwCurrentVID) const
{
	if (dwCurrentVID != 0 && dwCurrentVID == m_dwTargetHPCacheVID)
		return m_iTargetHPCachePct;
	return -1;
}

// ---------------------------------------------------------------------------
// Throttled nearby-characters snapshot
// ---------------------------------------------------------------------------
void CUserScriptManager::RefreshNearbySnapshot()
{
	m_nearby.clear();

	CPythonCharacterManager* pcm = CPythonCharacterManager::InstancePtr();
	if (!pcm)
		return;
	CInstanceBase* pMain = pcm->GetMainInstancePtr();
	if (!pMain)
		return;

	CPythonCharacterManager::CharacterIterator it  = pcm->CharacterInstanceBegin();
	CPythonCharacterManager::CharacterIterator end = pcm->CharacterInstanceEnd();
	for (; it != end; ++it)
	{
		if (m_nearby.size() >= kNearbyMaxEntries)
			break;
		CInstanceBase* pInst = *it;
		if (!pInst || pInst == pMain)
			continue;
		if (pInst->IsDead())
			continue;

		SUserScriptNearby e;
		const char* c_szName = pInst->GetNameString();
		e.strName   = c_szName ? c_szName : "";
		e.fDistance = pMain->GetDistance(pInst);
		if (pInst->IsStone())      e.iKind = USERSCRIPT_NEARBY_STONE;
		else if (pInst->IsPC())    e.iKind = USERSCRIPT_NEARBY_PC;
		else if (pInst->IsNPC())   e.iKind = USERSCRIPT_NEARBY_NPC;
		else                       e.iKind = USERSCRIPT_NEARBY_MONSTER;
		m_nearby.push_back(std::move(e));
	}
}

const std::vector<SUserScriptNearby>& CUserScriptManager::NearbySnapshot()
{
	// Rebuild at most every kNearbyThrottleSec regardless of how often (or from
	// how many scripts) nearby.* is called, so a per-frame caller stays cheap.
	if (m_dNearbyStamp < 0.0 || (m_dGlobalTime - m_dNearbyStamp) >= kNearbyThrottleSec)
	{
		RefreshNearbySnapshot();
		m_dNearbyStamp = m_dGlobalTime;
	}
	return m_nearby;
}

// ---------------------------------------------------------------------------
// Sandboxed config.* storage
//
// Path is FIXED from the (sanitised) script name - a script cannot choose it.
// Values are opaque, size-capped, type-tagged strings (never Lua code). Files
// are plain text with length-prefixed entries so any byte content round-trips.
// ---------------------------------------------------------------------------
std::string CUserScriptManager::SanitizeName(const std::string& strName) const
{
	std::string out;
	out.reserve(strName.size());
	for (char c : strName)
	{
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '_' || c == '-')
			out.push_back(c);
		else
			out.push_back('_');
	}
	if (out.empty())
		out = "unnamed";
	if (out.size() > 64)
		out.resize(64);
	return out;
}

bool CUserScriptManager::ConfigLoadFor(int iScriptIdx)
{
	if (iScriptIdx < 0 || iScriptIdx >= (int)m_scripts.size())
		return false;
	SUserScript& sc = m_scripts[iScriptIdx];
	if (sc.bConfigLoaded)
		return true;
	sc.bConfigLoaded = true;			// mark loaded even if the file is absent

	std::string dir = ConfigDir();
	if (dir.empty())
		return true;
	std::filesystem::path path =
		std::filesystem::path(dir) / (SanitizeName(sc.strName) + ".dat");

	std::ifstream f(path.string(), std::ios::binary);
	if (!f)
		return true;					// no file yet: empty config, that is fine

	std::stringstream ss;
	ss << f.rdbuf();
	const std::string buf = ss.str();
	if (buf.size() > kConfigMaxFile)
		return true;					// corrupt/oversized: ignore, start empty

	// Parse length-prefixed entries: "<klen>\n<key><vlen>\n<value>" repeated.
	size_t pos = 0;
	auto readCount = [&](size_t& outN) -> bool
	{
		size_t nl = buf.find('\n', pos);
		if (nl == std::string::npos) return false;
		outN = 0;
		for (size_t i = pos; i < nl; ++i)
		{
			char c = buf[i];
			if (c < '0' || c > '9') return false;
			outN = outN * 10 + (size_t)(c - '0');
			if (outN > kConfigMaxFile) return false;	// guard runaway
		}
		pos = nl + 1;
		return true;
	};
	while (pos < buf.size() && sc.configCache.size() < kConfigMaxKeys)
	{
		size_t klen = 0, vlen = 0;
		if (!readCount(klen)) break;
		if (pos + klen > buf.size()) break;
		std::string key = buf.substr(pos, klen);
		pos += klen;
		if (!readCount(vlen)) break;
		if (pos + vlen > buf.size()) break;
		std::string val = buf.substr(pos, vlen);
		pos += vlen;
		if (klen == 0 || klen > kConfigMaxKeyLen || vlen > kConfigMaxValLen)
			continue;					// skip malformed/oversized entry
		sc.configCache[key] = val;
	}
	return true;
}

bool CUserScriptManager::ConfigSaveFor(int iScriptIdx)
{
	if (iScriptIdx < 0 || iScriptIdx >= (int)m_scripts.size())
		return false;
	SUserScript& sc = m_scripts[iScriptIdx];

	std::string dir = ConfigDir();
	if (dir.empty())
		return false;

	// Serialise first so we can enforce the total-size cap before writing.
	std::string out;
	for (const auto& kv : sc.configCache)
	{
		char num[32];
		int n = _snprintf(num, sizeof(num), "%zu\n", kv.first.size());
		if (n > 0) out.append(num, (size_t)n);
		out.append(kv.first);
		n = _snprintf(num, sizeof(num), "%zu\n", kv.second.size());
		if (n > 0) out.append(num, (size_t)n);
		out.append(kv.second);
		if (out.size() > kConfigMaxFile)
			return false;
	}

	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	std::filesystem::path path =
		std::filesystem::path(dir) / (SanitizeName(sc.strName) + ".dat");
	std::ofstream f(path.string(), std::ios::binary | std::ios::trunc);
	if (!f)
		return false;
	f.write(out.data(), (std::streamsize)out.size());
	return (bool)f;
}

bool CUserScriptManager::ConfigGet(const char* c_szKey, std::string& strOut)
{
	if (!c_szKey || m_iCurrentScript < 0)
		return false;
	if (!ConfigLoadFor(m_iCurrentScript))
		return false;
	SUserScript& sc = m_scripts[m_iCurrentScript];
	auto it = sc.configCache.find(c_szKey);
	if (it == sc.configCache.end())
		return false;
	strOut = it->second;
	return true;
}

bool CUserScriptManager::ConfigSet(const char* c_szKey, const std::string& strEncoded)
{
	if (!c_szKey || m_iCurrentScript < 0)
		return false;
	std::string key(c_szKey);
	if (key.empty() || key.size() > kConfigMaxKeyLen)
		return false;
	if (strEncoded.size() > kConfigMaxValLen)
		return false;
	if (!ConfigLoadFor(m_iCurrentScript))
		return false;
	SUserScript& sc = m_scripts[m_iCurrentScript];

	if (strEncoded.empty())
	{
		// Empty encoding == delete (a real value always carries a type tag).
		sc.configCache.erase(key);
	}
	else
	{
		// Enforce the key-count cap only when adding a brand-new key.
		if (sc.configCache.find(key) == sc.configCache.end() &&
			sc.configCache.size() >= kConfigMaxKeys)
			return false;
		sc.configCache[key] = strEncoded;
	}

	// Do NOT write to disk here. A misbehaving script could call config.set()
	// every frame; re-serialising + truncating a up-to-64KB file ~60x/sec on the
	// render thread would hitch the client and thrash the disk. Instead just mark
	// the cache dirty - FlushDirtyConfigs() writes it out at most once per second
	// (and eagerly on disable/reload/shutdown so nothing is lost).
	sc.bConfigDirty = true;
	return true;
}

// Write out every dirty config cache. Called on a throttle from Update() and
// eagerly before any teardown/disable/reload so no committed value is lost.
void CUserScriptManager::FlushDirtyConfigs()
{
	for (int i = 0; i < (int)m_scripts.size(); ++i)
	{
		if (!m_scripts[i].bConfigDirty)
			continue;
		// Clear the flag first: even if the write fails we don't want to spin
		// retrying every second; the in-memory cache stays authoritative.
		m_scripts[i].bConfigDirty = false;
		ConfigSaveFor(i);
	}
}
