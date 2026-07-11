# ELEMENTIA-USERSCRIPT: vendored Lua 5.4.7 for the *client-side* userscript sandbox.
#
# This is DELIBERATELY separate from the server-side quest Lua (which is an
# ancient Lua 5.0.3 living in the server tree). The client embeds its own,
# modern, well-audited Lua 5.4.7 interpreter purely to run sandboxed player
# addons ("userscripts"). We compile the pristine upstream sources so the
# vendored copy stays trivially patchable/upgradable; the *sandboxing* is done
# in C++ at state-creation time (see src/UserInterface/UserScript*), NOT by
# hacking the Lua sources here.
#
# Note: the standalone interpreter (lua.c) and compiler (luac.c) mains were
# removed from src/ on vendoring - we only want the library.

project(lua LANGUAGES C)

file(GLOB LUA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
list(SORT LUA_SOURCES)

add_library(lua STATIC ${LUA_SOURCES})

target_include_directories(lua
	PUBLIC
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# LUA_USE_C89 is intentionally NOT set: on MSVC/Windows the default luaconf.h
# path is correct. We define nothing platform-specific here.
set_target_properties(lua PROPERTIES
	OUTPUT_NAME "lua54"
	POSITION_INDEPENDENT_CODE ON
	FOLDER vendor
)

# Lua is C; keep it isolated from the project-wide C++/Unicode definitions.
