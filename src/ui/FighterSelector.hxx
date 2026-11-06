#pragma once
#include "../common/FighterCatalog.hxx"
#include "../common/InstalledContent.hxx"
#include "../common/StageCatalog.hxx"
#include "SelectionArt.hxx"
#include "GameMenu.hxx"
#include "AppearanceGallery.hxx"
#include "SelectionLabels.hxx"
#include <functional>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
bool DrawStageSelector(int& nativeId, SelectionArt* art);
// One command of an Ultra's input: the Ultra it is (I or II; Ultra Double has
// Ultra I's and II's), its symbols (UltraCommand::symbols), and the same in
// words, facing-neutral, with what it needs after it: "QCF x2 + PPP",
// "Charge B, F, B, F + KKK (Ground)" (UltraNotation).
struct UltraInput { int ultra = 0; const char* symbols = nullptr; std::string words; };
// The Ultra's input in the edition, a command each, in order. None for an
// Ultra the edition does not have.
std::vector<UltraInput> UltraInputs(int fighter, int ultra, int edition);
// A picture fitted inside the box and centred, or, before it is loaded or
// where it is missing, a word that says so.
void ImageInRect(const SelectionImage& image, ImVec2 min, ImVec2 max);
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
    // The costume, colour and Ultra grids over the fighter's page.
    AppearanceGallery gallery_;
    // Where the selector was opened for one change (EmbeddedReturn::openOn),
    // or Board when it opened on its board.
    SelectionStart openOn_ = SelectionStart::Board;
};
} }
