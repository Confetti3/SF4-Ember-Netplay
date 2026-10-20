#include "NameMarks.hxx"
#include "CountryFlag.hxx"
#include "GameMenu.hxx"
#include "SelectionArt.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <cmath>

namespace sf4e { namespace ui {
namespace {
// USF4's own classes, D to Grand Master: the emblem's entry in the game's
// ranking archive, and the class's letters, which are the game's tokens and
// the same in every language. The two crowns are named in the catalog.
struct GameClass { const char* entry; const char* letters; };
constexpr GameClass Classes[SkillBadge::Classes] = {
    {"class_D.dds", "D"}, {"class_Dp.dds", "D+"}, {"class_C.dds", "C"}, {"class_Cp.dds", "C+"},
    {"class_B.dds", "B"}, {"class_Bp.dds", "B+"}, {"class_A.dds", "A"}, {"class_Ap.dds", "A+"},
    {"class_MASTER.dds", nullptr}, {"class_GM.dds", nullptr}};

// The class to draw: a placed player's, with its emblem loaded.
const GameClass* Shown(const SkillBadge& badge, SelectionImage& emblem) {
    auto* art = MenuArt();
    if (!art || !badge.Present() || badge.band > SkillBadge::Classes) return nullptr;
    const auto& shown = Classes[badge.band - 1];
    emblem = art->RankingEmblem(shown.entry);
    return emblem.texture ? &shown : nullptr;
}
float EmblemSide(float fontSize) { return std::round(fontSize * 1.1f); }
float EmblemSlot(float fontSize) { return EmblemSide(fontSize) + std::round(5 * Scale()); }

std::string ClassTip(const GameClass& shown, const SkillBadge& badge) {
    const std::string name = shown.letters ? shown.letters
        : loc::T(badge.band == SkillBadge::Classes ? "class.grand_master" : "class.master");
    const auto tip = loc::Tf("class.tip", name);
    return badge.settled ? tip : tip + "\n" + loc::T("class.unsettled");
}
}

float NameMarksWidth(const NameMarks& marks, float fontSize) {
    auto* art = MenuArt();
    SelectionImage emblem;
    const bool flag = art && marks.country.Present() && art->Flag(marks.country).texture;
    return (flag ? FlagSlotWidth(fontSize) : 0) + (Shown(marks.badge, emblem) ? EmblemSlot(fontSize) : 0);
}

DrawnMarks DrawNameMarks(ImDrawList* d, float x, float textTop, float fontSize, const NameMarks& marks) {
    DrawnMarks drawn;
    const auto flag = DrawCountryFlagBeside(d, x, textTop, fontSize, marks.country);
    if (flag.hovered) drawn.tip = CountryName(marks.country);
    drawn.width = flag.width;
    SelectionImage emblem;
    if (const auto* shown = Shown(marks.badge, emblem)) {
        const float side = EmblemSide(fontSize);
        const ImVec2 min(std::round(x + drawn.width), std::round(textTop + (fontSize - side) * .5f)), max(min.x + side, min.y + side);
        d->AddImage(emblem.texture, min, max, emblem.uvMin, emblem.uvMax,
            marks.badge.settled ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 115));
        drawn.width += EmblemSlot(fontSize);
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(min, max))
            drawn.tip = ClassTip(*shown, marks.badge);
    }
    return drawn;
}
} }
