#pragma once

// The battle's render command buffer is made once at startup with the sizes
// EvaApp::onInit (0x405210) pushes as immediates: `push 0xC00` ranges
// (68 00 0C 00 00) and `push 0x50000` dwords (68 00 00 05 00). Only the
// allocator reads them, so editing both before the game starts doubles the
// buffer. Both pushes change or neither does.
#include <cstdint>

#include "CodeEdit.hxx"

namespace sf4e { namespace render {

constexpr std::uint32_t NativeRanges = 0xC00, NativeDwords = 0x50000;
// Twice the native sizes: 2.55 MB instead of 1.27 MB, allocated once from the
// CRT heap. The merged copy of each frame still takes only what was written.
constexpr std::uint32_t WideRanges = 2 * NativeRanges, WideDwords = 2 * NativeDwords;

inline code_edit::Edit PushEdit(std::uint8_t* at, std::uint32_t native, std::uint32_t wide) {
    code_edit::Edit edit = { at, 5, { 0x68 }, { 0x68 } };
    for (int i = 0; i < 4; ++i) {
        edit.native[1 + i] = static_cast<std::uint8_t>(native >> (8 * i));
        edit.patched[1 + i] = static_cast<std::uint8_t>(wide >> (8 * i));
    }
    return edit;
}

// False, nothing changed, for bytes that are not those pushes or a place
// that cannot be made writable.
template <class Pages>
bool WidenBattleBuffer(std::uint8_t* rangesPush, std::uint8_t* dwordsPush, Pages& pages) {
    const code_edit::Edit edits[] = {
        PushEdit(rangesPush, NativeRanges, WideRanges),
        PushEdit(dwordsPush, NativeDwords, WideDwords),
    };
    return code_edit::ApplyAll(edits, pages);
}

}}
