// The Friends tab's choices (docs/design/PROFILE.md, "Friends tab"), read without a frame: what the tab says of the
// state, how friends are grouped and ordered, what A does on a row from where the friend is and whether the room can be
// joined, what More offers with each choice's reason when it cannot be done, the lines under a name, and the code
// typed on Add a friend.
#include "../ui/FriendsPanel.hxx"
#include "../common/Localization.hxx"
#include <cstdio>
#include <memory>
#include <string>

namespace {
using namespace sf4e;
namespace fp = ui::friends_panel;
namespace nf = netplay::friends;
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

// A on a row: Join where the room can be joined, else Invite when this player has a room to offer, else More; an
// offline friend has only More, and a private room or one that is no longer listed cannot be joined.
void Presses() {
    using fp::Press;
    Check(fp::PressOf(InRoom("A", true), false) == Press::Join && fp::PressOf(InRoom("A", true), true) == Press::Join, "a joinable room is not Join");
    Check(fp::PressOf(InRoom("A", false), true) == Press::Invite && fp::PressOf(InRoom("A", false), false) == Press::More, "a room that cannot be joined is not Invite, else More");
    auto privateRoom = InRoom("A", true);
    privateRoom.presence.privateRoom = true;
    Check(!fp::Joinable(privateRoom) && fp::PressOf(privateRoom, false) == Press::More, "a private room can be joined");
    auto unlisted = InRoom("A", true);
    unlisted.presence.hasRoom = false;
    Check(!fp::Joinable(unlisted), "a room no longer listed can be joined");
    Check(fp::PressOf(Online("A"), true) == Press::Invite && fp::PressOf(Online("A"), false) == Press::More, "an online friend is not Invite, else More");
    Check(fp::PressOf(Offline("A", 1), true) == Press::More, "an offline friend can be invited");
}

// More's choices on a friend: each is listed, with the reason in its detail when it cannot be done now.
void MoreOnAFriend() {
    nf::State state;
    state.friends = {InRoom("Fighter", true, nf::Status::Fighting, 0), InRoom("Waiting", true), InRoom("Closed", false, nf::Status::Fighting, 0), Online("Menus"), Offline("Away", 1)};
    state.kind = nf::State::Kind::Listed;
    const fp::Gate open{true, true, nullptr, nullptr};
    const auto fighter = fp::FriendChoices(state, state.friends[0], true, open);
    Check(fighter.size() == 5 && fighter[0].id == "join" && fighter[1].id == "watch" && fighter[2].id == "invite" && fighter[3].id == "remove" && fighter[4].id == "remove-block", "More on a friend is not Join, Watch, Invite, Remove, Remove and block");
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
    const fp::Gate busy{false, true, nullptr, "friends.busy"};
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
    Check(choices.size() == 3 && choices[0].id == "accept" && choices[1].id == "decline" && choices[2].id == "decline-block", "More on a request is not Accept, Decline, Decline and block");
    Check(choices[0].enabled && choices[1].enabled && choices[2].enabled, "a request cannot be answered with nothing in flight");
    const auto busy = fp::RequestChoices(fp::Gate{false, true, nullptr, "friends.busy"});
    Check(!busy[0].enabled && !busy[1].enabled && busy[0].detail == loc::T("friends.busy"), "a request can be answered while an action is in flight");
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

int main() {
    Cards();
    Grouping();
    Presses();
    MoreOnAFriend();
    MoreOnARequest();
    Modes();
    Lines();
    Codes();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("friends panel tests passed\n");
    return 0;
}
