#pragma once

// The facts a crash must leave behind, kept in a form that can be written
// from the crashing thread: a fixed ring of the last log lines, a name for
// each exit code the launcher can see, a header line for the crash record,
// and a fold over the process address space. Nothing here allocates or
// takes a lock. The host (sf4e__CrashDiagnostics) owns the Win32 side.
//
// Pure component: no game or platform dependencies, unit tested (F-015).

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <atomic>

namespace sf4e {
namespace crash {

// The last `Lines` log lines, each cut to `Width - 1` characters. Push runs
// on the logger's worker thread, ForEach on the crashing thread; a line
// being written while it is read may come out torn, which is acceptable
// for a last-resort record. Every slot is always terminated.
template <unsigned Lines, unsigned Width>
class LogRing {
public:
	static const unsigned LineCount = Lines;
	static const unsigned LineWidth = Width;

	LogRing() : next_(0) {
		for (unsigned i = 0; i < Lines; i++) lines_[i][0] = '\0';
	}

	void Push(const char* text, size_t length) {
		const unsigned slot = next_.fetch_add(1) % Lines;
		const size_t kept = length < Width - 1 ? length : Width - 1;
		memcpy(lines_[slot], text, kept);
		lines_[slot][kept] = '\0';
	}

	unsigned Count() const {
		const unsigned pushed = next_.load();
		return pushed < Lines ? pushed : Lines;
	}

	// Oldest line first.
	template <class Fn>
	void ForEach(Fn&& fn) const {
		const unsigned pushed = next_.load();
		const unsigned kept = pushed < Lines ? pushed : Lines;
		for (unsigned i = 0; i < kept; i++) fn(lines_[(pushed - kept + i) % Lines]);
	}

private:
	std::atomic<unsigned> next_;
	char lines_[Lines][Width];
};

// The exit codes a dead game process can hand the launcher, so the
// launcher log tells an assertion from an access violation.
inline const char* ExitCodeName(uint32_t code) {
	switch (code) {
	case 0: return "clean exit";
	case 1: return "exit(1), which is how a GGPO assertion ends the process";
	case 3: return "abort()";
	case 255: return "_exit(255), how the game's C runtime ends after a fatal runtime error";
	case 0xC0000005u: return "access violation";
	case 0xC0000374u: return "heap corruption";
	case 0xC0000409u: return "fail-fast (stack buffer overrun or __fastfail)";
	case 0xC00000FDu: return "stack overflow";
	case 0xE06D7363u: return "unhandled C++ exception";
	case 0xC0000135u: return "DLL not found (STATUS_DLL_NOT_FOUND)";
	case 0xC0000139u: return "entry point not found (STATUS_ENTRYPOINT_NOT_FOUND): a loaded DLL lacks an export, often a GGPO.dll outside the package";
	case 0xC0E90002u: return "blocked by Windows code integrity (STATUS_SYSTEM_INTEGRITY_POLICY_VIOLATION): Smart App Control or an application control policy refused a file";
	// Windows ends a window marked Not Responding with this code when the
	// player closes it, so it is a hang rather than a fault.
	case 0xCFFFFFFFu: return "ended by Windows as Not Responding: the game froze and its window was closed";
	default: return "unknown";
	}
}

// True when the process died of a fault its crash handler may have
// recorded, as opposed to a clean exit or a loader failure before Sidecar ran.
inline bool IsCrashExit(uint32_t code) {
	if (code == 0xC0000135u || code == 0xC0000139u || code == 0xC0E90002u) return false;
	return (code & 0xF0000000u) == 0xC0000000u || code == 0xE06D7363u || code == 3 || code == 255;
}

struct CrashFacts {
	const char* kind;         // unhandled_exception, heap_corruption, ggpo_assertion, terminate, purecall, invalid_parameter, exit
	uint32_t code;            // exception code, or 0
	uint64_t address;         // faulting address, or 0
	const char* module;       // file name of the module containing address, or ""
	uint64_t moduleOffset;    // address minus that module's base
	uint32_t threadId;
	const char* message;      // assertion text or the reason, or ""
};

// One line, terminated, cut to `size`. Returns the length written.
inline size_t FormatCrashHeader(char* out, size_t size, const CrashFacts& facts) {
	if (!out || !size) return 0;
	const int written = snprintf(out, size,
		"kind=%s code=0x%08X (%s) address=0x%08llX module=%s+0x%llX thread=%u message=%s\n",
		facts.kind ? facts.kind : "", facts.code, ExitCodeName(facts.code),
		(unsigned long long)facts.address, facts.module && facts.module[0] ? facts.module : "?",
		(unsigned long long)facts.moduleOffset, facts.threadId, facts.message ? facts.message : "");
	if (written < 0) { out[0] = '\0'; return 0; }
	return (size_t)written < size ? (size_t)written : size - 1;
}

// True when the 7 bytes before a return address end in an x86 call: E8
// rel32, or FF /2 through a register, [register], [register+disp] or
// [disp32], with or without a SIB byte. A stack word that passes is
// probably a caller; one that fails is data or a pushed constant.
inline bool EndsInCall(const uint8_t before[7]) {
	const uint8_t* at = before + 7; // at[-n] is the byte n before the return address
	if (at[-5] == 0xE8) return true;
	if (at[-2] == 0xFF && ((at[-1] & 0xF8) == 0x10 || (at[-1] & 0xF8) == 0xD0) && at[-1] != 0x14 && at[-1] != 0x15) return true;
	if (at[-3] == 0xFF && (at[-2] & 0xF8) == 0x50 && at[-2] != 0x54) return true;
	// FF 14 SIB is 3 bytes, unless the SIB base is 101, which adds a disp32.
	if (at[-3] == 0xFF && at[-2] == 0x14 && (at[-1] & 7) != 5) return true;
	if (at[-4] == 0xFF && at[-3] == 0x54) return true;
	if (at[-6] == 0xFF && (at[-5] == 0x15 || ((at[-5] & 0xF8) == 0x90 && at[-5] != 0x94))) return true;
	if (at[-7] == 0xFF && at[-6] == 0x94) return true;
	if (at[-7] == 0xFF && at[-6] == 0x14 && (at[-5] & 7) == 5) return true; // call [index*scale+disp32]
	return false;
}

// Appends " Module+0xOffset" to a callers line of `size` bytes; false when
// it no longer fits.
inline bool AppendCaller(char* out, size_t size, size_t& length, const char* module, uint64_t offset) {
	if (!out || length >= size) return false;
	const int written = snprintf(out + length, size - length, " %s+0x%llX", module && module[0] ? module : "?", (unsigned long long)offset);
	if (written < 0 || (size_t)written >= size - length) { out[length] = '\0'; return false; }
	length += (size_t)written;
	return true;
}

// Free virtual memory seen by a walk over the address space. For the
// 32-bit game the largest free region is the number that decides whether
// the next texture, save state or thread stack can still be placed.
struct AddressSpaceSummary {
	uint64_t largestFree = 0;
	uint64_t totalFree = 0;
	uint32_t freeRegions = 0;

	void AddFreeRegion(uint64_t size) {
		totalFree += size;
		++freeRegions;
		if (size > largestFree) largestFree = size;
	}
};

} // namespace crash
} // namespace sf4e
