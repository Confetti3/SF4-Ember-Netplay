#include "ProfilePanel.hxx"
#include "ShellView.hxx"
#include "MenuRows.hxx"
#include "PlayerCard.hxx"
#include "RowId.hxx"
#include "SignInSites.hxx"
#include "Theme.hxx"
#include "../common/LocalTime.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cfloat>

namespace sf4e { namespace ui {
namespace {
using netplay::tournament::Command;
constexpr const char* FighterRowPrefix = "main-";  // Select on a fighter makes it the main, as the main page's rows do.
constexpr const char* UnplayedRow = "fighters-unplayed";


std::string FighterDetail(const netplay::levels::FighterLevel& level) {
    return loc::Tf("level.value", level.points) + " \xC2\xB7 " + loc::Tf("level.games", level.games);
}

// A fighter's row on the Fighters board: its picture with the points on it,
// its name and games, and its meter.
void DrawFighterRow(ImDrawList* d, v2::Box box, int fighter, const netplay::levels::FighterLevel& level, FighterArt art, bool focused, bool hovered, bool enabled) {
    const auto& c = v2::Colors();
    const float s = Scale(), pad = 8 * s, side = box.H() - 2 * pad;
    v2::CardGround(d, box, focused, hovered);
    const v2::Box tile{ImVec2(box.min.x + pad, box.min.y + pad), ImVec2(box.min.x + pad + side, box.max.y - pad)};
    DrawCharacterPortrait(FighterPicture{fighter, art, 0, 0}, tile.min, tile.max, c.frame);
    v2::LevelBand(d, tile, level.points, level.tier);
    const float x = tile.max.x + 12 * s, room = box.max.x - pad - x;
    v2::Text(d, ImVec2(x, box.min.y + 12 * s), room, selection::FindFighter(fighter)->name, 15 * s, enabled ? c.text : c.muted, HeadingFont());
    v2::Text(d, ImVec2(x, box.min.y + 34 * s), room, loc::Tf("level.games", level.games), 12 * s, c.muted);
    v2::Meter(d, {ImVec2(x, box.max.y - pad - 8 * s), ImVec2(x + room, box.max.y - pad - 2 * s)}, player_card::Part(level), 0.f);
    if (focused) v2::Focus(d, box.min, box.max);
}

// The focused fighter's panel: a large picture and its points, the way to the
// next tier, its games and wins, how points are earned and the frames.
// Returns the panel's bottom.
float DrawFighterPanel(ImDrawList* d, v2::Box box, int fighter, const netplay::levels::Own& own, bool isMain, FighterArt art) {
    const auto& c = v2::Colors();
    const float s = Scale(), pad = 16 * s, picture = 112 * s, head = 40 * s;
    const auto& level = own.fighters[fighter];
    // The contents go over the ground, which waits for their height.
    ImDrawListSplitter split;
    split.Split(d, 2);
    split.SetCurrentChannel(d, 1);
    const float x = box.min.x + pad, room = box.W() - 2 * pad;
    float y = box.min.y + head + pad;
    const v2::Box tile{ImVec2(x, y), ImVec2(x + picture, y + picture)};
    DrawCharacterPortrait(FighterPicture{fighter, art, 0, 0}, tile.min, tile.max, c.frame);
    v2::LevelBand(d, tile, level.points, level.tier);
    const float tx = tile.max.x + pad, tw = room - picture - pad;
    v2::Text(d, ImVec2(tx, y), tw, loc::Tf("level.value", level.points), 22 * s, c.text, HeadingFont());
    v2::Meter(d, {ImVec2(tx, y + 38 * s), ImVec2(tx + tw, y + 48 * s)}, player_card::Part(level), 0.f);
    // The way to the next tier wraps rather than shortens, and the facts go under it when it is longer than the picture.
    const float away = v2::Paragraph(d, ImVec2(tx, y + 58 * s), tw,
        level.next ? loc::Tf("level.points_to", level.next - level.points, v2::TierName(v2::FrameTier(level.tier + 1))) : std::string(loc::T("level.top")), 14 * s, c.muted);
    y += (std::max)(picture, 58 * s + away) + pad;
    const std::vector<v2::TagView::Fact> facts{{loc::T("card.counted_games"), std::to_string(level.games)}, {loc::T("fighters.won"), std::to_string(level.wins)},
        {loc::T("fighters.since"), level.since ? LocalTime(level.since, "%Y-%m-%d") : std::string("-")}};
    y += v2::FactCells(d, ImVec2(x, y), room, facts, 3) + pad;
    v2::SectionHeading(d, ImVec2(x, y), loc::T("fighters.how"));
    y += 22 * s;
    y += v2::Paragraph(d, ImVec2(x, y), room, loc::T("fighters.how_detail"), 14 * s, c.muted) + 12 * s;
    // The five frames, each an empty frame in its tier's colour with the tier's name in it (the thresholds are the
    // bridge's, so they are not drawn). A frame is as wide as its name needs, at least 56 px, and the frames go on to
    // a new line when the next does not fit.
    float fx = x;
    const float frame = 28 * s;
    for (int tier = 0; tier < netplay::levels::TierCount; ++tier) {
        const v2::Tier award = v2::FrameTier(tier);
        const std::string name = v2::TierName(award);
        const float place = (std::max)(ImGui::GetFont()->CalcTextSizeA(12 * s, FLT_MAX, 0, name.c_str()).x + 16 * s, 56 * s);
        if (fx > x && fx + place > x + room) { fx = x; y += frame + 8 * s; }
        d->AddRectFilled(ImVec2(fx, y), ImVec2(fx + place, y + frame), c.ink, 4 * s);
        v2::Text(d, ImVec2(fx, y + (frame - 12 * s) * .5f), place, name, 12 * s, c.text, nullptr, true);
        d->AddRect(ImVec2(fx, y), ImVec2(fx + place, y + frame), v2::TierColor(award).stripe, 4 * s, 0, 2 * s);
        fx += place + 8 * s;
    }
    y += frame + 8 * s;
    if (!own.shown) y += v2::Paragraph(d, ImVec2(x, y), room, loc::T("level.hidden"), 14 * s, c.muted) + pad;
    box.max.y = y + pad * .5f;
    split.SetCurrentChannel(d, 0);
    v2::Panel(d, box, selection::FindFighter(fighter)->name, isMain ? loc::T("fighters.main") : nullptr);
    split.Merge(d);
    return box.max.y;
}

// The Fighters tab: the played fighters in two columns, most points first,
// one info row after them, and the focused fighter's panel beside (under, when narrow).
float DrawFightersTab(const ShellView& v, int main, const PlayerCardView& card, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, bool changed) {
    const auto& c = v2::Colors();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, gap = 12 * s, rowHeight = 76 * s;
    auto* d = ImGui::GetWindowDrawList();
    const bool narrow = v2::Narrow(ImGui::GetIO().DisplaySize.x, s);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const FighterArt art = v.preferences.roomCharacterPictures ? FighterArt::Character : FighterArt::Portrait;
    v2::Box list{origin, ImVec2(origin.x + width, FLT_MAX)};
    const v2::Box side = narrow ? v2::Box{} : v2::CutRight(list, (std::min)(400 * s, width * .4f), 24 * s);
    const auto& own = v.levels.value;
    float bottom = list.min.y;
    int placed = 0, focused = -1, first = -1;
    const int columns = narrow ? 1 : 2;
    const float cell = (list.W() - gap * (columns - 1)) / columns;
    for (const auto& e : rows) {
        const auto fighter = RowKey<int>(e.id, FighterRowPrefix);
        if (card.levels == CardLevels::Counted && fighter && *fighter < static_cast<int>(own.fighters.size())) {
            const float left = list.min.x + (placed % columns) * (cell + gap), top = list.min.y + (placed / columns) * (rowHeight + gap);
            const v2::Box box{ImVec2(left, top), ImVec2(left + cell, top + rowHeight)};
            const bool hovered = PressEntry(nav, rows, e, box, action, changed).hovered, isFocus = e.id == nav.Focus();
            DrawFighterRow(d, box, *fighter, own.fighters[*fighter], art, isFocus, hovered, e.enabled);
            if (isFocus) focused = *fighter;
            if (first < 0) first = *fighter;
            ++placed;
            bottom = box.max.y;
        } else if (e.id == DiscordConnectScreen) {
            bottom = DrawRowButton(DiscordConnectScreen, ImVec2(list.min.x, bottom > list.min.y ? bottom + gap : list.min.y), (std::min)(list.W(), 360 * s), rows, nav, action, changed);
        } else if (e.id == UnplayedRow || e.id == ProfileStateRow) {
            // The words after the fighters, or in their place when there are none.
            const float top = placed ? bottom + gap : list.min.y, wide = list.W(), height = (std::max)(48 * s, v2::Paragraph(nullptr, {}, wide - 28 * s, e.label, 14 * s, 0) + 16 * s);
            const v2::Box box{ImVec2(list.min.x, top), ImVec2(list.min.x + wide, top + height)};
            const bool hovered = PressEntry(nav, rows, e, box, action, changed).hovered;
            v2::RowGround(d, box, e.id == nav.Focus(), hovered);
            v2::Paragraph(d, ImVec2(box.min.x + 14 * s, box.min.y + 8 * s), wide - 28 * s, e.label, 14 * s, c.muted);
            if (e.id == nav.Focus()) v2::Focus(d, box.min, box.max);
            bottom = box.max.y;
        }
    }
    // The panel follows the focus and holds nothing focusable; with the focus on the words it shows the main, or the first.
    if (focused < 0) focused = main >= 0 && main < static_cast<int>(own.fighters.size()) && own.fighters[main].points ? main : first;
    if (card.levels == CardLevels::Counted && focused >= 0) {
        const v2::Box panel = narrow ? v2::Box{ImVec2(list.min.x, bottom + gap), ImVec2(list.max.x, 0)} : v2::Box{side.min, ImVec2(side.max.x, 0)};
        bottom = (std::max)(bottom, DrawFighterPanel(d, panel, focused, own, focused == main, art));
    }
    const float height = bottom + gap - origin.y;
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    return height;
}
}

void ProfilePanel::Dress(GameMenu& menu, const std::string& screen, const std::string& name) {
    menu.eyebrow = name.empty() ? std::string(loc::T("home.profile")) : loc::Tf("profile.eyebrow", name);
    // Friends (FRIENDS.md) goes between Fighters and Titles once the view offers friends.
    menu.tabs = {{CardScreen, loc::T("profile.tab.card")}, {FightersScreen, loc::T("profile.tab.fighters")},
        {"awards", loc::T("awards.titles")}, {"awards-icons", loc::T("awards.icons")}};
    for (std::size_t i = 0; i < menu.tabs.size(); ++i)
        if (menu.tabs[i].first == screen) menu.tab = static_cast<int>(i);
}

void ProfilePanel::Update(const ShellView& v, const std::string& screen, const std::string& bridge, const Submit& submit) {
    const bool visiting = InStrip(screen);
    if (visiting && !visiting_) want_ = true;
    visiting_ = visiting;
    if (bridge.empty() || v.identity.emberId.empty() || v.levels.loading) return;
    const std::string key = bridge + "\n" + v.identity.emberId;
    if (!want_ && key == read_) return;
    commands::Tournament action;
    action.tournament.op = Command::Op::LevelsMe;
    action.tournament.bridgeId = bridge;
    if (!submit(std::move(action))) return;
    read_ = key;
    want_ = false;
}

void ProfilePanel::Conceal() { visiting_ = want_ = false; }

std::vector<MenuEntry> ProfilePanel::Rows(const ShellView& v, const std::string& screen, const PlayerCardView& card, bool editable, std::string& title) const {
    const bool fighters = screen == FightersScreen;
    title = loc::T(fighters ? "profile.tab.fighters" : "profile.tab.card");
    std::vector<MenuEntry> rows;
    const auto& own = v.levels.value;
    if (card.levels == CardLevels::Counted) {
        if (fighters) {
            for (const int fighter : player_card::PlayedFighters(own)) {
                auto row = Row(FighterRowPrefix + std::to_string(fighter), selection::FindFighter(fighter)->name,
                    FighterDetail(own.fighters[fighter]) + "\n" + loc::T(editable ? "profile.choose_main_detail" : "profile.leave_room_to_edit"), editable);
                row.hint = loc::T("menu.hint.save_main");
                rows.push_back(std::move(row));
            }
            const int unplayed = static_cast<int>(own.fighters.size()) - card.facts.fightersPlayed;
            if (card.facts.fightersPlayed && unplayed > 0) {
                auto row = InfoRow(UnplayedRow, loc::Tf("fighters.unplayed", unplayed), {}, {});
                rows.push_back(std::move(row));
            }
        } else {
            for (const auto& fighter : card.fighters) {
                auto row = Row(CardFighterRow(fighter.fighter), selection::FindFighter(fighter.fighter)->name, FighterDetail(own.fighters[fighter.fighter]));
                row.hint = loc::T("profile.tab.fighters");
                rows.push_back(std::move(row));
            }
        }
    }
    if (const char* words = player_card::NoLevelWords(card)) {
        // Without an account the way to one is a row; the other states are only words.
        if (card.levels == CardLevels::NoAccount) {
            auto connect = Row(DiscordConnectScreen, loc::T("screen.connect_discord"), loc::T("rankings.connect_detail"));
            connect.value = loc::T("identity.discord_none"); connect.hint = loc::T("screen.connect_discord");
            rows.push_back(std::move(connect));
        }
        rows.insert(rows.begin(), InfoRow(ProfileStateRow, loc::T(words), {}, {}));
    }
    return rows;
}

void ProfilePanel::Draw(const ShellView& v, int main, const std::string& screen, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action) {
    const bool changed = nav.Focus() != focus_;
    focus_ = nav.Focus();
    const auto& card = MenuPlayerCard();
    if (screen == FightersScreen) DrawFightersTab(v, main, card, rows, nav, action, changed);
    else DrawCardTab(v, main, card, rows, nav, action, changed);
}
} }
