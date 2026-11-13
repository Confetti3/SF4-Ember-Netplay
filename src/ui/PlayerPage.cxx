#include "PlayerPage.hxx"
#include "AwardsWall.hxx"
#include "FightersTab.hxx"
#include "MenuRows.hxx"
#include "Theme.hxx"
#include "V2.hxx"
#include "../common/Localization.hxx"
#include <algorithm>

// What the page shows and offers, decided without a frame: the state of the open profile and the tabs it has, the Primary
// action by relation and presence, More's entries, the words of every state, and the subject the Full card is drawn from.
namespace sf4e { namespace ui { namespace player_page {
namespace {
using Key = profiles::Key;
namespace aw = netplay::awards;
const char* const kDot = " \xC2\xB7 ";

// The block the viewer holds on the player called `name`, or null.
const nf::Blocked* BlockOf(const nf::State& friends, const std::string& name) {
    if (name.empty()) return nullptr;
    for (const auto& blocked : friends.blocked)
        if (profiles::SameNameAnyCase(blocked.name, name)) return &blocked;
    return nullptr;
}

const netplay::levels::Card& NoCardAt() {
    static const netplay::levels::Card none;
    return none;
}

std::string Reason(const char* why) { return why ? std::string(loc::T(why)) : std::string(); }

// What a friend's choices read before the friends list shows them: each listed, none doable, and why.
std::vector<MenuChoice> Unlisted(const friends_panel::Gate& gate) {
    const std::string why = gate.go ? std::string(loc::T("friends.loading")) : Reason(gate.goWhy);
    return {{"join", loc::T("friends.join_room"), why, false}, {"watch", loc::T("friends.watch"), why, false}, {"invite", loc::T("friends.invite_room"), why, false},
        {"remove", loc::T("friends.remove"), why, false}, {"remove-block", loc::T("friends.remove_block"), why, false}};
}

const MenuChoice* Find(const std::vector<MenuChoice>& choices, const char* id) {
    const auto at = std::find_if(choices.begin(), choices.end(), [&](const MenuChoice& c) { return c.id == id; });
    return at == choices.end() ? nullptr : &*at;
}
}

bool OnProfile(const std::string& screen) {
    return screen == CardScreen || screen == FightersScreen || screen == SetsScreen || screen == AwardsScreen || screen == PickScreen ||
        screen == SetsFilterScreen || screen == SetsFighterScreen;
}

Shown ShownOf(const profiles::Profile& profile, const Key& key, const profiles::Known& known, bool service, profiles::Service offering, const nf::State& friends) {
    using State = profiles::Profile::State;
    Shown shown;
    // The view's profile is this page's only once it is the one the page asked for: until then the page is reading.
    const bool mine = profile.Open() && (!key.Present() || profile.key == key);
    if (!mine) {
        shown.key = key;
        shown.known = known;
        shown.name = known.name;
        if (key.kind == Key::Kind::Card) shown.token = key.value;
        shown.page = service ? Page::Reading : Page::Unreachable;
        return shown;
    }
    shown.key = profile.key;
    shown.known = profile.known;
    shown.token = profile.token;
    shown.hits = profile.hits;
    shown.name = profile.card.loaded && !profile.card.value.name.empty() ? profile.card.value.name : profile.known.name;
    switch (profile.state) {
    case State::Card:
        shown.profiled = offering != profiles::Service::Missing;
        if (profile.card.value.relation == Relation::Blocked) shown.page = Page::Blocked;
        else { shown.page = Page::Card; shown.card = &profile.card.value; }
        break;
    case State::NoCard: shown.page = BlockOf(friends, shown.name) ? Page::Blocked : Page::NoCard; break;
    case State::Nobody: shown.page = Page::Nobody; break;
    case State::Several: shown.page = Page::Several; break;
    case State::Unreachable: shown.page = Page::Unreachable; break;
    case State::Unsupported: shown.page = Page::Unsupported; break;
    case State::Reading: case State::Closed: shown.page = Page::Reading; break;
    }
    return shown;
}

bool OpensOwn(const Key& key, const profiles::Known& known, const std::string& ownName, const CountryCode& ownCountry) {
    if (ownName.empty() || key.kind != Key::Kind::Name || !profiles::SameNameAnyCase(key.value, ownName)) return false;
    const CountryCode& country = key.country.Present() ? key.country : known.country;
    return !country.Present() || country == ownCountry;
}

std::vector<std::pair<std::string, std::string>> TabsOf(const Shown& shown) {
    const std::pair<std::string, std::string> card{CardScreen, loc::T("profile.tab.card")}, fighters{FightersScreen, loc::T("profile.tab.fighters")},
        sets{SetsScreen, loc::T("profile.tab.sets")}, awards{AwardsScreen, loc::T("screen.awards")};
    switch (shown.page) {
    case Page::Reading: case Page::Unreachable: return {card, fighters, sets, awards};
    case Page::Card: return shown.profiled ? std::vector<std::pair<std::string, std::string>>{card, fighters, sets, awards} :
        std::vector<std::pair<std::string, std::string>>{card, fighters, sets};
    case Page::NoCard: case Page::Nobody: case Page::Unsupported: return {card, sets};
    case Page::Blocked: return {card};
    case Page::Several: break;
    }
    return {};
}

bool HasTab(const Shown& shown, const std::string& screen) {
    const auto tabs = TabsOf(shown);
    return std::any_of(tabs.begin(), tabs.end(), [&](const auto& tab) { return tab.first == screen; });
}

Standing StandingOf(const Shown& shown, const nf::State& friends, const Links& links) {
    Standing s;
    s.page = shown.page;
    s.relation = shown.card ? shown.card->relation : shown.page == Page::Blocked ? Relation::Blocked : Relation::None;
    s.named = !shown.name.empty();
    s.token = !shown.token.empty();
    s.byRequest = shown.key.kind == Key::Kind::Request;
    s.byCode = shown.key.kind == Key::Kind::Code;
    s.gate = links.gate;
    s.member = links.member;
    s.canInvite = links.canInvite;
    s.steam = links.steam;
    if (s.relation == Relation::Friend) {
        // The friendship's id as the card or the opener gave it, else the friend of that name.
        s.friendId = shown.card && !shown.card->friendId.empty() ? shown.card->friendId : shown.key.kind == Key::Kind::Friend ? shown.key.value : std::string();
        const nf::Friend* entry = friends.FindFriend(s.friendId);
        if (!entry && s.friendId.empty())
            for (const auto& one : friends.friends)
                if (profiles::SameNameAnyCase(one.name, shown.name)) { entry = &one; s.friendId = one.id; break; }
        if (entry) {
            s.joinable = friends_panel::Joinable(*entry);
            s.online = entry->presence.kind != nf::Presence::Kind::Offline;
            s.friendChoices = friends_panel::FriendChoices(friends, *entry, links.canInvite, links.gate);
        } else s.friendChoices = Unlisted(links.gate);
    }
    if (s.relation == Relation::Blocked)
        if (const auto* block = BlockOf(friends, shown.name)) s.blockId = block->id;
    return s;
}

Primary PrimaryOf(const Standing& s) {
    using Kind = Primary::Kind;
    const auto make = [](Kind kind, const char* label, const std::string& detail, bool enabled, const char* does) {
        Primary primary;
        primary.kind = kind;
        primary.label = loc::T(label);
        primary.detail = detail;
        primary.enabled = enabled;
        primary.does = does;
        return primary;
    };
    switch (s.page) {
    case Page::Reading: return make(Kind::Reading, "profile.reading_button", std::string(), false, "");
    case Page::Unreachable: return make(Kind::TryAgain, "profile.try_again", std::string(), true, "retry");
    case Page::Blocked:
        // Asks first (PlayerPage); without the block's id in the list there is nothing to lift from here.
        return make(Kind::Unblock, "friends.unblock", s.blockId.empty() ? std::string(loc::T("friends.blocked_member_detail")) : Reason(s.gate.actWhy), !s.blockId.empty() && s.gate.act, "unblock");
    case Page::NoCard: case Page::Nobody: case Page::Unsupported: case Page::Several:
        return make(Kind::ViewSets, "profile.view_sets", std::string(), s.named, "sets");
    case Page::Card: break;
    }
    switch (s.relation) {
    case Relation::Received: {
        const auto& gate = s.byRequest ? s.gate : s.member;
        return make(Kind::AcceptRequest, "friends.accept_member", Reason(gate.actWhy), gate.act && (s.token || s.byRequest), "accept");
    }
    case Relation::Sent: return make(Kind::RequestSent, "friends.request_sent", loc::T("profile.request_sent_detail"), false, "");
    case Relation::Friend:
        if (s.joinable) return make(Kind::JoinRoom, "friends.join_room", Reason(s.gate.goWhy), s.gate.go, "join");
        if (s.online && s.canInvite) return make(Kind::Invite, "friends.invite_room", Reason(s.gate.actWhy), s.gate.act, "invite");
        return make(Kind::ViewSets, "profile.view_sets", std::string(), s.named, "sets");
    case Relation::Blocked: case Relation::None: break;
    }
    return make(Kind::AddFriend, "friends.add_member", Reason(s.member.actWhy), s.member.act && (s.token || s.byCode), "add");
}

std::vector<MenuChoice> MoreOf(const Standing& s) {
    std::vector<MenuChoice> more;
    const bool isFriend = s.relation == Relation::Friend, blocked = s.relation == Relation::Blocked;
    if (!isFriend && !blocked) {
        const auto& gate = s.byRequest ? s.gate : s.member;
        const std::string why = Reason(gate.actWhy);
        if (s.relation == Relation::Received) more.push_back({"accept", loc::T("friends.accept_member"), why, gate.act && (s.token || s.byRequest)});
        else if (s.relation == Relation::Sent) more.push_back({"sent", loc::T("friends.request_sent"), loc::T("profile.request_sent_detail"), false});
        else more.push_back({"add", loc::T("friends.add_member"), why, gate.act && (s.token || s.byCode)});
    }
    if (isFriend)
        for (const char* id : {"join", "watch", "invite"})
            if (const auto* choice = Find(s.friendChoices, id)) more.push_back(*choice);
    more.push_back({"sets", loc::T("profile.view_sets"), std::string(), s.named});
    more.push_back({"steam", loc::T("room.steam_profile"), loc::T(s.steam ? "room.steam_profile.detail" : "room.steam_profile.none"), s.steam});
    if (isFriend)
        for (const char* id : {"remove", "remove-block"})
            if (const auto* choice = Find(s.friendChoices, id)) more.push_back(*choice);
    if (blocked) more.push_back({"unblock", loc::T("friends.unblock"), s.blockId.empty() ? std::string(loc::T("friends.blocked_member_detail")) : Reason(s.gate.actWhy), !s.blockId.empty() && s.gate.act});
    // A block names the player by their card token; a friend opened by their id has none, and Remove and block does it.
    else if (s.token) more.push_back({"block", loc::T("friends.block"), Reason(s.member.actWhy), s.member.act});
    return more;
}

bool Asks(const std::string& choice) { return choice == "remove" || choice == "remove-block" || choice == "block" || choice == "unblock"; }

std::vector<MenuChoice> QuestionOf(const std::string& choice, const std::string& name) {
    const char* label = choice == "remove" ? "friends.remove" : choice == "remove-block" ? "friends.remove_block" : choice == "block" ? "friends.block" : "friends.unblock";
    return {{"yes", loc::T(label), name, true}};
}

std::vector<std::string> NotesOf(Page page, const std::string& name) {
    const std::string who = name.empty() ? std::string(loc::T("profile.this_player")) : name;
    switch (page) {
    case Page::Reading: return {loc::Tf("profile.reading", who)};
    case Page::NoCard: return {loc::Tf("profile.no_card", who), loc::T("profile.sets_by_name")};
    case Page::Blocked: return {loc::Tf("profile.blocked", who)};
    case Page::Nobody: return {loc::Tf("profile.nobody", who)};
    case Page::Unreachable: return {loc::T("profile.unreachable")};
    case Page::Unsupported: return {loc::T("profile.unsupported")};
    case Page::Card: case Page::Several: break;
    }
    return {};
}

player_card::Subject SubjectOf(const Shown& shown, const Standing& standing, const Around& around, const player_card::SetsView& sets) {
    const auto& card = shown.card ? *shown.card : NoCardAt();
    const bool reading = shown.page == Page::Reading || shown.page == Page::Unreachable;
    // Reading and Unreachable draw the whole card at zero (the words say why); a state with no card draws the name and the words.
    const bool profiled = shown.page == Page::Card ? shown.profiled : reading;
    auto subject = player_card::CardSubject(card, shown.known, around.roomFighter, around.inRoom && around.roomFighter >= 0, profiled);
    if (subject.name.empty()) subject.name = shown.name;
    subject.levels = shown.page == Page::Card ? CardLevels::Counted : shown.page == Page::Reading ? CardLevels::Pending :
        shown.page == Page::Unreachable ? CardLevels::Unreachable : CardLevels::Unavailable;
    subject.versus = shown.page == Page::Card;
    subject.relation = standing.relation;
    subject.now = around.now;
    if (around.friendEntry) {
        const auto& entry = *around.friendEntry;
        subject.friendSince = entry.since;
        subject.presenceMark = entry.presence.kind == nf::Presence::Kind::InRoom ? player_card::PresenceMark::InRoom :
            entry.presence.kind == nf::Presence::Kind::Online ? player_card::PresenceMark::Online : player_card::PresenceMark::Offline;
        subject.presence = friends_panel::Status(entry, around.now);
    } else if (around.inRoom) {
        subject.presenceMark = player_card::PresenceMark::InRoom;
        subject.presence = loc::T("profile.in_this_room");
    }
    subject.sets = sets;
    if (shown.page == Page::Blocked) subject.sets.shown = false;
    subject.notes = NotesOf(shown.page, shown.name);
    // The Primary has no glyph, and More takes Y: the hero's two buttons, in the boxes of Edit card and Fighter select.
    subject.buttons = {{PrimaryRow, nullptr, v2::ButtonKind::Primary}, {MoreRow, "Y", v2::ButtonKind::Secondary}};
    return subject;
}

std::vector<MenuEntry> CardRows(const player_card::FullCardView& view, const Primary& primary, const Standing& standing, const std::string& ask, const std::string& name) {
    auto rows = player_card::CardRows(view);
    auto main = Row(PrimaryRow, primary.label, primary.detail, primary.enabled);
    main.hint = primary.label;
    rows.push_back(std::move(main));
    auto more = Row(MoreRow, loc::T("friends.more"), std::string());
    more.hint = loc::T("friends.more");
    more.choices = ask.empty() ? MoreOf(standing) : QuestionOf(ask, name);
    more.popout = true;
    rows.push_back(std::move(more));
    return rows;
}

std::string FocusOnOpen(const Primary& primary, const player_card::FullCardView& view) {
    if (primary.enabled) return PrimaryRow;
    if (!view.strip.empty()) return view.strip.front().row;
    return MoreRow;
}

bool OffersMore(const Shown& shown) { return shown.page != Page::Several; }

std::vector<MenuEntry> FighterRows(const Shown& shown) {
    const auto own = shown.card ? fighters_tab::LevelsOfCard(*shown.card) : netplay::levels::Own();
    const int main = shown.card && shown.card->hasMain ? shown.card->main : -1;
    const auto groups = player_card::Grouped(own, main);
    std::vector<MenuEntry> rows;
    for (const auto* group : {&groups.played, &groups.unplayed}) {
        for (const int fighter : *group) {
            const auto& level = own.fighters[fighter];
            // Their games per fighter are never served: the points and the tier are all there is to say.
            auto row = Row(fighters_tab::RowPrefix + std::to_string(fighter), selection::FindFighter(fighter)->name,
                loc::Tf("level.value", level.points) + (level.points ? kDot + v2::TierName(v2::FrameTier(level.tier)) : std::string()));
            row.info = true;
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

std::vector<MenuEntry> AwardRows(const Shown& shown) {
    const auto counters = shown.card ? aw::CountersOf(*shown.card) : aw::Counters();
    const bool counted = shown.page == Page::Card;
    std::vector<MenuEntry> rows;
    for (const auto& family : aw::Catalog) {
        // Their friends count is never served, so the family is left out.
        if (family.counter == aw::Counter::Friends) continue;
        const int level = aw::LevelOf(counters, family);
        auto row = Row(std::string("award:") + family.id, level > 0 ? player_card::LevelTitle(*aw::LevelAt(family, level)) : std::string(loc::T("awards.not_reached")),
            std::string(loc::T(family.name)) + "\n" + awards_wall::LevelLines(counters, family, counted));
        row.info = true;
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<MenuEntry> PickRows(const Shown& shown) {
    std::vector<MenuEntry> rows;
    for (std::size_t i = 0; i < shown.hits.size(); ++i) {
        const auto& hit = shown.hits[i];
        auto row = Row(HitPrefix + std::to_string(i), hit.name, hit.since ? loc::Tf("card.since", player_card::MonthWords(hit.since)) : std::string());
        row.userText = true;
        row.key = i;
        row.hint = loc::T("room.member_card");
        rows.push_back(std::move(row));
    }
    return rows;
}

} } }
