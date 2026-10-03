#pragma once

// The engine's Vfx::Object memento (record 0x5BBC60, restore 0x5BBCB0) moves
// 0xE4 dwords between the memento and object+0x174, which ends 4 bytes past
// the 0x500-byte object. The containers keep objects in arrays (16 reserved,
// 32 loose), so for the last object of each array the extra dword is the heap
// header of the next block. Rollback recorded it, the game reallocated that
// block, and restore wrote the old size back into the new header; the next
// free of the block failed with 0xC0000374. Copying 0xE3 dwords stops at the
// end of the object.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sf4e { namespace vfx_memento {

const std::size_t kObjectStride = 0x500;
const std::size_t kCopyOffset = 0x174;
const std::uint32_t kNativeDwords = 0xE4;
const std::uint32_t kFixedDwords = 0xE3;

static_assert(kCopyOffset + kNativeDwords * 4 == kObjectStride + 4, "the native copy overruns by one dword");
static_assert(kCopyOffset + kFixedDwords * 4 == kObjectStride, "the fixed copy ends with the object");

enum class Patch { Applied, AlreadyApplied, Unexpected };

// `instruction` is the `mov ecx, imm32` (B9 imm32) that sets the rep movsd
// count. Anything other than the native or fixed count is left alone.
inline Patch ClampCopyCount(std::uint8_t* instruction) {
    if (instruction[0] != 0xB9) return Patch::Unexpected;
    std::uint32_t count = 0;
    std::memcpy(&count, instruction + 1, sizeof(count));
    if (count == kFixedDwords) return Patch::AlreadyApplied;
    if (count != kNativeDwords) return Patch::Unexpected;
    std::memcpy(instruction + 1, &kFixedDwords, sizeof(kFixedDwords));
    return Patch::Applied;
}

inline const char* Describe(Patch patch) {
    switch (patch) {
    case Patch::Applied: return "applied";
    case Patch::AlreadyApplied: return "already applied";
    default: return "unexpected bytes, left alone";
    }
}

}}
