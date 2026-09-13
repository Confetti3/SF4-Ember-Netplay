#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace sf4e { namespace selection {

constexpr int FighterCount = 44;
constexpr int UltraEdition = 14;

struct Fighter {
    const char* code;
    const char* name;
    std::array<const char*, 2> ultras;
    std::uint32_t editions;
};

struct Edition {
    int id;
    const char* name;
    const char* shortName;
};

// A lookup failure is explicit: invalid input must not index the native tables.
const Fighter* FindFighter(int nativeId);
const std::array<Edition, 6>& EditionList();
const Edition* FindEdition(int nativeId);
std::vector<int> AllowedEditions(int fighterId, bool editionSelect);
bool EditionAllowed(int fighterId, int editionId, bool editionSelect);
int NormalizeEdition(int fighterId, int editionId, bool editionSelect);

struct Pick {
    int fighter = 0;
    int costume = 0;
    int color = 0;
    int ultra = 0;
    int edition = UltraEdition;
    int personalAction = 0;
    int winQuote = 0;
    int handicap = 0;
};

// Native customization stores a separate availability bit for each costume and
// each color. Asset counts alone cannot represent this: colors 11/12 use shader
// variants, and newer costumes also expose the numbered palettes 13 through 22.
struct Availability {
    bool ready = false;
    std::uint32_t costumes = 0;
    std::array<std::uint32_t, 8> colors{};
    std::uint16_t personalActions = 0;
};

int BaseCostumeCount(int fighterId);
int CostumeCount(int fighterId);
int ColorCount(int fighterId, int costumeId);
const char* CostumePack(int fighterId, int costumeId);
std::vector<int> AllowedUltras(int fighterId, int editionId);
struct UltraCommand {
    const char* symbols;
    const char* condition;
};
// Space-separated motion/button tokens, facing right. A leading ~ means charge.
// Double has no independent input; ask for each constituent Ultra instead.
std::vector<UltraCommand> UltraCommands(int fighterId, int ultraId, int editionId);
std::vector<int> AllowedCostumes(int fighterId, const Availability& availability);
std::vector<int> AllowedColors(int fighterId, int costumeId, const Availability& availability);
std::vector<int> AllowedPersonalActions(const Availability& availability);
// Network validity uses the game's supported options; local availability also
// applies the current player's unlock/DLC state. Never compare a peer's DLC
// ownership against the receiving player's ownership.
bool Valid(const Pick& pick, bool editionSelect);
bool Available(const Pick& pick, bool editionSelect, const Availability& availability);
bool Normalize(Pick& pick, bool editionSelect, const Availability* availability = nullptr);

template<class Native> Pick FromNative(const Native& native) {
    Pick pick;
    pick.fighter = native.charaID; pick.costume = native.costume;
    pick.color = native.color; pick.ultra = native.ultraCombo; pick.edition = native.unc_edition;
    pick.personalAction = native.personalAction; pick.winQuote = native.winQuote; pick.handicap = native.handicap;
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
}

} }
