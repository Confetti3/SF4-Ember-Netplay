#pragma once
#include "../common/FighterCatalog.hxx"
#include "../common/InstalledContent.hxx"
#include "../common/StageCatalog.hxx"
#include "SelectionArt.hxx"
#include "GameMenu.hxx"
#include <functional>
#include <string>

namespace sf4e { namespace ui {
bool DrawStageSelector(int& nativeId, SelectionArt* art);
// The names every screen uses for a costume ("Original", "Alternate 1 / pack",
// a custom costume's installed name or "Custom 12") and an Ultra, so a saved
// choice reads the same as the card that made it.
std::string CostumeLabel(const selection::Pick& pick, const selection::InstalledContent* installed = nullptr);
const char* UltraLabel(int ultra);
// Home's short forms of the same: "Alt 1" (a pack costume by its pack's name
// alone, one costume each), "Alt 1, color 3" and "U1", "U2" or "Double".
std::string ShortCostumeLabel(const selection::Pick& pick, const selection::InstalledContent* installed = nullptr);
std::string ShortAppearanceLabel(const selection::Pick& pick, const selection::InstalledContent* installed = nullptr);
const char* ShortUltraLabel(int ultra);
// The fighter's Ultra by the game's own name, wherever a player chooses or
// readies with it: "Ultra I: Metsu Hadoken", or "Ultra Double" (UltraName);
// the move alone, "Metsu Hadoken" (UltraMove). Empty for no fighter.
std::string UltraName(int fighter, int ultra);
std::string UltraMove(int fighter, int ultra);
// A stage's name (a custom stage's installed name, or "Custom C12"), or Random.
std::string StageLabel(int stageId, const selection::InstalledContent* installed = nullptr);
class FighterSelector {
public:
    MenuNavigation& Navigation() { return menu_.navigation; }
    using AvailabilityReader = std::function<selection::Availability(int)>;
    // selectionError explains why the current choice is not usable. The room
    // screens tell the player to open Fighter Select, so Fighter Select has to
    // be able to say what is wrong once they arrive.
    // randomPool, given with stageId, adds the Random stage pool page. installed
    // adds the custom costumes, colors and stages installed on this PC.
    // fighterKept: an Arcade room keeps this player, its last winner, on their
    // fighter (room::KeepsFighter); the fighter and its edition cannot change.
    // random, where the pick can be Random, adds the grid's Random card and
    // holds the choice: Random saved, `pick` is the fighter it rolled
    // (docs/design/RANDOM_FIGHTER.md). Training's dummy has none.
    bool Draw(selection::Pick& pick, bool editionSelect, SelectionArt* art, const AvailabilityReader& readAvailability,
              int* stageId = nullptr, bool editable = true, const std::string& selectionError = {},
              selection::RandomPool* randomPool = nullptr, const selection::InstalledContent* installed = nullptr,
              bool fighterKept = false, bool* random = nullptr);
private:
    // The board: the grid eight across, nothing beside it (AxPick.dc.html).
    // randomSaved marks the Random card saved instead of the rolled fighter.
    static constexpr int BoardColumns = 8;
    void DrawBoard(const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float height,
                   const selection::Pick& pick, bool randomSaved, SelectionArt* art, const AvailabilityReader& readAvailability);
    std::string boardFocus_;
    GameMenu menu_;
    // The page ("stage" or "options") opened for one change
    // (EmbeddedReturn::openOn), or empty when the selector opened on its board.
    std::string openOn_;
};
} }
