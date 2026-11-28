#pragma once

// The last check on the game's merged render command stream before its range
// walk runs (sf4e__RenderStream.hxx). The writers of the stream check no
// capacity, so a frame that records more than its buffer holds writes on into
// the range table behind it; a range then starts at stream data instead of an
// index, and the walk faults or spins forever on a zero command word.
//
// A range is a dword index into the stream; the walk runs its command words,
// each (op << 16) | length in dwords, up to the whole word 0x10000. A range is
// sound when that walk starts and ends inside the written stream and every
// word moves it forward. A range overwritten with a small number can still
// pass: this catches what the field dumps held, not every overflow.

#include <cstddef>
#include <cstdint>

namespace sf4e { namespace render {

constexpr std::uint32_t RangeTerminator = 0x10000;

struct Range {
    std::uint32_t firstDword;
    std::uint32_t sortKey;
};

enum class RangeFault {
    None,
    StartOutside,
    // A command word of length 0: the walk would never move on.
    ZeroLength,
    // The walk leaves the written stream before its terminator.
    RunsPastEnd,
};

inline RangeFault CheckRange(const std::uint32_t* stream, std::size_t writtenDwords, Range range) {
    std::size_t at = range.firstDword;
    if (at >= writtenDwords) return RangeFault::StartOutside;
    while (stream[at] != RangeTerminator) {
        const std::size_t length = stream[at] & 0xFFFF;
        if (length == 0) return RangeFault::ZeroLength;
        at += length;
        if (at >= writtenDwords) return RangeFault::RunsPastEnd;
    }
    return RangeFault::None;
}

struct KeptRanges {
    std::size_t kept = 0;
    // The first unsound range, as it was in the table.
    RangeFault fault = RangeFault::None;
    std::size_t firstBad = 0;
    std::uint32_t badStart = 0;
};

// Drops the unsound ranges from the table, keeping the others in their order
// at its front. The walk then replays every sound range: a range left out
// keeps the resource references its commands hold.
inline KeptRanges KeepSoundRanges(const std::uint32_t* stream, std::size_t writtenDwords, Range* table, std::size_t count) {
    KeptRanges result;
    for (std::size_t i = 0; i < count; ++i) {
        const RangeFault fault = CheckRange(stream, writtenDwords, table[i]);
        if (fault == RangeFault::None) {
            table[result.kept++] = table[i];
        } else if (result.fault == RangeFault::None) {
            result.fault = fault;
            result.firstBad = i;
            result.badStart = table[i].firstDword;
        }
    }
    return result;
}

inline const char* FaultName(RangeFault fault) {
    switch (fault) {
    case RangeFault::None: return "none";
    case RangeFault::StartOutside: return "starts outside the stream";
    case RangeFault::ZeroLength: return "has a zero-length command";
    case RangeFault::RunsPastEnd: return "runs past the stream";
    }
    return "?";
}

}} // namespace sf4e::render
