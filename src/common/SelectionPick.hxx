#pragma once
// A player's pick as Ember's shared code carries it, and the game's report of
// what the player owns. The pick is the wire's ConfirmedCharaConditions with
// SF4's field names, which the wire keeps (the library plan, section 6).
// Nothing here knows a roster: which picks are valid or owned, and how they
// are named, is the game's to say through its catalog (GameCatalog.hxx).
//
// C++11 and no game header: the Linux room host compiles it with g++ alone.
#include <array>
#include <cstdint>
#include <memory>

namespace sf4e { namespace selection {

struct Pick {
    int fighter = 0;
    int costume = 0;
    int color = 0;
    int ultra = 0;
    // SF4's Ultra edition (FighterCatalog's UltraEdition), the wire's default;
    // a game without editions ignores it.
    int edition = 14;
    int personalAction = 0;
    int winQuote = 0;
    int handicap = 0;
    // A game's own model size choice: 0 normal, 1 tiny (SFV's Tiny fighters
    // allowed rooms). The picture only; nothing of the fight changes with it.
    // SF4 always 0: its catalog never accepts another. It rides in the wire
    // pick's spare byte (ConfirmedCharaConditions::_unused).
    int size = 0;
};

// What the player owns of one fighter, as the game reports it
// (GameState::FighterAvailability). Only the game's own catalog rules read
// it. The layout is SF4's: native customization stores a separate
// availability bit for each costume and each color. Asset counts alone cannot
// represent this: colors 11/12 use shader variants, and newer costumes also
// expose the numbered palettes 13 through 22.
struct Availability {
    bool ready = false;
    // Costumes the player's Steam licences cover. Bit 0 is the fighter itself.
    std::uint32_t costumes = 0;
    std::array<std::uint32_t, 8> colors{};
    std::uint16_t personalActions = 0;
};

// The custom content installed on this PC (SF4's InstalledContent.hxx). The
// shared code only hands it on, to the game's catalog rules and labels.
struct InstalledContent;
typedef std::shared_ptr<const InstalledContent> InstalledContentPtr;

template<class Native> Pick FromNative(const Native& native) {
    Pick pick;
    pick.fighter = native.charaID; pick.costume = native.costume;
    pick.color = native.color; pick.ultra = native.ultraCombo; pick.edition = native.unc_edition;
    pick.personalAction = native.personalAction; pick.winQuote = native.winQuote; pick.handicap = native.handicap;
    // The spare byte: 1 is tiny, anything else (SF4's padding) normal.
    pick.size = native._unused == 1 ? 1 : 0;
    return pick;
}
template<class Native> void ToNative(const Pick& pick, Native& native) {
    native.charaID = static_cast<std::uint8_t>(pick.fighter);
    native.costume = static_cast<std::uint8_t>(pick.costume);
    native.color = static_cast<std::uint8_t>(pick.color);
    native.ultraCombo = static_cast<std::uint8_t>(pick.ultra);
    native.unc_edition = static_cast<std::uint8_t>(pick.edition);
    native.personalAction = static_cast<std::uint8_t>(pick.personalAction);
    native.winQuote = static_cast<std::uint8_t>(pick.winQuote);
    native.handicap = static_cast<std::uint8_t>(pick.handicap);
    native._unused = static_cast<std::uint8_t>(pick.size);
}

} }
