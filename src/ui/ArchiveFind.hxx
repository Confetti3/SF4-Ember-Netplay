#pragma once
// "Find this player's sets": the choice a row offers for each player it names
// (a match on this PC, a set in the public archive), and the name a pick of it
// stands for. Both panels use it, so neither knows the other's rows.
#include "MenuNavigation.hxx"
#include "../common/Localization.hxx"
#include <optional>
#include <string>
#include <vector>

namespace sf4e { namespace ui {

// Find 1 and Find 2 for the players of sides 0 and 1 that have a name.
inline std::vector<MenuChoice> FindChoices(const std::string (&names)[2]) {
    std::vector<MenuChoice> choices;
    for (int side = 0; side < 2; ++side)
        if (!names[side].empty()) choices.push_back({"find-" + std::to_string(side + 1), loc::Tf("archive.find", names[side]), loc::Tf("archive.find_detail", names[side])});
    return choices;
}
// The name a chosen option asks the archive for, if it is a Find of a named side.
inline std::optional<std::string> FoundName(const std::string& choice, const std::string (&names)[2]) {
    if (choice != "find-1" && choice != "find-2") return std::nullopt;
    const std::string& name = names[choice == "find-2" ? 1 : 0];
    if (name.empty()) return std::nullopt;
    return name;
}

} }
