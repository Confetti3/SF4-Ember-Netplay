#include "ProfilePanel.hxx"
#include "ShellView.hxx"
#include "MenuRows.hxx"
#include "PlayerCard.hxx"
#include "PublicRoomsPanel.hxx"
#include "../netplay/CardBundle.hxx"
#include "RowId.hxx"
#include "SignInSites.hxx"
#include "Theme.hxx"
#include "../common/LocalTime.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>

namespace sf4e { namespace ui {
namespace {
using netplay::tournament::Command;
using Level = netplay::levels::Level;
constexpr const char* FighterRowPrefix = "main-";  // Select on a fighter makes it the main, as the main page's rows do.

std::string FighterDetail(std::uint64_t points, std::uint64_t games) {
    return loc::Tf("level.value", points) + " \xC2\xB7 " + player_card::GamesWords(games);
}

// What the Fighters tab reads of the answer: the counts while there are some, else a level for each fighter at 0.
const netplay::levels::Own& OwnOf(const ShellView& v, const PlayerCardView& card) {
    static const netplay::levels::Own none;
    return card.levels == CardLevels::Counted ? v.levels.value : none;
}

// A fighter's tile: its picture on its tier edge with the points on it (a
// fighter not played has its picture faded, `0 FP` and the plain edge), the
// "Main" chip on the main's, and under it the name and games.
void DrawFighterTile(ImDrawList* d, v2::Box box, int fighter, const Level& level, FighterArt art, bool isMain, bool focused, bool hovered, bool enabled, float alpha, float picture) {
    const auto& c = v2::Colors();
    const float s = Scale();
    v2::CardGround(d, box, focused, hovered);
    const v2::Box tile{box.min, ImVec2(box.max.x, box.min.y + picture)};
    DrawCharacterPortrait(FighterPicture{fighter, art, 0, 0}, tile.min, tile.max, c.frame);
    if (!level.points) d->AddRectFilled(tile.min, tile.max, v2::color::Alpha(c.frame, .5f));
    v2::LevelBand(d, tile, level.points, alpha);
    DrawFighterEdge(d, tile, level.points, level.tier);
    if (isMain) v2::Chip(d, ImVec2(tile.min.x + 6 * s, tile.min.y + 6 * s), loc::T("fighters.main"), c.accentLight);
    const float x = box.min.x + 6 * s, room = box.W() - 12 * s, y = tile.max.y + 6 * s;
    v2::Text(d, ImVec2(x, y), room, selection::FindFighter(fighter)->name, 14 * s, enabled ? c.text : c.muted, HeadingFont());
    v2::Text(d, ImVec2(x, y + 20 * s), room, player_card::GamesWords(level.games), player_card::TextFloor * s, c.muted);
    if (focused) v2::Focus(d, box.min, box.max, false);
}

// A fighter's picture on its tier edge at `at`, `side` square, and beside it the
// points line, the meter and the way to the next frame (or "Not played yet");
// with `named`, the name (and "Main") over them. Returns the bottom.
float DrawFighterHead(ImDrawList* d, ImVec2 at, float width, float side, float digits, int fighter, const Level& level, FighterArt art, bool isMain, bool named, float alpha) {
    const auto& c = v2::Colors();
    const float s = Scale(), gap = 16 * s, x = at.x + side + gap, room = width - side - gap;
    const v2::Box picture{at, ImVec2(at.x + side, at.y + side)};
    DrawCharacterPortrait(FighterPicture{fighter, art, 0, 0}, picture.min, picture.max, c.frame);
    if (!level.points) d->AddRectFilled(picture.min, picture.max, v2::color::Alpha(c.frame, .5f));
    DrawFighterEdge(d, picture, level.points, level.tier);
    float y = at.y;
    if (named) {
        const float chip = isMain ? v2::ChipWidth(loc::T("fighters.main")) + 8 * s : 0;
        v2::Text(d, ImVec2(x, y), room - chip, selection::FindFighter(fighter)->name, 17 * s, c.text, HeadingFont());
        if (isMain) v2::Chip(d, ImVec2(x + room - chip + 8 * s, y), loc::T("fighters.main"), c.accentLight);
        y += 24 * s;
    }
    v2::Level(d, ImVec2(x, y), digits, level.points, "points.label", alpha);
    y += digits + 10 * s;
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

// What A does on the focused fighter: save it as the main with the A button's
// glyph, say it is the main, or say why it cannot be saved now. Returns the height.
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

// The focused fighter's panel beside the grid, pinned at `top`: its head, record, how points are earned and the frames, and
// what A does. It holds nothing focusable.
void DrawFighterPanel(ImDrawList* d, v2::Box box, int fighter, const Level& level, bool isMain, bool editable, FighterArt art, float alpha) {
    const float s = Scale(), pad = 16 * s, head = 40 * s;
    // The contents go over the ground, which waits for their height.
    ImDrawListSplitter split;
    split.Split(d, 2);
    split.SetCurrentChannel(d, 1);
    const float x = box.min.x + pad, room = box.W() - 2 * pad;
    float y = DrawFighterHead(d, ImVec2(x, box.min.y + head + pad), room, 112 * s, 26 * s, fighter, level, art, isMain, false, alpha) + pad;
    y += DrawRecord(d, ImVec2(x, y), room, level) + pad;
    y = DrawEarning(d, x, y, room, level.points ? level.tier : -1);
    y += DrawSaveLine(d, ImVec2(x, y + 4 * s), room, isMain, editable) + 4 * s;
    box.max.y = y + pad * .5f;
    split.SetCurrentChannel(d, 0);
    v2::Panel(d, box, selection::FindFighter(fighter)->name, isMain ? loc::T("fighters.main") : nullptr);
    split.Merge(d);
}

// The Fighters tab: every fighter in two groups, the played ones (most points first) and the rest (roster order), each a
// row of tiles under its heading; the state's words and Connect Discord above them when there are no points; and the
// focused fighter's panel beside (narrow: a band above the grid, and how points are earned after it).
float DrawFightersTab(const ShellView& v, int main, const PlayerCardView& card, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, bool changed) {
    const auto& c = v2::Colors();
    const float s = Scale(), width = ImGui::GetContentRegionAvail().x, gap = 12 * s;
    auto* d = ImGui::GetWindowDrawList();
    const bool narrow = v2::Narrow(ImGui::GetIO().DisplaySize.x, s);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const FighterArt art = v.preferences.roomCharacterPictures ? FighterArt::Character : FighterArt::Portrait;
    const auto& own = OwnOf(v, card);
    const float alpha = card.levels == CardLevels::Pending ? .4f : 1.f;
    v2::Box list{origin, ImVec2(origin.x + width, FLT_MAX)};
    const v2::Box side = narrow ? v2::Box{} : v2::CutRight(list, 400 * s, 24 * s);
    // The panel follows the focus (the main with the focus on something else) and holds nothing focusable.
    const auto focusedRow = RowKey<int>(nav.Focus(), FighterRowPrefix);
    const int shown = focusedRow && *focusedRow < static_cast<int>(own.fighters.size()) ? *focusedRow : main;
    const auto row = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == FighterRowPrefix + std::to_string(shown); });
    const bool editable = row != rows.end() && row->enabled;
    float y = list.min.y;
    // Narrow: a band for the focused fighter above the grid.
    if (narrow && shown >= 0 && shown < static_cast<int>(own.fighters.size())) {
        const v2::Box band{ImVec2(list.min.x, y), ImVec2(list.max.x, y + 120 * s)};
        d->AddRectFilled(band.min, band.max, v2::PanelGround(), 4 * s);
        d->AddRect(band.min, band.max, c.border, 4 * s, 0, s);
        DrawFighterHead(d, ImVec2(band.min.x + 12 * s, band.min.y + 12 * s), band.W() - 24 * s, 96 * s, 22 * s, shown, own.fighters[shown], art, shown == main, true, alpha);
        y = band.max.y + gap;
    }
    // Why there are no points, and the way to them without an account.
    if (const char* why = player_card::NoLevelWords(card); why && card.levels != CardLevels::Pending) y += v2::Paragraph(d, ImVec2(list.min.x, y), list.W(), loc::T(why), 14 * s, c.muted) + gap;
    const float below = DrawRowButton(DiscordConnectScreen, ImVec2(list.min.x, y), (std::min)(list.W(), 360 * s), rows, nav, action, changed);
    y = below + (below != y ? gap : 0);

    const int played = static_cast<int>(std::count_if(rows.begin(), rows.end(), [&](const MenuEntry& e) {
        const auto fighter = RowKey<int>(e.id, FighterRowPrefix);
        return fighter && *fighter < static_cast<int>(own.fighters.size()) && own.fighters[*fighter].points;
    }));
    const int fighters = static_cast<int>(std::count_if(rows.begin(), rows.end(), [](const MenuEntry& e) { return RowKey<int>(e.id, FighterRowPrefix).has_value(); }));
    const float tile = narrow ? (list.W() - 3 * gap) / 4 : 120 * s, picture = tile, height = picture + 48 * s;
    const int columns = narrow ? 4 : (std::max)(1, static_cast<int>((list.W() + gap) / (tile + gap)));
    int column = 0, group = -1;
    float top = y, bottom = y;
    for (const auto& e : rows) {
        const auto fighter = RowKey<int>(e.id, FighterRowPrefix);
        if (!fighter || *fighter >= static_cast<int>(own.fighters.size())) continue;
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
        DrawFighterTile(d, box, *fighter, level, art, *fighter == main, e.id == nav.Focus(), hovered, e.enabled, alpha, picture);
        bottom = box.max.y;
        if (++column == columns) { column = 0; top += height + gap; }
    }
    // Beside the grid, the panel stays where the grid's scroll has left the top of the body.
    if (!narrow && shown >= 0 && shown < static_cast<int>(own.fighters.size())) {
        const float pinned = (std::max)(origin.y, ImGui::GetWindowPos().y);
        DrawFighterPanel(d, v2::Box{ImVec2(side.min.x, pinned), ImVec2(side.max.x, 0)}, shown, own.fighters[shown], shown == main, editable, art, alpha);
    }
    // Narrow: how points are earned, and what the focused fighter did, after the grid.
    if (narrow && shown >= 0 && shown < static_cast<int>(own.fighters.size())) {
        bottom += gap;
        bottom += DrawRecord(d, ImVec2(list.min.x, bottom), list.W(), own.fighters[shown]) + gap;
        bottom = DrawEarning(d, list.min.x, bottom, list.W(), own.fighters[shown].points ? own.fighters[shown].tier : -1);
        bottom += DrawSaveLine(d, ImVec2(list.min.x, bottom), list.W(), shown == main, editable);
    }
    const float total = bottom + gap - origin.y;
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, total));
    return total;
}
}

void ProfilePanel::Dress(GameMenu& menu, const std::string& screen, const std::string& name) {
    menu.eyebrow = name.empty() ? std::string(loc::T("home.profile")) : loc::Tf("profile.eyebrow", name);
    const int requests = MenuPlayerCard().requests;
    menu.tabs = {{CardScreen, loc::T("profile.tab.card")}, {FightersScreen, loc::T("profile.tab.fighters")}, {SetsScreen, loc::T("profile.tab.sets")},
        {FriendsScreen, requests ? loc::Tf("friends.tab_count", requests) : std::string(loc::T("profile.tab.friends"))}, {"awards", loc::T("screen.awards")}};
    for (std::size_t i = 0; i < menu.tabs.size(); ++i)
        if (menu.tabs[i].first == screen) menu.tab = static_cast<int>(i);
}

void ProfilePanel::Update(const ShellView& v, const std::string& screen, const std::string& bridge, const Submit& submit) {
    bridge_ = bridge;
    // The recent sets are asked for while the Card tab is on show: once when it comes on show and when the name or service
    // changes, never every frame and never while another page shows.
    sets_.Update(v, bridge, v.preferences.displayName, screen == CardScreen, submit);
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

std::vector<MenuEntry> ProfilePanel::Rows(const ShellView& v, const netplay::PlayerPreferences& prefs, const std::string& screen, const PlayerCardView& card, bool editable, bool connect, std::string& title) const {
    const bool fighters = screen == FightersScreen;
    title = loc::T(fighters ? "profile.tab.fighters" : "profile.tab.card");
    std::vector<MenuEntry> rows;
    if (fighters) {
        // Every fighter, the played ones first (the main leads its group), at 0 points when there are none; and, without
        // an account, the way to one.
        const auto& own = OwnOf(v, card);
        const auto groups = player_card::Grouped(own, card.mainFighter);
        for (const auto* group : {&groups.played, &groups.unplayed}) {
            for (const int fighter : *group) {
                auto row = Row(FighterRowPrefix + std::to_string(fighter), selection::FindFighter(fighter)->name,
                    FighterDetail(own.fighters[fighter].points, own.fighters[fighter].games) + "\n" + loc::T(editable ? "profile.choose_main_detail" : "profile.leave_room_to_edit"), editable);
                row.hint = loc::T("menu.hint.save_main");
                rows.push_back(std::move(row));
            }
        }
        if (connect) rows.push_back(ConnectDiscordRow());
        return rows;
    }
    // The Card tab's rows: the way to choose a main when there is none, then the card's own parts (the strip's tiles, the
    // award tiles, Most played, the recent sets and All sets: player_card::CardRows, the parts drawn), Edit card, Fighter
    // select, and Connect Discord when there is no account.
    const auto view = player_card::FullOf(SubjectOf(v, prefs), player_card::CardSize());
    if (view.hero < 0) {
        auto choose = Row(ChooseMainRow, loc::T("card.choose_main"), loc::T("profile.choose_main_detail"));
        choose.hint = loc::T("card.choose_main");
        rows.push_back(std::move(choose));
    }
    for (auto& row : player_card::CardRows(view)) rows.push_back(std::move(row));
    auto edit = Row(CardEditRow, loc::T("card.edit"), loc::T("card.edit_detail"));
    edit.hint = loc::T("card.edit");
    rows.push_back(std::move(edit));
    // Fighter select, with what Home used to say of it: the pick, or why it cannot change now.
    auto select = Row(FighterSelectRow, loc::T("home.fighter_select"),
        v.canEditSelection ? v.selectionSummary : v.selectionLockReason.empty() ? std::string(loc::T("home.selection_locked")) : v.selectionLockReason);
    select.hint = loc::T("home.fighter_select");
    rows.push_back(std::move(select));
    if (connect) rows.push_back(ConnectDiscordRow());
    return rows;
}

player_card::Subject ProfilePanel::OwnSubjectOf(const ShellView& v, const netplay::PlayerPreferences& prefs) {
    player_card::OwnInputs in;
    in.levels = &v.levels;
    in.name = prefs.displayName;
    in.country = ShownCountry(prefs.country, v.automaticCountry);
    const auto& standing = v.rankings.standing;
    if (standing.loaded && standing.value.kind == netplay::rankings::Standing::Kind::Ranked && standing.value.classesShown) in.badge = standing.value.badge;
    in.main = prefs.mainFighter;
    in.worn.title = prefs.wornTitle;
    in.worn.icon = prefs.wornIcon;
    in.pins = prefs.cardPins;
    // How the card says the player plays: their choices and what the game measures now (the card save's own rule).
    in.style = netplay::profiles::OwnStyle(prefs, netplay::profiles::AutomaticDevice(v.inputDevice.type, v.inputDevice.connected, v.gameplayBrand), v.ownLink,
        PublicRoomsPanel::KnownRegion(v));
    const auto* friends = v.friends.state.value.get();
    in.friends = friends && friends->kind == netplay::friends::State::Kind::Listed ? static_cast<std::uint32_t>(friends->friends.size()) : 0;
    return player_card::OwnSubject(in);
}

player_card::Subject ProfilePanel::SubjectOf(const ShellView& v, const netplay::PlayerPreferences& prefs) const {
    auto subject = OwnSubjectOf(v, prefs);
    subject.sets = sets_.View(v);
    subject.now = v.unixNow;
    subject.buttons = {{CardEditRow, "Y", v2::ButtonKind::Secondary}, {FighterSelectRow, "X", v2::ButtonKind::Secondary}};
    return subject;
}

OtherCardView ProfilePanel::OwnPanel(const ShellView& v, const netplay::PlayerPreferences& prefs) {
    return PanelOf(OwnSubjectOf(v, prefs), v.preferences.roomCharacterPictures ? FighterArt::Character : FighterArt::Portrait);
}

bool ProfilePanel::Activate(const ShellView& v, const MenuAction& a, MenuNavigation& nav, const Submit& submit, WatchQueue& queue, std::string& error) {
    if (a.id == CardEditRow) { nav.Push(CardEditRow); return true; }
    // A strip tile opens the Fighters page on that fighter.
    if (a.id.compare(0, 13, "card-fighter:") == 0) {
        nav.Replace(FightersScreen);
        nav.Prefer("main-" + a.id.substr(13));
        return true;
    }
    // An award tile opens the wall on that family, and All sets the Sets tab.
    if (a.id.compare(0, std::string(player_card::AwardRowPrefix).size(), player_card::AwardRowPrefix) == 0) {
        nav.Replace("awards");
        nav.Prefer("award:" + a.id.substr(std::string(player_card::AwardRowPrefix).size()));
        return true;
    }
    if (a.id == player_card::AllSetsRow) { nav.Replace(SetsScreen); return true; }
    // Most played opens that player's profile by the token the points answer carries.
    if (a.id == player_card::MostPlayedRow) {
        if (!v.levels.value.hasMostPlayed || bridge_.empty()) return true;
        const auto& rival = v.levels.value.mostPlayed;
        commands::ProfileOpen open;
        open.bridge = bridge_;
        open.key = netplay::profiles::Key::OfCard(rival.card);
        open.known.name = rival.name;
        open.known.country = rival.country;
        open.known.badge = rival.badge;
        if (submit(std::move(open))) nav.Push(PlayerScreen);
        return true;
    }
    return sets_.Activate(a.id, v, submit, queue, error);
}

void ProfilePanel::Draw(const ShellView& v, const netplay::PlayerPreferences& prefs, const std::string& screen, const std::vector<MenuEntry>& rows, MenuNavigation& nav,
    MenuAction& action, float height) {
    const bool changed = nav.Focus() != focus_;
    focus_ = nav.Focus();
    const auto& card = MenuPlayerCard();
    if (screen == FightersScreen) { DrawFightersTab(v, prefs.mainFighter, card, rows, nav, action, changed); return; }
    const auto view = player_card::FullOf(SubjectOf(v, prefs), player_card::CardSize());
    DrawFullCard(view, v.preferences.roomCharacterPictures ? FighterArt::Character : FighterArt::Portrait, rows, nav, action, changed, height);
}
} }
