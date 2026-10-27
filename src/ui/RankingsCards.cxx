#include "RankingsCards.hxx"
#include "ApplicationShell.hxx"
#include "MenuProbes.hxx"
#include "NameMarks.hxx"
#include "PublicRoomsCards.hxx"
#include "RoomCards.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <cfloat>
#include <initializer_list>
#include <string>

namespace sf4e { namespace ui {
namespace {
using netplay::rankings::Standing;
constexpr ImU32 ChipFill = IM_COL32(16, 15, 14, 230);

float Measure(float size, const std::string& text) { return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x; }

// The marks before a line of text, with their tooltip while the pointer is on one.
float Marks(float x, float textTop, float size, const NameMarks& marks) {
    const auto drawn = DrawNameMarks(ImGui::GetWindowDrawList(), x, textTop, size, marks);
    if (!drawn.tip.empty()) ImGui::SetTooltip("%s", drawn.tip.c_str());
    return drawn.width;
}

// The badge the standing shows: none while classes are hidden or the player does not take part.
SkillBadge Shown(const Standing& standing) {
    return standing.kind == Standing::Kind::Ranked && standing.classesShown ? standing.badge : SkillBadge();
}

// Your class: its emblem at heading size, then "Your class", the heading and the line.
void DrawStandingCard(const MenuEntry& e, const Standing& standing, ImVec2 min, ImVec2 max) {
    const float s = Scale(), pad = 16 * s, font = ImGui::GetFontSize(), heading = font * 1.5f;
    const SkillBadge badge = Shown(standing);
    // "Your class" heads a class or a placing rating, not the developer mark.
    const bool ranked = badge.HasClass() || badge.kind == SkillBadge::Kind::Placing;
    const float x = min.x + pad, emblem = Marks(x, min.y + (max.y - min.y - heading * 1.4f) * .5f, heading * 1.4f, {CountryCode(), badge});
    const float left = x + emblem, width = max.x - pad - left;
    float y = min.y + 12 * s;
    if (ranked) {
        DrawCardLine(ImVec2(left, y), font * .8f, palette::Muted, loc::T("rankings.your_class"), width, "rk-standing-label", font);
        y += font * .8f + 2 * s;
    }
    DrawCardLine(ImVec2(left, y), heading, palette::Ivory, e.label, width, "rk-standing", heading * 1.2f);
    y += heading + 4 * s;
    DrawCardParagraph(ImVec2(left, y), font * .85f, palette::Muted, e.detail, width, max.y - 8 * s - y, "rk-standing-detail");
}

// A row with marks before its label and, on the right, before its value. A
// player's own name may elide; an interface label must fit. `own` puts the
// You chip before the value's place.
void DrawMarkedRow(const MenuEntry& e, ImVec2 min, ImVec2 max, const NameMarks& left, const NameMarks& right, bool own) {
    const float s = Scale(), pad = ImGui::GetStyle().FramePadding.x, font = ImGui::GetFontSize(), top = min.y + (max.y - min.y - font) * .5f;
    auto* d = ImGui::GetWindowDrawList();
    float end = max.x - pad;
    if (!e.value.empty()) {
        if (e.userText) NoteUserText(e.value);
        const float valueWidth = Measure(font, e.value);
        end -= valueWidth;
        d->AddText(ImVec2(end, top), e.valueHighlighted ? palette::Ember : palette::Ivory, e.value.c_str());
        end -= Marks(end - NameMarksWidth(right, font), top, font, right) + 8 * s;
    }
    if (own) {
        const std::string you = loc::T("rankings.you");
        const float chip = Measure(13 * s, you) + 14 * s;
        end -= chip;
        DrawChip(ImVec2(end, min.y + (max.y - min.y - 20 * s) * .5f), you.c_str(), palette::Ember, ChipFill, "rkb-you-chip", chip);
        end -= 8 * s;
    }
    const float x = min.x + pad + Marks(min.x + pad, top, font, left), width = end - x;
    const ImU32 color = own ? palette::Ember : e.info && !e.userText ? palette::Ivory : ImGui::GetColorU32(ImGuiCol_Text);
    if (e.userText) {
        NoteUserText(e.label);
        std::string elided;
        DrawCardText(elided, ImVec2(x, top), width, e.label, font, color);
        FlushCardTip(elided);
    } else {
        ReportMenuText(e.id.c_str(), font, max.y - min.y, Measure(font, e.label), width);
        d->AddText(ImVec2(x, top), color, e.label.c_str());
    }
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

bool StartsWith(const std::string& text, const char* prefix) { return text.compare(0, std::char_traits<char>::length(prefix), prefix) == 0; }

bool StateCard(const std::string& id) {
    for (const char* card : {"rk-checking", "rk-needs-setup", "rk-loading", "rk-unavailable", "rk-error", "rkb-hidden", "rkb-loading", "rkb-error", "rkb-empty"})
        if (id == card) return true;
    return false;
}
}

bool DrawRankingsRow(const ShellView& v, const MenuEntry& e, ImVec2 min, ImVec2 max) {
    // The panel builds these rows only from the usable service's answers.
    const auto& rankings = v.rankings;
    const Standing* standing = rankings.standing.loaded ? &rankings.standing.value : nullptr;
    const auto* board = rankings.board.loaded ? rankings.board.value.get() : nullptr;
    const bool known = StateCard(e.id) || e.id == "rk-standing" || e.id == "rk-board-as" || e.id == "rkb-you" ||
        StartsWith(e.id, "rkb-class:") || StartsWith(e.id, "rkb-p:");
    if (!known) return false;
    // A board of hundreds paints only the rows in view.
    if (!ImGui::IsRectVisible(min, max)) return true;
    if (StateCard(e.id)) DrawStateCard(e, min, max);
    else if (e.id == "rk-standing" && standing) DrawStandingCard(e, *standing, min, max);
    else if (e.id == "rk-board-as") {
        const bool listed = standing && standing->listed && e.value == standing->listing.name;
        DrawMarkedRow(e, min, max, {}, {listed ? standing->listing.country : CountryCode(), {}}, false);
    } else if (e.id == "rkb-you") DrawMarkedRow(e, min, max, {}, {CountryCode(), standing ? Shown(*standing) : SkillBadge()}, false);
    else if (StartsWith(e.id, "rkb-class:")) {
        const auto badge = SkillBadge::OfClass(static_cast<std::uint8_t>(e.key), true);
        DrawMarkedRow(e, min, max, {CountryCode(), badge}, {}, false);
    } else {
        const auto index = static_cast<std::size_t>(e.key);
        if (!board || index >= board->size()) return false;
        const auto& entry = (*board)[index];
        DrawMarkedRow(e, min, max, {entry.listing.country, {}}, {}, entry.own);
    }
    return true;
}
} }
