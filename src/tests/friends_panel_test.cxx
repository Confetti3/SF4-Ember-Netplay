// The Friends tab's choices (docs/design/PROFILE.md, "Friends tab"), read without a frame: what the tab says of the
// state, how friends are grouped and ordered, what A does on a row from where the friend is and whether the room can be
// joined, what More offers with each choice's reason when it cannot be done, the lines under a name, and the code
// typed on Add a friend.
#include "../ui/FindPlayer.hxx"
#include "../ui/FriendsPanel.hxx"
#include "../ui/PlayerOpening.hxx"
#include "../ui/PostMatchFriend.hxx"
#include "../ui/ShellView.hxx"
#include "../netplay/Rankings.hxx"
#include "../common/Localization.hxx"
#include <cstdio>
#include <memory>
#include <string>

namespace {
using namespace sf4e;
namespace fp = ui::friends_panel;
namespace nf = netplay::friends;
namespace pr = netplay::profiles;
int failures = 0;
void Check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    std::printf("FAIL: %s\n", what);
}

constexpr std::uint64_t Now = 1800000000, Hour = 3600, Day = 24 * Hour;

nf::Friend Pal(const char* name) {
    nf::Friend entry;
    entry.id = std::string("fr_") + name; entry.name = name;
    return entry;
}
nf::Friend InRoom(const char* name, bool joinable, nf::Status status = nf::Status::Idle, int table = -1) {
    auto entry = Pal(name);
    auto& p = entry.presence;
    p.kind = nf::Presence::Kind::InRoom; p.status = status; p.table = table; p.hasRoom = true;
    p.room.id = std::string(32, 'a'); p.room.name = "Player 5's room"; p.room.joinable = joinable;
    return entry;
}
nf::Friend Online(const char* name) { auto entry = Pal(name); entry.presence.kind = nf::Presence::Kind::Online; return entry; }
nf::Friend Offline(const char* name, std::uint64_t seen) { auto entry = Pal(name); entry.presence.seenAt = seen; return entry; }

nf::View ListedView(nf::State state) {
    nf::View view;
    view.bridge = "brg_1";
    state.kind = nf::State::Kind::Listed;
    view.state.value = std::make_shared<const nf::State>(std::move(state));
    view.state.loaded = true;
    return view;
}

std::vector<std::string> Names(const std::vector<const nf::Friend*>& list) {
    std::vector<std::string> names;
    for (const auto* entry : list) names.push_back(entry->name);
    return names;
}
const ui::MenuChoice* Pick(const std::vector<ui::MenuChoice>& choices, const char* id) {
    for (const auto& choice : choices) if (choice.id == id) return &choice;
    return nullptr;
}

// The four groups: requests newest first; in a room the joinable first, then by name; online by name (any case); offline
// the most recently seen first.
void Grouping() {
    nf::State state;
    nf::Incoming old, fresh;
    old.id = "rq_1"; old.sentAt = 100; fresh.id = "rq_2"; fresh.sentAt = 200;
    state.incoming = {old, fresh};
    state.friends = {InRoom("Zed", false), InRoom("Cleo", true), InRoom("Abel", false), InRoom("Bo", true),
        Online("mia"), Online("Nina"), Online("Ann"), Offline("Old", Now - 2 * Day), Offline("Recent", Now - Hour), Offline("Alpha", Now - Hour)};
    const auto groups = fp::Arrange(state);
    Check(groups.requests.size() == 2 && groups.requests[0]->id == "rq_2", "requests are not newest first");
    Check(Names(groups.inRoom) == std::vector<std::string>({"Bo", "Cleo", "Abel", "Zed"}), "in a room is not the joinable first, then by name");
    Check(Names(groups.online) == std::vector<std::string>({"Ann", "mia", "Nina"}), "online is not by name");
    Check(Names(groups.offline) == std::vector<std::string>({"Alpha", "Recent", "Old"}), "offline is not the most recently seen first, then by name");
    Check(fp::Arrange(nf::State{}).requests.empty() && fp::Arrange(nf::State{}).offline.empty(), "an empty state has groups");
}

// A on a row: Join where the room can be joined, else Invite when this player has a room to offer, else Player card; an
// offline friend has only the card, and a private room or one that is no longer listed cannot be joined.
void Presses() {
    using fp::Press;
    Check(fp::PressOf(InRoom("A", true), false) == Press::Join && fp::PressOf(InRoom("A", true), true) == Press::Join, "a joinable room is not Join");
    Check(fp::PressOf(InRoom("A", false), true) == Press::Invite && fp::PressOf(InRoom("A", false), false) == Press::Card, "a room that cannot be joined is not Invite, else the card");
    auto privateRoom = InRoom("A", true);
    privateRoom.presence.privateRoom = true;
    Check(!fp::Joinable(privateRoom) && fp::PressOf(privateRoom, false) == Press::Card, "a private room can be joined");
    auto unlisted = InRoom("A", true);
    unlisted.presence.hasRoom = false;
    Check(!fp::Joinable(unlisted), "a room no longer listed can be joined");
    Check(fp::PressOf(Online("A"), true) == Press::Invite && fp::PressOf(Online("A"), false) == Press::Card, "an online friend is not Invite, else the card");
    Check(fp::PressOf(Offline("A", 1), true) == Press::Card, "an offline friend does not open their card");
}

// More's choices on a friend: each is listed, with the reason in its detail when it cannot be done now.
void MoreOnAFriend() {
    nf::State state;
    state.friends = {InRoom("Fighter", true, nf::Status::Fighting, 0), InRoom("Waiting", true), InRoom("Closed", false, nf::Status::Fighting, 0), Online("Menus"), Offline("Away", 1)};
    state.kind = nf::State::Kind::Listed;
    const fp::Gate open{true, true, nullptr, nullptr};
    const auto fighter = fp::FriendChoices(state, state.friends[0], true, open);
    Check(fighter.size() == 6 && fighter[0].id == "card" && fighter[0].enabled && fighter[1].id == "join" && fighter[2].id == "watch" && fighter[3].id == "invite" && fighter[4].id == "remove" && fighter[5].id == "remove-block", "More on a friend is not Player card, Join, Watch, Invite, Remove, Remove and block");
    Check(Pick(fighter, "join")->enabled && Pick(fighter, "watch")->enabled && Pick(fighter, "invite")->enabled && Pick(fighter, "remove")->enabled, "a friend fighting in a room cannot be joined, watched or invited");
    // Waiting in a room: not a game to watch, with the reason.
    const auto waiting = fp::FriendChoices(state, state.friends[1], true, open);
    Check(Pick(waiting, "join")->enabled && !Pick(waiting, "watch")->enabled && Pick(waiting, "watch")->detail == loc::T("friends.cannot_watch"), "a friend not fighting can be watched, or says no reason");
    // A room that cannot be joined (full, locked, another build).
    const auto closed = fp::FriendChoices(state, state.friends[2], true, open);
    Check(!Pick(closed, "join")->enabled && Pick(closed, "join")->detail == loc::T("friends.cannot_join") && !Pick(closed, "watch")->enabled, "a room that cannot be joined can be joined or watched");
    // Nothing to offer, and an offline friend.
    const auto menus = fp::FriendChoices(state, state.friends[3], false, open);
    Check(!Pick(menus, "join")->enabled && !Pick(menus, "invite")->enabled && Pick(menus, "invite")->detail == loc::T("friends.nothing_to_offer"), "an invite without a room to offer is possible, or says no reason");
    const auto away = fp::FriendChoices(state, state.friends[4], true, open);
    Check(!Pick(away, "invite")->enabled && Pick(away, "invite")->detail == loc::T("friends.invite_unavailable"), "an offline friend can be invited");
    // Inviting while appearing offline says what it shows.
    state.appearOffline = true;
    const auto hiding = fp::FriendChoices(state, state.friends[0], true, open);
    Check(Pick(hiding, "invite")->enabled && Pick(hiding, "invite")->detail == loc::T("friends.invite_reveals"), "an invite while appearing offline does not say it shows where the player is");
    // An action in flight holds the bridge's actions but not Join or Watch; a stale list holds all.
    const fp::Gate busy{false, true, "friends.busy", nullptr};
    const auto held = fp::FriendChoices(state, state.friends[0], true, busy);
    Check(Pick(held, "join")->enabled && Pick(held, "watch")->enabled && !Pick(held, "invite")->enabled && !Pick(held, "remove")->enabled && !Pick(held, "remove-block")->enabled &&
        Pick(held, "remove")->detail == loc::T("friends.busy"), "an action in flight does not hold the bridge's actions only");
    const fp::Gate stale{false, false, "friends.unreachable", "friends.unreachable"};
    const auto old = fp::FriendChoices(state, state.friends[0], true, stale);
    Check(!Pick(old, "join")->enabled && !Pick(old, "watch")->enabled && Pick(old, "join")->detail == loc::T("friends.unreachable"), "a list that is not current can be joined");
    // A friend in a private room is joined by invitation only.
    auto privateRoom = InRoom("Private", true);
    privateRoom.presence.privateRoom = true;
    state.friends.push_back(privateRoom);
    const auto hidden = fp::FriendChoices(state, state.friends.back(), true, open);
    Check(!Pick(hidden, "join")->enabled && Pick(hidden, "join")->detail == loc::T("friends.private_invite_only"), "a private room can be joined, or says no reason");
}

// More on a request, and the question that asks again.
void MoreOnARequest() {
    const auto choices = fp::RequestChoices(fp::Gate{true, true, nullptr, nullptr});
    Check(choices.size() == 4 && choices[0].id == "card" && choices[1].id == "accept" && choices[2].id == "decline" && choices[3].id == "decline-block", "More on a request is not Player card, Accept, Decline, Decline and block");
    Check(choices[0].enabled && choices[1].enabled && choices[2].enabled && choices[3].enabled, "a request cannot be answered with nothing in flight");
    const auto busy = fp::RequestChoices(fp::Gate{false, true, "friends.busy", nullptr});
    Check(busy[0].enabled && !busy[1].enabled && !busy[2].enabled && busy[1].detail == loc::T("friends.busy"), "a request can be answered while an action is in flight, or its card cannot be opened");
    const auto ask = fp::AskChoices("remove-block", "Player 3");
    Check(ask.size() == 1 && ask[0].id == "yes" && ask[0].label == loc::T("friends.remove_block") && ask[0].detail == "Player 3", "the question that asks again does not name the choice and the player");
    Check(fp::AskChoices("decline", "x")[0].label == loc::T("friends.decline") && fp::AskChoices("decline-block", "x")[0].label == loc::T("friends.decline_block") &&
        fp::AskChoices("remove", "x")[0].label == loc::T("friends.remove"), "the question's label is not the choice's");
}

// What the tab can say: loading, no account, a service without friends, a read that failed with nothing before it, the list.
void Modes() {
    using fp::Mode;
    nf::View view;
    Check(fp::ModeOf(view, true) == Mode::Loading, "a view not read is not loading");
    Check(fp::ModeOf(view, false) == Mode::Unavailable, "no service is not a service without friends");
    view.bridge = "brg_1";
    view.state.error = "bridge_unreachable";
    Check(fp::ModeOf(view, true) == Mode::Unreachable, "a read that failed with nothing before it is not unreachable");
    view.state.loading = true;
    Check(fp::ModeOf(view, true) == Mode::Loading, "a read in flight is not loading");
    view.state.loading = false; view.state.error.clear(); view.state.loaded = true;
    Check(fp::ModeOf(view, true) == Mode::Unavailable, "a service that said it has no friends is not unavailable");
    auto none = std::make_shared<nf::State>();
    none->kind = nf::State::Kind::NoAccount;
    view.state.value = none;
    Check(fp::ModeOf(view, true) == Mode::NoAccount, "a player without a Discord account is not NoAccount");

    auto listed = ListedView({});
    Check(fp::ModeOf(listed, true) == Mode::Listed && !fp::Stale(listed), "a list is not listed and current");
    // The last sync failed: the state it kept shows as not updated, and nothing can be done on it.
    listed.state.error = "bridge_unreachable";
    Check(fp::ModeOf(listed, true) == Mode::Listed && fp::Stale(listed), "a failed sync does not leave the list stale");
    auto gate = fp::GateOf(listed, true, false);
    Check(!gate.act && !gate.go && std::string(gate.goWhy) == "friends.unreachable", "a stale list can be acted on");
    // An action the bridge refused leaves its code in the error too, and the list is current.
    listed.state.error = listed.refusal = "friend_code_unknown";
    gate = fp::GateOf(listed, true, false);
    Check(!fp::Stale(listed) && gate.act && gate.go, "a refused action makes the list stale");
    // An action in flight, or one just sent, holds the bridge's actions and not Join or Watch.
    listed.state.error.clear(); listed.refusal.clear();
    listed.busy = true;
    gate = fp::GateOf(listed, true, false);
    Check(!gate.act && gate.go && std::string(gate.actWhy) == "friends.busy" && gate.goWhy == nullptr, "an action in flight does not hold the actions");
    listed.busy = false;
    Check(!fp::GateOf(listed, true, true).act && fp::GateOf(listed, true, true).go, "an action just sent does not hold the actions");
    Check(!fp::GateOf(view, true, false).act && !fp::GateOf(view, true, false).go, "a player without an account can act on friends");
}

// The lines under a name.
void Lines() {
    Check(fp::Ago(Now, Now - 10) == loc::Tf("friends.hours_ago", 1) && fp::Ago(Now, Now - 3 * Hour - 5) == loc::Tf("friends.hours_ago", 3) &&
        fp::Ago(Now, Now - 23 * Hour) == loc::Tf("friends.hours_ago", 23), "hours ago are not an hour at least");
    Check(fp::Ago(Now, Now - Day) == loc::Tf("friends.days_ago", 1) && fp::Ago(Now, Now - 9 * Day - 3 * Hour) == loc::Tf("friends.days_ago", 9), "days ago are wrong");
    Check(fp::Ago(0, Now).empty() && fp::Ago(Now, 0).empty(), "an unknown time has words");

    Check(fp::Status(Online("A"), Now) == loc::T("friends.menus"), "an online friend is not at the menus");
    auto offlinePlay = Online("A");
    offlinePlay.presence.activity = nf::Activity::OfflinePlay;
    Check(fp::Status(offlinePlay, Now) == loc::T("friends.offline_play"), "a friend playing offline is not playing offline");
    Check(fp::Status(Offline("A", Now - 3 * Hour), Now) == loc::Tf("friends.last_online", loc::Tf("friends.hours_ago", 3)) && fp::Status(Offline("A", 0), Now).empty(), "an offline friend's line is wrong");
    Check(fp::Status(InRoom("A", true, nf::Status::Fighting, 0), Now) == loc::Tf("friends.playing_table", 1, "Player 5's room"), "a friend fighting is not at their table, numbered from 1");
    Check(fp::Status(InRoom("A", true, nf::Status::Watching, 2), Now) == loc::Tf("friends.watching_table", 3, "Player 5's room"), "a friend watching is not at the table they watch");
    Check(fp::Status(InRoom("A", true, nf::Status::Training), Now) == loc::Tf("friends.training_room", "Player 5's room"), "a friend in Training does not say so");
    Check(fp::Status(InRoom("A", true, nf::Status::Queued), Now) == loc::Tf("friends.waiting_room", "Player 5's room") &&
        fp::Status(InRoom("A", true, nf::Status::Fighting, -1), Now) == loc::Tf("friends.waiting_room", "Player 5's room"), "a friend in a room with no table is not in the room");
    auto privateRoom = InRoom("A", true);
    privateRoom.presence.privateRoom = true;
    Check(fp::Status(privateRoom, Now) == loc::T("friends.private_room"), "a friend in a private room names it");

    nf::Incoming request;
    Check(fp::PlayedLine(request, Now) == loc::T("friends.not_played"), "a request from a stranger says they played");
    request.games = 6; request.lastGameAt = Now - Day;
    Check(fp::PlayedLine(request, Now) == loc::Tf("friends.played_you", 6, loc::Tf("friends.days_ago", 1)), "a request's played line is wrong");
}

// The code typed on Add a friend: letters and numbers only count, any case and dashes or spaces allowed, eight make a code.
void Codes() {
    Check(fp::CodeSymbols("4f2a-91cx") == fp::CodeLength && fp::CodeSymbols("4F2A 91CX") == fp::CodeLength && fp::CodeSymbols("4f2a91c") == 7 && fp::CodeSymbols("--- ") == 0, "the symbols of a code are miscounted");
    Check(fp::CodeShown("4f2a91cx") == "4F2A-91CX" && fp::CodeShown(" 4f2a-91cx ") == "4F2A-91CX" && fp::CodeShown("4f2a") == "4F2A" && fp::CodeShown("4f2a9") == "4F2A-9", "a typed code is not shown as XXXX-XXXX");
    Check(fp::IdOf("friend:fr_3", fp::FriendPrefix) == "fr_3" && fp::IdOf("request:rq_1", fp::FriendPrefix).empty() && fp::IdOf("sent:sq_9", fp::SentPrefix) == "sq_9", "a row's id is not read after its prefix");
}

// A friend's card: the main and its points, and the account's as a second line once the bridge sent them (BP on a
// friend's card); an older bridge's state has the one line, and a friend with no main shows only the account.
void Cards() {
    auto entry = Pal("Player 3");
    entry.country = CountryCode::Parsed("FR");
    entry.hasMain = true; entry.main.fighter = 5; entry.main.showsPoints = true; entry.main.points = 1986; entry.main.tier = 3;
    auto card = fp::CardOf(entry, ui::FighterArt::Portrait);
    Check(card.name == "Player 3" && card.fighter == 5 && card.points && card.fighterPoints == 1986 && card.fighterTier == 3, "a friend's card does not show their main and its points");
    Check(!card.accountKnown, "a friend the bridge sent no account for shows an account line");
    entry.hasAccount = true; entry.account = {2331, 2};
    card = fp::CardOf(entry, ui::FighterArt::Portrait);
    Check(card.accountKnown && card.accountPoints == 2331 && card.fighterPoints == 1986, "a friend's account points are not on their card beside the main's");
    // A main not yet played shows 0 FP beside the account's points.
    entry.main.showsPoints = false; entry.main.points = 0; entry.main.tier = 0;
    card = fp::CardOf(entry, ui::FighterArt::Portrait);
    Check(card.points && card.fighterPoints == 0 && card.accountKnown && card.accountPoints == 2331, "an unplayed main hid the account's points");
    // No main and no account: no points lines at all.
    entry.hasMain = false; entry.hasAccount = false;
    card = fp::CardOf(entry, ui::FighterArt::Portrait);
    Check(card.fighter == -1 && !card.points && !card.accountKnown, "a friend with nothing to show has points lines");
}
}

// How an action the panel sent ends, every way with words: a refusal says its own, one that went through says what
// it did, one that found friends off says so. The runtime answers or refuses every action, so nothing else ends one.
void Outcomes() {
    using fp::Outcome;
    auto view = ListedView({});
    view.state.finished = 4; view.refusals = 2;
    Check(fp::OutcomeOf(view, 4, 2) == Outcome::Waiting, "an action with no answer yet did not wait");
    view.busy = true; view.state.finished = 5;
    Check(fp::OutcomeOf(view, 4, 2) == Outcome::Waiting, "a sync's answer while the action is held or in flight ended it");
    view.busy = false;
    Check(fp::OutcomeOf(view, 4, 2) == Outcome::Done, "an answer with nothing in flight did not end it as done");
    view.refusals = 3;
    Check(fp::OutcomeOf(view, 4, 2) == Outcome::Refused, "a refusal did not end it as refused");
    auto off = ListedView({});
    off.state.value = std::make_shared<const nf::State>([] { nf::State s; s.kind = nf::State::Kind::Off; return s; }());
    off.state.finished = 5;
    Check(fp::OutcomeOf(off, 4, 2) == Outcome::Off, "friends found off did not say so");
}

// A room member's Add friend and Block name them by card: a list the last sync failed on does not hold them, an action
// in flight does, and no list says why in its own words (not "could not reach" for a player without a Discord account).
void MemberGate() {
    auto listed = ListedView({});
    listed.state.error = "rate_limited";
    Check(fp::Stale(listed) && !fp::GateOf(listed, true, false).act, "the test's list is not stale");
    auto gate = fp::MemberGateOf(listed, true, false);
    Check(gate.act && gate.actWhy == nullptr, "a stale list held a member's Add friend");
    gate = fp::MemberGateOf(listed, true, true);
    Check(!gate.act && std::string(gate.actWhy) == "friends.busy", "an action just sent did not hold a member's Add friend");
    listed.busy = true;
    Check(!fp::MemberGateOf(listed, true, false).act, "an action in flight did not hold a member's Add friend");
    nf::View loading;
    loading.bridge = "brg_1";
    Check(std::string(fp::MemberGateOf(loading, true, false).actWhy) == "friends.loading", "a list not read yet does not say so");
    auto none = ListedView({});
    none.state.value = std::make_shared<const nf::State>([] { nf::State s; s.kind = nf::State::Kind::NoAccount; return s; }());
    Check(std::string(fp::MemberGateOf(none, true, false).actWhy) == "friends.no_account", "a player without a Discord account is told the service could not be reached");
    auto off = ListedView({});
    off.state.value = std::make_shared<const nf::State>([] { nf::State s; s.kind = nf::State::Kind::Off; return s; }());
    Check(std::string(fp::MemberGateOf(off, true, false).actWhy) == "friends.off", "friends turned off do not say so");
    Check(fp::ModeWords(fp::Mode::Listed) == nullptr && std::string(fp::ModeWords(fp::Mode::Unavailable)) == "friends.unavailable", "a mode's words are wrong");
}

// What Accept says: who is a friend now, by the request's name; nothing for a request no longer there.
void AcceptWords() {
    nf::State state;
    nf::Incoming request;
    request.id = "rq_1"; request.name = "Player 9";
    state.incoming = {request};
    Check(fp::AcceptedWords(state, "rq_1") == loc::Tf("friends.now_friends", "Player 9") && fp::AcceptedWords(state, "rq_2").empty(), "Accept does not say who is now a friend");
}

// What waits in Notifications: every request, newest first, then the invites still good, for as long as the list holds
// them; nothing at all while the friends are off, without an account, unavailable or not read.
void WaitingInNotifications() {
    nf::State state;
    nf::Incoming old, fresh;
    old.id = "rq_1"; old.name = "Player 9"; old.sentAt = Now - Day;
    fresh.id = "rq_2"; fresh.name = "Player 2"; fresh.sentAt = Now - Hour;
    state.incoming = {old, fresh};
    state.friends = {InRoom("Player 3", true)};
    nf::Invite good, late, stranger;
    good.id = "iv_1"; good.friendId = "fr_Player 3"; good.expiresAt = Now + Hour; good.room.value = std::string(32, 'a');
    late.id = "iv_2"; late.friendId = "fr_Player 3"; late.expiresAt = Now - 1;
    stranger.id = "iv_3"; stranger.friendId = "fr_nobody"; stranger.expiresAt = Now + Hour;
    state.invites = {good, late, stranger};
    const auto view = ListedView(state);
    const auto waiting = fp::WaitingOf(view, Now);
    Check(waiting.requests.size() == 2 && waiting.requests[0]->id == "rq_2" && waiting.requests[1]->id == "rq_1", "requests waiting are not every request, newest first");
    Check(waiting.invites.size() == 1 && waiting.invites[0]->id == "iv_1" && waiting.Count() == 3, "an invite run out, or from no friend in the list, waits");
    Check(waiting.Holds("rq_1") && waiting.Holds("iv_1") && !waiting.Holds("iv_2") && !waiting.Holds("rq_9"), "Holds does not say what waits");
    // As Notifications' items: keyed by their rows' ids, in the same order, with the toast's words and buttons, and
    // asking to be seen.
    const auto notices = fp::NoticesOf(view, Now);
    Check(notices.size() == 3 && notices[0].key == "request:rq_2" && notices[1].key == "request:rq_1" && notices[2].key == "invite:iv_1",
        "what waits is not told into Notifications by its rows' ids, in order");
    Check(notices[0].Head() == loc::Tf("friends.toast_request", "Player 2") && notices[0].text == notices[0].Head() + "\n" + loc::T("friends.not_played") &&
        notices[0].actions.size() == 2 && notices[0].actions[0].label == loc::T("friends.accept") && notices[0].actions[1].label == loc::T("friends.later") && notices[0].again,
        "a request's item does not say who asks, whether they played, Accept and Later");
    Check(notices[2].text == loc::Tf("friends.toast_invite", "Player 3") + "\nPlayer 5's room" && notices[2].actions[0].label == loc::T("toast.join") &&
        notices[2].actions[1].label == loc::T("friends.dismiss"), "an invite's item does not say who invites, to which room, Join and Dismiss");
    Check(notices[0].actions[0].commands.empty(), "a request's Accept goes around the panel");
    // A stale list still holds what waits; no list holds nothing, and says nothing there.
    auto stale = view;
    stale.state.error = "bridge_unreachable";
    Check(fp::WaitingOf(stale, Now).Count() == 3, "a list a sync failed on dropped what waits");
    for (const auto kind : {nf::State::Kind::Off, nf::State::Kind::NoAccount, nf::State::Kind::Unavailable}) {
        auto other = view;
        auto copy = state; copy.kind = kind;
        other.state.value = std::make_shared<const nf::State>(copy);
        Check(fp::WaitingOf(other, Now).Count() == 0 && fp::NoticesOf(other, Now).empty(), "friends off, without an account or unavailable list something waiting");
    }
    Check(fp::WaitingOf(nf::View{}, Now).Count() == 0, "a list not read yet has something waiting");
    Check(fp::NotificationRow("request:rq_1") && fp::NotificationRow("invite:iv_1") && !fp::NotificationRow("friend:fr_3") && !fp::NotificationRow("toast-4"), "the rows Notifications hands the panel are wrong");
    const auto choices = fp::InviteChoices(fp::Gate{true, true, nullptr, nullptr});
    Check(choices.size() == 2 && choices[0].id == "join" && choices[1].id == "dismiss" && choices[0].enabled && choices[1].enabled, "More on an invite is not Join and Dismiss");
    const auto busy = fp::InviteChoices(fp::Gate{false, true, "friends.busy", nullptr});
    Check(busy[0].enabled && !busy[1].enabled && busy[1].detail == loc::T("friends.busy"), "an action in flight holds an invite's Join, or not its Dismiss");
}

// Find a player: a whole friend code is eight letters and numbers with dashes and spaces among them and nothing else; Search
// asks for a name from two characters, or the code, and on a service without profiles only the code.
void FindAsks() {
    namespace fnd = ui::find_player;
    Check(fnd::WholeCode("4f2a-91cx") && fnd::WholeCode(" 4F2A 91CX ") && fnd::WholeCode("4f2a91cx"), "a code typed in any form is not a whole code");
    Check(!fnd::WholeCode("4f2a91c") && !fnd::WholeCode("4f2a-91cx9") && !fnd::WholeCode("4f2a.91cx") && !fnd::WholeCode("Player 9") && !fnd::WholeCode(""), "a name or a short code is a whole code");
    Check(fnd::Trimmed("  Pl \t") == "Pl" && fnd::Trimmed("   ").empty(), "the text is not trimmed");

    auto name = fnd::AskOf("  Pl ", pr::Service::Offered);
    Check(name.ok && !name.code && name.text == "Pl" && name.why == nullptr, "two characters are not a name to search");
    auto one = fnd::AskOf("P", pr::Service::Offered);
    Check(!one.ok && std::string(one.why) == "find.text_detail" && !fnd::AskOf("", pr::Service::Unknown).ok, "one character is a name to search");
    auto code = fnd::AskOf(" 4f2a-91cx ", pr::Service::Offered);
    Check(code.ok && code.code && code.text == "4f2a-91cx", "a whole code is not searched as a code");
    // A service without profiles has no search by name, and still finds a code.
    auto missing = fnd::AskOf("Player", pr::Service::Missing);
    Check(!missing.ok && std::string(missing.why) == "profile.unsupported" && fnd::AskOf("4f2a91cx", pr::Service::Missing).ok, "a service without profiles is searched by name");

    fnd::Page page;
    page.typed = "P";
    page.offering = pr::Service::Offered;
    const auto search = fnd::SearchEntry(page);
    Check(search.id == fnd::SearchRow && !search.enabled && search.detail == loc::T("find.text_detail"), "Search is open before two characters");
    page.typed = "Pl";
    Check(fnd::SearchEntry(page).enabled && fnd::SearchEntry(page).hint == loc::T("find.search"), "Search is not open at two characters");
}

pr::Hit HitOf(const char* token, const char* name, std::uint64_t since) {
    pr::Hit hit;
    hit.card = token; hit.name = name; hit.country = CountryCode::Parsed("GB"); hit.hasMain = true; hit.main = 5; hit.badge = SkillBadge::OfClass(4, true); hit.since = since;
    return hit;
}

// What Find a player lists under Search: how the search stands, the players it found for the text typed (and not for other
// text), then the players met lately; and whom each row opens.
void FindRows() {
    namespace fnd = ui::find_player;
    pr::SearchView search;
    std::vector<pr::Recent> recent;
    fnd::Page page;
    page.typed = "Player"; page.offering = pr::Service::Offered; page.search = &search; page.recent = &recent; page.now = Now;
    // Nothing searched and nobody met: the line that says what shows here.
    auto rows = fnd::ResultRows(page);
    Check(rows.size() == 1 && rows[0].id == fnd::FindNoteRow && rows[0].label == loc::T("find.recent") && rows[0].detail == loc::T("find.recent_empty") && rows[0].info, "an empty page does not say who shows here");
    // In flight.
    search.text = "Player"; search.answer.loading = true;
    rows = fnd::ResultRows(page);
    Check(rows.size() == 1 && rows[0].label == loc::T("find.searching") && fnd::AnswerOf(page) == nullptr, "a search in flight does not say so");
    // Answered: a row for each player, under their count, then the note when more match.
    search.answer.loading = false; search.answer.loaded = true;
    search.answer.value.players = {HitOf("c1.a", "Player 2", Now - 400 * Day), HitOf("c1.b", "Player 3", 0)};
    rows = fnd::ResultRows(page);
    Check(rows.size() == 2 && rows[0].id == "find-player:0" && rows[1].id == "find-player:1" && rows[0].label == "Player 2" && rows[0].group == loc::Tf("find.players", 2) && rows[1].group == rows[0].group,
        "a search's players are not a row each under their count");
    Check(rows[0].height == 64 && rows[0].hint == loc::T("room.member_card") && rows[0].userText && !rows[0].info && rows[1].detail.empty() && !rows[0].detail.empty(), "a result is not a row of 64 that opens a card, with since when");
    search.answer.value.more = true;
    rows = fnd::ResultRows(page);
    Check(rows.size() == 3 && rows[2].id == fnd::FindNoteRow && rows[2].label == loc::T("find.more"), "more players than the search lists do not say so");
    search.answer.value.players.clear(); search.answer.value.more = false;
    rows = fnd::ResultRows(page);
    Check(rows.size() == 1 && rows[0].label == loc::T("find.none"), "a search that found nobody does not say so");
    // An answer for other text, or asked the other way, is not shown under this.
    search.answer.value.players = {HitOf("c1.a", "Player 2", 0)};
    page.typed = "Other";
    Check(fnd::AnswerOf(page) == nullptr && fnd::ResultRows(page).size() == 1 && fnd::ResultRows(page)[0].detail == loc::T("find.recent_empty"), "an answer for other text is shown");
    page.typed = "4f2a-91cx";
    search.text = "4f2a-91cx"; search.byCode = false;
    Check(fnd::AnswerOf(page) == nullptr, "a name search's answer is shown for a code");
    search.byCode = true;
    Check(fnd::AnswerOf(page) != nullptr, "a code search's answer is not shown for the code");
    page.typed = "Player"; search.text = "Player"; search.byCode = false;
    // The players met lately follow, newest first, with when and where.
    pr::Recent first;
    first.card = "c1.r0"; first.name = "Player 5"; first.country = CountryCode::Parsed("DE"); first.fighter = 12; first.room = "Player 5's room"; first.at = Now - 2 * Hour;
    pr::Recent second = first;
    second.card = "c1.r1"; second.name = "Player 6"; second.fighter = -1; second.room.clear(); second.at = Now - 30 * Hour;
    recent = {first, second};
    search.answer.value.players = {HitOf("c1.a", "Player 2", 0), HitOf("c1.b", "Player 3", 0)};
    rows = fnd::ResultRows(page);
    Check(rows.size() == 4 && rows[2].id == "find-recent:0" && rows[3].id == "find-recent:1" && rows[2].group == loc::T("find.recent") && rows[3].group == rows[2].group, "the players met lately are not listed under their heading");
    Check(rows[2].detail == loc::Tf("find.recent_line", loc::Tf("friends.hours_ago", 2), "Player 5's room") && rows[3].detail == loc::Tf("find.recent_when", loc::Tf("friends.days_ago", 1)),
        "a recent opponent's row does not say when and where");
    // Whom a row opens: by the token the search or the visit kept, shown with what the row knew.
    const auto hit = fnd::OpeningOfRow(page, "find-player:1");
    Check(hit && hit->key == pr::Key::OfCard("c1.b") && hit->known.name == "Player 3" && hit->known.fighter == 5 && hit->known.badge == SkillBadge::OfClass(4, true), "a result does not open by its card token");
    const auto met = fnd::OpeningOfRow(page, "find-recent:0");
    Check(met && met->key == pr::Key::OfCard("c1.r0") && met->known.name == "Player 5" && met->known.fighter == 12 && met->known.country == CountryCode::Parsed("DE"), "a recent opponent does not open by their token");
    Check(!fnd::OpeningOfRow(page, "find-player:2") && !fnd::OpeningOfRow(page, "find-recent:2") && !fnd::OpeningOfRow(page, "friend:fr_1") && !fnd::OpeningOfRow(page, "find-player:x"), "a row that lists nobody opens someone");
    const auto pick = fnd::PickOfRow(page, "find-recent:1", true);
    Check(pick && pick->name == "Player 6" && pick->fighter == -1 && pick->characterArt && !fnd::PickOfRow(page, "friends-search", false), "a row is not drawn from the player it lists");
}

// One opening for each kind of row, so no screen builds its own: by the key the list holds them by, with what the row knows.
void Openings() {
    namespace po = ui::player_opening;
    const auto badge = SkillBadge::OfClass(5, true);
    room::Member member;
    member.id = 2; member.name = "Player 2"; member.country = CountryCode::Parsed("GB"); member.badge = badge; member.fighter = 12; member.card = CardToken::Parsed("c1.abc");
    auto opening = po::OpeningOf(member);
    Check(opening.key == pr::Key::OfCard("c1.abc") && opening.known.name == "Player 2" && opening.known.country == CountryCode::Parsed("GB") && opening.known.badge == badge && opening.known.fighter == 12,
        "a room member does not open by their token with the fighter they picked");
    // A private room has no token: their name and flag.
    member.card = CardToken();
    Check(po::OpeningOf(member).key == pr::Key::OfName("Player 2", CountryCode::Parsed("GB")), "a member with no token does not open by name and flag");
    member.fighter = 9999;
    Check(po::OpeningOf(member).known.fighter == -1, "a fighter the game does not know is shown");

    auto entry = Pal("Player 3");
    entry.country = CountryCode::Parsed("FR"); entry.badge = badge; entry.hasMain = true; entry.main.fighter = 5;
    opening = po::OpeningOf(entry);
    Check(opening.key == pr::Key::OfFriend("fr_Player 3") && opening.known.name == "Player 3" && opening.known.fighter == 5 && opening.known.badge == badge && opening.known.country == CountryCode::Parsed("FR"),
        "a friend does not open by their friend id with their main");
    entry.hasMain = false;
    Check(po::OpeningOf(entry).known.fighter == -1, "a friend with no main shows a fighter");

    nf::Incoming request;
    request.id = "rq_1"; request.name = "Player 9"; request.country = CountryCode::Parsed("IT");
    opening = po::OpeningOf(request);
    Check(opening.key == pr::Key::OfRequest("rq_1") && opening.known.name == "Player 9" && opening.known.country == CountryCode::Parsed("IT") && opening.known.fighter == -1, "a request does not open by its id");

    netplay::rankings::BoardEntry ranked;
    ranked.listing = {"Player 4", CountryCode::Parsed("SE")}; ranked.band = 6;
    opening = po::OpeningOf(ranked);
    Check(opening.key == pr::Key::OfName("Player 4", CountryCode::Parsed("SE")) && opening.known.badge == SkillBadge::OfClass(6, true) && opening.known.name == "Player 4", "a ranking row does not open by name and flag with its class");

    opening = po::OpeningOfSide("Player 7", CountryCode(), badge, 5);
    Check(opening.key == pr::Key::OfName("Player 7") && opening.known.badge == badge && opening.known.fighter == 5 && !opening.known.country.Present(), "a side of a game does not open by name");
    Check(po::OpeningOfSide("Player 7", CountryCode(), badge, -1).known.fighter == -1, "a side with no fighter shows one");
}

// The post-match prompt's Add friend: the box by how the opponent stands with the player, live (a row, the View glyph) only
// for Add friend and Accept request, a flat box for Request sent and Friends, nothing without a card or for a block; a press
// made here is as good as answered before the card says so.
void PostMatchFriendStates() {
    namespace pm = ui::post_match_friend;
    using Kind = pm::State::Kind;
    using Relation = netplay::levels::Relation;
    pm::Inputs in;
    Check(pm::StateOf(in).kind == Kind::None && !pm::StateOf(in).Shown(), "an opponent with no card has a box");
    in.card = true;
    const auto of = [&](Relation relation, bool asked) { in.relation = relation; in.asked = asked; return pm::StateOf(in); };
    Check(of(Relation::None, false).kind == Kind::Add && of(Relation::None, false).Live(), "a stranger is not Add friend");
    Check(of(Relation::Received, false).kind == Kind::Accept && of(Relation::Received, false).Live(), "a request received is not Accept request");
    Check(of(Relation::Sent, false).kind == Kind::Sent && !of(Relation::Sent, false).Live() && of(Relation::Sent, false).Shown(), "a request sent is not a flat Request sent");
    Check(of(Relation::Friend, false).kind == Kind::Friends && !of(Relation::Friend, false).Live(), "a friend is not a flat Friends");
    Check(of(Relation::Blocked, false).kind == Kind::None && of(Relation::Blocked, true).kind == Kind::None, "a block has a box");
    Check(of(Relation::None, true).kind == Kind::Sent && of(Relation::Received, true).kind == Kind::Friends && of(Relation::Sent, true).kind == Kind::Sent && of(Relation::Friend, true).kind == Kind::Friends,
        "a request just pressed does not turn the box to Request sent or Friends");
    Check(std::string(pm::LabelOf(Kind::Add)) == "friends.add_member" && std::string(pm::LabelOf(Kind::Accept)) == "friends.accept_member" &&
        std::string(pm::LabelOf(Kind::Sent)) == "friends.request_sent" && std::string(pm::LabelOf(Kind::Friends)) == "friends.are_friends" && pm::LabelOf(Kind::None) == nullptr, "the box's words are wrong");
    // Only a live button is a row: wide (a footer, not a button of the bar), with the detail that names the opponent.
    pm::State add;
    add.kind = Kind::Add;
    const auto row = pm::RowOf(add, "Player 2");
    Check(row && row->id == pm::RowId && row->wide && row->enabled && row->label == loc::T("friends.add_member") && row->hint == row->label &&
        row->detail == loc::Tf("postmatch.add_friend.detail", "Player 2"), "Add friend is not a wide row with its detail");
    add.kind = Kind::Accept;
    Check(pm::RowOf(add, "Player 2")->detail == loc::Tf("postmatch.accept_friend.detail", "Player 2"), "Accept request does not say what it does");
    add.kind = Kind::Sent;
    Check(!pm::RowOf(add, "Player 2") && !pm::RowOf(pm::State{}, "Player 2"), "a box that cannot be pressed is a row");
    // The inputs: a card token and a card read; never this PC's own member.
    ui::ShellView view;
    view.room.localMember = 1;
    // Public rooms have cards; a private room has none (PointsOf), so the prompt offers nothing there.
    view.room.serverOwned = true;
    room::Member them;
    them.id = 2; them.name = "Player 2"; them.card = CardToken::Parsed("c1.abc");
    Check(!pm::InputsOf(view, &them, false).card && !pm::InputsOf(view, nullptr, false).card, "a card not read, or no opponent, has a box");
    auto cards = std::make_shared<netplay::levels::CardMap>();
    netplay::levels::Card card;
    card.relation = Relation::Received;
    (*cards)["c1.abc"] = card;
    view.memberCards = cards;
    auto read = pm::InputsOf(view, &them, true);
    Check(read.card && read.relation == Relation::Received && read.asked, "an opponent's card is not read from the room's cards");
    them.card = CardToken();
    Check(!pm::InputsOf(view, &them, false).card, "an opponent in a private room has a box");
    them.card = CardToken::Parsed("c1.abc"); them.id = 1;
    Check(!pm::InputsOf(view, &them, false).card, "this PC's own member has a box");
}

int main() {
    WaitingInNotifications();
    Outcomes();
    MemberGate();
    AcceptWords();
    Cards();
    Grouping();
    Presses();
    MoreOnAFriend();
    MoreOnARequest();
    Modes();
    Lines();
    Codes();
    FindAsks();
    FindRows();
    Openings();
    PostMatchFriendStates();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("friends panel tests passed\n");
    return 0;
}
