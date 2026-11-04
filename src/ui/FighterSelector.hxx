#pragma once
#include "../common/FighterCatalog.hxx"
#include "../common/InstalledContent.hxx"
#include "../common/StageCatalog.hxx"
#include "SelectionArt.hxx"
#include "GameMenu.hxx"
#include "SelectionLabels.hxx"
#include <functional>
#include <string>

namespace sf4e { namespace ui {
bool DrawStageSelector(int& nativeId, SelectionArt* art);
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
