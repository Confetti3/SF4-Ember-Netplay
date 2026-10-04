#pragma once
#include <cstddef>
#include <cstdint>

namespace sf4e { namespace ui {
// A font's line span in em: hhea ascender minus descender over head.unitsPerEm,
// read from the sfnt tables of the bytes as embedded. TrueType and CFF outlines
// share the layout. 0 when the data is not a font this can read, so a caller
// never divides by it unchecked.
inline float FontLineSpanEm(const unsigned char* data, std::size_t size) {
    if (!data || size < 12) return 0.f;
    const auto u16 = [&](std::size_t at) { return static_cast<unsigned>(data[at] << 8 | data[at + 1]); };
    const auto s16 = [&](std::size_t at) { const unsigned v = u16(at); return v >= 0x8000u ? static_cast<int>(v) - 0x10000 : static_cast<int>(v); };
    const auto u32 = [&](std::size_t at) { return static_cast<std::uint32_t>(u16(at)) << 16 | u16(at + 2); };
    const std::uint32_t tag = u32(0);
    // 0x00010000 TrueType, 'OTTO' CFF, 'true' Apple. A collection is not embedded.
    if (tag != 0x00010000u && tag != 0x4F54544Fu && tag != 0x74727565u) return 0.f;
    const unsigned tables = u16(4);
    if (12 + static_cast<std::size_t>(tables) * 16 > size) return 0.f;
    std::size_t head = 0, hhea = 0;
    for (unsigned i = 0; i < tables; ++i) {
        const std::size_t entry = 12 + static_cast<std::size_t>(i) * 16;
        const std::uint32_t name = u32(entry), offset = u32(entry + 8);
        if (name == 0x68656164u) head = offset;       // 'head'
        else if (name == 0x68686561u) hhea = offset;  // 'hhea'
    }
    // head.unitsPerEm is at 18, hhea.ascender at 4 and descender at 6.
    if (!head || !hhea || head + 20 > size || hhea + 8 > size) return 0.f;
    const unsigned unitsPerEm = u16(head + 18);
    const int span = s16(hhea + 4) - s16(hhea + 6);
    if (!unitsPerEm || span <= 0) return 0.f;
    return static_cast<float>(span) / static_cast<float>(unitsPerEm);
}
} }
