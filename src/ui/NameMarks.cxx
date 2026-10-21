#include "NameMarks.hxx"
#include "CountryFlag.hxx"
#include "GameMenu.hxx"
#include "SelectionArt.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <cfloat>
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
constexpr ImU32 PillFill = IM_COL32(56, 51, 46, 255), PillBorder = IM_COL32(255, 135, 56, 160);

// How a badge is drawn: a class as the game's emblem once it has loaded, the
// developer mark as Ember's own art; placing as "?" in a pill, since the game
// has no art for it, and the developer mark as a word in a pill until its art
// has loaded; nothing for none, or a class whose emblem has not loaded.
struct Mark {
    SelectionImage emblem;
    bool faded = false;
    std::string pill;
    bool Drawn() const { return emblem.texture || !pill.empty(); }
};
Mark MarkOf(const SkillBadge& badge) {
    Mark mark;
    switch (badge.kind) {
    case SkillBadge::Kind::Placing: mark.pill = "?"; break;
    case SkillBadge::Kind::Developer:
        if (auto* art = MenuArt()) mark.emblem = art->DeveloperMark();
        if (!mark.emblem.texture) mark.pill = loc::T("class.developer");
        break;
    case SkillBadge::Kind::Class:
        if (auto* art = MenuArt()) mark.emblem = art->RankingEmblem(Classes[badge.band - 1].entry);
        mark.faded = !badge.settled;
        break;
    default: break;
    }
    return mark;
}
float EmblemSide(float fontSize) { return std::round(fontSize * 1.1f); }
// An emblem is drawn EmblemSide tall at its own shape (the class emblems are
// square, the developer mark wide).
float EmblemWidth(const SelectionImage& emblem, float fontSize) {
    const float side = EmblemSide(fontSize);
    return emblem.height > 0 ? std::round(side * emblem.width / emblem.height) : side;
}
float Gap() { return std::round(5 * Scale()); }
float PillTextSize(float fontSize) { return std::round(fontSize * .78f); }
float PillWidth(const std::string& pill, float fontSize) {
    const float size = PillTextSize(fontSize);
    return std::round(ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, pill.c_str()).x + size * .9f);
}
float MarkWidth(const Mark& mark, float fontSize) {
    return mark.emblem.texture ? EmblemWidth(mark.emblem, fontSize) : PillWidth(mark.pill, fontSize);
}
// The room a drawn mark takes, its gap included.
float MarkSlot(const Mark& mark, float fontSize) { return mark.Drawn() ? MarkWidth(mark, fontSize) + Gap() : 0; }

std::string MarkTip(const SkillBadge& badge) {
    if (badge.kind == SkillBadge::Kind::Placing) return loc::T("rankings.placing");
    if (badge.kind == SkillBadge::Kind::Developer) return loc::T("class.developer_tip");
    const auto tip = loc::Tf("class.tip", ClassName(badge.band));
    return badge.settled ? tip : tip + "\n" + loc::T("class.unsettled");
}
}

std::string ClassName(std::uint8_t band) {
    if (band == 0 || band > SkillBadge::Classes) return {};
    const auto& shown = Classes[band - 1];
    return shown.letters ? shown.letters : loc::T(band == SkillBadge::Classes ? "class.grand_master" : "class.master");
}

float NameMarksWidth(const NameMarks& marks, float fontSize) {
    auto* art = MenuArt();
    const bool flag = art && marks.country.Present() && art->Flag(marks.country).texture;
    return (flag ? FlagSlotWidth(fontSize) : 0) + MarkSlot(MarkOf(marks.badge), fontSize);
}

DrawnMarks DrawNameMarks(ImDrawList* d, float x, float textTop, float fontSize, const NameMarks& marks) {
    DrawnMarks drawn;
    const auto flag = DrawCountryFlagBeside(d, x, textTop, fontSize, marks.country);
    if (flag.hovered) drawn.tip = CountryName(marks.country);
    drawn.width = flag.width;
    const Mark mark = MarkOf(marks.badge);
    if (!mark.Drawn()) return drawn;
    const float side = EmblemSide(fontSize), width = MarkWidth(mark, fontSize);
    const ImVec2 min(std::round(x + drawn.width), std::round(textTop + (fontSize - side) * .5f)), max(min.x + width, min.y + side);
    if (mark.emblem.texture) {
        d->AddImage(mark.emblem.texture, min, max, mark.emblem.uvMin, mark.emblem.uvMax,
            mark.faded ? IM_COL32(255, 255, 255, 115) : IM_COL32_WHITE);
    } else {
        const float size = PillTextSize(fontSize), rounding = side * .5f;
        const float textWidth = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, mark.pill.c_str()).x;
        d->AddRectFilled(min, max, PillFill, rounding);
        d->AddRect(min, max, PillBorder, rounding, 0, std::round(Scale()));
        d->AddText(ImGui::GetFont(), size, ImVec2(std::round(min.x + (width - textWidth) * .5f), std::round(min.y + (side - size) * .5f)),
            palette::Ivory, mark.pill.c_str());
    }
    drawn.width += MarkSlot(mark, fontSize);
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(min, max))
        drawn.tip = MarkTip(marks.badge);
    return drawn;
}
} }
