#pragma once

// DbgHelp stores each thread's stack from just below its stack pointer, but
// when that pointer lies below the TEB's StackLimit it stores the whole
// [StackLimit, StackBase) range instead and leaves the live frames out (Wine's
// dbghelp does this; its stack grows past a StackLimit it never moved, and a
// frozen game's main thread sat 1 MB below it). Wine's dbghelp also ignores
// MemoryCallback, so the missing bytes ride in a stream of our own.
//
// Pure arithmetic and encoding, no Windows calls, so a test can run it anywhere.

#include <stdint.h>
#include <string.h>
#include <vector>

namespace sf4e {
namespace crash {

// Above the range Microsoft reserves for its own streams (0xFFFF).
constexpr uint32_t StackSupplementStream = 0x53463401u;
constexpr uint32_t StackSupplementVersion = 1;
// All threads together. Each thread's range runs to StackLimit with no gap, so
// the reader can join it to the stored stack; the 1 MB of the first dump that
// showed this fits twice over. A stack that does not fit is cut at esp's end.
constexpr uint32_t StackSupplementTotal = 2 * 1024 * 1024;
// A stack pointer further below the limit than any stack reserves is garbage.
constexpr uint32_t StackSupplementMaxGap = 16 * 1024 * 1024;

struct ThreadStackBounds {
	uint32_t threadId;
	uint32_t esp;
	uint32_t limit;
	uint32_t base;
};

struct StackRange {
	uint32_t threadId;
	uint32_t start;
	uint32_t size;
};

// A range as read from the game: only reads that returned bytes become one.
struct StackCopy {
	uint32_t threadId;
	uint32_t start;
	std::vector<unsigned char> bytes;
};

// The part of the stack DbgHelp leaves out for each thread: from the word below
// esp (where it would have started) up to StackLimit, cut to the total above.
// Nothing for a thread whose esp it keeps, or one whose bounds make no sense.
inline std::vector<StackRange> PlanStackSupplement(const std::vector<ThreadStackBounds>& threads) {
	std::vector<StackRange> ranges;
	uint32_t total = 0;
	for (const auto& thread : threads) {
		if (thread.esp < 4 || thread.limit >= thread.base) continue;
		const uint32_t start = thread.esp - 4;
		if (start >= thread.limit || thread.limit - start > StackSupplementMaxGap) continue;
		uint32_t size = thread.limit - start;
		if (size > StackSupplementTotal - total) size = StackSupplementTotal - total;
		if (!size) break;
		ranges.push_back({ thread.threadId, start, size });
		total += size;
	}
	return ranges;
}

// Stream: version, count, then one {threadId, start, size} per range, then the
// bytes of each range in order. Little endian, as every x86 host writes it.
inline void AppendWord(std::vector<unsigned char>& out, uint32_t value) {
	for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<unsigned char>(value >> shift));
}

inline std::vector<unsigned char> EncodeStackSupplement(const std::vector<StackCopy>& copies) {
	std::vector<unsigned char> out;
	if (copies.empty()) return out;
	AppendWord(out, StackSupplementVersion);
	AppendWord(out, static_cast<uint32_t>(copies.size()));
	for (const auto& copy : copies) {
		AppendWord(out, copy.threadId);
		AppendWord(out, copy.start);
		AppendWord(out, static_cast<uint32_t>(copy.bytes.size()));
	}
	for (const auto& copy : copies) out.insert(out.end(), copy.bytes.begin(), copy.bytes.end());
	return out;
}

} // namespace crash
} // namespace sf4e
