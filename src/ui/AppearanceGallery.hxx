#pragma once
#include "../common/FighterCatalog.hxx"
#include "../common/InstalledContent.hxx"
#include "FighterArt.hxx"
#include "MenuNavigation.hxx"
#include <string>
#include <vector>

namespace sf4e { namespace ui {
// Fighter select's picture grids, over the fighter's own page
// (FighterSelector's "look"): the costumes, their colours and the Ultras.
// Select on its Costume row shows a card for every costume the player has,
// each in the colour worn now; Select on a costume shows all of that
// costume's colours at once, and Select on a colour saves the costume and
// colour together. Select on the Color row opens the worn costume's colours.
// Select on the Ultra row shows a card for each Ultra the edition allows: its
// picture, its name and its input in words; Select on one saves it. Back
// steps out without saving: the costume chosen waits on its colour. Only
// what AllowedCostumes, AllowedColors and AllowedUltras give is a card, so
// the Steam licences and the edition decide what is offered.
class AppearanceGallery {
public:
    enum class Page { Costumes, Colors, Ultras };
    // What Select on a row (Choose) did: nothing the gallery knows, a grid
    // opened, or the pick changed (the caller normalizes and saves it).
    enum class Choice { Unhandled, Opened, Saved };
    // The screen is one of the grids.
    static bool Shows(const std::string& screen);
    // The grid's title, and what its status line promises Select does.
    static const char* Title(const std::string& screen);
    static const char* StatusKey(const std::string& screen);
    // Opens `page` over the screen shown, on what the pick wears or uses.
    void Open(MenuNavigation& nav, Page page, const selection::Pick& pick);
    // The cards of the grid `screen` for this frame, one per costume, colour
    // or Ultra (keyed by its id), or a single row that says why there are none.
    // An Ultra's detail is its input, a line for each command (UltraInputLines).
    std::vector<MenuEntry> Build(const std::string& screen, const selection::Pick& pick, const selection::Availability& availability,
                                 const selection::InstalledContent* installed, bool editable);
    // The built cards in place of the list (GameMenu::Body): every one at once,
    // as large as fits, each its picture over its name (and an Ultra's input);
    // the one worn or saved marked.
    void DrawBody(const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, float height);
    // Select on the look page's Costume, Color or Ultra row, or on a card.
    Choice Choose(const std::string& id, MenuNavigation& nav, selection::Pick& pick, const selection::Availability& availability,
                  const selection::InstalledContent* installed);
private:
    // Back to the screen under the grids (the look page).
    static void Leave(MenuNavigation& nav);
    // A built row's card: the fighter as it draws them (an Ultra's card only
    // where the Ultra has no picture of its own), an Ultra's input lines, and
    // whether it is what the pick has now.
    struct Card {
        FighterPicture figure;
        std::vector<std::string> inputs;
        bool worn = false;
    };
    // The costume whose colours the colour grid shows: chosen on the costume
    // grid and not saved until a colour is.
    int costume_ = 0;
    // The grid Build last built.
    Page page_ = Page::Costumes;
    // Each built row's card, in the order of Build's rows; empty when Build
    // gave the row that says why there are none.
    std::vector<Card> cards_;
    // The focus last drawn, so a focus the pad moved scrolls into view.
    std::string focus_;
};
} }
