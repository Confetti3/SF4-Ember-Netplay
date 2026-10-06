#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

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
    std::int32_t I32(std::uint64_t at) const { return static_cast<std::int32_t>(U32(at)); }
    bool Zero(std::uint64_t at, std::size_t count) const {
        for (std::size_t i = 0; i < count; ++i) if (data[at + i]) return false;
        return true;
    }
    // A slot holds printable ASCII, a NUL, then only zeros.
    bool Text(std::uint64_t at, std::size_t slot, std::string& out) const {
        std::size_t length = 0;
        while (length < slot && data[at + length]) {
            if (data[at + length] < 0x20 || data[at + length] > 0x7E) return false;
            ++length;
        }
        if (length == slot || !Zero(at + length, slot - length)) return false;
        out.assign(reinterpret_cast<const char*>(data + at), length);
        return true;
    }
};
}
