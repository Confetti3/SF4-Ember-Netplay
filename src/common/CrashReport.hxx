#pragma once

// The facts a crash must leave behind, kept in a form that can be written
// from the crashing thread: a fixed ring of the last log lines, a name for
// each exit code the launcher can see, a header line for the crash record,
// a fold over the process address space, and what a fault in unmapped code
// leaves to name its cause (the unloaded modules, the region, the stack).
// Nothing here allocates or takes a lock. The host (sf4e__CrashDiagnostics)
// owns the Win32 side.
//
// Pure component: no game or platform dependencies, unit tested (F-015).

#include <stddef.h>
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

// How far the game's start-up had got when the process ended, as the launcher
// saw it: Sidecar signals the launcher once it is running in the game.
enum class StartPhase { None, BeforeSignal, AfterSignal };

// The game's C runtime ends a failed start-up with _exit(255). Within this
// long after Sidecar's signal such an exit is still the start-up failing, not
// the game crashing while it ran.
constexpr uint64_t StartFailureWindowMs = 3000;

// None unless the game ended with the start-failure exit code during
// start-up: before the signal (`endedBeforeSignal`), or within
// StartFailureWindowMs of it (`msSinceSignal`, when `signalled`).
inline StartPhase ClassifyStartFailure(uint32_t exitCode, bool endedBeforeSignal, bool signalled, uint64_t msSinceSignal) {
	if (exitCode != 255) return StartPhase::None;
	if (endedBeforeSignal) return StartPhase::BeforeSignal;
	if (signalled && msSinceSignal <= StartFailureWindowMs) return StartPhase::AfterSignal;
	return StartPhase::None;
}

// The phase as the report's `phase` field names it; "" for None.
inline const char* StartPhaseName(StartPhase phase) {
	switch (phase) {
	case StartPhase::BeforeSignal: return "before_signal";
	case StartPhase::AfterSignal: return "after_signal";
	default: return "";
	}
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

// The 32-bit registers at the fault.
struct CrashRegisters {
	uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip;
};

// One line, terminated, cut to `size`. Returns the length written.
inline size_t FormatRegisters(char* out, size_t size, const CrashRegisters& r) {
	if (!out || !size) return 0;
	const int written = snprintf(out, size,
		"registers: eax=0x%08X ebx=0x%08X ecx=0x%08X edx=0x%08X esi=0x%08X edi=0x%08X ebp=0x%08X esp=0x%08X eip=0x%08X\n",
		r.eax, r.ebx, r.ecx, r.edx, r.esi, r.edi, r.ebp, r.esp, r.eip);
	if (written < 0) { out[0] = '\0'; return 0; }
	return (size_t)written < size ? (size_t)written : size - 1;
}

// The code bytes at the faulting instruction as one terminated line, cut to
// `size`: "code at 0x<address>: 8B 40 10 ...", or "unreadable" when none could
// be read. Returns the length written.
inline size_t FormatCodeBytes(char* out, size_t size, uint64_t address, const uint8_t* bytes, size_t count) {
	if (!out || !size) return 0;
	int written = snprintf(out, size, "code at 0x%08llX:", (unsigned long long)address);
	if (written < 0) { out[0] = '\0'; return 0; }
	size_t length = (size_t)written < size ? (size_t)written : size - 1;
	if (!bytes || !count) {
		written = snprintf(out + length, size - length, " unreadable");
		if (written > 0) length += (size_t)written < size - length ? (size_t)written : size - length - 1;
	}
	for (size_t i = 0; bytes && i < count; ++i) {
		written = snprintf(out + length, size - length, " %02X", bytes[i]);
		if (written < 0 || (size_t)written >= size - length) break;
		length += (size_t)written;
	}
	if (length + 1 < size) { out[length++] = '\n'; out[length] = '\0'; }
	else { out[size - 1] = '\0'; length = size - 1; }
	return length;
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

// Moves `length` past what snprintf just wrote at out + length; false, with
// that line dropped, when it did not fit in `size`.
inline bool Advance(char* out, size_t size, size_t& length, int printed) {
	if (printed < 0 || (size_t)printed >= size - length) { out[length] = '\0'; return false; }
	length += (size_t)printed;
	return true;
}

// Appends " Module+0xOffset" to a callers line of `size` bytes; false when
// it no longer fits.
inline bool AppendCaller(char* out, size_t size, size_t& length, const char* module, uint64_t offset) {
	if (!out || length >= size) return false;
	return Advance(out, size, length, snprintf(out + length, size - length, " %s+0x%llX", module && module[0] ? module : "?", (unsigned long long)offset));
}

// Where an address lies in the address space, as VirtualQuery says, kept as
// numbers so the line is formatted and tested without Windows.
enum class RegionState { Unknown, Free, Reserved, Committed };
enum class RegionType { None, Image, Mapped, Private };

struct AddressRegion {
	RegionState state = RegionState::Unknown;
	RegionType type = RegionType::None;
	uint32_t protect = 0;
	uint64_t allocationBase = 0;
	uint64_t regionBase = 0;
	uint64_t regionSize = 0;
};

// One terminated line: "address 0x64A1A5E0: free region=0x64A1A000+0x5E000
// protect=0x1". A region in use adds its type and allocation base, which a free
// one does not have.
inline size_t FormatAddressRegion(char* out, size_t size, uint64_t address, const AddressRegion& region) {
	if (!out || !size) return 0;
	out[0] = '\0';
	size_t length = 0;
	if (region.state == RegionState::Unknown) {
		Advance(out, size, length, snprintf(out, size, "address 0x%08llX: not queryable\n", (unsigned long long)address));
		return length;
	}
	const bool free = region.state == RegionState::Free;
	const char* state = free ? "free" : region.state == RegionState::Reserved ? "reserved" : "committed";
	const char* type = free ? "" : region.type == RegionType::Image ? " image" : region.type == RegionType::Mapped ? " mapped" :
		region.type == RegionType::Private ? " private" : "";
	bool whole = Advance(out, size, length, snprintf(out, size, "address 0x%08llX: %s%s region=0x%08llX+0x%llX protect=0x%X",
		(unsigned long long)address, state, type, (unsigned long long)region.regionBase, (unsigned long long)region.regionSize, region.protect));
	if (whole && !free)
		whole = Advance(out, size, length, snprintf(out + length, size - length, " allocation_base=0x%08llX", (unsigned long long)region.allocationBase));
	if (!whole || length + 1 >= size) { out[0] = '\0'; return 0; }
	out[length++] = '\n';
	out[length] = '\0';
	return length;
}

// ntdll keeps the last modules it unloaded in a circular array, which names
// the image a thread was still running in when its code vanished. This is
// RTL_UNLOAD_EVENT_TRACE as a 32-bit process has it; the SDK headers lack it.
struct UnloadEventTrace32 {
	uint32_t baseAddress;
	uint32_t sizeOfImage;
	uint32_t sequence;
	uint32_t timeDateStamp;
	uint32_t checkSum;
	uint16_t imageName[32];
	uint32_t version[2];
};
static_assert(offsetof(UnloadEventTrace32, imageName) == 20 && sizeof(UnloadEventTrace32) == 88, "RTL_UNLOAD_EVENT_TRACE layout");

constexpr size_t UnloadEntryNeeded = offsetof(UnloadEventTrace32, imageName) + sizeof(uint16_t) * 32;
constexpr uint32_t UnloadTraceMaxElements = 64;   // RTL_UNLOAD_EVENT_TRACE_NUMBER
constexpr uint32_t UnloadTraceMaxElementSize = 256;
constexpr size_t UnloadTraceMaxBytes = (size_t)UnloadTraceMaxElements * UnloadTraceMaxElementSize;
constexpr unsigned UnloadedKept = 8;

struct UnloadedImage {
	uint64_t base = 0;
	uint64_t size = 0;
	uint32_t sequence = 0;
	char name[33] = {};
};

// The newest unloads, newest first.
struct UnloadedImages {
	UnloadedImage items[UnloadedKept];
	unsigned count = 0;
};

// The bytes of an array ntdll reports as `elementCount` elements of
// `elementSize`, or 0 when the report cannot be right. Entries are stepped by
// that size, which may exceed the struct above.
inline size_t UnloadTraceBytes(uint32_t elementSize, uint32_t elementCount) {
	if (elementSize < UnloadEntryNeeded || elementSize > UnloadTraceMaxElementSize) return 0;
	if (!elementCount || elementCount > UnloadTraceMaxElements) return 0;
	return (size_t)elementSize * elementCount;
}

// The newest entries of a copy of ntdll's array. An empty slot, and an entry
// that does not look like an image (no 64 KB aligned base, no size, no
// printable name), are skipped, so memory that is not the array parses to
// nothing. Names are narrowed; a character outside ASCII becomes '?'.
inline UnloadedImages ParseUnloadTrace(const uint8_t* array, uint32_t elementSize, uint32_t elementCount) {
	UnloadedImages found;
	if (!array || !UnloadTraceBytes(elementSize, elementCount)) return found;
	for (uint32_t i = 0; i < elementCount; ++i) {
		UnloadEventTrace32 entry;
		memset(&entry, 0, sizeof(entry));
		memcpy(&entry, array + (size_t)i * elementSize, sizeof(entry) < elementSize ? sizeof(entry) : elementSize);
		if (!entry.baseAddress || (entry.baseAddress & 0xFFFFu) || !entry.sizeOfImage || entry.sizeOfImage >= 0x40000000u) continue;
		if (entry.imageName[0] < 0x20 || entry.imageName[0] > 0x7E) continue;
		UnloadedImage image;
		image.base = entry.baseAddress;
		image.size = entry.sizeOfImage;
		image.sequence = entry.sequence;
		for (size_t c = 0; c < 32 && entry.imageName[c]; ++c)
			image.name[c] = entry.imageName[c] >= 0x20 && entry.imageName[c] <= 0x7E ? (char)entry.imageName[c] : '?';
		unsigned at = 0;
		while (at < found.count && found.items[at].sequence >= image.sequence) ++at;
		if (at >= UnloadedKept) continue;
		for (unsigned j = found.count < UnloadedKept ? found.count : UnloadedKept - 1; j > at; --j) found.items[j] = found.items[j - 1];
		found.items[at] = image;
		if (found.count < UnloadedKept) ++found.count;
	}
	return found;
}

// The newest unloaded image whose range holds `address`, or nullptr.
inline const UnloadedImage* FindUnloaded(const UnloadedImages& images, uint64_t address) {
	for (unsigned i = 0; i < images.count; ++i) {
		const UnloadedImage& image = images.items[i];
		if (address >= image.base && address - image.base < image.size) return &image;
	}
	return nullptr;
}

// "unloaded modules: name@0xBase+0xSize ...", newest first, as one terminated
// line; nothing when the trace holds none.
inline size_t FormatUnloadedList(char* out, size_t size, const UnloadedImages& images) {
	if (!out || !size) return 0;
	out[0] = '\0';
	if (!images.count) return 0;
	size_t length = 0;
	if (!Advance(out, size, length, snprintf(out, size, "unloaded modules:"))) return 0;
	for (unsigned i = 0; i < images.count; ++i) {
		const UnloadedImage& image = images.items[i];
		if (!Advance(out, size, length, snprintf(out + length, size - length, " %s@0x%08llX+0x%llX", image.name,
			(unsigned long long)image.base, (unsigned long long)image.size))) break;
	}
	if (length + 1 >= size) { out[0] = '\0'; return 0; }
	out[length++] = '\n';
	out[length] = '\0';
	return length;
}

// "unloaded module: name+0xOffset" when the faulting address is inside an
// unloaded image, and the same with " (esi)" for each of ebx, esi and edi that
// points into one, a line each. Nothing when none does.
inline size_t FormatUnloadedHits(char* out, size_t size, const UnloadedImages& images, uint64_t faultAddress, const CrashRegisters& r) {
	if (!out || !size) return 0;
	out[0] = '\0';
	size_t length = 0;
	struct Probe { const char* suffix; uint64_t address; };
	const Probe probes[] = { { "", faultAddress }, { " (ebx)", r.ebx }, { " (esi)", r.esi }, { " (edi)", r.edi } };
	for (const Probe& probe : probes) {
		const UnloadedImage* image = FindUnloaded(images, probe.address);
		if (!image) continue;
		if (!Advance(out, size, length, snprintf(out + length, size - length, "unloaded module: %s+0x%llX%s\n", image->name,
			(unsigned long long)(probe.address - image->base), probe.suffix))) break;
	}
	return length;
}

// "name+0xOffset" for an address inside a loaded module (`loadedName` and
// `loadedBase` as the host found them; base 0 for none), or "name+0xOffset
// (unloaded)" for one inside an unloaded image; empty for neither.
inline size_t DescribeImage(char* out, size_t size, uint64_t address, const char* loadedName, uint64_t loadedBase, const UnloadedImages& unloaded) {
	if (!out || !size) return 0;
	out[0] = '\0';
	size_t length = 0;
	if (loadedBase && address >= loadedBase) {
		Advance(out, size, length, snprintf(out, size, "%s+0x%llX", loadedName && loadedName[0] ? loadedName : "?", (unsigned long long)(address - loadedBase)));
		return length;
	}
	if (const UnloadedImage* image = FindUnloaded(unloaded, address))
		Advance(out, size, length, snprintf(out, size, "%s+0x%llX (unloaded)", image->name, (unsigned long long)(address - image->base)));
	return length;
}

// "label: 0xAddress description" as one terminated line, without the
// description when it is empty. Nothing when it does not fit.
inline size_t FormatLocatedAddress(char* out, size_t size, const char* label, uint64_t address, const char* description) {
	if (!out || !size) return 0;
	out[0] = '\0';
	size_t length = 0;
	const bool described = description && description[0];
	Advance(out, size, length, snprintf(out, size, "%s: 0x%08llX%s%s\n", label, (unsigned long long)address, described ? " " : "", described ? description : ""));
	return length;
}

// The raw stack words from esp under a "stack from esp=0x..." line, one line
// each: "stack+0x04: 0x69FFA5E0 name+0x5E0 (unloaded)". `describe(word, buffer,
// size)` fills the annotation of a word that points into an image.
template <class Describe>
inline size_t FormatStackWords(char* out, size_t size, uint32_t esp, const uint32_t* words, size_t count, Describe&& describe) {
	if (!out || !size) return 0;
	out[0] = '\0';
	size_t length = 0;
	if (!words || !count) return 0;
	if (!Advance(out, size, length, snprintf(out, size, "stack from esp=0x%08X:\n", esp))) return 0;
	for (size_t i = 0; i < count; ++i) {
		char label[16], description[96];
		snprintf(label, sizeof(label), "stack+0x%02X", (unsigned)(i * 4));
		description[0] = '\0';
		describe(words[i], description, sizeof(description));
		const size_t line = FormatLocatedAddress(out + length, size - length, label, words[i], description);
		if (!line) break;
		length += line;
	}
	return length;
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
