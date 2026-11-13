// Another player's profile (docs/design/PROFILE.md, "Another player's profile"), read without a frame: which state the
// open profile is in and which tabs it has, the Primary action by relation and presence, More's entries and whether each
// can be done now, the words of every state, the subject the Full card is drawn from, the focus the page opens with,
// and the rows of its Fighters tab, its wall and its pick list.
#include "../ui/PlayerPage.hxx"
#include "../ui/CountryFlag.hxx"
#include "../common/LocalTime.hxx"
#include "../common/Localization.hxx"
#include <cstdio>
#include <memory>
#include <string>

namespace {
using namespace sf4e;
namespace pp = ui::player_page;
namespace fp = ui::friends_panel;
namespace nf = netplay::friends;
namespace pr = netplay::profiles;
namespace player_card = ui::player_card;
namespace v2 = ui::v2;
using ui::MenuChoice;
using ui::MenuEntry;
using Relation = netplay::levels::Relation;
using Page = pp::Page;
using Kind = pp::Primary::Kind;
int failures = 0;
void Check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    std::printf("FAIL: %s\n", what);
}

constexpr std::uint64_t Now = 1800000000, Hour = 3600, Day = 24 * Hour;

// What the friends paths allow: everything, or nothing with the words that say why.
fp::Gate Open() { return fp::Gate{true, true, nullptr, nullptr}; }
fp::Gate Shut() { return fp::Gate{false, false, "friends.unreachable", "friends.unreachable"}; }

pp::Standing Stranger() {
    pp::Standing s;
    s.page = Page::Card;
    s.named = true;
    s.token = true;
    s.gate = s.member = Open();
    return s;
}
pp::Standing Friend(bool joinable, bool online, bool canInvite) {
    auto s = Stranger();
    s.relation = Relation::Friend;
    s.friendId = "fr_3";
    s.joinable = joinable;
    s.online = online;
    s.canInvite = canInvite;
    return s;
}

const MenuChoice* Pick(const std::vector<MenuChoice>& choices, const char* id) {
    for (const auto& choice : choices) if (choice.id == id) return &choice;
    return nullptr;
}
std::vector<std::string> Ids(const std::vector<MenuChoice>& choices) {
    std::vector<std::string> ids;
    for (const auto& choice : choices) ids.push_back(choice.id);
    return ids;
}

// One Primary by relation and presence (the table in the design).
void PrimaryByRelation() {
    // No relation, they have a card: Add friend, with the token.
    auto add = pp::PrimaryOf(Stranger());
    Check(add.kind == Kind::AddFriend && add.enabled && add.does == "add" && add.label == loc::T("friends.add_member"), "a stranger's Primary is not Add friend");
    // By a friend code there is no token, and the code asks.
    auto byCode = Stranger();
    byCode.token = false;
    byCode.byCode = true;
    Check(pp::PrimaryOf(byCode).enabled, "Add friend by a friend code is not possible");
    auto nothing = Stranger();
    nothing.token = false;
    Check(!pp::PrimaryOf(nothing).enabled, "Add friend with no way to name them is possible");
    // The friends paths refusing says why in the detail, and disables it.
    auto shut = Stranger();
    shut.member = Shut();
    const auto refused = pp::PrimaryOf(shut);
    Check(refused.kind == Kind::AddFriend && !refused.enabled && refused.detail == loc::T("friends.unreachable"), "Add friend does not say why it cannot be done");

    // They asked: Accept request, by the token, or by the request when opened by it.
    auto asked = Stranger();
    asked.relation = Relation::Received;
    Check(pp::PrimaryOf(asked).kind == Kind::AcceptRequest && pp::PrimaryOf(asked).enabled && pp::PrimaryOf(asked).does == "accept", "a request received is not Accept request");
    asked.token = false;
    Check(!pp::PrimaryOf(asked).enabled, "Accept request with neither token nor request is possible");
    asked.byRequest = true;
    Check(pp::PrimaryOf(asked).enabled, "Accept request by the request is not possible");

    // You asked: Request sent, disabled, and says where to cancel it.
    auto sent = Stranger();
    sent.relation = Relation::Sent;
    const auto waiting = pp::PrimaryOf(sent);
    Check(waiting.kind == Kind::RequestSent && !waiting.enabled && waiting.does.empty() && waiting.detail == loc::T("profile.request_sent_detail"), "Request sent is not disabled with where to cancel it");

    // A friend in a joinable public room: Join their room. Online with a room to offer: Invite. Otherwise View sets.
    const auto join = pp::PrimaryOf(Friend(true, true, true));
    Check(join.kind == Kind::JoinRoom && join.enabled && join.does == "join" && join.label == loc::T("friends.join_room"), "a friend in a joinable room does not get Join their room");
    const auto invite = pp::PrimaryOf(Friend(false, true, true));
    Check(invite.kind == Kind::Invite && invite.enabled && invite.does == "invite", "an online friend with a room to offer does not get Invite");
    Check(pp::PrimaryOf(Friend(false, true, false)).kind == Kind::ViewSets, "an online friend with nothing to offer does not get View sets");
    Check(pp::PrimaryOf(Friend(false, false, true)).kind == Kind::ViewSets, "an offline friend does not get View sets");
    auto stale = Friend(true, true, true);
    stale.gate = Shut();
    Check(pp::PrimaryOf(stale).kind == Kind::JoinRoom && !pp::PrimaryOf(stale).enabled, "Join their room is possible while the friends could not be read");

    // A player you blocked: Unblock, when the block is in the list.
    auto blocked = Stranger();
    blocked.page = Page::Blocked;
    blocked.relation = Relation::Blocked;
    Check(pp::PrimaryOf(blocked).kind == Kind::Unblock && !pp::PrimaryOf(blocked).enabled && pp::PrimaryOf(blocked).detail == loc::T("friends.blocked_member_detail"),
        "Unblock without the block in the list is possible or says nothing");
    blocked.blockId = "bl_1";
    Check(pp::PrimaryOf(blocked).enabled && pp::PrimaryOf(blocked).does == "unblock", "Unblock is not possible with the block in the list");

    // No card, nobody by the name, a service without profiles: View sets, while there is a name to list by.
    for (const Page page : {Page::NoCard, Page::Nobody, Page::Unsupported}) {
        auto s = Stranger();
        s.page = page;
        Check(pp::PrimaryOf(s).kind == Kind::ViewSets && pp::PrimaryOf(s).enabled && pp::PrimaryOf(s).does == "sets", "a state with no card does not offer View sets");
        s.named = false;
        Check(!pp::PrimaryOf(s).enabled, "View sets with no name to list by is possible");
    }
    // Reading: the reading words, disabled. A failed read: Try again.
    auto reading = Stranger();
    reading.page = Page::Reading;
    Check(pp::PrimaryOf(reading).kind == Kind::Reading && !pp::PrimaryOf(reading).enabled && pp::PrimaryOf(reading).label == loc::T("profile.reading_button"), "Reading is not a disabled Primary");
    reading.page = Page::Unreachable;
    Check(pp::PrimaryOf(reading).kind == Kind::TryAgain && pp::PrimaryOf(reading).enabled && pp::PrimaryOf(reading).does == "retry", "an unreachable service does not offer Try again");
}

// More lists every entry that applies to the relation, with its reason when it cannot be done.
void MoreEntries() {
    // A stranger: Add friend, View sets, Steam profile, Block.
    auto stranger = pp::MoreOf(Stranger());
    Check(Ids(stranger) == std::vector<std::string>({"add", "sets", "steam", "block"}), "a stranger's More is not Add friend, View sets, Steam profile, Block");
    Check(Pick(stranger, "add")->enabled && Pick(stranger, "sets")->enabled && Pick(stranger, "block")->enabled, "a stranger's entries are not all possible");
    Check(!Pick(stranger, "steam")->enabled && Pick(stranger, "steam")->detail == loc::T("room.steam_profile.none"), "Steam profile is possible, or does not say why not");
    auto sharing = Stranger();
    sharing.steam = true;
    Check(Pick(pp::MoreOf(sharing), "steam")->enabled, "a member who shares a Steam profile cannot have it opened");
    // They asked, or you asked.
    auto asked = Stranger();
    asked.relation = Relation::Received;
    Check(Ids(pp::MoreOf(asked)).front() == "accept", "a request received does not lead More with Accept request");
    asked.relation = Relation::Sent;
    const auto sent = pp::MoreOf(asked);
    Check(Ids(sent).front() == "sent" && !Pick(sent, "sent")->enabled && Pick(sent, "sent")->detail == loc::T("profile.request_sent_detail"), "Request sent is not listed disabled with where to cancel it");
    // The friends paths shut: every entry that goes through them says why.
    auto shut = Stranger();
    shut.member = Shut();
    const auto closed = pp::MoreOf(shut);
    Check(!Pick(closed, "add")->enabled && Pick(closed, "add")->detail == loc::T("friends.unreachable") && !Pick(closed, "block")->enabled && Pick(closed, "sets")->enabled,
        "More does not say why the friends paths cannot be used, or locks View sets");
    // A block needs their token: without one it is not offered.
    auto tokenless = Stranger();
    tokenless.token = false;
    tokenless.byCode = true;
    Check(Pick(pp::MoreOf(tokenless), "block") == nullptr, "Block is offered with no token to name them by");

    // A friend: the relationship entries give way to Join, Watch and Invite, and Remove friend and Remove and block join.
    auto friends = Friend(true, true, true);
    friends.friendChoices = {{"join", loc::T("friends.join_room"), "", true}, {"watch", loc::T("friends.watch"), "reason", false}, {"invite", loc::T("friends.invite_room"), "", true},
        {"remove", loc::T("friends.remove"), "", true}, {"remove-block", loc::T("friends.remove_block"), "", true}};
    const auto friendMore = pp::MoreOf(friends);
    Check(Ids(friendMore) == std::vector<std::string>({"join", "watch", "invite", "sets", "steam", "remove", "remove-block", "block"}), "a friend's More is not in the design's order");
    Check(!Pick(friendMore, "watch")->enabled && Pick(friendMore, "watch")->detail == "reason", "Watch does not carry the Friends tab's reason");
    // A friend opened by their id has no token: Remove and block does it, so there is no Block.
    friends.token = false;
    Check(Pick(pp::MoreOf(friends), "block") == nullptr, "a friend with no token can be blocked by Block");
    // A friend the list does not show yet: every friend entry is listed, none can be done.
    const auto listed = pp::StandingOf([] {
        pp::Shown shown;
        shown.page = Page::Card;
        shown.name = "Player 3";
        static netplay::levels::Card card;
        card = netplay::levels::Card();
        card.relation = Relation::Friend;
        card.friendId = "fr_3";
        shown.card = &card;
        return shown;
    }(), nf::State(), pp::Links{Open(), Open(), true, false});
    const auto unread = pp::MoreOf(listed);
    Check(Pick(unread, "join") && !Pick(unread, "join")->enabled && Pick(unread, "remove") && !Pick(unread, "remove")->enabled && !Pick(unread, "remove")->detail.empty(),
        "a friend the list does not show yet is offered Join or Remove friend");

    // A player you blocked: Unblock instead of Block, and no relationship entry.
    auto blocked = Stranger();
    blocked.page = Page::Blocked;
    blocked.relation = Relation::Blocked;
    blocked.blockId = "bl_1";
    const auto lift = pp::MoreOf(blocked);
    Check(Ids(lift) == std::vector<std::string>({"sets", "steam", "unblock"}) && Pick(lift, "unblock")->enabled, "a blocked player's More is not View sets, Steam profile, Unblock");
    blocked.blockId.clear();
    Check(!Pick(pp::MoreOf(blocked), "unblock")->enabled && Pick(pp::MoreOf(blocked), "unblock")->detail == loc::T("friends.blocked_member_detail"), "Unblock without the block in the list is possible");

    // What asks first: Remove friend, Remove and block, Block and Unblock; the question is one yes.
    for (const char* id : {"remove", "remove-block", "block", "unblock"}) Check(pp::Asks(id), "a choice that should ask first does not");
    for (const char* id : {"add", "accept", "join", "watch", "invite", "sets", "steam"}) Check(!pp::Asks(id), "a choice that should not ask does");
    const auto question = pp::QuestionOf("remove-block", "Player 3");
    Check(question.size() == 1 && question[0].id == "yes" && question[0].label == loc::T("friends.remove_block") && question[0].detail == "Player 3" && question[0].enabled,
        "the question is not one yes naming the player");
}

// The Standing the page reads from the friends: a friend's presence, the choices the Friends tab offers, a block.
void StandingFromFriends() {
    nf::State state;
    state.kind = nf::State::Kind::Listed;
    nf::Friend room;
    room.id = "fr_3"; room.name = "Player 3";
    room.presence.kind = nf::Presence::Kind::InRoom; room.presence.status = nf::Status::Idle; room.presence.hasRoom = true;
    room.presence.room.id = std::string(32, 'a'); room.presence.room.name = "Player 5's room"; room.presence.room.joinable = true;
    nf::Friend menus;
    menus.id = "fr_6"; menus.name = "Player 6";
    menus.presence.kind = nf::Presence::Kind::Online;
    state.friends = {room, menus};
    state.blocked = {{"bl_1", "Player 16", Now - Day}};

    netplay::levels::Card card;
    card.relation = Relation::Friend;
    card.friendId = "fr_3";
    pp::Shown shown;
    shown.page = Page::Card;
    shown.name = "Player 3";
    shown.card = &card;
    const pp::Links links{Open(), Open(), true, false};
    auto standing = pp::StandingOf(shown, state, links);
    Check(standing.friendId == "fr_3" && standing.joinable && standing.online && standing.canInvite, "a friend in a joinable room is not read as one");
    Check(pp::PrimaryOf(standing).kind == Kind::JoinRoom, "a friend in a joinable room does not get Join their room");
    Check(Pick(standing.friendChoices, "join") && Pick(standing.friendChoices, "join")->enabled, "the Friends tab's Join is not offered");
    // A friend with no id from the bridge is found by name.
    card.friendId.clear();
    shown.name = "Player 6";
    standing = pp::StandingOf(shown, state, links);
    Check(standing.friendId == "fr_6" && !standing.joinable && standing.online && pp::PrimaryOf(standing).kind == Kind::Invite, "a friend with no id is not found by name");
    // Opened by friend id, with no card yet naming them.
    shown.key = pr::Key::OfFriend("fr_3");
    card.friendId.clear();
    shown.name = "Nobody";
    standing = pp::StandingOf(shown, state, links);
    Check(standing.friendId == "fr_3", "a profile opened by friend id does not act on that friend");

    // A player you blocked, from the card or from the block list when there is no card.
    pp::Shown blocked;
    blocked.page = Page::Blocked;
    blocked.name = "player 16";
    standing = pp::StandingOf(blocked, state, links);
    Check(standing.relation == Relation::Blocked && standing.blockId == "bl_1", "a blocked player's block is not found by name in any case");
}

// The state of the open profile, and what is read of the view's before it answers the page's own open.
void ShownStates() {
    using State = pr::Profile::State;
    nf::State friends;
    pr::Profile profile;
    pr::Known known;
    known.name = "Player 2";
    const auto key = pr::Key::OfCard("c1.lab");
    // Nothing open yet, or another player's: the page waits for its own open, reading what the opener knew.
    auto shown = pp::ShownOf(profile, key, known, true, pr::Service::Unknown, friends);
    Check(shown.page == Page::Reading && shown.name == "Player 2" && shown.token == "c1.lab", "a page waiting on its open is not reading what the opener knew");
    shown = pp::ShownOf(profile, key, known, false, pr::Service::Unknown, friends);
    Check(shown.page == Page::Unreachable, "a page with no service to ask is not unreachable");
    profile.state = State::Card;
    profile.key = pr::Key::OfCard("c1.other");
    shown = pp::ShownOf(profile, key, known, true, pr::Service::Unknown, friends);
    Check(shown.page == Page::Reading, "another player's card is read as this page's");

    profile.key = key;
    profile.known = known;
    profile.token = key.value;
    profile.card.loaded = true;
    profile.card.value.name = "Player 2 (card)";
    shown = pp::ShownOf(profile, key, known, true, pr::Service::Offered, friends);
    Check(shown.page == Page::Card && shown.card == &profile.card.value && shown.profiled && shown.name == "Player 2 (card)", "a card on show is not the card, named as it names itself");
    shown = pp::ShownOf(profile, key, known, true, pr::Service::Missing, friends);
    Check(shown.page == Page::Card && !shown.profiled, "a card from a service without profiles is read as having them");
    profile.card.value.relation = Relation::Blocked;
    shown = pp::ShownOf(profile, key, known, true, pr::Service::Offered, friends);
    Check(shown.page == Page::Blocked && !shown.card, "a card of a player you blocked is drawn");
    profile.card.value.relation = Relation::None;

    const std::pair<State, Page> states[] = {{State::Reading, Page::Reading}, {State::NoCard, Page::NoCard}, {State::Nobody, Page::Nobody}, {State::Several, Page::Several},
        {State::Unreachable, Page::Unreachable}, {State::Unsupported, Page::Unsupported}};
    for (const auto& pair : states) {
        profile.state = pair.first;
        Check(pp::ShownOf(profile, key, known, true, pr::Service::Offered, friends).page == pair.second, "a profile state is not the page that shows it");
    }
    // No card for a player the viewer blocked is told from any other no card by the block list.
    profile.card.loaded = false;
    profile.card.value = netplay::levels::Card();
    friends.blocked = {{"bl_1", "player 2", Now}};
    profile.state = State::NoCard;
    Check(pp::ShownOf(profile, key, known, true, pr::Service::Offered, friends).page == Page::Blocked, "a missing card of a blocked player is not the blocked page");
    // No wanted key: the view's profile as it stands (the lab, a page opened elsewhere).
    Check(pp::ShownOf(profile, pr::Key(), known, true, pr::Service::Offered, friends).page == Page::Blocked, "a page with no wanted key does not read the view's profile");
    profile.state = State::Closed;
    Check(pp::ShownOf(profile, pr::Key(), known, true, pr::Service::Offered, friends).page == Page::Reading, "a closed profile is not read as reading");
}

// The tabs each state has.
void Tabs() {
    const auto tabs = [](Page page, bool profiled = true) {
        pp::Shown shown;
        shown.page = page;
        shown.profiled = profiled;
        std::vector<std::string> ids;
        for (const auto& tab : pp::TabsOf(shown)) ids.push_back(tab.first);
        return ids;
    };
    using V = std::vector<std::string>;
    const V all{"player-card", "player-fighters", "player-sets", "player-awards"};
    Check(tabs(Page::Reading) == all && tabs(Page::Card) == all && tabs(Page::Unreachable) == all, "reading, a card and a failed read do not have all four tabs");
    Check(tabs(Page::Card, false) == V({"player-card", "player-fighters", "player-sets"}), "a card from a service without profiles has the Awards tab");
    Check(tabs(Page::NoCard) == V({"player-card", "player-sets"}) && tabs(Page::Nobody) == V({"player-card", "player-sets"}) && tabs(Page::Unsupported) == V({"player-card", "player-sets"}),
        "no card, nobody by the name and a service without profiles do not have Card and Sets");
    Check(tabs(Page::Blocked) == V({"player-card"}), "a blocked player has more than the Card tab");
    Check(tabs(Page::Several).empty(), "the pick list has tabs");
    pp::Shown nocard;
    nocard.page = Page::NoCard;
    Check(pp::HasTab(nocard, "player-sets") && !pp::HasTab(nocard, "player-awards"), "HasTab does not follow the tabs");
    Check(pp::OnProfile("player-card") && pp::OnProfile("player-pick") && pp::OnProfile("player-sets-filter") && !pp::OnProfile("player") && !pp::OnProfile("sets"),
        "the page's screens are not told from Settings' player page and the own Sets tab");
}

// Every state has its own words.
void Words() {
    const auto notes = [](Page page) { return pp::NotesOf(page, "Player 2"); };
    Check(notes(Page::Reading) == std::vector<std::string>({loc::Tf("profile.reading", "Player 2")}), "reading does not say it is reading the card");
    Check(notes(Page::NoCard) == std::vector<std::string>({loc::Tf("profile.no_card", "Player 2"), loc::T("profile.sets_by_name")}), "no card does not say their sets are still listed by name");
    Check(notes(Page::Blocked) == std::vector<std::string>({loc::Tf("profile.blocked", "Player 2")}), "a blocked player's words are not the block's");
    Check(notes(Page::Nobody) == std::vector<std::string>({loc::Tf("profile.nobody", "Player 2")}), "nobody by the name does not say so");
    Check(notes(Page::Unreachable) == std::vector<std::string>({loc::T("profile.unreachable")}), "an unreachable service does not say so");
    Check(notes(Page::Unsupported) == std::vector<std::string>({loc::T("profile.unsupported")}), "a service without profiles does not say so");
    Check(notes(Page::Card).empty() && notes(Page::Several).empty(), "a card or the pick list has words in the card's place");
    // A name the opener did not know is not an empty gap in the sentence.
    Check(pp::NotesOf(Page::Reading, "") == std::vector<std::string>({loc::Tf("profile.reading", loc::T("profile.this_player"))}), "an unknown name leaves a gap in the words");
}

// Their own name opens the player's own Profile.
void OwnName() {
    pr::Known known;
    const CountryCode de = CountryCode::Parsed("DE"), fr = CountryCode::Parsed("FR");
    Check(pp::OpensOwn(pr::Key::OfName("player 1"), known, "Player 1", de), "the player's own name, in any case, does not open their own Profile");
    Check(!pp::OpensOwn(pr::Key::OfName("Player 2"), known, "Player 1", de), "another name opens the player's own Profile");
    Check(!pp::OpensOwn(pr::Key::OfCard("c1.lab"), known, "Player 1", de), "a card token opens the player's own Profile by name");
    Check(!pp::OpensOwn(pr::Key::OfName("Player 1", fr), known, "Player 1", de), "another player's name from another country opens the player's own Profile");
    Check(pp::OpensOwn(pr::Key::OfName("Player 1", de), known, "Player 1", de), "the player's own name and country do not open their own Profile");
    Check(!pp::OpensOwn(pr::Key::OfName("Player 1"), known, "", de), "any name opens the player's own Profile when they have none");
}

netplay::levels::Card Card() {
    netplay::levels::Card card;
    card.name = "Player 2";
    card.country = CountryCode::Parsed("GB");
    card.hasMain = true; card.main = 12;
    card.hasSince = true; card.since = Now - 120 * Day;
    card.awards.games = 2840; card.awards.wins = 1164; card.awards.bestStreak = 12;
    card.account = {4012, 2};
    card.fighters = {{12, 1986, 3}, {5, 860, 2}};
    card.hasVersus = true; card.versus = {3, 5, Now - 2 * Hour};
    return card;
}

// The subject the Full card is drawn from: presence, the hero, the relation cell and the words of a state without a card.
void Subject() {
    auto card = Card();
    card.relation = Relation::Friend;
    pp::Shown shown;
    shown.page = Page::Card;
    shown.card = &card;
    shown.name = "Player 2";
    pp::Standing standing;
    standing.relation = Relation::Friend;
    pp::Around around;
    around.now = Now;
    nf::Friend entry;
    entry.id = "fr_2"; entry.name = "Player 2"; entry.since = Now - 20 * Day;
    entry.presence.kind = nf::Presence::Kind::Online;
    around.friendEntry = &entry;
    player_card::SetsView sets;
    auto subject = pp::SubjectOf(shown, standing, around, sets);
    Check(subject.levels == ui::CardLevels::Counted && subject.profiled && subject.versus && subject.relation == Relation::Friend, "a card is not a whole card with the relation");
    Check(subject.presenceMark == player_card::PresenceMark::Online && subject.friendSince == entry.since && !subject.presence.empty(), "a friend's presence is not the Friends tab's");
    Check(subject.buttons.size() == 2 && subject.buttons[0].row == pp::PrimaryRow && subject.buttons[0].glyph == nullptr && subject.buttons[0].kind == v2::ButtonKind::Primary &&
        subject.buttons[1].row == pp::MoreRow && std::string(subject.buttons[1].glyph) == "Y" && subject.buttons[1].kind == v2::ButtonKind::Secondary,
        "the hero's buttons are not the Primary without a glyph and More on Y");
    Check(subject.notes.empty(), "a card has words in its own place");
    // The words the card makes of it: the relation cell says since when, and the hero is their main.
    auto view = player_card::FullOf(subject, player_card::Size::Wide);
    Check(view.rival.kind == player_card::Rival::Kind::Versus && view.rival.cells.size() == 3 && view.rival.cells[2].value == loc::Tf("card.since_date", LocalTime(entry.since, "%Y-%m-%d")),
        "the relation cell does not say since when they are friends");
    Check(view.hero == 12 && view.heroWord == loc::T("profile.main_badge"), "their hero is not their main");
    Check(view.record.size() == 4 && view.record[1].key == loc::T("card.wins"), "their record is not four cells with Wins");
    Check(view.points.shown && !view.points.hero.meter, "their points have meters");

    // A member of the room: in the room's words, the hero the fighter they picked, PLAYING.
    around.friendEntry = nullptr;
    around.inRoom = true;
    around.roomFighter = 5;
    standing.relation = Relation::None;
    subject = pp::SubjectOf(shown, standing, around, sets);
    view = player_card::FullOf(subject, player_card::Size::Wide);
    Check(subject.presenceMark == player_card::PresenceMark::InRoom && subject.presence == loc::T("profile.in_this_room"), "a member of this room is not In this room");
    Check(view.hero == 5 && view.heroWord == loc::T("card.playing"), "a member's hero is not the fighter they picked, PLAYING");
    around.inRoom = false;
    subject = pp::SubjectOf(shown, standing, around, sets);
    Check(subject.presenceMark == player_card::PresenceMark::None, "a stranger has a presence line");

    // Not friends, and the cell says so.
    view = player_card::FullOf(subject, player_card::Size::Wide);
    Check(view.rival.cells[2].value == loc::T("card.not_friends"), "a stranger's relation cell is not Not friends");
    standing.relation = Relation::Received;
    card.relation = Relation::Received;
    view = player_card::FullOf(pp::SubjectOf(shown, standing, around, sets), player_card::Size::Wide);
    Check(view.rival.cells[2].value == loc::T("card.they_asked"), "a request received is not They asked you");

    // No card: the name as the opener knew it and the words, none of the points, record or awards.
    pp::Shown none;
    none.page = Page::NoCard;
    none.name = "Player 2";
    none.known.name = "Player 2";
    none.known.fighter = 7;
    subject = pp::SubjectOf(none, pp::Standing(), around, sets);
    view = player_card::FullOf(subject, player_card::Size::Wide);
    Check(view.name == "Player 2" && !view.points.shown && view.record.empty() && view.awards.total == 0 && view.rival.kind == player_card::Rival::Kind::None, "no card draws points, record or awards");
    Check(view.notes == pp::NotesOf(Page::NoCard, "Player 2") && view.hero == 7, "no card has not the words and the fighter the opener knew");
    // Reading: the whole card at zero, and the reading words.
    pp::Shown reading;
    reading.page = Page::Reading;
    reading.name = "Player 2";
    view = player_card::FullOf(pp::SubjectOf(reading, pp::Standing(), around, sets), player_card::Size::Wide);
    Check(view.levels == ui::CardLevels::Pending && view.record.size() == 4 && view.awards.total > 0 && view.notes == pp::NotesOf(Page::Reading, "Player 2"), "reading is not the whole card at zero with its words");
    // A service without profiles: points and fighters, no chips, record or awards.
    shown.profiled = false;
    view = player_card::FullOf(pp::SubjectOf(shown, standing, around, sets), player_card::Size::Wide);
    Check(view.points.shown && view.record.empty() && view.awards.total == 0 && view.plays.empty() && view.about == ui::CountryName(CountryCode::Parsed("GB")), "a card without profiles has more than points and fighters");
    // A blocked player has no recent sets.
    pp::Shown blocked;
    blocked.page = Page::Blocked;
    blocked.name = "Player 2";
    sets.state = player_card::SetsView::State::Rows;
    Check(!pp::SubjectOf(blocked, pp::Standing(), around, sets).sets.shown, "a blocked player has recent sets");
}

// The rows of the Card tab, the focus the page opens with, and More's question.
void CardTabRows() {
    auto card = Card();
    pp::Shown shown;
    shown.page = Page::Card;
    shown.card = &card;
    shown.name = "Player 2";
    auto standing = Stranger();
    const auto primary = pp::PrimaryOf(standing);
    const auto view = player_card::FullOf(pp::SubjectOf(shown, standing, pp::Around(), player_card::SetsView()), player_card::Size::Wide);
    auto rows = pp::CardRows(view, primary, standing, "", "Player 2");
    const auto row = [&](const std::string& id) -> const MenuEntry* {
        for (const auto& r : rows) if (r.id == id) return &r;
        return nullptr;
    };
    Check(row(pp::PrimaryRow) && row(pp::PrimaryRow)->label == primary.label && row(pp::PrimaryRow)->enabled, "the Primary is not a row with its label");
    Check(row(pp::MoreRow) && row(pp::MoreRow)->popout && !row(pp::MoreRow)->choices.empty() && row(pp::MoreRow)->choices.front().id == "add", "More is not a popout of the entries");
    Check(row(ui::CardFighterRow(12)) && row(ui::CardFighterRow(5)), "their strip's tiles are not rows");
    // A question More asked is its one choice.
    rows = pp::CardRows(view, primary, standing, "block", "Player 2");
    Check(row(pp::MoreRow)->choices.size() == 1 && row(pp::MoreRow)->choices[0].id == "yes", "More does not carry its question");
    // The focus: the Primary when it can be pressed, else their first fighter, else More.
    Check(pp::FocusOnOpen(primary, view) == pp::PrimaryRow, "the page does not open on the Primary");
    auto sent = Stranger();
    sent.relation = Relation::Sent;
    Check(pp::FocusOnOpen(pp::PrimaryOf(sent), view) == ui::CardFighterRow(12), "a Primary that cannot be pressed does not give the focus to the first fighter");
    Check(pp::FocusOnOpen(pp::PrimaryOf(sent), player_card::FullCardView()) == pp::MoreRow, "with nothing else to focus the page does not open on More");
    Check(pp::OffersMore(shown) && !pp::OffersMore([] { pp::Shown s; s.page = Page::Several; return s; }()), "More is offered on the pick list");
}

// Their Fighters, their wall and the pick list.
void TabRows() {
    auto card = Card();
    pp::Shown shown;
    shown.page = Page::Card;
    shown.card = &card;
    const auto fighters = pp::FighterRows(shown);
    Check(fighters.size() == static_cast<std::size_t>(selection::FighterCount) && fighters.front().id == "main-12" && fighters[1].id == "main-5", "their fighters are not every fighter, the main first, then by points");
    Check(fighters.front().info && fighters[2].info, "a fighter's tile does something when pressed");
    // The wall: every family but Friends, at the level their marks reach.
    const auto wall = pp::AwardRows(shown);
    bool friendsListed = false;
    for (const auto& r : wall) friendsListed = friendsListed || r.id == "award:friends";
    Check(wall.size() == netplay::awards::CatalogSize - 1 && !friendsListed, "their wall has the Friends family");
    for (const auto& r : wall) Check(r.info, "a family on their wall does something when pressed");
    pp::Shown none;
    none.page = Page::Reading;
    const auto empty = pp::AwardRows(none);
    Check(!empty.empty() && empty.front().label == loc::T("awards.not_reached"), "a family not reached is not Not reached yet");
    // The pick list: one row each, in the order the search gave.
    shown.hits.resize(2);
    shown.hits[0].name = "Player 2"; shown.hits[0].since = Now - 30 * Day;
    shown.hits[1].name = "Player 2";
    const auto pick = pp::PickRows(shown);
    Check(pick.size() == 2 && pick[0].id == "player-hit:0" && pick[1].id == "player-hit:1" && pick[0].userText && !pick[0].detail.empty() && pick[1].detail.empty(), "the pick list is not a row for each player");
}

}

int main() {
    PrimaryByRelation();
    MoreEntries();
    StandingFromFriends();
    ShownStates();
    Tabs();
    Words();
    OwnName();
    Subject();
    CardTabRows();
    TabRows();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("player page tests passed\n");
    return 0;
}
