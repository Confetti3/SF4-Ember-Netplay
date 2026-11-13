#include "FightersTab.hxx"
#include "MenuGlyphs.hxx"
#include "PlayerCard.hxx"
#include "RowId.hxx"
#include "ShellView.hxx"
#include "SignInSites.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/LocalTime.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>

namespace sf4e { namespace ui { namespace fighters_tab {
namespace {
using Level = netplay::levels::Level;

// A fighter's tile: its picture on its tier line with the points on it (a fighter not played has its picture faded,
// `0 FP` and no line), the "Main" chip on the main's, and under it the name and, on the player's own tab, the games.
void DrawTile(ImDrawList* d, v2::Box box, int fighter, const Level& level, const Shown& shown, bool isMain, bool focused, bool hovered, bool enabled, float picture) {
    const auto& c = v2::Colors();
    const float s = Scale();
    v2::CardGround(d, box, focused, hovered);
    const v2::Box tile{box.min, ImVec2(box.max.x, box.min.y + picture)};
    DrawCharacterPortrait(FighterPicture{fighter, shown.art, 0, 0}, tile.min, tile.max, c.frame);
    if (!level.points) d->AddRectFilled(tile.min, tile.max, v2::color::Alpha(c.frame, .5f));
    v2::LevelBand(d, tile, level.points, shown.alpha);
    DrawFighterEdge(d, tile, level.points, level.tier);
    if (isMain) v2::Chip(d, ImVec2(tile.min.x + 6 * s, tile.min.y + 6 * s), loc::T("fighters.main"), c.accentLight);
    const float x = box.min.x + 6 * s, room = box.W() - 12 * s, y = tile.max.y + 6 * s;
    v2::Text(d, ImVec2(x, y), room, selection::FindFighter(fighter)->name, 14 * s, enabled ? c.text : c.muted, HeadingFont());
    if (!shown.others) v2::TextShrunk(d, ImVec2(x, y + 20 * s), room, player_card::GamesWords(level.games), player_card::TextFloor * s, 10 * s, c.muted);
    if (focused) v2::Focus(d, box.min, box.max, false);
}

// A fighter's picture on its tier line at `at`, `side` square, and beside it the points line and, on the player's own tab,
// the meter and the way to the next frame (or "Not played yet"), on another's the tier in words; with `named`, the name
// (and "Main") over them. Returns the bottom.
float DrawHead(ImDrawList* d, ImVec2 at, float width, float side, float digits, int fighter, const Level& level, const Shown& shown, bool isMain, bool named) {
    const auto& c = v2::Colors();
    const float s = Scale(), gap = 16 * s, x = at.x + side + gap, room = width - side - gap;
    const v2::Box picture{at, ImVec2(at.x + side, at.y + side)};
    DrawCharacterPortrait(FighterPicture{fighter, shown.art, 0, 0}, picture.min, picture.max, c.frame);
    if (!level.points) d->AddRectFilled(picture.min, picture.max, v2::color::Alpha(c.frame, .5f));
    DrawFighterEdge(d, picture, level.points, level.tier);
    float y = at.y;
    if (named) {
        const float chip = isMain ? v2::ChipWidth(loc::T("fighters.main")) + 8 * s : 0;
        v2::Text(d, ImVec2(x, y), room - chip, selection::FindFighter(fighter)->name, 17 * s, c.text, HeadingFont());
        if (isMain) v2::Chip(d, ImVec2(x + room - chip + 8 * s, y), loc::T("fighters.main"), c.accentLight);
        y += 24 * s;
    }
    v2::Level(d, ImVec2(x, y), digits, level.points, "points.label", shown.alpha);
    y += digits + 10 * s;
    if (shown.others) {
        // Their tier in words: no meter, as the frame's start and the next one's are not served.
        const std::string tier = level.points ? v2::TierName(v2::FrameTier(level.tier)) : std::string(loc::T("level.not_played"));
        y += v2::Paragraph(d, ImVec2(x, y), room, tier, 14 * s, c.muted);
        return (std::max)(y, picture.max.y);
    }
    const auto progress = player_card::ProgressOf(std::string(), level, false);
    v2::Meter(d, {ImVec2(x, y), ImVec2(x + room, y + 10 * s)}, progress.part, 0.f);
    y += 18 * s;
    y += v2::Paragraph(d, ImVec2(x, y), room, progress.words, 14 * s, c.muted);
    return (std::max)(y, picture.max.y);
}

// How points are earned and the five frames, each an empty frame in its tier's colour with the tier's name in it (the
// thresholds are the bridge's, so they are not drawn). A frame is as wide as its name needs, at least 56 px, and the frames
// go on to a new line when the next does not fit. Returns the bottom.
float DrawEarning(ImDrawList* d, float x, float y, float room, int active) {
    const auto& c = v2::Colors();
    const float s = Scale(), frame = 28 * s;
    v2::SectionHeading(d, ImVec2(x, y), loc::T("fighters.how"));
    y += 22 * s;
    y += v2::Paragraph(d, ImVec2(x, y), room, loc::T("fighters.how_detail"), 14 * s, c.muted) + 12 * s;
    float fx = x;
    for (int tier = 0; tier < netplay::levels::TierCount; ++tier) {
        const v2::Tier award = v2::FrameTier(tier);
        const std::string name = v2::TierName(award);
        const float place = (std::max)(ImGui::GetFont()->CalcTextSizeA(player_card::TextFloor * s, FLT_MAX, 0, name.c_str()).x + 16 * s, 56 * s);
        if (fx > x && fx + place > x + room) { fx = x; y += frame + 8 * s; }
        // The tier is the plate and its word, the focused fighter's at full strength: never a frame.
        const float strength = tier == active ? 1.f : .4f;
        d->AddRectFilled(ImVec2(fx, y), ImVec2(fx + place, y + frame), v2::color::Alpha(v2::TierColor(award).plate, strength), 4 * s);
        v2::Text(d, ImVec2(fx, y + (frame - player_card::TextFloor * s) * .5f), place, name, player_card::TextFloor * s, v2::color::Alpha(c.text, strength), nullptr, true);
        fx += place + 8 * s;
    }
    return y + frame + 8 * s;
}

// The fighter's Games, Won and Since, across `room`. Returns the height.
float DrawRecord(ImDrawList* d, ImVec2 at, float room, const Level& level) {
    const std::vector<v2::TagView::Fact> facts{{loc::T("card.counted_games"), std::to_string(level.games)}, {loc::T("fighters.won"), std::to_string(level.wins)},
        {loc::T("fighters.since"), level.since ? LocalTime(level.since, "%Y-%m-%d") : std::string("-")}};
    return v2::FactCells(d, at, room, facts, 3);
}

// What A does on the focused fighter: save it as the main with the A button's glyph, say it is the main, or say why it
// cannot be saved now. Returns the height.
float DrawSaveLine(ImDrawList* d, ImVec2 at, float room, bool isMain, bool editable) {
    const auto& c = v2::Colors();
    const float s = Scale(), side = 26 * s, font = 15 * s;
    if (isMain || !editable) {
        v2::Paragraph(d, at, room, isMain ? loc::T("fighters.your_main") : loc::T("profile.leave_room_to_edit"), 14 * s, c.muted);
        return 22 * s;
    }
    DrawPromptGlyph(d, "A", ImVec2(at.x, at.y), side, MenuArt(), s);
    v2::Text(d, ImVec2(at.x + side + 8 * s, at.y + (side - font) * .5f), room - side - 8 * s, loc::T("menu.hint.save_main"), font, c.text, HeadingFont());
    return side;
}

// The focused fighter's panel beside the grid, pinned at `top`: its head and, on the player's own tab, its record, how points
// are earned and the frames, and what A does. Another's has the head and the frames. It holds nothing focusable.
void DrawPanel(ImDrawList* d, v2::Box box, int fighter, const Level& level, const Shown& shown, bool isMain, bool editable) {
    const float s = Scale(), pad = 16 * s, head = 40 * s;
    // The contents go over the ground, which waits for their height.
    ImDrawListSplitter split;
    split.Split(d, 2);
    split.SetCurrentChannel(d, 1);
    const float x = box.min.x + pad, room = box.W() - 2 * pad;
    float y = DrawHead(d, ImVec2(x, box.min.y + head + pad), room, 112 * s, 26 * s, fighter, level, shown, isMain, false) + pad;
    if (!shown.others) y += DrawRecord(d, ImVec2(x, y), room, level) + pad;
    y = DrawEarning(d, x, y, room, level.points ? level.tier : -1);
    if (!shown.others) y += DrawSaveLine(d, ImVec2(x, y + 4 * s), room, isMain, editable) + 4 * s;
    box.max.y = y + pad * .5f;
    split.SetCurrentChannel(d, 0);
    v2::Panel(d, box, selection::FindFighter(fighter)->name, isMain ? loc::T("fighters.main") : nullptr);
    split.Merge(d);
}
}

netplay::levels::Own LevelsOfCard(const netplay::levels::Card& card) {
    netplay::levels::Own own;
    own.kind = netplay::levels::Own::Kind::Counted;
    for (const auto& one : card.fighters) {
        if (one.fighter >= own.fighters.size()) continue;
        own.fighters[one.fighter].points = one.points;
        own.fighters[one.fighter].tier = one.tier;
    }
    return own;
}

float Draw(const Shown& shown, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, bool changed) {
    const auto& c = v2::Colors();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, gap = 12 * s;
    auto* d = ImGui::GetWindowDrawList();
    const bool narrow = v2::Narrow(ImGui::GetIO().DisplaySize.x, s);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    static const netplay::levels::Own none;
    const auto& own = shown.levels ? *shown.levels : none;
    const int count = static_cast<int>(own.fighters.size());
    v2::Box list{origin, ImVec2(origin.x + width, FLT_MAX)};
    const v2::Box side = narrow ? v2::Box{} : v2::CutRight(list, 400 * s, 24 * s);
    // The panel follows the focus (the main with the focus on something else) and holds nothing focusable.
    const auto focusedRow = RowKey<int>(nav.Focus(), RowPrefix);
    const int focus = focusedRow && *focusedRow < count ? *focusedRow : shown.main;
    const auto row = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == RowPrefix + std::to_string(focus); });
    const bool editable = !shown.others && row != rows.end() && row->enabled;
    float y = list.min.y;
    // Narrow: a band for the focused fighter above the grid.
    if (narrow && focus >= 0 && focus < count) {
        const v2::Box band{ImVec2(list.min.x, y), ImVec2(list.max.x, y + 120 * s)};
        d->AddRectFilled(band.min, band.max, v2::PanelGround(), 4 * s);
        d->AddRect(band.min, band.max, c.border, 4 * s, 0, s);
        DrawHead(d, ImVec2(band.min.x + 12 * s, band.min.y + 12 * s), band.W() - 24 * s, 96 * s, 22 * s, focus, own.fighters[focus], shown, focus == shown.main, true);
        y = band.max.y + gap;
    }
    // Why there are no points, and the way to them without an account.
    if (shown.why) y += v2::Paragraph(d, ImVec2(list.min.x, y), list.W(), loc::T(shown.why), 14 * s, c.muted) + gap;
    const float below = DrawRowButton(DiscordConnectScreen, ImVec2(list.min.x, y), (std::min)(list.W(), 360 * s), rows, nav, action, changed);
    y = below + (below != y ? gap : 0);

    const int played = static_cast<int>(std::count_if(rows.begin(), rows.end(), [&](const MenuEntry& e) {
        const auto fighter = RowKey<int>(e.id, RowPrefix);
        return fighter && *fighter < count && own.fighters[*fighter].points;
    }));
    const int fighters = static_cast<int>(std::count_if(rows.begin(), rows.end(), [](const MenuEntry& e) { return RowKey<int>(e.id, RowPrefix).has_value(); }));
    const float tile = narrow ? (list.W() - 3 * gap) / 4 : 120 * s, picture = tile, height = picture + (shown.others ? 28 : 48) * s;
    const int columns = narrow ? 4 : (std::max)(1, static_cast<int>((list.W() + gap) / (tile + gap)));
    int column = 0, group = -1;
    float top = y, bottom = y;
    for (const auto& e : rows) {
        const auto fighter = RowKey<int>(e.id, RowPrefix);
        if (!fighter || *fighter >= count) continue;
        const Level& level = own.fighters[*fighter];
        // A group's heading and its first tile's place.
        if (group != (level.points ? 1 : 0)) {
            group = level.points ? 1 : 0;
            if (column) top += height + gap;
            if (bottom > y) top += gap;
            column = 0;
            v2::SectionHeading(d, ImVec2(list.min.x, top), loc::Tf(group ? "fighters.played_heading" : "fighters.unplayed_heading", group ? played : fighters - played));
            top += 24 * s;
        }
        const float left = list.min.x + column * (tile + gap);
        const v2::Box box{ImVec2(left, top), ImVec2(left + tile, top + height)};
        const bool hovered = PressEntry(nav, rows, e, box, action, changed).hovered;
        DrawTile(d, box, *fighter, level, shown, *fighter == shown.main, e.id == nav.Focus(), hovered, e.enabled, picture);
        bottom = box.max.y;
        if (++column == columns) { column = 0; top += height + gap; }
    }
    // Beside the grid, the panel stays where the grid's scroll has left the top of the body.
    if (!narrow && focus >= 0 && focus < count) {
        const float pinned = (std::max)(origin.y, ImGui::GetWindowPos().y);
        DrawPanel(d, v2::Box{ImVec2(side.min.x, pinned), ImVec2(side.max.x, 0)}, focus, own.fighters[focus], shown, focus == shown.main, editable);
    }
    // Narrow: how points are earned, and what the focused fighter did, after the grid.
    if (narrow && focus >= 0 && focus < count) {
        bottom += gap;
        if (!shown.others) bottom += DrawRecord(d, ImVec2(list.min.x, bottom), list.W(), own.fighters[focus]) + gap;
        bottom = DrawEarning(d, list.min.x, bottom, list.W(), own.fighters[focus].points ? own.fighters[focus].tier : -1);
        if (!shown.others) bottom += DrawSaveLine(d, ImVec2(list.min.x, bottom), list.W(), focus == shown.main, editable);
    }
    const float total = bottom + gap - origin.y;
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, total));
    return total;
}

} } }
