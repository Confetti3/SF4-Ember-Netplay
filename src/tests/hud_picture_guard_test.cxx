// A HUD sprite's picture is read only when its sheet has it: a number past the
// sheet's last picture, or a texture the game could not make, is drawn as no picture.
#include "../common/HudPictureGuard.hxx"
#include <cstdint>

#include "test_support.hxx"

namespace guard = sf4e::hud_picture;

// The game's texture list as 0x6ED070 reads it: items at +4, count at +0xC.
struct FakeList {
    std::uint32_t vtbl = 0;
    const void* const* items = nullptr;
    std::uint32_t capacity = 0, count = 0;
};

int main() {
    int textures[3] = {};
    const void* items[16] = { &textures[0], &textures[1], &textures[2] };   // the rest: slots past the count
    FakeList list;
    list.items = items; list.capacity = 16; list.count = 3;
    const auto missing = [&](const FakeList* l, std::int32_t picture) { return guard::Missing(l != nullptr, l ? l->items : nullptr, l ? l->count : 0, picture); };

    // The pictures the sheet has are read.
    CHECK(!missing(&list, 0) && !missing(&list, 2));
    // The game's own cases of no picture are left to it.
    CHECK(!missing(&list, -1) && !missing(nullptr, 5));
    // Past the last picture (an Ultra cockpit layout naming 9 to 14 with a 9 picture sheet), even within the capacity.
    CHECK(missing(&list, 3) && missing(&list, 14) && missing(&list, 0x7FFFFFFF));
    // A texture the game could not make.
    items[1] = nullptr;
    CHECK(missing(&list, 1) && !missing(&list, 2));
    // A list without items.
    list.items = nullptr;
    CHECK(missing(&list, 0));

    // One line a battle: the first skip is logged, the rest counted, and a new battle starts again.
    guard::Skips skips;
    CHECK(skips.Note() && !skips.Note() && !skips.Note() && skips.Count() == 3);
    skips.Reset();
    CHECK(skips.Count() == 0 && skips.Note());
    return 0;
}
