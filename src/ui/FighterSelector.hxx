#pragma once
#include "../common/FighterCatalog.hxx"
#include "../common/StageCatalog.hxx"
#include "SelectionArt.hxx"
#include "GameMenu.hxx"
#include <functional>
#include <string>

namespace sf4e { namespace ui {
bool DrawStageSelector(int& nativeId, SelectionArt* art);
// The names every screen uses for a costume ("Original", "Alternate 1 / pack")
// and an Ultra, so a saved choice reads the same as the card that made it.
std::string CostumeLabel(const selection::Pick& pick);
const char* UltraLabel(int ultra);
// A stage's name, or Random.
const char* StageLabel(int stageId);
class FighterSelector {
public:
    enum class Page { Fighter, Appearance, Ultra, Stage };
    Page CurrentPage() const { return page_; }
    MenuNavigation& Navigation() { return menu_.navigation; }
    using AvailabilityReader = std::function<selection::Availability(int)>;
    // selectionError explains why the current choice is not usable. The room
    // screens tell the player to open Fighter Select, so Fighter Select has to
    // be able to say what is wrong once they arrive.
    // randomStageExcluded, given with stageId, adds the Random stage pool page.
    bool Draw(selection::Pick& pick, bool editionSelect, SelectionArt* art, const AvailabilityReader& readAvailability,
              int* stageId = nullptr, bool editable = true, const std::string& selectionError = {},
              selection::StageMask* randomStageExcluded = nullptr);
private:
    GameMenu menu_;
    Page page_ = Page::Fighter;
    // The page opened for one change (EmbeddedReturn::openOn), or empty.
    std::string openOn_;
    // The Ultra step was just pushed: focus the Ultra the new fighter has,
    // which the caller restores after this frame.
    bool focusSavedUltra_ = false;
    // A fighter was just picked. Whether its Ultra step is needed is decided
    // on the next frame, from the pick as the caller restored it for that
    // fighter (its own saved edition), not from the previous fighter's.
    bool ultraStepPending_ = false;
    std::string notice_;
};
} }
