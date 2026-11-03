#include "NameMarks.hxx"
#include "CountryFlag.hxx"
#include "GameMenu.hxx"
#include "SelectionArt.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <imgui_internal.h>
#include <algorithm>
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
    bool faded = false, squared = false;
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
        mark.squared = true;
        break;
    default: break;
    }
    return mark;
}
// How big a mark is drawn: `side` tall, a pill's word `pillText` in `font`
// with a border `border` wide. Beside menu text all of it follows the text
// (Beside); the match HUD gives the height and font (Alone).
struct MarkSize { float side, pillText; ImFont* font; float border; };
MarkSize Beside(float fontSize) {
    return {std::round(fontSize * 1.1f), std::round(fontSize * .78f), ImGui::GetFont(), std::round(Scale())};
}
MarkSize Alone(float side, ImFont* font) {
    side = std::round(side);
    return {side, std::round(side * .7f), font, (std::max)(1.f, std::round(side / 18))};
}
// An emblem's painted shape: its texture less the transparent margin the art
// is cropped of.
float EmblemAspect(const SelectionImage& emblem) {
    const float width = emblem.width * (emblem.uvMax.x - emblem.uvMin.x), height = emblem.height * (emblem.uvMax.y - emblem.uvMin.y);
    return height > 0 ? width / height : 1.f;
}
// A class emblem takes a square slot and sits centred in it, however wide or
// tall its art, so a crown is as big as a bubble; the developer mark is drawn
// `side` tall at its own wide shape; a pill fits its word.
float MarkWidth(const Mark& mark, const MarkSize& size) {
    if (mark.emblem.texture) return mark.squared ? size.side : std::round(size.side * EmblemAspect(mark.emblem));
    return std::round(size.font->CalcTextSizeA(size.pillText, FLT_MAX, 0, mark.pill.c_str()).x + size.pillText * .9f);
}
// A drawn mark with its top left at `at` (rounded to whole pixels); returns its box.
ImRect PaintMark(ImDrawList* d, ImVec2 at, const Mark& mark, const MarkSize& size) {
    const float width = MarkWidth(mark, size);
    const ImVec2 min(std::round(at.x), std::round(at.y)), max(min.x + width, min.y + size.side);
    if (mark.emblem.texture) {
        // Fitted inside its slot, centred on both axes.
        const float aspect = EmblemAspect(mark.emblem), fit = (std::min)(width, size.side * aspect), tall = fit / aspect;
        const ImVec2 from(std::round(min.x + (width - fit) * .5f), std::round(min.y + (size.side - tall) * .5f));
        d->AddImage(mark.emblem.texture, from, ImVec2(from.x + fit, from.y + tall), mark.emblem.uvMin, mark.emblem.uvMax,
            mark.faded ? IM_COL32(255, 255, 255, 115) : IM_COL32_WHITE);
    } else {
        const float rounding = size.side * .5f;
        const float textWidth = size.font->CalcTextSizeA(size.pillText, FLT_MAX, 0, mark.pill.c_str()).x;
        d->AddRectFilled(min, max, PillFill, rounding);
        d->AddRect(min, max, PillBorder, rounding, 0, size.border);
        d->AddText(size.font, size.pillText, ImVec2(std::round(min.x + (width - textWidth) * .5f), std::round(min.y + (size.side - size.pillText) * .5f)),
            palette::Ivory, mark.pill.c_str());
    }
    return ImRect(min, max);
}
float Gap() { return std::round(5 * Scale()); }
// The room a drawn mark takes beside text, its gap included.
float MarkSlot(const Mark& mark, float fontSize) { return mark.Drawn() ? MarkWidth(mark, Beside(fontSize)) + Gap() : 0; }

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
    const auto size = Beside(fontSize);
    const ImRect box = PaintMark(d, ImVec2(x + drawn.width, textTop + (fontSize - size.side) * .5f), mark, size);
    drawn.width += MarkSlot(mark, fontSize);
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(box.Min, box.Max))
        drawn.tip = MarkTip(marks.badge);
    return drawn;
}

float BadgeWidth(const SkillBadge& badge, float side, ImFont* font) {
    const Mark mark = MarkOf(badge);
    return mark.Drawn() ? MarkWidth(mark, Alone(side, font)) : 0;
}

float DrawBadge(ImDrawList* d, ImVec2 at, float side, ImFont* font, const SkillBadge& badge) {
    const Mark mark = MarkOf(badge);
    return mark.Drawn() ? PaintMark(d, at, mark, Alone(side, font)).GetWidth() : 0;
}
} }
