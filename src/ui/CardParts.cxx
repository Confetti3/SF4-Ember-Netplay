#include "CardParts.hxx"
#include "GameMenu.hxx"
#include "NameMarks.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace sf4e { namespace ui { namespace card_parts {
using player_card::Chip;
namespace {
constexpr float Pi = 3.14159265f;

std::string Trimmed(std::string text) {
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

// The pixels the chips of a group take, a chip `chipGap` from the next.
float ChipsWidth(const std::vector<Chip>& chips, float chipGap) {
    float width = 0;
    for (const auto& chip : chips) width += v2::ChipWidth(chip.text) + (&chip == &chips.front() ? 0 : chipGap);
    return width;
}

// One chip with no ground and a 1 px accent outline: Looking for.
void DrawOutlinedChip(ImDrawList* d, ImVec2 at, const std::string& text) {
    const auto& c = v2::Colors();
    const float s = Scale(), size = v2::ChipFont(), width = v2::ChipWidth(text);
    d->AddRect(at, ImVec2(at.x + width, at.y + size + 8 * s), c.accent, 4 * s, 0, Hairline());
    d->AddText(ImGui::GetFont(), size, ImVec2(at.x + 8 * s, at.y + 4 * s), c.text, text.c_str());
}

// Chips from `x` along a line `row` tall at `y`; a chip that would pass `right` starts the next line (`wrapGap` under).
// Returns the lines it took.
int DrawChips(ImDrawList* d, float x, float y, float right, float row, float wrapGap, const std::vector<Chip>& chips, bool outlined) {
    const float s = Scale(), chipHeight = v2::ChipFont() + 8 * s, gap = 8 * s, start = x;
    int lines = 1;
    for (const auto& chip : chips) {
        const float width = v2::ChipWidth(chip.text);
        if (x > start && x + width > right) { x = start; y += row + wrapGap; ++lines; }
        if (d) {
            const ImVec2 at(x, y + (row - chipHeight) * .5f);
            if (outlined) DrawOutlinedChip(d, at, chip.text);
            else v2::Chip(d, at, chip.text, v2::Colors().muted);
        }
        x += width + gap;
    }
    return lines;
}

// The stacked groups drawn (`d`) or only measured (null). Returns the height.
float Stacked(ImDrawList* d, ImVec2 at, float width, float row, float labelSize, float lineGap, const std::vector<Chip>& plays, const std::vector<Chip>& looking) {
    const float s = Scale();
    const std::string playsLabel = loc::T("card.plays"), lookingLabel = loc::T("card.looking_row");
    float y = at.y;
    if (!plays.empty()) {
        const auto shown = player_card::GiveWay(plays, [&](const std::vector<Chip>& chips) { return GroupWidth(playsLabel, chips, labelSize) <= width; });
        if (d) DrawGroup(d, ImVec2(at.x, y), row, labelSize, playsLabel, shown);
        y += row + lineGap;
    }
    if (!looking.empty()) {
        const float label = Across(lookingLabel, labelSize * s) + 10 * s;
        if (d) d->AddText(ImGui::GetFont(), labelSize * s, ImVec2(at.x, y + (row - labelSize * s) * .5f), v2::Colors().muted, lookingLabel.c_str());
        const int lines = DrawChips(d, at.x + label, y, at.x + width, row, 4 * s, looking, true);
        y += lines * row + (lines - 1) * 4 * s + lineGap;
    }
    return (std::max)(0.f, y - at.y - (plays.empty() && looking.empty() ? 0 : lineGap));
}
}

float Across(const std::string& text, float size, ImFont* font) {
    if (!font) font = ImGui::GetFont();
    return font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str()).x;
}

float GroupWidth(const std::string& label, const std::vector<Chip>& chips, float labelSize) {
    if (chips.empty()) return 0;
    const float s = Scale();
    return Across(label, labelSize * s) + 10 * s + ChipsWidth(chips, 8 * s);
}

float DrawGroup(ImDrawList* d, ImVec2 at, float row, float labelSize, const std::string& label, const std::vector<Chip>& chips) {
    if (chips.empty()) return 0;
    const float s = Scale(), labelWidth = Across(label, labelSize * s) + 10 * s;
    d->AddText(ImGui::GetFont(), labelSize * s, ImVec2(at.x, at.y + (row - labelSize * s) * .5f), v2::Colors().muted, label.c_str());
    DrawChips(d, at.x + labelWidth, at.y, FLT_MAX, row, 0, chips, chips.front().kind == Chip::Kind::Looking);
    return GroupWidth(label, chips, labelSize);
}

float DrawGroupsTogether(ImDrawList* d, ImVec2 at, float width, float row, float labelSize, float gap, float lineGap, const std::vector<Chip>& plays,
    const std::vector<Chip>& looking) {
    const std::string playsLabel = loc::T("card.plays"), lookingLabel = loc::T("card.looking_row");
    const float wide = GroupWidth(playsLabel, plays, labelSize), tall = GroupWidth(lookingLabel, looking, labelSize);
    float x = at.x, y = at.y;
    if (wide > 0) { DrawGroup(d, ImVec2(x, y), row, labelSize, playsLabel, plays); x += wide + gap; }
    if (tall <= 0) return wide > 0 ? row : 0;
    // Wraps to a line of its own when the two do not fit.
    if (wide > 0 && x + tall > at.x + width) { x = at.x; y += row + lineGap; }
    DrawGroup(d, ImVec2(x, y), row, labelSize, lookingLabel, looking);
    return y + row - at.y;
}

float DrawGroupsStacked(ImDrawList* d, ImVec2 at, float width, float row, float labelSize, float lineGap, const std::vector<Chip>& plays, const std::vector<Chip>& looking) {
    return Stacked(d, at, width, row, labelSize, lineGap, plays, looking);
}

float GroupsStackedHeight(float width, float row, float labelSize, float lineGap, const std::vector<Chip>& plays, const std::vector<Chip>& looking) {
    return Stacked(nullptr, ImVec2(0, 0), width, row, labelSize, lineGap, plays, looking);
}

void DrawIconPlate(ImDrawList* d, ImVec2 at, float side, float icon, int family, v2::Tier tier) {
    const auto colors = v2::TierColor(tier);
    d->AddRectFilled(at, ImVec2(at.x + side, at.y + side), colors.plate, 4 * Scale());
    v2::Icon(d, family, ImVec2(at.x + (side - icon) * .5f, at.y + (side - icon) * .5f), icon, colors.stripe);
}

float DrawPips(ImDrawList* d, float centre, float y, int count, int level, float pip, float gap, v2::Tier tier) {
    const float total = count * pip + (count > 0 ? count - 1 : 0) * gap;
    const float x = centre - total * .5f;
    for (int i = 0; i < count; ++i) {
        const ImVec2 at(x + i * (pip + gap), y);
        // A level reached is filled; one not reached is a muted outline (3:1 and more against the plate), not a faint fill.
        if (i < level) d->AddRectFilled(at, ImVec2(at.x + pip, at.y + pip), v2::TierColor(tier).stripe);
        else d->AddRect(at, ImVec2(at.x + pip, at.y + pip), v2::Colors().muted, 0, 0, Hairline());
    }
    return total;
}

void DrawFamilyTile(ImDrawList* d, v2::Box box, const player_card::FamilyTile& tile, const player_card::Metrics& m, bool focused, bool hovered) {
    const auto& c = v2::Colors();
    const float s = Scale(), plate = m.plate * s, centre = (box.min.x + box.max.x) * .5f;
    v2::CardGround(d, box, focused, hovered);
    const float stack = m.plate + 10 + m.pip + 8 + 2 * 16;
    const float top = box.min.y + (std::max)(8.f, (m.familyHeight - stack) * .45f) * s;
    DrawIconPlate(d, ImVec2(centre - plate * .5f, top), plate, m.icon * s, tile.family->icon, tile.tier);
    float y = top + plate + 10 * s;
    DrawPips(d, centre, y, static_cast<int>(tile.family->count), tile.level, m.pip * s, m.pipGap * s, tile.tier);
    y += m.pip * s + 8 * s;
    // The title in its heading font, on two lines when it needs them, broken at a space and never inside a word.
    auto* font = HeadingFont();
    const float size = m.familyTitleText * s, inner = box.W() - 12 * s, least = player_card::TextFloor * s;
    const auto across = [&](const std::string& words) { return font->CalcTextSizeA(size, FLT_MAX, 0, words.c_str()).x; };
    std::string first = tile.title, second;
    if (across(first) > inner) {
        const std::size_t space = first.find(' ');
        if (space != std::string::npos) {
            // The most whole words that fit the first line (the first word at least).
            std::size_t cut = space;
            for (std::size_t next = first.find(' ', cut + 1); next != std::string::npos && across(first.substr(0, next)) <= inner; next = first.find(' ', next + 1)) cut = next;
            const std::size_t rest = first.find_first_not_of(' ', cut);
            if (rest != std::string::npos) { second = first.substr(rest); first.resize(cut); }
        } else if (!first.empty() && static_cast<unsigned char>(first[0]) >= 0xE1) {
            // Japanese, Korean and Chinese have no spaces and may break between characters.
            const char* text = first.c_str();
            const char* cut = font->CalcWordWrapPositionA(size / font->FontSize, text, text + first.size(), inner);
            if (cut > text && cut < text + first.size()) { second = std::string(cut); first.resize(cut - text); }
        }
    }
    // A line wider than the tile (one long word) is set smaller, to no less than the card's floor, and only then cut short.
    const auto line = [&](const std::string& words, float top) {
        const float wide = across(words), used = wide > inner && wide > 0 ? (std::max)((std::min)(size, least), size * inner / wide) : size;
        v2::Text(d, ImVec2(box.min.x + 6 * s, top + (size - used) * .5f), inner, words, used, c.text, font, true);
    };
    line(first, y);
    if (!second.empty()) line(second, y + size * 1.25f);
    if (focused) v2::Focus(d, box.min, box.max);
}

const char* DeviceGlyph(const char* padButton) {
    if (!padButton) return nullptr;
    if (!std::strcmp(padButton, "X")) return MenuShortcutGlyph(MenuInput::Fighter);
    if (!std::strcmp(padButton, "Y")) return MenuShortcutGlyph(MenuInput::Options);
    return padButton;
}

void DrawPresence(ImDrawList* d, ImVec2 at, float radius, player_card::PresenceMark mark) {
    using player_card::PresenceMark;
    const auto& c = v2::Colors();
    const float s = Scale();
    if (mark == PresenceMark::InRoom) d->AddCircleFilled(at, radius, c.ready, 20);
    else if (mark == PresenceMark::Online) d->AddCircle(at, radius - s, c.ready, 20, 2 * s);
    else if (mark == PresenceMark::Offline) {
        // Eight short arcs round the ring.
        for (int i = 0; i < 8; ++i) {
            d->PathArcTo(at, radius - s, Pi / 4 * i, Pi / 4 * i + Pi / 6, 4);
            d->PathStroke(c.muted, 0, 2 * s);
        }
    }
}

void DrawPlayerLine(ImDrawList* d, float x, float y, float room, float size, float emblem, const std::string& lead, const std::string& name, CountryCode country, const SkillBadge& badge) {
    const auto& c = v2::Colors();
    const float s = Scale();
    float left = x;
    if (!lead.empty()) {
        d->AddText(HeadingFont(), size, ImVec2(left, y), c.muted, lead.c_str());
        left += Across(lead, size, HeadingFont()) + 6 * s;
    }
    NameMarks flag;
    flag.country = country;
    left += DrawNameMarks(d, left, y, size, flag).width;
    const float badgeWidth = badge.HasClass() ? BadgeWidth(badge, emblem, ImGui::GetFont()) : 0;
    NoteUserText(name);
    const std::string shown = v2::Fit(name, x + room - left - (badgeWidth > 0 ? badgeWidth + 8 * s : 0), size, HeadingFont(), false);
    d->AddText(HeadingFont(), size, ImVec2(left, y), c.text, shown.c_str());
    if (badgeWidth > 0) DrawBadge(d, ImVec2(left + Across(shown, size, HeadingFont()) + 8 * s, y + (size - emblem) * .5f), emblem, ImGui::GetFont(), badge);
}

void DrawSetRow(ImDrawList* d, v2::Box box, const player_card::SetRow& row, FighterArt art, const player_card::Metrics& m, bool focused, bool hovered) {
    const auto& c = v2::Colors();
    const float s = Scale(), picture = m.rowPicture * s;
    v2::CardGround(d, box, focused, hovered);
    float x = box.min.x + 14 * s;
    const float top = box.min.y + (box.H() - picture) * .5f;
    for (const int fighter : {row.mine, row.theirs}) {
        const v2::Box face{ImVec2(x, top), ImVec2(x + picture, top + picture)};
        if (fighter >= 0) DrawCharacterPortrait(FighterPicture{fighter, art, 0, 0}, face.min, face.max, c.frame);
        else d->AddRectFilled(face.min, face.max, c.frame);
        x += picture + 4 * s;
    }
    x += 8 * s;
    const float nameSize = m.rowNameText * s, lineSize = m.rowLineText * s, block = nameSize + 5 * s + lineSize, y = box.min.y + (box.H() - block) * .5f;
    const float room = box.max.x - 14 * s - x;
    DrawPlayerLine(d, x, y, room, nameSize, nameSize * 1.4f, Trimmed(loc::Tf("card.set_vs", std::string())), row.opponent, row.country, row.badge);
    NoteUserText(row.line);
    v2::Text(d, ImVec2(x, y + nameSize + 5 * s), room, row.line, lineSize, c.muted);
    if (focused) v2::Focus(d, box.min, box.max);
}

void DrawRivalRow(ImDrawList* d, v2::Box box, const player_card::Rival& rival, const player_card::Metrics& m, bool focused, bool hovered) {
    const auto& c = v2::Colors();
    const float s = Scale(), nameSize = m.rowNameText * s, lineSize = m.rowLineText * s, block = nameSize + 5 * s + lineSize, y = box.min.y + (box.H() - block) * .5f;
    v2::CardGround(d, box, focused, hovered);
    const float x = box.min.x + 14 * s, room = box.W() - 28 * s - 20 * s;
    DrawPlayerLine(d, x, y, room, nameSize, nameSize * 1.4f, std::string(), rival.player.name, rival.player.country, rival.player.badge);
    v2::Text(d, ImVec2(x, y + nameSize + 5 * s), room, rival.line, lineSize, c.muted);
    v2::Chevron(d, ImVec2(box.max.x - 24 * s, box.min.y + box.H() * .5f), 7 * s, c.muted);
    if (focused) v2::Focus(d, box.min, box.max);
}

void DrawLinkRow(ImDrawList* d, v2::Box box, const std::string& label, float text, bool focused, bool hovered) {
    const auto& c = v2::Colors();
    const float s = Scale(), mid = box.min.y + box.H() * .5f;
    v2::CardGround(d, box, focused, hovered);
    v2::Text(d, ImVec2(box.min.x + 18 * s, mid - text * s * .5f), box.W() - 60 * s, label, text * s, c.text, HeadingFont());
    v2::Chevron(d, ImVec2(box.max.x - 24 * s, mid), 7 * s, c.muted);
    if (focused) v2::Focus(d, box.min, box.max);
}

void DrawReadingRow(ImDrawList* d, v2::Box box, const std::string& words, float text) {
    const float s = Scale();
    v2::CardGround(d, box, false, false);
    if (!words.empty()) v2::Text(d, ImVec2(box.min.x + 14 * s, box.min.y + (box.H() - text * s) * .5f), box.W() - 28 * s, words, text * s, v2::Colors().muted);
}

} } }
