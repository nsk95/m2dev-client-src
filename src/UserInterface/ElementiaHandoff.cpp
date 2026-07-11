#include "StdAfx.h"
#include "ElementiaHandoff.h"

#include <cctype>
#include <cstdlib>

// ELEMENTIA-HANDOFF — see ElementiaHandoff.h for the protocol overview.

namespace
{
	bool				g_bHandoffPending = false;
	SElementiaHandoff	g_kHandoff;

	// Extract the VALUE from `--flag=VALUE` in a raw command line. Handoff values
	// are pipe names / hex nonces, so VALUE runs to the next whitespace or quote.
	std::string ExtractFlagValue(const std::string& cmd, const std::string& flag)
	{
		const std::string needle = flag + "=";
		size_t p = cmd.find(needle);
		if (p == std::string::npos)
			return std::string();
		p += needle.size();
		size_t e = p;
		while (e < cmd.size() && !isspace((unsigned char)cmd[e]) && cmd[e] != '"')
			++e;
		return cmd.substr(p, e - p);
	}

	void SkipWs(const std::string& s, size_t& i)
	{
		while (i < s.size() && isspace((unsigned char)s[i]))
			++i;
	}

	// Locate the position just after `"key" :` (whitespace-tolerant, flat JSON).
	bool FindKey(const std::string& json, const char* key, size_t& pos)
	{
		const std::string quoted = std::string("\"") + key + "\"";
		size_t k = json.find(quoted);
		if (k == std::string::npos)
			return false;
		size_t i = k + quoted.size();
		SkipWs(json, i);
		if (i >= json.size() || json[i] != ':')
			return false;
		++i;
		SkipWs(json, i);
		pos = i;
		return true;
	}

	bool ExtractString(const std::string& json, const char* key, std::string& out)
	{
		size_t i;
		if (!FindKey(json, key, i))
			return false;
		if (i >= json.size() || json[i] != '"')
			return false;
		++i;
		std::string v;
		while (i < json.size() && json[i] != '"')
		{
			if (json[i] == '\\' && i + 1 < json.size())
			{
				++i;
				const char c = json[i];
				switch (c)
				{
					case 'n': v += '\n'; break;
					case 't': v += '\t'; break;
					case 'r': v += '\r'; break;
					default:  v += c;    break;	// \" \\ \/ and others -> literal
				}
			}
			else
			{
				v += json[i];
			}
			++i;
		}
		out = v;
		return true;
	}

	bool ExtractUInt(const std::string& json, const char* key, unsigned long& out)
	{
		size_t i;
		if (!FindKey(json, key, i))
			return false;
		if (i < json.size() && json[i] == '"')	// defensively accept quoted numbers
			++i;
		const size_t start = i;
		while (i < json.size() && isdigit((unsigned char)json[i]))
			++i;
		if (i == start)
			return false;
		out = strtoul(json.substr(start, i - start).c_str(), NULL, 10);
		return true;
	}
}

bool Elementia_ParseHandoffJson(const std::string& json, SElementiaHandoff& out)
{
	std::string name, host;
	unsigned long key = 0, port = 0, slot = 0;

	if (!ExtractString(json, "name", name))	return false;
	if (!ExtractUInt(json, "key", key))		return false;
	if (!ExtractString(json, "host", host))	return false;
	if (!ExtractUInt(json, "port", port))	return false;
	if (!ExtractUInt(json, "slot", slot))	return false;

	out.name = name;
	out.key  = (DWORD)key;
	out.host = host;
	out.port = (WORD)port;
	out.slot = (BYTE)slot;
	return true;
}

bool Elementia_ParseHandoffArgs(const char* lpCmdLine, std::string& pipeName, std::string& nonce)
{
	if (!lpCmdLine)
		return false;
	const std::string cmd(lpCmdLine);
	pipeName = ExtractFlagValue(cmd, "--handoff-pipe");
	nonce    = ExtractFlagValue(cmd, "--handoff-nonce");
	return !pipeName.empty() && !nonce.empty();
}

bool Elementia_ReadHandoffPipe(const std::string& pipeName, const std::string& nonce, SElementiaHandoff& out)
{
	const std::string path = "\\\\.\\pipe\\" + pipeName;

	HANDLE h = INVALID_HANDLE_VALUE;
	// The Electron pipe server may not be listening the instant we launch; retry
	// briefly (~5s worst case) before giving up.
	for (int attempt = 0; attempt < 50; ++attempt)
	{
		h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
						0, NULL, OPEN_EXISTING, 0, NULL);
		if (h != INVALID_HANDLE_VALUE)
			break;

		if (GetLastError() == ERROR_PIPE_BUSY)
			WaitNamedPipeA(path.c_str(), 200);
		else
			Sleep(100);
	}
	if (h == INVALID_HANDLE_VALUE)
		return false;

	bool ok = false;
	do
	{
		const std::string auth = "AUTH " + nonce + "\n";
		DWORD written = 0;
		if (!WriteFile(h, auth.data(), (DWORD)auth.size(), &written, NULL) || written != auth.size())
			break;

		// Read the 4-byte little-endian length prefix (may arrive in pieces).
		unsigned char lenbuf[4];
		DWORD got = 0;
		while (got < 4)
		{
			DWORD r = 0;
			if (!ReadFile(h, lenbuf + got, 4 - got, &r, NULL) || r == 0)
				break;
			got += r;
		}
		if (got != 4)
			break;

		const DWORD len = (DWORD)lenbuf[0]        | ((DWORD)lenbuf[1] << 8) |
						 ((DWORD)lenbuf[2] << 16) | ((DWORD)lenbuf[3] << 24);
		if (len == 0 || len > (1u << 20))	// sanity cap: 1 MB
			break;

		std::string json;
		json.resize(len);
		DWORD total = 0;
		while (total < len)
		{
			DWORD r = 0;
			if (!ReadFile(h, &json[0] + total, len - total, &r, NULL) || r == 0)
				break;
			total += r;
		}
		if (total != len)
			break;

		ok = Elementia_ParseHandoffJson(json, out);
	}
	while (false);

	CloseHandle(h);
	return ok;
}

bool Elementia_InitHandoffFromCommandLine(const char* lpCmdLine)
{
	std::string pipeName, nonce;
	if (!Elementia_ParseHandoffArgs(lpCmdLine, pipeName, nonce))
		return false;	// no handoff args -> normal launch, untouched

	SElementiaHandoff ho;
	if (!Elementia_ReadHandoffPipe(pipeName, nonce, ho))
	{
		TraceError("[ELEMENTIA-HANDOFF] pipe read failed (pipe=%s)", pipeName.c_str());
		return false;
	}

	g_kHandoff        = ho;
	g_bHandoffPending = true;
	Tracef("[ELEMENTIA-HANDOFF] handoff consumed: name=%s host=%s port=%u slot=%u\n",
		   ho.name.c_str(), ho.host.c_str(), (unsigned)ho.port, (unsigned)ho.slot);
	return true;
}

bool Elementia_IsHandoffPending()
{
	return g_bHandoffPending;
}

const SElementiaHandoff& Elementia_GetHandoff()
{
	return g_kHandoff;
}

void Elementia_ClearHandoff()
{
	// Best effort: overwrite the key before dropping the struct so the u32
	// does not linger in this global once it has been handed to the stream.
	g_kHandoff.key = 0;
	g_kHandoff = SElementiaHandoff();
	g_bHandoffPending = false;
}
