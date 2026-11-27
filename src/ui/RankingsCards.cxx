#include "RankingsCards.hxx"
#include "ApplicationShell.hxx"
#include "MenuProbes.hxx"
#include "NameMarks.hxx"
#include "PublicRoomsCards.hxx"
#include "RoomCards.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <initializer_list>
#include <string>

namespace sf4e { namespace ui {
namespace {
using netplay::rankings::Standing;

float Measure(float size, const std::string& text) { return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x; }

// The badge the standing shows: none while classes are hidden or the player does not take part.
SkillBadge Shown(const Standing& standing) {
    return standing.kind == Standing::Kind::Ranked && standing.classesShown ? standing.badge : SkillBadge();
}

// Board as: its label, and at the right the name the board lists you by after
// its flag. The label must fit, so the name takes only what it leaves.
void DrawBoardAs(const MenuEntry& e, ImVec2 min, ImVec2 max, const NameMarks& marks) {
    const float s = Scale(), pad = 16 * s, font = ImGui::GetFontSize(), top = min.y + (max.y - min.y - font) * .5f;
    auto* d = ImGui::GetWindowDrawList();
    const float x = min.x + pad, labelNeed = Measure(font, e.label) + 8 * s;
    float end = max.x - pad;
    if (!e.value.empty()) {
        if (e.userText) NoteUserText(e.value);
        const float flag = NameMarksWidth(marks, font);
        const std::string value = v2::Fit(e.value, (std::max)(0.f, end - x - labelNeed - flag), font, nullptr, false);
        end -= Measure(font, value);
        d->AddText(ImVec2(end, top), e.valueHighlighted ? palette::Ember : palette::Ivory, value.c_str());
        const auto drawn = DrawNameMarks(d, end - flag, top, font, marks);
        if (!drawn.tip.empty()) ImGui::SetTooltip("%s", drawn.tip.c_str());
    }
    ReportMenuText(e.id.c_str(), font, max.y - min.y, Measure(font, e.label), end - x);
    d->AddText(ImVec2(x, top), e.info ? palette::Ivory : ImGui::GetColorU32(ImGuiCol_Text), e.label.c_str());
}

// A card that says where the screen stands (loading, a failure, nothing
// listed): its label and detail wrapped to the card, so a long translation
// shrinks instead of running out of it.
void DrawStateCard(const MenuEntry& e, ImVec2 min, ImVec2 max) {
    const float s = Scale(), pad = 16 * s, width = max.x - min.x - 2 * pad, font = ImGui::GetFontSize();
    const float labelHeight = e.detail.empty() ? max.y - min.y - 2 * pad : (max.y - min.y) * .45f;
    const float used = DrawCardParagraph(ImVec2(min.x + pad, min.y + pad), font * 1.1f, e.enabled ? palette::Ivory : palette::Muted, e.label, width,
        labelHeight, e.id.c_str());
    if (!e.detail.empty()) {
        const float y = min.y + pad + used + 6 * s;
        DrawCardParagraph(ImVec2(min.x + pad, y), font * .85f, palette::Muted, e.detail, width, max.y - pad - y, (e.id + "-detail").c_str());
    }
}

bool StateCard(const std::string& id) {
    for (const char* card : {"rk-checking", "rk-needs-setup", "rk-loading", "rk-unavailable", "rk-error"})
        if (id == card) return true;
    return false;
}

// Your class's card: the class's emblem beside your tag, its name and its
// line, with the class's tone along its left edge as the site's cards.
void DrawStanding(const MenuEntry& e, const Standing& standing, ImVec2 min, ImVec2 max) {
    const float s = Scale(), h = max.y - min.y;
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    const auto& card = MenuPlayerCard();
    v2::TagView you;
    you.name = standing.listed ? standing.listing.name : card.name;
    you.marks.country = standing.listed ? standing.listing.country : card.country;
    const SkillBadge badge = Shown(standing);
    if (badge.HasClass()) d->AddRectFilled(min, ImVec2(min.x + 4 * s, max.y), v2::ClassTone(badge.band), 4 * s, ImDrawFlags_RoundCornersLeft);
    // The emblem gives a narrow card's words most of its width.
    const float pad = 20 * s, emblem = (std::min)(80 * s, (max.x - min.x) * .2f);
    const NameMarks marks{CountryCode(), badge};
    const float mark = NameMarksWidth(marks, emblem), x = min.x + pad + (mark > 0 ? mark + 14 * s : 0), room = max.x - pad - x;
    DrawNameMarks(d, min.x + pad, min.y + (h - emblem) * .5f, emblem, marks);
    // The class's name under the tag, smaller where its line is too short
    // for it, down to 12 px and then on two lines; then the detail's first
    // line, cut to the card, while there is room for it (the detail pane
    // says it whole).
    const float wide = HeadingFont()->CalcTextSizeA(20 * s, FLT_MAX, 0, e.label.c_str()).x;
    const float size = wide > room ? (std::max)(12 * s, 20 * s * room / wide) : 20 * s;
    const float label = v2::HeadingHeight(e.label, room, size, HeadingFont());
    const bool line = label <= size;
    float y = min.y + (h - 30 * s - (line ? 42 * s : label)) * .5f;
    y += v2::Tag(d, ImVec2(x, y), room, you, v2::TagSize::S) + 4 * s;
    v2::Heading(d, ImVec2(x, y), room, e.label, size, c.text, HeadingFont());
    if (line) v2::Text(d, ImVec2(x, y + 26 * s), room, e.detail.substr(0, e.detail.find('\n')), 16 * s, c.muted, ImGui::GetFont(), false, false);
}
}

bool DrawRankingsRow(const ShellView& v, const MenuEntry& e, ImVec2 min, ImVec2 max) {
    // The panel builds these rows only from the usable service's answers.
    const auto& standing = v.rankings.standing;
    if (StateCard(e.id)) DrawStateCard(e, min, max);
    else if (e.id == "rk-standing" && standing.loaded) DrawStanding(e, standing.value, min, max);
    else if (e.id == "rk-board-as") {
        const bool listed = standing.loaded && standing.value.listed && e.value == standing.value.listing.name;
        DrawBoardAs(e, min, max, {listed ? standing.value.listing.country : CountryCode(), {}});
    } else return false;
    return true;
}
} }
