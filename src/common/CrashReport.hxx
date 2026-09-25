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
	case 0xC0000005u: return "access violation";
	case 0xC0000374u: return "heap corruption";
	case 0xC0000409u: return "fail-fast (stack buffer overrun or __fastfail)";
	case 0xC00000FDu: return "stack overflow";
	case 0xE06D7363u: return "unhandled C++ exception";
	default: return "unknown";
	}
}

struct CrashFacts {
	const char* kind;         // unhandled_exception, ggpo_assertion, terminate, purecall, invalid_parameter
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
