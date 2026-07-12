#pragma once
//
// ELEMENTIA-HARDENING — compile-time string / constant obfuscation.
// -----------------------------------------------------------------------------
// Sensitive literals (tool names, device paths, ntdll export names, config keys)
// must NOT appear as plaintext in the shipped binary, otherwise a `strings` dump
// instantly reveals every detection heuristic. This header XOR-encodes such
// literals at COMPILE time (constexpr) and decodes them into a heap string at
// RUNTIME. The binary only ever contains the XORed bytes.
//
// Usage:
//     std::string  s = OBF("Cheat Engine");     // narrow
//     std::wstring w = OBFW(L"\\\\.\\dbk64");    // wide
//
// Notes:
//  * Purely obfuscation, NOT cryptography. It raises the bar against trivial
//    static inspection; it does not protect real secrets. Do NOT put genuine
//    keys here (the project's real crypto stays in libsodium / server side).
//  * Fail-safe: decode is a pure function returning std::string/std::wstring;
//    it cannot throw beyond a normal allocation and never affects control flow.
//
#include <string>
#include <cstddef>

namespace elob
{
	// Per-index rolling key. Kept small & branch-free so the whole ObfString
	// ctor folds to a constant at compile time under /O2.
	constexpr unsigned char KeyByte(std::size_t i)
	{
		// mix a fixed seed with the index; wraps in unsigned char
		return (unsigned char)(0x5Au + i * 0x1Du + (i >> 2) * 0x07u);
	}

	template <typename CH, std::size_t N>
	struct ObfString
	{
		CH data[N]{};

		constexpr ObfString(const CH (&s)[N])
		{
			for (std::size_t i = 0; i < N; ++i)
				data[i] = (CH)(s[i] ^ (CH)KeyByte(i));
		}

		// Runtime decode. Not constexpr on purpose: we want the decode to happen
		// at run time so the plaintext is never materialised in .rdata.
		std::basic_string<CH> decode() const
		{
			std::basic_string<CH> out;
			if (N == 0)
				return out;
			out.resize(N - 1); // drop the terminating NUL
			for (std::size_t i = 0; i + 1 < N; ++i)
				out[i] = (CH)(data[i] ^ (CH)KeyByte(i));
			return out;
		}
	};
}

// The immediately-invoked lambda forces the ObfString to be a constexpr object
// (so its bytes are baked in already-XORed), while decode() runs at load time.
#define OBF(str)  ([]{ constexpr elob::ObfString<char, sizeof(str)> _o(str); return _o; }().decode())
#define OBFW(str) ([]{ constexpr elob::ObfString<wchar_t, sizeof(str)/sizeof(wchar_t)> _o(str); return _o; }().decode())
