#include "RankingsCards.hxx"
#include "ApplicationShell.hxx"
#include "MenuProbes.hxx"
#include "NameMarks.hxx"
#include "PublicRoomsCards.hxx"
#include "RoomCards.hxx"
#include "RowId.hxx"
#include "Theme.hxx"
#include "V2.hxx"
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

// The Rankings design board: your class as a big emblem beside your tag,
// its name and its line; you and the board's players as S tags, you with the
// You chip; a class's heading in the accent with its count. False for the
// rest (the state cards, Board as), which DrawRankingsRow draws plainly.
bool DrawRankingsBoardRow(const MenuEntry& e, const Standing* standing, const std::vector<netplay::rankings::BoardEntry>* board, ImVec2 min, ImVec2 max) {
    const float s = Scale(), h = max.y - min.y;
    const auto& c = v2::Colors();
    auto* d = ImGui::GetWindowDrawList();
    // You: as the board lists you, else as rooms show you.
    const auto& card = MenuPlayerCard();
    v2::TagView you;
    you.name = standing && standing->listed ? standing->listing.name : card.name;
    you.marks.country = standing && standing->listed ? standing->listing.country : card.country;
    const auto chip = [&](float right) {
        const std::string text = loc::T("rankings.you");
        const float w = v2::ChipWidth(text);
        v2::Chip(d, ImVec2(right - w, min.y + (h - ImGui::GetFontSize() * .78f - 8 * s) * .5f), text, c.accent);
        return w + 12 * s;
    };
    if (e.id == "rk-standing" && standing) {
        const float pad = 20 * s, emblem = 80 * s;
        const NameMarks marks{CountryCode(), Shown(*standing)};
        const float mark = NameMarksWidth(marks, emblem), x = min.x + pad + (mark > 0 ? mark + 14 * s : 0), room = max.x - pad - x;
        DrawNameMarks(d, min.x + pad, min.y + (h - emblem) * .5f, emblem, marks);
        float y = min.y + (h - 26 * s - 26 * s - 20 * s) * .5f;
        y += v2::Tag(d, ImVec2(x, y), room, you, v2::TagSize::S) + 4 * s;
        d->AddText(HeadingFont(), 20 * s, ImVec2(x, y), c.text, e.label.c_str());
        d->AddText(ImGui::GetFont(), 16 * s, ImVec2(x, y + 26 * s), c.muted, e.detail.substr(0, e.detail.find('\n')).c_str());
        return true;
    }
    if (e.id == "rkb-you") {
        you.marks.badge = standing ? Shown(*standing) : SkillBadge();
        const float right = max.x - 16 * s - ImGui::CalcTextSize(e.value.c_str()).x;
        d->AddText(ImVec2(right, min.y + (h - ImGui::GetFontSize()) * .5f), c.muted, e.value.c_str());
        const float x = min.x + 16 * s, room = right - 12 * s - chip(right - 12 * s) - x;
        v2::Tag(d, ImVec2(x, min.y + (h - 26 * s) * .5f), room, you, v2::TagSize::S);
        return true;
    }
    if (StartsWith(e.id, "rkb-class:")) {
        const float size = 14 * s, y = min.y + (h - size) * .5f;
        d->AddText(HeadingFont(), size, ImVec2(min.x + 16 * s, y), c.accent, e.label.c_str());
        d->AddText(ImGui::GetFont(), size, ImVec2(max.x - 16 * s - ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, e.value.c_str()).x, y), c.muted, e.value.c_str());
        return true;
    }
    if (StartsWith(e.id, "rkb-p:")) {
        const auto index = RowKey<std::size_t>(e.id, "rkb-p:");
        if (!board || !index || *index >= board->size()) return false;
        const auto& entry = (*board)[*index];
        v2::TagView tag;
        tag.name = entry.listing.name; tag.marks = {entry.listing.country, SkillBadge::OfClass(entry.band, true)};
        const float x = min.x + 12 * s, right = max.x - 12 * s;
        v2::Tag(d, ImVec2(x, min.y + (h - 26 * s) * .5f), (entry.own ? right - chip(right) : right) - x, tag, v2::TagSize::S);
        return true;
    }
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
    if (DrawRankingsBoardRow(e, standing, board, min, max)) return true;
    if (StateCard(e.id)) DrawStateCard(e, min, max);
    else if (e.id == "rk-board-as") {
        const bool listed = standing && standing->listed && e.value == standing->listing.name;
        DrawMarkedRow(e, min, max, {}, {listed ? standing->listing.country : CountryCode(), {}}, false);
    } else {
        const auto index = static_cast<std::size_t>(e.key);
        if (!board || index >= board->size()) return false;
        const auto& entry = (*board)[index];
        DrawMarkedRow(e, min, max, {entry.listing.country, {}}, {}, entry.own);
    }
    return true;
}
} }
