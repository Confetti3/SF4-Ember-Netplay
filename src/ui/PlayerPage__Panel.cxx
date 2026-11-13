#include "PlayerPage.hxx"
#include "FightersTab.hxx"
#include "MenuRows.hxx"
#include "ProfilePanel.hxx"
#include "RowId.hxx"
#include "ShellView.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include "../netplay/RuntimeCommands.hxx"
#include <algorithm>

// The page between frames: what it was asked to open, when it asks the runtime, how it keeps to the tabs its state has,
// and what a press does (always through the friends paths, never a second version of them).
namespace sf4e { namespace ui {
using namespace player_page;
namespace {
ArchivePlace SetsPlace() {
    ArchivePlace place;
    place.profile = true;
    place.lists = {SetsScreen, ProfilePanel::SetsScreen};
    place.filter = SetsFilterScreen;
    place.fighter = SetsFighterScreen;
    return place;
}

bool IsTab(const std::string& screen) { return screen == CardScreen || screen == FightersScreen || screen == SetsScreen || screen == AwardsScreen; }
// The screens of a Sets list: another player's, the player's own, and the pages over either.
bool OnSets(const std::string& screen) { return screen == SetsScreen || screen == ProfilePanel::SetsScreen || screen == SetsFilterScreen || screen == SetsFighterScreen; }
}

bool PlayerPage::Owns(const std::string& screen) { return OnProfile(screen) || screen == ProfilePanel::SetsScreen; }

PlayerPage::PlayerPage() : sets_(SetsPlace()) {}

void PlayerPage::Begin(const netplay::profiles::Key& key, const netplay::profiles::Known& known, MenuNavigation& nav) {
    key_ = key;
    known_ = known;
    asked_ = false;
    focused_ = 0;
    ask_.clear();
    raise_ = Raise();
    // Opened from a profile (a set's opponent, the pick list) it replaces that one, so Back goes where the first came from.
    if (OnProfile(nav.Screen())) nav.Replace(CardScreen);
    else nav.Push(CardScreen);
}

void PlayerPage::Open(const netplay::profiles::Key& key, const netplay::profiles::Known& known, MenuNavigation& nav) {
    if (!key.Present()) return;
    // The player themselves are not another's page: their own Profile opens on Card.
    if (OpensOwn(key, known, ownName_, ownCountry_)) {
        if (OnProfile(nav.Screen())) nav.Replace(ProfilePanel::CardScreen);
        else nav.Push(ProfilePanel::CardScreen);
        return;
    }
    Begin(key, known, nav);
}

void PlayerPage::Conceal() { focus_.clear(); }

Links PlayerPage::PlainLinks(const ShellView& v, const Shown& shown) const {
    Links links;
    links.gate = friends_panel::GateOf(v.friends, !bridge_.empty(), false);
    links.member = friends_panel::MemberGateOf(v.friends, !bridge_.empty(), false);
    links.canInvite = v.friends.canInvite;
    links.steam = shown.known.steam.Present();
    return links;
}

Links PlayerPage::LinksOf(const ShellView& v, const FriendsPanel& friends, const Shown& shown) const {
    Links links = PlainLinks(v, shown);
    links.gate = friends.Gates(v);
    links.member = friends.MemberGates(v);
    return links;
}

Shown PlayerPage::ShownNow(const ShellView& v) const {
    return ShownOf(v.profile, key_, known_, !bridge_.empty(), v.profilesService, friends_panel::StateOf(v.friends));
}

Standing PlayerPage::StandingNow(const ShellView& v, const Shown& shown, const Links& links) const {
    return StandingOf(shown, friends_panel::StateOf(v.friends), links);
}

Around PlayerPage::AroundOf(const ShellView& v, const Shown& shown, const Standing& standing) const {
    Around around;
    around.now = v.unixNow;
    if (!standing.friendId.empty()) around.friendEntry = friends_panel::StateOf(v.friends).FindFriend(standing.friendId);
    // A member of the room this PC is in, found by the card token their ticket carried: the fighter they picked is the hero.
    if (!shown.token.empty())
        for (const auto& member : v.room.members)
            if (member.card.Present() && member.card.ToString() == shown.token) {
                around.inRoom = true;
                around.roomFighter = member.fighter;
                break;
            }
    return around;
}

player_card::Subject PlayerPage::SubjectNow(const ShellView& v, const Shown& shown, const Standing& standing) const {
    return SubjectOf(shown, standing, AroundOf(v, shown, standing), cardSets_.View(v));
}

void PlayerPage::Update(const ShellView& v, const std::string& screen, MenuNavigation& nav, const std::string& bridge, const std::string& ownName,
    const CountryCode& ownCountry, const Submit& submit) {
    bridge_ = bridge;
    ownName_ = ownName;
    ownCountry_ = ownCountry;
    const bool on = OnProfile(screen);
    // Leaving the page closes the profile: its late answers are dropped. What it was asked to open is kept, so coming back
    // (from Notifications, opened over it) asks for it again.
    if (onProfile_ && !on && submit(commands::ProfileClose{})) {
        onProfile_ = false;
        asked_ = false;
        ask_.clear();
    }
    if (on) onProfile_ = true;
    // The open goes out as soon as there is a service to send it to (and again for Try again).
    if (on && !asked_ && key_.Present() && !bridge.empty()) {
        commands::ProfileOpen open;
        open.bridge = bridge;
        open.key = key_;
        open.known = known_;
        if (submit(std::move(open))) asked_ = true;
    }
    const Shown shown = ShownNow(v);
    // Their recent sets on the Card tab, and the Sets lists: the sets of their name, or of the player's own on the own tab.
    cardSets_.Update(v, bridge, shown.name, screen == CardScreen && shown.page != Page::Blocked, submit);
    if (screen == ProfilePanel::SetsScreen) sets_.BrowseName(ownName);
    else if (on && OnSets(screen)) sets_.BrowseName(shown.name);
    sets_.Update(v, screen, bridge, nav.Focus(), submit);
    if (!on) return;
    // Several players go by the name: the pick list asks which. A tab the state does not have gives way to Card.
    if (shown.page == Page::Several && screen != PickScreen && IsTab(screen)) nav.Replace(PickScreen);
    else if (shown.page != Page::Several && screen == PickScreen && key_.Present() && v.profile.Open() && v.profile.key == key_) nav.Replace(CardScreen);
    else if (IsTab(screen) && shown.page != Page::Several && !HasTab(shown, screen)) nav.Replace(CardScreen);
    // The first settled card opens with the focus on the Primary when it can be pressed, else on its first fighter.
    if (screen == CardScreen && shown.page != Page::Reading && shown.page != Page::Several && focused_ != v.profile.opened) {
        focused_ = v.profile.opened;
        const auto standing = StandingNow(v, shown, PlainLinks(v, shown));
        const auto view = player_card::FullOf(SubjectNow(v, shown, standing), player_card::Size::Wide);
        nav.Prefer(FocusOnOpen(PrimaryOf(standing), view));
    }
}

std::vector<MenuEntry> PlayerPage::Rows(const ShellView& v, const std::string& screen, const FriendsPanel& friends, bool idle, bool dialog, std::string& title) {
    if (OnSets(screen)) {
        auto rows = sets_.Rows(v, screen, idle, title);
        if (sets_.IsList(screen)) title = loc::T("profile.tab.sets");
        return rows;
    }
    const Shown shown = ShownNow(v);
    if (screen == PickScreen) {
        title = loc::Tf("profile.pick_title", shown.name);
        return PickRows(shown);
    }
    if (screen == FightersScreen) { title = loc::T("profile.tab.fighters"); return FighterRows(shown); }
    if (screen == AwardsScreen) { title = loc::T("screen.awards"); return AwardRows(shown); }
    title = loc::T("profile.tab.card");
    // A question More asked reopens More with its one choice, until it is answered or the popout is closed.
    if (!ask_.empty()) {
        if (raise_.issued) {
            if (dialog) raise_.seen = true;
            else if (raise_.seen || ++raise_.waits > 3) { ask_.clear(); raise_ = Raise(); }
        } else if (++raise_.waits > 8) { ask_.clear(); raise_ = Raise(); }
    }
    const auto standing = StandingNow(v, shown, LinksOf(v, friends, shown));
    const auto view = player_card::FullOf(SubjectNow(v, shown, standing), player_card::CardSize());
    return CardRows(view, PrimaryOf(standing), standing, ask_, shown.name);
}

void PlayerPage::Do(const ShellView& v, const std::string& what, const Shown& shown, const Standing& standing, const Hands& h) {
    auto& friends = h.friends;
    if (what == "add") {
        if (!shown.token.empty()) friends.RequestMember(v, shown.token, shown.name, false, h.submit);
        else if (standing.byCode) friends.RequestByCode(v, shown.key.value, shown.name, h.submit);
    } else if (what == "accept") {
        if (standing.byRequest) friends.ActOnFriend(v, "accept", shown.key.value, h.submit);
        else if (!shown.token.empty()) friends.RequestMember(v, shown.token, shown.name, true, h.submit);
    } else if (what == "join" || what == "watch" || what == "invite" || what == "remove" || what == "remove-block") {
        friends.ActOnFriend(v, what, standing.friendId, h.submit);
    } else if (what == "unblock") {
        friends.ActOnFriend(v, "unblock", standing.blockId, h.submit);
    } else if (what == "block") {
        if (!shown.token.empty()) friends.BlockMember(v, shown.token, shown.name, h.submit);
    } else if (what == "sets") {
        h.navigation.Replace(SetsScreen);
    } else if (what == "steam") {
        // The runtime opens Steam's overlay on the profile when it is up, and the browser page otherwise.
        if (!shown.known.steam.Present()) return;
        commands::Service open;
        open.generation = v.session.generation;
        open.service = platform::ServiceAction::OpenSteamProfile;
        open.steamProfile = shown.known.steam;
        if (!h.submit(std::move(open))) h.error = loc::T("error.queue_failed");
        else h.error.clear();
    } else if (what == "retry") {
        asked_ = false;
    }
}

bool PlayerPage::Activate(const ShellView& v, const MenuAction& a, const Hands& h) {
    auto& nav = h.navigation;
    if (a.id == PrimaryRow) {
        const Shown shown = ShownNow(v);
        const auto standing = StandingNow(v, shown, LinksOf(v, h.friends, shown));
        const auto primary = PrimaryOf(standing);
        // Unblock asks first, as More's does.
        if (primary.enabled && primary.does == "unblock") Ask("unblock");
        else if (primary.enabled && !primary.does.empty()) Do(v, primary.does, shown, standing, h);
        return true;
    }
    // A strip tile opens their Fighters on that fighter; an award tile their wall on that family; All sets their Sets.
    if (HasPrefix(a.id, "card-fighter:")) {
        nav.Replace(FightersScreen);
        nav.Prefer(fighters_tab::RowPrefix + a.id.substr(13));
        return true;
    }
    if (HasPrefix(a.id, player_card::AwardRowPrefix)) {
        nav.Replace(AwardsScreen);
        nav.Prefer("award:" + a.id.substr(std::string(player_card::AwardRowPrefix).size()));
        return true;
    }
    if (a.id == player_card::AllSetsRow) { nav.Replace(SetsScreen); return true; }
    if (card_sets::IsRow(a.id)) return cardSets_.Activate(a.id, v, h.submit, h.queue, h.error);
    if (const auto hit = RowKey<std::size_t>(a.id, HitPrefix)) {
        const Shown shown = ShownNow(v);
        if (*hit < shown.hits.size()) {
            const auto& one = shown.hits[*hit];
            netplay::profiles::Known known;
            known.name = one.name;
            known.country = one.country;
            known.badge = one.badge;
            known.fighter = one.hasMain ? one.main : -1;
            Begin(netplay::profiles::Key::OfCard(one.card), known, nav);
        }
        return true;
    }
    // Information rows (their fighters, their families) have nothing to do.
    if (HasPrefix(a.id, "main-") || HasPrefix(a.id, "award:")) return true;
    return sets_.Activate(a, nav);
}

bool PlayerPage::Choose(const ShellView& v, const MenuAction& a, const Hands& h) {
    if (a.id == MoreRow) {
        const Shown shown = ShownNow(v);
        const auto standing = StandingNow(v, shown, LinksOf(v, h.friends, shown));
        if (!ask_.empty()) {
            const std::string what = ask_;
            ask_.clear();
            raise_ = Raise();
            if (a.text == "yes") Do(v, what, shown, standing, h);
        } else if (Asks(a.text)) Ask(a.text);
        else Do(v, a.text, shown, standing, h);
        return true;
    }
    // A set's row offers the cards of the players in it: they replace this profile.
    if (const auto player = sets_.PlayerOf(a)) {
        Open(player->key, player->known, h.navigation);
        return true;
    }
    return sets_.Choose(a, v, h.submit, h.navigation, h.queue, h.error);
}

bool PlayerPage::Adjust(const MenuAction& a) { return sets_.Adjust(a); }

bool PlayerPage::TakesShortcut(const std::string& screen, const MenuAction& a) const { return screen == CardScreen && a.delta == MenuInput::Options; }

void PlayerPage::Shortcut(const ShellView& v, const MenuAction& a, const std::string& screen, const std::vector<MenuEntry>& rows, MenuNavigation& nav) {
    if (screen == CardScreen) {
        if (a.delta == MenuInput::Options) nav.Ask(MoreRow, rows);
        return;
    }
    sets_.Shortcut(a, v, screen, nav);
}

void PlayerPage::Dress(const ShellView& v, const std::string& screen, const std::string& ownName, const std::vector<MenuEntry>& rows, GameMenu& menu) {
    const bool sets = sets_.IsList(screen);
    if (screen == ProfilePanel::SetsScreen) ProfilePanel::Dress(menu, screen, ownName);
    else if (OnProfile(screen) && screen != SetsFilterScreen && screen != SetsFighterScreen) {
        const Shown shown = ShownNow(v);
        NoteUserText(shown.name);
        menu.eyebrow = loc::Tf("profile.eyebrow", shown.name.empty() ? std::string(loc::T("profile.this_player")) : shown.name);
        if (screen != PickScreen) {
            menu.tabs = TabsOf(shown);
            for (std::size_t i = 0; i < menu.tabs.size(); ++i)
                if (menu.tabs[i].first == screen) menu.tab = static_cast<int>(i);
        }
        if (screen == AwardsScreen) {
            const auto awards = player_card::AwardsOf(shown.card ? netplay::awards::CountersOf(*shown.card) : netplay::awards::Counters(), {}, false);
            menu.headerChips.push_back({loc::Tf("awards.earned_count", awards.earned, awards.total), v2::Colors().accentLight});
        }
        // Y is More where the card has one, in a room too (TakesShortcut).
        if (screen == CardScreen && OffersMore(shown)) menu.shortcutHints = ShortcutHints({{MenuInput::Options, loc::T("friends.more")}});
    }
    if (sets) {
        menu.customDetail = ArchiveTab::IsRow(menu.navigation.Focus());
        menu.paneWidth = 380;
        for (const auto& chip : sets_.Chips()) menu.headerChips.push_back({chip, v2::Colors().muted});
        menu.shortcutHints = sets_.Hints(v, screen);
    }
    // The question More asked opens More again, on its one choice.
    if (!ask_.empty() && !raise_.issued && menu.navigation.Ask(MoreRow, rows)) { raise_.issued = true; raise_.seen = false; raise_.waits = 0; }
}

} }
