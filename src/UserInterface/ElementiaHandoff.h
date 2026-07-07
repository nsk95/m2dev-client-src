#pragma once
//
// ELEMENTIA-HANDOFF — exe-side intake of the Electron client's session handoff.
//
// The Electron client performs login (+ char-select), mints/holds the u32 login
// key, then hands { name, key, host, port, slot } to this exe over a one-shot
// loopback named pipe. This module reads that handoff at boot; the parsed result
// is later fed into CPythonNetworkStream::DirectEnterFromHandoff() to reuse the
// existing DirectEnter / auto-select path.
//
// Wire format (mirrors src/net/handoff.js / tools/mock-exe-receiver.js):
//   exe  -> pipe:  "AUTH <nonce>\n"
//   pipe -> exe:   [u32 LE length][json bytes]   then the pipe closes
//                  json = { "name":..., "key":<u32>, "host":..., "port":<u16>, "slot":<u8> }
//
// Launch argv (from Electron): --handoff-pipe=<pipeName> --handoff-nonce=<hex>
//
// NOTE: include this header only in translation units that already include the
// project StdAfx.h first (it relies on the Windows typedefs DWORD/WORD/BYTE).

#include <string>

struct SElementiaHandoff
{
	std::string	name;	// character name
	DWORD		key;	// u32 login key minted by the Electron auth leg
	std::string	host;	// game server host (from the char-list lAddr)
	WORD		port;	// game server port (from the char-list wPort)
	BYTE		slot;	// character slot to auto-select

	SElementiaHandoff() : key(0), port(0), slot(0) {}
};

// Parse --handoff-pipe/--handoff-nonce from the raw command line; if both are
// present, open the loopback pipe, authenticate with the nonce and read the JSON
// payload. Stores the result for later retrieval via the accessors below.
// Returns true iff a valid handoff was consumed. With no handoff args this is a
// cheap no-op that returns false, leaving the normal login flow untouched.
bool Elementia_InitHandoffFromCommandLine(const char* lpCmdLine);

// Was a valid handoff read at boot?
bool Elementia_IsHandoffPending();

// The parsed handoff (only meaningful when Elementia_IsHandoffPending() is true).
const SElementiaHandoff& Elementia_GetHandoff();

// Lower-level building blocks (exposed for testing / reuse).
bool Elementia_ParseHandoffArgs(const char* lpCmdLine, std::string& pipeName, std::string& nonce);
bool Elementia_ReadHandoffPipe(const std::string& pipeName, const std::string& nonce, SElementiaHandoff& out);
bool Elementia_ParseHandoffJson(const std::string& json, SElementiaHandoff& out);
