#include "ProfilePanel.hxx"
#include "FightersTab.hxx"
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
constexpr const char* FighterRowPrefix = fighters_tab::RowPrefix;  // Select on a fighter makes it the main, as the main page's rows do.

std::string FighterDetail(std::uint64_t points, std::uint64_t games) {
    return loc::Tf("level.value", points) + " \xC2\xB7 " + player_card::GamesWords(games);
}

// What the Fighters tab reads of the answer: the counts while there are some, else a level for each fighter at 0.
const netplay::levels::Own& OwnOf(const ShellView& v, const PlayerCardView& card) {
    static const netplay::levels::Own none;
    return card.levels == CardLevels::Counted ? v.levels.value : none;
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
    const auto view = player_card::FullOf(SubjectOf(v, prefs), CardSize());
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

bool ProfilePanel::MostPlayedOf(const ShellView& v, netplay::profiles::Key& key, netplay::profiles::Known& known) {
    if (!v.levels.value.hasMostPlayed) return false;
    const auto& rival = v.levels.value.mostPlayed;
    key = netplay::profiles::Key::OfCard(rival.card);
    known = netplay::profiles::Known();
    known.name = rival.name;
    known.country = rival.country;
    known.badge = rival.badge;
    return key.Present();
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
    return sets_.Activate(a.id, v, submit, queue, error);
}

void ProfilePanel::Draw(const ShellView& v, const netplay::PlayerPreferences& prefs, const std::string& screen, const std::vector<MenuEntry>& rows, MenuNavigation& nav,
    MenuAction& action, float height) {
    const bool changed = nav.Focus() != focus_;
    focus_ = nav.Focus();
    const auto& card = MenuPlayerCard();
    if (screen == FightersScreen) {
        fighters_tab::Shown shown;
        shown.levels = &OwnOf(v, card);
        shown.main = prefs.mainFighter;
        shown.art = v.preferences.roomCharacterPictures ? FighterArt::Character : FighterArt::Portrait;
        shown.alpha = card.levels == CardLevels::Pending ? .4f : 1.f;
        // Why there are no points, while the points are not being read.
        shown.why = card.levels == CardLevels::Pending ? nullptr : player_card::NoLevelWords(card);
        fighters_tab::Draw(shown, rows, nav, action, changed);
        return;
    }
    const auto view = player_card::FullOf(SubjectOf(v, prefs), CardSize());
    DrawFullCard(view, v.preferences.roomCharacterPictures ? FighterArt::Character : FighterArt::Portrait, rows, nav, action, changed, height);
}
} }
