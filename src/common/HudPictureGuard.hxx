#pragma once

// A battle HUD sprite takes its picture from the textures of its sheet by the
// number its layout names (SSFIV.exe 0x6ED070). The game checks only that the
// number is not negative, then reads items[number] and that texture's width:
// a number past the sheet's last picture (a layout from a newer edition than
// its sheet), or a texture the game could not make (the list is cut short at
// the first one; under Wine a picture's texture can fail), reads through a
// bad pointer. Such a picture is drawn as the game draws a sprite with none.
#include <cstdint>

namespace sf4e { namespace hud_picture {

// Whether picture `picture` of a texture list of `count` textures at `items`
// is not there. A negative number, or a sprite with no list, is the game's own
// case of no picture and is left to it.
inline bool Missing(bool hasList, const void* const* items, std::uint32_t count, std::int32_t picture) {
    if (!hasList || picture < 0) return false;
    return !items || static_cast<std::uint32_t>(picture) >= count || !items[picture];
}

// One line a battle names the first missing picture; the rest are counted.
class Skips {
public:
    // A new battle's HUD.
    void Reset() { count_ = 0; }
    // Counts a skip; true for the first of the battle, the one to log.
    bool Note() { return count_++ == 0; }
    unsigned Count() const { return count_; }
private:
    unsigned count_ = 0;
};

} }
