#pragma once
// The custom content a separate setup program installed on this PC (Ember never downloads anything), and how a
// match's picks are shown with it. Read once when the game starts (sf4e::custom scans the patch folder) and never
// changed afterwards, so it is shared read-only (InstalledContentPtr) by the hooks, the runtime snapshot and the
// selectors. The native unlocks stay in Availability; the selection queries take both.
#include "FighterCatalog.hxx"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace sf4e { namespace selection {

struct InstalledContent {
    // Custom costumes (IsCustomCostume) of each fighter that have all ten colors, ascending.
    std::array<std::vector<int>, FighterCount> costumes;
    // Custom colors (IsCustomColor) of a fighter's costume, the game's or an installed custom one, ascending.
    std::map<std::pair<int, int>, std::vector<int>> colors;
    // Custom stages (StageCatalog's IsCustomStage), ascending.
    std::vector<int> stages;
    // The names the setup program gave them: (fighter, costume, color or -1 for the costume itself), and by stage id.
    std::map<std::tuple<int, int, int>, std::string> names;
    std::map<int, std::string> stageNames;

    const std::vector<int>& Costumes(int fighterId) const;
    const std::vector<int>& Colors(int fighterId, int costumeId) const;
    bool HasCostume(int fighterId, int costumeId) const;
    bool HasColor(int fighterId, int costumeId, int colorId) const;
    bool HasStage(int stageId) const;
    // Empty when it has no name.
    std::string Name(int fighterId, int costumeId, int colorId = -1) const;
    std::string StageName(int stageId) const;
};
typedef std::shared_ptr<const InstalledContent> InstalledContentPtr;

// One side's look as the game is handed it.
struct Look {
    Look() : fighter(0), costume(0), color(0) {}
    Look(int fighter, int costume, int color) : fighter(fighter), costume(costume), color(color) {}
    int fighter, costume, color;
    bool operator==(const Look& o) const { return fighter == o.fighter && costume == o.costume && color == o.color; }
};
// Content the game is given in place of what it loads: while it builds the file names of `fighter`'s costume
// `standIn` it is given costume `custom`; likewise color `standIn` of costume `costume` (as the game sees it, a custom
// costume's stand-in) and custom color `custom`. `custom` -1: no swap.
struct CostumeSwap {
    CostumeSwap() : fighter(-1), standIn(-1), custom(-1) {}
    CostumeSwap(int fighter, int standIn, int custom) : fighter(fighter), standIn(standIn), custom(custom) {}
    int fighter, standIn, custom;
};
struct ColorSwap {
    ColorSwap() : fighter(-1), costume(-1), standIn(-1), custom(-1) {}
    ColorSwap(int fighter, int costume, int standIn, int custom) : fighter(fighter), costume(costume), standIn(standIn), custom(custom) {}
    int fighter, costume, standIn, custom;
};
struct Appearance {
    Look shown[2];              // what this PC hands the game for each side
    CostumeSwap costumes[2];
    ColorSwap colors[2];
};

// What this PC shows for both picks of a match (fighters and spectators alike), from what is installed here:
// - a custom costume it doesn't have becomes the fighter's original costume in color 1, a custom color it doesn't
//   have color 1 of that costume;
// - two sides that would look the same (same fighter, costume and color, as picked or after falling back): the
//   second side takes color 1, or color 2 when the first is in color 1, so every PC shows the same two looks;
// - a custom costume it has plays as a stand-in: the fighter's original costume, or in a mirror match where the
//   other side wears that, the next of the first three it isn't using (the file-name swap can't tell the two
//   sides' loads apart); a custom color it has (of a game costume or a custom one) plays as color 1 the same way,
//   or the next the other side isn't using.
// The match data itself stays as sent; replays record the stand-ins, so they never hold custom content.
Appearance ResolveAppearance(const Look (&picks)[2], const InstalledContent& installed);

} }
