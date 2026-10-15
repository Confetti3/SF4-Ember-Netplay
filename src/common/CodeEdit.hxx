#pragma once

// Edits Ember makes to the game's code: a few bytes that read one way in this
// game build and another once edited. `pages` gives write access to the code:
// `pages.Unlock(at, length, saved)` makes the bytes writable and keeps their
// protection in `saved`; `pages.Lock(at, length, saved)` puts it back.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sf4e { namespace code_edit {

// `length` bytes at `at` that read `native` and become `patched`.
struct Edit {
    std::uint8_t* at;
    std::uint8_t length;
    std::uint8_t native[6];
    std::uint8_t patched[6];
};

// Makes every edit or none: some of them alone would change the game with the
// setting off. Every place is made writable before any changes, so nothing has
// to be undone.
template <std::size_t N, class Pages>
bool ApplyAll(const Edit (&edits)[N], Pages& pages) {
    for (const Edit& edit : edits)
        if (std::memcmp(edit.at, edit.native, edit.length) != 0) return false;
    unsigned long saved[N] = {};
    std::size_t unlocked = 0;
    while (unlocked < N && pages.Unlock(edits[unlocked].at, edits[unlocked].length, saved[unlocked])) ++unlocked;
    const bool all = unlocked == N;
    if (all)
        for (const Edit& edit : edits) std::memcpy(edit.at, edit.patched, edit.length);
    while (unlocked--) pages.Lock(edits[unlocked].at, edits[unlocked].length, saved[unlocked]);
    return all;
}

// Whether the place reads the edit's patched or its native bytes.
inline bool Reads(const Edit& edit, bool patched) {
    return std::memcmp(edit.at, patched ? edit.patched : edit.native, edit.length) == 0;
}

// Puts one edit in its patched or its native state, as often as asked. Bytes
// already there need no write; bytes that are neither (another game build)
// are left alone, as is a place that cannot be made writable.
template <class Pages>
bool Set(const Edit& edit, bool patched, Pages& pages) {
    if (Reads(edit, patched)) return true;
    unsigned long saved = 0;
    if (!Reads(edit, !patched) || !pages.Unlock(edit.at, edit.length, saved)) return false;
    std::memcpy(edit.at, patched ? edit.patched : edit.native, edit.length);
    pages.Lock(edit.at, edit.length, saved);
    return true;
}

}}
