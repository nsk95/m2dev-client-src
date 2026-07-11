#pragma once
// ELEMENTIA-USERSCRIPT
// Internal glue between the sandbox manager and the curated Lua API surface.
// Not exposed to Lua; only the C++ side includes this.

struct lua_State;

// Opens ONLY the memory-safe subset of the Lua standard library into the given
// state's real globals (base, string, table, math, coroutine, utf8). The
// dangerous libraries (os, io, package/require, debug) are deliberately NEVER
// opened. Call once, right after the state is created.
void UserScript_OpenSafeLibs(lua_State* L);

// Pushes a brand-new, per-script sandbox environment table onto the stack.
// The table contains a curated whitelist copied from the safe libs plus the
// ELEMENTIA API (log/player/event/timer/ui). Each script gets its OWN copy so
// scripts cannot clobber one another (or internal state) through shared tables.
// Returns with exactly one new value (the env table) on the stack.
void UserScript_PushSandboxEnv(lua_State* L);
