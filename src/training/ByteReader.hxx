#pragma once
#include <cstddef>
#include <cstdint>

// Little-endian reads from a game file held in memory, shared by the file
// readers. Offsets are 64-bit so a 32-bit offset from the file can be added
// to a position without wrapping, on a 32-bit build as well. Has guards a
// read; the reads themselves do not check.
namespace sf4e {
struct ByteReader {
    const std::uint8_t* data;
    std::size_t size;
    bool Has(std::uint64_t at, std::uint64_t count) const { return at <= size && count <= size - at; }
    std::uint16_t U16(std::uint64_t at) const { return static_cast<std::uint16_t>(data[at] | data[at + 1] << 8); }
    std::uint32_t U32(std::uint64_t at) const { return U16(at) | static_cast<std::uint32_t>(U16(at + 2)) << 16; }
};
}
