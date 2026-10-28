#pragma once
#include "MenuNavigation.hxx"
#include "../common/Country.hxx"
#include <imgui.h>
#include <optional>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
// The country choice Settings opens. Search leads, then Automatic (Windows'
// region) and Hidden, then every country by its name in the interface
// language. The search keeps the countries whose name or code holds its text,
// ignoring case and accents. A typed letter goes to the next country whose
// name starts with it; Left and Right go to the start of this letter, then of
// the one before, or to the next letter. Select on a choice gives it back; the
// owner saves it and leaves the screen.
class CountryPicker {
public:
    static constexpr const char* Screen = "country";
    // Opens with no search; the screen's focus belongs on RowId(current).
    void Open();
    std::vector<MenuEntry> Rows(const CountryPreference& current, const CountryCode& automatic, bool enabled) const;
    // The search row's accepted text.
    void Search(std::string text);
    const std::string& SearchText() const { return search_; }
    // The row a typed character (UTF-32) goes to from the focused row, or empty
    // when no country shown starts with it.
    std::string Jump(unsigned typed, const std::string& focused) const;
    // Select on a row: the choice it makes, or none for a row that makes none.
    std::optional<CountryPreference> Choose(const std::string& id) const;
    // The row that holds `choice`.
    static std::string RowId(const CountryPreference& choice);
    // `choice` in words, for the Settings row that opens the picker.
    static std::string ChoiceText(const CountryPreference& choice, const CountryCode& automatic);
    // Draws a country row, and Automatic's, with its flag before the name; false
    // for the other rows, which draw as usual (GameMenu::Card).
    bool DrawRow(const MenuEntry& entry, const CountryCode& automatic, ImVec2 min, ImVec2 max) const;
    // V2's board (Country.dc.html): Automatic and Hidden, the letters the names
    // start with seven across, and two across the countries of the focused
    // letter (or of the focused country's). Up, Down, Left and Right go across
    // the grids; Select on a letter goes to its first country (a letter's row
    // is "letter-" and that country's code). False where the language has too
    // many initials for a grid (CJK): the list stays.
    bool Lettered() const;
    std::vector<MenuEntry> BoardRows(const CountryPreference& current, const CountryCode& automatic, bool enabled, const std::string& focused);
    void DrawBoard(const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, const CountryCode& automatic);
private:
    struct Name { CountryCode code; std::string text; std::wstring wide; wchar_t initial = 0; };
    // The countries by name in the active catalog, built again when it changes.
    const std::vector<Name>& Names() const;
    std::vector<const Name*> Shown() const;
    std::string search_, boardFocus_;
    wchar_t letter_ = 0;
    mutable std::vector<Name> names_;
    mutable std::string namesKey_;
};
} }
