#pragma once
// What leads a member's name in the room: their country's flag, then, in a
// public room, their class as USF4's own emblem (faint while it is
// unsettled), or "?" while their rating places and "Dev" for Ember's
// developers. A player without a badge shows nothing.
#include "../common/Country.hxx"
#include "../common/SkillBadge.hxx"
#include <imgui.h>
#include <string>

namespace sf4e { namespace ui {
struct NameMarks {
    CountryCode country;
    SkillBadge badge;
};
struct DrawnMarks {
    // The room the marks took, gaps included; 0 for none.
    float width = 0;
    // The country's or the class's name while the pointer is on its mark.
    std::string tip;
};
// A class by its band, 1 to SkillBadge::Classes: the game's letters (D to A+)
// or the crown's name in the catalog; empty for none.
std::string ClassName(std::uint8_t band);
// The room the marks take beside text of `fontSize`: only the marks whose art
// has loaded, so a name centred with them is centred as drawn.
float NameMarksWidth(const NameMarks& marks, float fontSize);
// The marks on a line of text of `fontSize` whose top is `textTop`, from `x`.
DrawnMarks DrawNameMarks(ImDrawList* drawList, float x, float textTop, float fontSize, const NameMarks& marks);
} }
