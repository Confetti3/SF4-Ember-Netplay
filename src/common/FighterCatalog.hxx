#pragma once
#include "SelectionPick.hxx"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sf4e { namespace selection {

constexpr int FighterCount = 44;
constexpr int UltraEdition = 14;
// Ultra I, Ultra II and, in Ultra's own edition, both (Pick::ultra 0 to 2).
constexpr int UltraChoices = 3;
// Native fighter IDs in USFIV's own character-select order, row-major with 15 to
// a row (the game's last cell is Random, which Ember has no card for). Display
// only: native IDs and everything saved or sent stay as they are.
constexpr int RosterDisplayOrder[FighterCount] = {
    43, 0, 1, 3, 29, 30, 31, 16, 18, 17, 25, 26, 23, 38, 40,
    35, 34, 2, 7, 15, 12, 11, 10, 20, 22, 28, 27, 33, 39, 41,
    37, 6, 4, 5, 13, 14, 9, 8, 21, 19, 32, 24, 36, 42};

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

// The pick and the native availability are SelectionPick.hxx's. The custom
// content installed on this PC (InstalledContent.hxx): the selection queries
// below take it alongside the native availability, and without it offer the
// game's own content only.

// Custom costumes, added by a separate setup program: slots 8..99 (indices 7..98) of every fighter, ten colors
// each. Any of them is a valid pick on the network; each PC shows its own install of that slot, or the
// fighter's original costume (ResolveAppearance).
constexpr int FirstCustomCostume = 7;
constexpr int CostumeLimit = 99;
constexpr int CustomColorCount = 10;
inline bool IsCustomCostume(int costumeId) { return costumeId >= FirstCustomCostume && costumeId < CostumeLimit; }
// Custom colors, added the same way: colors 30..99 (indices 29..98) of any costume, the game's or a custom one,
// leaving the game's own colors (up to 22) room.
// Each PC shows its own install of that color, or the costume's color 1 (ResolveAppearance).
constexpr int FirstCustomColor = 29;
inline bool IsCustomColor(int colorId) { return colorId >= FirstCustomColor && colorId < CostumeLimit; }
// The costume's own colors, or a custom color.
bool ColorInRange(int fighterId, int costumeId, int colorId);

int BaseCostumeCount(int fighterId);
int CostumeCount(int fighterId);      // the game's own costumes
int ColorCount(int fighterId, int costumeId);
const char* CostumePack(int fighterId, int costumeId);
std::vector<int> AllowedUltras(int fighterId, int editionId);
// What an Ultra's command needs besides its motion and buttons: where the
// fighter is, a stance, how it started, or a press after it.
enum class UltraCondition {
    None, Ground, AntiAir, GroundOrAir, InAir, CloseRange, Counter, MantisStance, CraneStance, CraneStanceOrAir,
    PunchStart, KickStart, Forward, DelayOrCancel, StopHealing
};
struct UltraCommand {
    // Space-separated motion and button tokens, facing right: numpad
    // directions ("236"), a leading ~ for a charge, PPP or KKK for all three.
    const char* symbols = nullptr;
    UltraCondition condition = UltraCondition::None;
};
// Ultra I's or II's commands in an edition, one per way into the move (Gen's
// two stances, Decapre's ground and anti-air), or none where the fighter has
// no such Ultra there. Ultra Double has no input of its own: ask for I and II.
// Sources and edition exceptions: docs/design/ULTRA_INPUTS.md.
std::vector<UltraCommand> UltraCommands(int fighterId, int ultraId, int editionId);
// Facing-neutral short notation for a token string, the same whichever side
// the player starts on: "236 236 + PPP" is "QCF x2 + PPP", "~4 6 4 6 + KKK" is
// "Charge B, F, B, F + KKK", with `charge` the word for a charge.
std::string UltraNotation(const std::string& tokens, const std::string& charge = "Charge");
// The player does not own this fighter. Only 35-43 are sold separately;
// unknown availability is not locked.
bool FighterLocked(int fighterId, const Availability& availability);
std::vector<int> AllowedCostumes(int fighterId, const Availability& availability, const InstalledContent* installed = nullptr);
std::vector<int> AllowedColors(int fighterId, int costumeId, const Availability& availability, const InstalledContent* installed = nullptr);
std::vector<int> AllowedPersonalActions(const Availability& availability);
// Network validity uses the game's supported options; local availability also
// applies the current player's unlock/DLC state. Never compare a peer's DLC
// ownership against the receiving player's ownership.
bool Valid(const Pick& pick, bool editionSelect);
bool Available(const Pick& pick, bool editionSelect, const Availability& availability, const InstalledContent* installed = nullptr);
bool Normalize(Pick& pick, bool editionSelect, const Availability* availability = nullptr, const InstalledContent* installed = nullptr);

// The same side with another fighter: its first costume and color, and the
// edition, Ultra and the rest kept where that fighter has them, else made
// valid for it (Normalize).
template<class Native> void ChangeFighter(Native& native, int fighterId, bool editionSelect) {
    Pick pick = FromNative(native);
    pick.fighter = fighterId; pick.costume = pick.color = 0;
    Normalize(pick, editionSelect);
    ToNative(pick, native);
}

} }
