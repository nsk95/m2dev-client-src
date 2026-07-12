#include "StdAfx.h"
#include "ElementiaGuard.h"

// =============================================================================
// ELEMENTIA-HARDENING implementation. See ElementiaGuard.h for the policy /
// kill-switch contract. When ELEMENTIA_PROTECT == 0 the whole body below
// collapses to three empty functions (bottom of file).
// =============================================================================

#if ELEMENTIA_PROTECT

#include "ElementiaObf.h"

#include <intrin.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>

namespace
{
	// =========================================================================
	// Scoring model (reworked to be false-positive-safe).
	//
	// Hard signals are grouped into a small set of DISTINCT CATEGORIES. Each
	// category records only its LAST-SEEN timestamp — there is NO monotonic
	// lifetime accumulator, so a signal naturally "decays": it stops counting
	// CAT_FRESH_MS after it was last observed.
	//
	// The reaction only arms when >= ARM_DISTINCT_CATS *different* hard
	// categories are simultaneously fresh (i.e. corroborated within the same
	// recent window) — never on a lifetime sum, never on one category alone.
	// Concretely that means a lone persistent .text CRC mismatch (a legit
	// AV/overlay inline-hooking our code) can NEVER arm the reaction, because it
	// only ever lights CAT_INTEGRITY. A real cheat/RE session lights several
	// (e.g. CAT_TOOLPROC + CAT_TOOLDRIVER, or CAT_DEBUGGER + CAT_TOOLPROC).
	//
	// Soft signals (window-class matches, NtGlobalFlag, timing, OutputDebugString,
	// ntdll inline hooks) are telemetry-only: they feed a separately-decaying
	// counter and NEVER contribute to arming.
	// =========================================================================
	enum HardCat
	{
		CAT_DEBUGGER = 0,   // a live user-mode debugger (port/object/HW-bp/PEB)
		CAT_TOOLPROC,       // an exact-named cheat/RE tool process is running
		CAT_TOOLDRIVER,     // a cheat-tool kernel driver device is open-able
		CAT_INTEGRITY,      // our code section was patched (confirmed, streak)
		CAT_COUNT
	};

	std::atomic<uint64_t> g_catSeen[CAT_COUNT];      // last-seen tick, 0 = never
	std::atomic<int>      g_softScore{ 0 };          // telemetry only, decays
	std::atomic<bool>     g_compromised{ false };
	std::atomic<uint64_t> g_reactAtTick{ 0 };        // scheduled reaction (0=none)
	std::atomic<bool>     g_initDone{ false };

	constexpr uint64_t CAT_FRESH_MS      = 45000;    // a category is "current" 45s
	constexpr int      ARM_DISTINCT_CATS = 2;        // need >=2 distinct fresh cats

	void MarkCat(HardCat c)
	{
		g_catSeen[c].store((uint64_t)GetTickCount64(), std::memory_order_relaxed);
	}

	int ActiveHardCats()
	{
		const uint64_t now = (uint64_t)GetTickCount64();
		int n = 0;
		for (int i = 0; i < CAT_COUNT; ++i)
		{
			const uint64_t t = g_catSeen[i].load(std::memory_order_relaxed);
			if (t != 0 && (now - t) <= CAT_FRESH_MS)
				++n;
		}
		return n;
	}

	void AddSoft(int n)
	{
		if (n > 0)
			g_softScore.fetch_add(n, std::memory_order_relaxed);
	}

	// ---- ntdll dynamic resolver (avoids plaintext IAT entries) --------------
	typedef LONG (WINAPI* fnNtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);

	fnNtQueryInformationProcess ResolveNtQIP()
	{
		static fnNtQueryInformationProcess s_fn = nullptr;
		if (s_fn)
			return s_fn;
		HMODULE h = GetModuleHandleW(OBFW(L"ntdll.dll").c_str());
		if (!h)
			return nullptr;
		s_fn = (fnNtQueryInformationProcess)GetProcAddress(h, OBF("NtQueryInformationProcess").c_str());
		return s_fn;
	}

	// Undocumented PROCESSINFOCLASS values.
	constexpr ULONG kProcessDebugPort          = 7;
	constexpr ULONG kProcessDebugObjectHandle  = 0x1E;
	constexpr ULONG kProcessDebugFlags         = 0x1F;

	// =========================================================================
	// Anti-debug primitives.
	// =========================================================================

	// PEB->BeingDebugged (offset 0x02 on both x86 and x64).
	bool Peb_BeingDebugged()
	{
#if defined(_WIN64)
		const unsigned char* peb = (const unsigned char*)__readgsqword(0x60);
#else
		const unsigned char* peb = (const unsigned char*)__readfsdword(0x30);
#endif
		if (!peb)
			return false;
		return peb[0x02] != 0;
	}

	// PEB->NtGlobalFlag heap-debug bits (0x70). NOTE: these are ALSO set by
	// page-heap / Application Verifier / an IFEO GlobalFlag registry value WITHOUT
	// any debugger present -> this is treated as a SOFT signal only (see Tick).
	bool Peb_NtGlobalFlag()
	{
#if defined(_WIN64)
		const unsigned char* peb = (const unsigned char*)__readgsqword(0x60);
		const size_t off = 0xBC;
#else
		const unsigned char* peb = (const unsigned char*)__readfsdword(0x30);
		const size_t off = 0x68;
#endif
		if (!peb)
			return false;
		DWORD flags = *(const DWORD*)(peb + off);
		return (flags & 0x70) != 0;
	}

	// Hardware breakpoints: any of Dr0..Dr3 non-zero => an external debugger armed
	// a HW breakpoint on this thread. Low false-positive.
	bool HardwareBreakpointsSet()
	{
		CONTEXT ctx;
		SecureZeroMemory(&ctx, sizeof(ctx));
		ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
		if (!GetThreadContext(GetCurrentThread(), &ctx))
			return false; // can't read -> no signal (fail-safe)
		return (ctx.Dr0 | ctx.Dr1 | ctx.Dr2 | ctx.Dr3) != 0;
	}

	// NtQueryInformationProcess-based checks. Returns hit count (>0 => debugger).
	int NtDebugChecks()
	{
		fnNtQueryInformationProcess NtQIP = ResolveNtQIP();
		if (!NtQIP)
			return 0;

		int hits = 0;
		HANDLE self = GetCurrentProcess();

		{
			DWORD_PTR port = 0;
			if (NtQIP(self, kProcessDebugPort, &port, sizeof(port), nullptr) == 0 && port != 0)
				++hits;
		}
		{
			HANDLE dbgObj = nullptr;
			LONG st = NtQIP(self, kProcessDebugObjectHandle, &dbgObj, sizeof(dbgObj), nullptr);
			if (st == 0 && dbgObj != nullptr)
			{
				CloseHandle(dbgObj);
				++hits;
			}
		}
		{
			DWORD flags = 1;
			if (NtQIP(self, kProcessDebugFlags, &flags, sizeof(flags), nullptr) == 0 && flags == 0)
				++hits;
		}
		return hits;
	}

	// SOFT probe: modern Windows makes this unreliable -> telemetry only.
	bool OutputDebugStringProbe()
	{
		SetLastError(0xC0DEC0DE);
		OutputDebugStringW(OBFW(L"i").c_str());
		return GetLastError() == 0xC0DEC0DE;
	}

	// SOFT probe: very false-positive prone (preemption/VM/throttle) -> telemetry.
	bool TimingAnomaly()
	{
		LARGE_INTEGER f, a, b;
		if (!QueryPerformanceFrequency(&f) || f.QuadPart == 0)
			return false;
		QueryPerformanceCounter(&a);
		volatile unsigned x = 0;
		for (int i = 0; i < 1000; ++i) x += (unsigned)i;
		QueryPerformanceCounter(&b);
		double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
		return ms > 60.0;
	}

	// Aggregate live-debugger detection into a single boolean (=> one category).
	bool LiveDebuggerDetected()
	{
		if (IsDebuggerPresent())      return true;
		if (Peb_BeingDebugged())      return true;
		{
			BOOL remote = FALSE;
			if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote) && remote)
				return true;
		}
		if (HardwareBreakpointsSet()) return true;
		if (NtDebugChecks() > 0)      return true;
		return false;
	}

	// =========================================================================
	// Tool / cheat-utility detection.
	// =========================================================================

	std::wstring LowerBasename(const wchar_t* path)
	{
		std::wstring s(path ? path : L"");
		size_t slash = s.find_last_of(L"\\/");
		if (slash != std::wstring::npos)
			s = s.substr(slash + 1);
		std::transform(s.begin(), s.end(), s.begin(), ::towlower);
		return s;
	}

	// Exact process-name match against unambiguous cheat/RE tools only. Dual-use
	// utilities (Process Hacker, Process Explorer, Wireshark, task managers) are
	// deliberately excluded so a legitimate power user can never be flagged.
	// Returns number of distinct matches (>0 => lights CAT_TOOLPROC).
	int ScanToolProcesses()
	{
		std::vector<std::wstring> bl = {
			OBFW(L"cheatengine-x86_64.exe"), OBFW(L"cheatengine-i386.exe"),
			OBFW(L"cheatengine.exe"),        OBFW(L"ollydbg.exe"),
			OBFW(L"x64dbg.exe"),             OBFW(L"x32dbg.exe"),
			OBFW(L"ida.exe"),                OBFW(L"ida64.exe"),
			OBFW(L"idaq.exe"),               OBFW(L"idaq64.exe"),
			OBFW(L"wpespy.dll"),             OBFW(L"wpepro.exe"),
			OBFW(L"rpe.exe"),                OBFW(L"artmoney.exe"),
			OBFW(L"cheat engine.exe"),       OBFW(L"scylla_x64.exe"),
			OBFW(L"scylla_x86.exe"),         OBFW(L"windbg.exe"),
			OBFW(L"dnspy.exe"),              OBFW(L"immunitydebugger.exe"),
		};

		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE)
			return 0;

		int hits = 0;
		PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
		if (Process32FirstW(snap, &pe))
		{
			do
			{
				std::wstring name = LowerBasename(pe.szExeFile);
				for (const std::wstring& bad : bl)
				{
					if (!bad.empty() && name == bad)
					{
						++hits;
						break;
					}
				}
			} while (Process32NextW(snap, &pe));
		}
		CloseHandle(snap);
		return hits;
	}

	struct EnumCtx { const std::vector<std::wstring>* classes; bool found; };

	BOOL CALLBACK WndEnumProc(HWND hWnd, LPARAM lp)
	{
		EnumCtx* ctx = reinterpret_cast<EnumCtx*>(lp);
		wchar_t cls[128] = { 0 };
		GetClassNameW(hWnd, cls, 127);
		std::wstring c(cls);
		std::transform(c.begin(), c.end(), c.begin(), ::towlower);
		for (const std::wstring& n : *ctx->classes)
		{
			// EXACT class-name match (not a title substring), so an innocent
			// window merely *containing* a tool name in its TITLE (a browser tab,
			// a chat message, a folder) can never match.
			if (!n.empty() && c == n)
			{
				ctx->found = true;
				return FALSE; // de-dup: one hit is enough, stop enumerating
			}
		}
		return TRUE;
	}

	// SOFT signal. Matches only the exact top-level window CLASS names of a few
	// tools whose class is distinctive. Returns 0/1 (de-duped).
	int ScanToolWindows()
	{
		std::vector<std::wstring> classes = {
			OBFW(L"ollydbg"),          // OllyDbg / Immunity main frame class
			OBFW(L"windbgframeclass"), // WinDbg (classic) main frame class
		};
		EnumCtx ctx{ &classes, false };
		EnumWindows(WndEnumProc, reinterpret_cast<LPARAM>(&ctx));
		return ctx.found ? 1 : 0;
	}

	// Cheat Engine's kernel driver only (loaded only while CE runs in kernel
	// mode). Fail-safe: a failed open (the normal case) yields no signal.
	// Returns number of open-able devices (>0 => lights CAT_TOOLDRIVER).
	int ScanToolDrivers()
	{
		std::vector<std::wstring> devs = {
			OBFW(L"\\\\.\\dbk64"),
			OBFW(L"\\\\.\\dbk32"),
			OBFW(L"\\\\.\\EagleXNt"),
		};
		int hits = 0;
		for (const std::wstring& d : devs)
		{
			HANDLE h = CreateFileW(d.c_str(), GENERIC_READ, 0, nullptr,
								   OPEN_EXISTING, 0, nullptr);
			if (h != INVALID_HANDLE_VALUE)
			{
				CloseHandle(h);
				++hits;
			}
		}
		return hits;
	}

	// SOFT signal: AV/EDR and overlays legitimately hook these ntdll exports.
	bool LooksHooked(const wchar_t* mod, const char* fn)
	{
		HMODULE h = GetModuleHandleW(mod);
		if (!h)
			return false;
		const unsigned char* p = (const unsigned char*)GetProcAddress(h, fn);
		if (!p)
			return false;
		if (p[0] == 0xE9) return true;
		if (p[0] == 0xFF && p[1] == 0x25) return true;
		if (p[0] == 0x68 && p[5] == 0xC3) return true;
		if (p[0] == 0xCC) return true;
		return false;
	}

	int ScanNtdllHooks()
	{
		std::wstring ntdll = OBFW(L"ntdll.dll");
		int hits = 0;
		if (LooksHooked(ntdll.c_str(), OBF("NtQueryInformationProcess").c_str())) ++hits;
		if (LooksHooked(ntdll.c_str(), OBF("NtProtectVirtualMemory").c_str()))    ++hits;
		if (LooksHooked(ntdll.c_str(), OBF("NtReadVirtualMemory").c_str()))       ++hits;
		return hits;
	}

	// =========================================================================
	// Anti-dump: scrub only structures Windows does NOT consult at runtime (DOS
	// stub, Rich header, section NAME fields). e_magic / PE signature / optional
	// header / section RVAs / SizeOfImage are intentionally preserved (they are
	// read by RtlImageNtHeader inside FindResource / version.dll / DbgHelp) — see
	// the report; full header nuking is left to a commercial packer.
	// =========================================================================
	void ScrubHeaders()
	{
		HMODULE hMod = GetModuleHandleW(nullptr);
		if (!hMod)
			return;
		unsigned char* base = (unsigned char*)hMod;

		IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return;
		if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
			return;

		IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return;

		const size_t stubStart = sizeof(IMAGE_DOS_HEADER);
		const size_t stubEnd   = (size_t)dos->e_lfanew;
		if (stubEnd > stubStart && stubEnd <= 0x1000)
		{
			DWORD oldProt = 0;
			if (VirtualProtect(base + stubStart, stubEnd - stubStart, PAGE_READWRITE, &oldProt))
			{
				SecureZeroMemory(base + stubStart, stubEnd - stubStart);
				VirtualProtect(base + stubStart, stubEnd - stubStart, oldProt, &oldProt);
			}
		}

		IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
		WORD n = nt->FileHeader.NumberOfSections;
		if (n == 0 || n > 96)
			return;
		DWORD oldProt = 0;
		unsigned char* secBytes = (unsigned char*)sec;
		size_t secRegion = (size_t)n * sizeof(IMAGE_SECTION_HEADER);
		if (VirtualProtect(secBytes, secRegion, PAGE_READWRITE, &oldProt))
		{
			for (WORD i = 0; i < n; ++i)
				SecureZeroMemory(sec[i].Name, IMAGE_SIZEOF_SHORT_NAME);
			VirtualProtect(secBytes, secRegion, oldProt, &oldProt);
		}
	}

	// =========================================================================
	// Integrity: CRC of the primary executable code section.
	//
	// A standard (zlib) CRC32 is used via a locally-built table so the work can
	// be CHUNKED across frames (F4): each integrity tick folds only CRC_CHUNK
	// bytes into a running register, so the render loop never sees the full-image
	// pass in a single frame. Baseline is taken once at Init (start-up, off the
	// render loop) AFTER the loader applied relocations. Two consecutive full-pass
	// mismatches are required before CAT_INTEGRITY is lit (absorbs transients),
	// and even then it is only ONE category — it can never arm on its own.
	// =========================================================================
	const unsigned char* g_textBase = nullptr;
	size_t               g_textSize = 0;
	uint32_t             g_textCrc  = 0;      // baseline
	int                  g_textMismatchStreak = 0;

	uint32_t g_crcTable[256];
	bool     g_crcTableReady = false;

	void BuildCrcTable()
	{
		for (uint32_t i = 0; i < 256; ++i)
		{
			uint32_t c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1) ? (0xEDB88820u ^ (c >> 1)) : (c >> 1);
			g_crcTable[i] = c;
		}
		g_crcTableReady = true;
	}

	// Fold [p,p+n) into the running (pre-final-XOR) register.
	uint32_t CrcFold(uint32_t reg, const unsigned char* p, size_t n)
	{
		for (size_t i = 0; i < n; ++i)
			reg = g_crcTable[(reg ^ p[i]) & 0xFF] ^ (reg >> 8);
		return reg;
	}

	uint32_t CrcFull(const unsigned char* p, size_t n)
	{
		return CrcFold(0xFFFFFFFFu, p, n) ^ 0xFFFFFFFFu;
	}

	bool LocateTextSection()
	{
		HMODULE hMod = GetModuleHandleW(nullptr);
		if (!hMod)
			return false;
		unsigned char* base = (unsigned char*)hMod;
		IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;

		IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
		WORD count = nt->FileHeader.NumberOfSections;
		// Key on characteristics (survives the section-name scrub).
		for (WORD i = 0; i < count; ++i)
		{
			const DWORD ch = sec[i].Characteristics;
			if ((ch & IMAGE_SCN_CNT_CODE) && (ch & IMAGE_SCN_MEM_EXECUTE))
			{
				DWORD vsize = sec[i].Misc.VirtualSize;
				DWORD rsize = sec[i].SizeOfRawData;
				DWORD sz = vsize ? vsize : rsize;
				if (!sz)
					return false;
				g_textBase = base + sec[i].VirtualAddress;
				g_textSize = (size_t)sz;
				return true;
			}
		}
		return false;
	}

	// Chunked integrity state.
	constexpr size_t CRC_CHUNK = 256 * 1024;   // bytes folded per integrity tick
	size_t   g_crcOff = 0;
	uint32_t g_crcReg = 0xFFFFFFFFu;
	bool     g_crcInProgress = false;

	// Advance the chunked CRC by one chunk. When a full pass completes, compare to
	// the baseline and (on a confirmed 2x streak) light CAT_INTEGRITY.
	void CheckIntegrityChunked()
	{
		if (!g_textBase || !g_textSize || !g_crcTableReady)
			return;

		if (!g_crcInProgress)
		{
			g_crcOff = 0;
			g_crcReg = 0xFFFFFFFFu;
			g_crcInProgress = true;
		}

		size_t remain = g_textSize - g_crcOff;
		size_t take = remain < CRC_CHUNK ? remain : CRC_CHUNK;
		g_crcReg = CrcFold(g_crcReg, g_textBase + g_crcOff, take);
		g_crcOff += take;

		if (g_crcOff >= g_textSize)
		{
			uint32_t now = g_crcReg ^ 0xFFFFFFFFu;
			g_crcInProgress = false;   // ready to restart next pass

			if (now != g_textCrc)
			{
				if (++g_textMismatchStreak >= 2)
				{
					MarkCat(CAT_INTEGRITY);
					g_textMismatchStreak = 0;
				}
			}
			else
			{
				g_textMismatchStreak = 0;
			}
		}
	}

	// =========================================================================
	// Reaction: quiet + delayed + far from the check site. Arms ONLY when
	// >= ARM_DISTINCT_CATS distinct hard categories are simultaneously fresh, and
	// never in PASSIVE builds.
	// =========================================================================
	void MaybeReact()
	{
		if (ActiveHardCats() < ARM_DISTINCT_CATS)
			return;

		g_compromised.store(true, std::memory_order_relaxed);

#if !defined(ELEMENTIA_GUARD_PASSIVE)
		uint64_t scheduled = g_reactAtTick.load(std::memory_order_relaxed);
		if (scheduled == 0)
		{
			DWORD delay = 15000 + (GetTickCount() % 45000); // 15..60s
			g_reactAtTick.store((uint64_t)GetTickCount64() + delay, std::memory_order_relaxed);
			return;
		}
		if ((uint64_t)GetTickCount64() >= scheduled)
			TerminateProcess(GetCurrentProcess(), 0);
#endif
	}

	void DecaySoft()
	{
		int s = g_softScore.load(std::memory_order_relaxed);
		if (s > 0)
			g_softScore.store(s - 1, std::memory_order_relaxed);
	}
}

// -----------------------------------------------------------------------------
// Public entry points.
// -----------------------------------------------------------------------------

void Elementia_Guard_Init()
{
	if (g_initDone.exchange(true))
		return;

	for (int i = 0; i < CAT_COUNT; ++i)
		g_catSeen[i].store(0, std::memory_order_relaxed);

	// 1) CRC table + code-section integrity baseline (full pass; start-up only,
	//    not on the render loop). Captured BEFORE scrubbing section names.
	BuildCrcTable();
	if (LocateTextSection())
		g_textCrc = CrcFull(g_textBase, g_textSize);

	// 2) Anti-dump header scrub (runtime-inert fields only).
	ScrubHeaders();

	// 3) One cheap anti-debug snapshot. Only records the category; the loop's
	//    MaybeReact() owns the (guarded, corroborated, delayed) response so a
	//    borderline start-up environment never aborts the launch.
	if (LiveDebuggerDetected())
		MarkCat(CAT_DEBUGGER);
}

void Elementia_Guard_Tick()
{
	if (!g_initDone.load(std::memory_order_relaxed))
		return;

	// Throttle: run one rotating bucket every ~2.5..5.5s. Almost every frame call
	// returns immediately here.
	static DWORD s_lastRun = 0;
	static DWORD s_nextGap = 2000;
	static unsigned s_bucket = 0;

	DWORD nowTick = GetTickCount();
	if (s_lastRun != 0 && (nowTick - s_lastRun) < s_nextGap)
		return;
	s_lastRun = nowTick;
	s_nextGap = 2500 + (nowTick % 3000);

	switch (s_bucket++ % 4)
	{
	case 0: // anti-debug
	{
		if (LiveDebuggerDetected())
			MarkCat(CAT_DEBUGGER);

		// soft / telemetry-only signals (never arm): NtGlobalFlag is set by
		// page-heap / AppVerifier / IFEO GlobalFlag with NO debugger, so it is
		// SOFT here.
		int soft = 0;
		if (Peb_NtGlobalFlag())       ++soft;
		if (OutputDebugStringProbe()) ++soft;
		if (TimingAnomaly())          ++soft;
		AddSoft(soft);
		break;
	}
	case 1: // tool processes (hard) + tool windows (soft)
	{
		if (ScanToolProcesses() > 0)
			MarkCat(CAT_TOOLPROC);
		AddSoft(ScanToolWindows());
		break;
	}
	case 2: // tool drivers (hard) + ntdll hooks (soft)
	{
		if (ScanToolDrivers() > 0)
			MarkCat(CAT_TOOLDRIVER);
		AddSoft(ScanNtdllHooks());
		break;
	}
	case 3: // integrity (chunked) + soft decay
	default:
		CheckIntegrityChunked();
		DecaySoft();
		break;
	}

	MaybeReact();
}

bool Elementia_Guard_IsCompromised()
{
	return g_compromised.load(std::memory_order_relaxed);
}

#else // ELEMENTIA_PROTECT == 0  -> no-op stubs (Debug builds / kill-switch)

void Elementia_Guard_Init()          {}
void Elementia_Guard_Tick()          {}
bool Elementia_Guard_IsCompromised() { return false; }

#endif // ELEMENTIA_PROTECT
