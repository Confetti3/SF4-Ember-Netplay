#pragma once
#include "../common/FighterCatalog.hxx"
#include "SelectionArt.hxx"
#include "GameMenu.hxx"
#include <functional>
#include <string>

namespace sf4e { namespace ui {
bool DrawStageSelector(int& nativeId, SelectionArt* art);
class FighterSelector {
public:
    enum class Page { Fighter, Appearance, Ultra, Stage };
    Page CurrentPage() const { return page_; }
    MenuNavigation& Navigation() { return menu_.navigation; }
    using AvailabilityReader = std::function<selection::Availability(int)>;
    bool Draw(selection::Pick& pick, bool editionSelect, SelectionArt* art, const AvailabilityReader& readAvailability,
              int* stageId = nullptr, bool editable = true);
private:
    GameMenu menu_;
    Page page_ = Page::Fighter;
    std::string notice_;
};
} }
