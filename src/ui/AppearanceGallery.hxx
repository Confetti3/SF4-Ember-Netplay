#pragma once
#include "../common/FighterCatalog.hxx"
#include "../common/InstalledContent.hxx"
#include "FighterArt.hxx"
#include "MenuNavigation.hxx"
#include <string>
#include <vector>

namespace sf4e { namespace ui {
// Fighter select's costume and colour grids, over the fighter's own page
// (FighterSelector's "look"). Select on its Costume row shows a card for
// every costume the player has, each in the colour worn now; Select on a
// costume shows all of that costume's colours at once, and Select on a colour
// saves the costume and colour together. Select on the Color row opens the
// worn costume's colours. Back steps out without saving: the costume chosen
// waits on its colour. Only what AllowedCostumes and AllowedColors give is a
// card, so the Steam licences decide what is offered.
class AppearanceGallery {
public:
    enum class Page { Costumes, Colors };
    // What Select on a row (Choose) did: nothing the gallery knows, a grid
    // opened, or the pick changed (the caller normalizes and saves it).
    enum class Choice { Unhandled, Opened, Saved };
    // The screen is one of the grids.
    static bool Shows(const std::string& screen);
    // The grid's title, and what its status line promises Select does.
    static const char* Title(const std::string& screen);
    static const char* StatusKey(const std::string& screen);
    // Opens `page` over the screen shown, on what the pick wears.
    void Open(MenuNavigation& nav, Page page, const selection::Pick& pick);
    // The cards of the grid `screen` for this frame, one per costume or
    // colour (keyed by its id), or a single row that says why there are none.
    std::vector<MenuEntry> Build(const std::string& screen, const selection::Pick& pick, const selection::Availability& availability,
                                 const selection::InstalledContent* installed, bool editable);
    // The built cards in place of the list (GameMenu::Body): every one at once,
    // as large as fits, each its picture over its name; the worn one marked.
    void DrawBody(const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float height, const selection::Pick& pick);
    // Select on the look page's Costume or Color row, or on a card.
    Choice Choose(const std::string& id, MenuNavigation& nav, selection::Pick& pick, const selection::Availability& availability,
                  const selection::InstalledContent* installed);
private:
    // Back to the screen under the grids (the look page).
    static void Leave(MenuNavigation& nav);
    // The costume whose colours the colour grid shows: chosen on the costume
    // grid and not saved until a colour is.
    int costume_ = 0;
    // The grid Build last built.
    Page page_ = Page::Costumes;
    // Each built card's picture, in the order of Build's rows; empty when
    // Build gave the row that says why there are none.
    std::vector<FighterPicture> pictures_;
    // The focus last drawn, so a focus the pad moved scrolls into view.
    std::string focus_;
};
} }
