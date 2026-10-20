#pragma once
// What leads a member's name in the room: their country's flag, then, in a
// public room, their class as USF4's own emblem (faint while it is
// unsettled). A player with no class shows no emblem.
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
// The room the marks take beside text of `fontSize`: only the marks whose art
// has loaded, so a name centred with them is centred as drawn.
float NameMarksWidth(const NameMarks& marks, float fontSize);
// The marks on a line of text of `fontSize` whose top is `textTop`, from `x`.
DrawnMarks DrawNameMarks(ImDrawList* drawList, float x, float textTop, float fontSize, const NameMarks& marks);
} }
