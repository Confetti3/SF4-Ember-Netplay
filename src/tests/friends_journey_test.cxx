// The Friends tab (docs/design/PROFILE.md, "Friends tab"): it keeps the friends fresh while it shows (Visit as it opens,
// Leave as it closes), lists requests and the friends by where they are, A does the row's word (Accept, Join, Invite,
// More), Y opens More and Remove or Decline ask again, X opens Add a friend where a typed code sends a request,
// Appear offline, Sent and Blocked take back a request and lift a block, each state without a list is one card, and a
// Join that asks first opens its question.
#include "public_rooms_journey_support.hxx"
#include "../ui/FriendsPanel.hxx"
#include <imgui.h>
#include <memory>
namespace {
namespace nf = sf4e::netplay::friends;
using Kind = nf::Command::Kind;
constexpr std::uint64_t Now = 1800000000, Hour = 3600, Day = 24 * Hour;

nf::Friend Pal(const char* id, const char* name) {
 nf::Friend entry; entry.id = std::string("fr_") + id; entry.name = name; entry.since = Now - 20 * Day;
 return entry;
}
nf::Friend FriendIn(const char* id, const char* name, bool joinable, nf::Status status, int table) {
 auto entry = Pal(id, name);
 auto& p = entry.presence;
 p.kind = nf::Presence::Kind::InRoom; p.status = status; p.table = table; p.hasRoom = true;
 p.room.id = std::string(32, 'a'); p.room.name = "Player 5's room"; p.room.joinable = joinable; p.room.members = 7; p.room.capacity = 16;
 return entry;
}

// Two requests, a friend fighting in a room that can be joined, one in a room that cannot, one at the menus and one offline.
std::shared_ptr<nf::State> FullState() {
 auto state = std::make_shared<nf::State>();
 state->kind = nf::State::Kind::Listed; state->code = "4F2A-91CX";
 nf::Incoming first, second;
 first.id = "rq_1"; first.name = "Player 9"; first.sentAt = Now - Hour; first.games = 6; first.lastGameAt = Now - Day;
 second.id = "rq_2"; second.name = "Player 12"; second.sentAt = Now - 2 * Hour;
 state->incoming = {first, second};
 auto menus = Pal("6", "Player 6"); menus.presence.kind = nf::Presence::Kind::Online;
 auto away = Pal("8", "Player 8"); away.presence.seenAt = Now - 3 * Hour;
 state->friends = {away, menus, FriendIn("5", "Player 5", false, nf::Status::Idle, -1), FriendIn("3", "Player 3", true, nf::Status::Fighting, 0)};
 state->outgoing = {{"sq_1", "Player 14", Now - 2 * Day}};
 state->blocked = {{"bl_1", "Player 16", Now - 9 * Day}};
 return state;
}

// A journey on a service the player trusts, whose friends the runtime has answered.
struct Social : Journey {
 explicit Social(std::shared_ptr<nf::State> state = FullState()) {
  h.Screen("home"); h.Frame(0, 2); answerIdentityUntilIdle();
  h.view.unixNow = Now;
  Give(std::move(state));
 }
 nf::View& view() { return h.view.friends; }
 // The runtime installs a new state.
 void Give(std::shared_ptr<nf::State> state, bool loaded = true) {
  auto& f = view();
  f.bridge = "brg_1"; f.state.value = std::move(state); f.state.loaded = loaded; ++f.state.revision; ++f.state.finished; f.canInvite = true;
  h.Frame(0, 2);
 }
 // The commands of that kind sent so far.
 std::vector<const nf::Command*> sent(Kind kind) const {
  std::vector<const nf::Command*> out;
  for (const auto& a : h.actions) if (const auto* f = As<commands::Friends>(a)) if (f->friends.kind == kind) out.push_back(&f->friends);
  return out;
 }
 void open(const char* screen = "friends") { h.Screen(screen); h.Frame(0, 3); }
 // Focus on a row at once (the board's rows are placed by where they are drawn, which a walk would have to reach).
 void focus(const char* id) { h.shell.Navigation().Focus(id, rows); h.Frame(); }
 // Select on a row.
 void select(const char* id) { focus(id); h.Press(MenuInput::Select); }
 bool choosing() { return h.shell.Navigation().Choosing(); }
 std::string screen() { return h.shell.Navigation().Screen(); }
 // The nth option of the open choice, picked with the pad.
 void pick(int index) { for (int i = 0; i < index; ++i) h.Press(MenuInput::Down); h.Press(MenuInput::Select); }
 // Types into the open editor and presses Enter.
 void type(const char* text) {
  ImGui::GetIO().AddInputCharactersUTF8(text); h.Frame();
  ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true); h.Frame(); ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false); h.Frame();
 }
};

std::vector<std::string> Ids(const Social& j) {
 std::vector<std::string> ids;
 for (const auto& row : j.rows) ids.push_back(row.id);
 return ids;
}

// The tab keeps the friends fresh: Visit as it opens (and not again for a page over it), Leave as it closes, and Visit
// again once its cadence lapsed.
void VisitJourney() {
 Social j;
 Check(j.sent(Kind::Visit).empty(), "Home visited the friends");
 j.open();
 Check(j.sent(Kind::Visit).size() == 1 && j.sent(Kind::Visit)[0]->bridgeId == "brg_1", "Opening Friends did not send one Visit for the service");
 j.h.Frame(0, 30);
 Check(j.sent(Kind::Visit).size() == 1, "Friends sent Visit again while it showed");
 j.view().visiting = true; j.h.Frame(0, 2);
 j.h.shell.Navigation().Push("friends-add"); j.h.Frame(0, 3);
 Check(j.screen() == "friends-add" && j.sent(Kind::Visit).size() == 1 && j.sent(Kind::Leave).empty(), "A page over Friends sent Visit or Leave");
 j.h.Press(MenuInput::Back);
 Check(j.screen() == "friends" && j.sent(Kind::Leave).empty(), "Back from Add a friend left Friends");
 // The runtime's cadence lapsed while the tab showed.
 j.view().visiting = false; j.h.Frame(0, 2);
 Check(j.sent(Kind::Visit).size() == 2, "Friends did not say it still shows once the cadence lapsed");
 j.h.Press(MenuInput::Back);
 Check(j.screen() == "home" && j.sent(Kind::Leave).size() == 1, "Closing Friends did not send Leave");
 j.h.Frame(0, 10);
 Check(j.sent(Kind::Leave).size() == 1, "Leave was sent again");
 // Stepping to the next tab and back is a Leave and a Visit.
 j.open();
 j.h.Press(MenuInput::NextTab);
 Check(j.screen() == "awards" && j.sent(Kind::Leave).size() == 2, "Stepping off Friends did not send Leave");
}

// The rows: Add a friend, the requests newest first, friends in a room (joinable first), online, offline, then Sent,
// Blocked and Appear offline; each says what A does.
void RowsJourney() {
 Social j;
 j.open();
 const std::vector<std::string> expected = {"friends-add", "request:rq_1", "request:rq_2", "friend:fr_3", "friend:fr_5", "friend:fr_6", "friend:fr_8", "friends-sent", "friends-blocked", "friends-appear-offline"};
 Check(Ids(j) == expected, "Friends does not list Add a friend, the requests, the friends by group, Sent, Blocked and Appear offline");
 const auto* add = j.row("friends-add");
 Check(add && add->value == loc::Tf("friends.your_code", "4F2A-91CX") && add->hint == loc::T("friends.open"), "Add a friend does not show the code");
 Check(j.row("request:rq_1")->group == loc::Tf("friends.requests_heading", 2) && j.row("friend:fr_3")->group == loc::Tf("friends.in_room", 2) &&
  j.row("friend:fr_6")->group == loc::Tf("friends.online", 1) && j.row("friend:fr_8")->group == loc::Tf("friends.offline", 1), "The friends' groups are not named with their counts");
 Check(j.row("request:rq_1")->value == loc::T("friends.accept") && j.row("request:rq_1")->detail == loc::Tf("friends.played_you", 6, loc::Tf("friends.days_ago", 1)) &&
  j.row("request:rq_2")->detail == loc::T("friends.not_played"), "A request does not say Accept and whether they played");
 // With a room to offer: Join where it can be joined, else Invite; the offline friend has only More.
 Check(j.row("friend:fr_3")->hint == loc::T("friends.join") && j.row("friend:fr_5")->hint == loc::T("friends.invite") && j.row("friend:fr_6")->hint == loc::T("friends.invite") &&
  j.row("friend:fr_8")->hint == loc::T("friends.more") && j.row("friend:fr_8")->value.empty(), "A friend's row does not say Join, Invite or More");
 Check(j.row("friend:fr_3")->detail == loc::Tf("friends.playing_table", 1, "Player 5's room") && j.row("friend:fr_6")->detail == loc::T("friends.menus") &&
  j.row("friend:fr_8")->detail == loc::Tf("friends.last_online", loc::Tf("friends.hours_ago", 3)), "A friend's row does not say where they are");
 Check(j.row("friends-sent")->label == loc::Tf("friends.sent", 1) && j.row("friends-blocked")->label == loc::Tf("friends.blocked", 1) &&
  j.row("friends-appear-offline")->value == loc::T("common.off"), "Sent, Blocked and Appear offline are not listed with their counts and state");
 // Nothing to offer: the same rows say More where they said Invite.
 j.view().canInvite = false; j.h.Frame(0, 2);
 Check(j.row("friend:fr_5")->hint == loc::T("friends.more") && j.row("friend:fr_3")->hint == loc::T("friends.join"), "Without a room to offer a friend's row still says Invite");
 // The tab's label counts the requests.
 Check(ui::MenuPlayerCard().requests == 2, "The Friends tab does not count the requests");
}

// A on a joinable friend sends Join; on a request, Accept; on a friend to invite, Invite. While an action is in flight
// the bridge's actions wait and Join does not; a list that is not current takes none.
void ActionsJourney() {
 Social j;
 j.open();
 j.select("friend:fr_3");
 Check(j.sent(Kind::Join).size() == 1 && j.sent(Kind::Join)[0]->id == "fr_3" && j.sent(Kind::Join)[0]->bridgeId == "brg_1", "A on a joinable friend did not send Join");
 j.select("request:rq_2");
 Check(j.sent(Kind::Accept).size() == 1 && j.sent(Kind::Accept)[0]->id == "rq_2", "A on a request did not send Accept");
 // The action is in flight: the bridge's actions wait, and its row says so.
 j.view().busy = true; j.h.Frame(0, 2);
 j.select("request:rq_1");
 j.select("friend:fr_6");
 Check(j.sent(Kind::Accept).size() == 1 && j.sent(Kind::Invite).empty(), "An action went out while another was in flight");
 j.select("friend:fr_3");
 Check(j.sent(Kind::Join).size() == 2, "Join waited for an action in flight");
 // It finishes with the state: the next action goes.
 j.view().busy = false; ++j.view().state.finished; j.h.Frame(0, 3);
 j.select("friend:fr_6");
 Check(j.sent(Kind::Invite).size() == 1 && j.sent(Kind::Invite)[0]->id == "fr_6", "A on an online friend did not send Invite");
 // A list the last sync failed on is stale: nothing goes on it.
 j.view().busy = false; ++j.view().state.finished; j.view().state.error = "bridge_unreachable"; j.h.Frame(0, 3);
 j.select("friend:fr_3");
 j.select("request:rq_1");
 Check(j.sent(Kind::Join).size() == 2 && j.sent(Kind::Accept).size() == 1, "An action went out on a list that is not current");
 // A friend who cannot be joined or invited opens More on A.
 j.view().state.error.clear(); j.view().canInvite = false; j.h.Frame(0, 3);
 j.select("friend:fr_5");
 Check(j.choosing() && j.sent(Kind::Invite).size() == 1, "A on a friend with nothing to do did not open More");
}

// Y opens More on a friend; its choices are listed with their reasons; Watch is sent; Remove asks again.
void MoreJourney() {
 Social j;
 j.open();
 j.focus("friend:fr_3");
 j.h.Press(MenuInput::Options);
 Check(j.choosing(), "Y did not open More on a friend");
 const auto* row = j.row("friend:fr_3");
 Check(row && row->popout && row->choices.size() == 5 && row->choices[0].id == "join" && row->choices[1].id == "watch" && row->choices[2].id == "invite" &&
  row->choices[3].id == "remove" && row->choices[4].id == "remove-block", "More on a friend is not Join, Watch, Invite, Remove, Remove and block");
 Check(row->choices[0].enabled && row->choices[1].enabled && row->choices[2].enabled && row->choices[3].enabled, "More on a friend fighting in a room cannot do each choice");
 j.pick(1);
 Check(!j.choosing() && j.sent(Kind::Watch).size() == 1 && j.sent(Kind::Watch)[0]->id == "fr_3", "Watch in More did not send Watch");
 // The friend in a room that cannot be joined: Join and Watch are listed with their reasons.
 j.focus("friend:fr_5");
 j.h.Press(MenuInput::Options);
 row = j.row("friend:fr_5");
 Check(j.choosing() && row && !row->choices[0].enabled && row->choices[0].detail == loc::T("friends.cannot_join") && !row->choices[1].enabled &&
  row->choices[1].detail == loc::T("friends.cannot_watch"), "More does not say why a room cannot be joined or watched");
 j.h.Press(MenuInput::Back);
 Check(!j.choosing() && j.sent(Kind::Join).empty(), "Back from More sent something");
 // Remove friend asks again; Back declines, Select goes ahead.
 j.focus("friend:fr_6");
 j.h.Press(MenuInput::Options);
 j.pick(3);
 Check(j.choosing() && j.sent(Kind::Remove).empty(), "Remove friend did not ask again");
 row = j.row("friend:fr_6");
 Check(row && row->choices.size() == 1 && row->choices[0].id == "yes" && row->choices[0].label == loc::T("friends.remove") && row->choices[0].detail == "Player 6", "The question does not name Remove friend and the player");
 j.h.Press(MenuInput::Back);
 Check(!j.choosing() && j.sent(Kind::Remove).empty(), "Back from the question removed the friend");
 j.focus("friend:fr_6");
 j.h.Press(MenuInput::Options);
 j.pick(4);
 j.pick(0);
 Check(!j.choosing() && j.sent(Kind::Remove).size() == 1 && j.sent(Kind::Remove)[0]->id == "fr_6" && j.sent(Kind::Remove)[0]->flag, "Remove and block did not send Remove with block");
 // Delete asks to remove the friend at once (a key the pad has no button for).
 j.view().busy = false; ++j.view().state.finished; j.h.Frame(0, 3);
 j.focus("friend:fr_8");
 j.h.Press(MenuInput::Leave);
 Check(j.choosing(), "Delete on a friend did not ask to remove them");
 j.pick(0);
 Check(j.sent(Kind::Remove).size() == 2 && j.sent(Kind::Remove)[1]->id == "fr_8" && !j.sent(Kind::Remove)[1]->flag, "Delete and yes did not send Remove");
}

// More on a request: Decline asks again; Decline and block blocks.
void RequestJourney() {
 Social j;
 j.open();
 j.focus("request:rq_1");
 j.h.Press(MenuInput::Options);
 const auto* row = j.row("request:rq_1");
 Check(j.choosing() && row && row->choices.size() == 3 && row->choices[0].id == "accept" && row->choices[1].id == "decline" && row->choices[2].id == "decline-block", "More on a request is not Accept, Decline, Decline and block");
 j.pick(1);
 Check(j.choosing() && j.sent(Kind::Decline).empty(), "Decline did not ask again");
 j.pick(0);
 Check(j.sent(Kind::Decline).size() == 1 && j.sent(Kind::Decline)[0]->id == "rq_1" && !j.sent(Kind::Decline)[0]->flag, "Decline and yes did not send Decline");
 j.view().busy = false; ++j.view().state.finished; j.h.Frame(0, 3);
 j.focus("request:rq_2");
 j.h.Press(MenuInput::Options);
 j.pick(2);
 j.pick(0);
 Check(j.sent(Kind::Decline).size() == 2 && j.sent(Kind::Decline)[1]->id == "rq_2" && j.sent(Kind::Decline)[1]->flag, "Decline and block did not send Decline with block");
}

// X opens Add a friend; the code is typed as it is, Send request waits for eight symbols and sends it; the page goes back
// once it went through, and stays with a refusal.
void AddJourney() {
 Social j;
 j.open();
 j.focus("friend:fr_6");
 j.h.Press(MenuInput::Fighter);
 Check(j.screen() == "friends-add", "X did not open Add a friend");
 Check(j.row("friends-code") && j.row("friends-code")->value == "4F2A-91CX", "Add a friend does not show the player's code");
 Check(j.row("friends-send") && !j.row("friends-send")->enabled, "Send request is open with no code");
 j.select("friends-their-code");
 j.type("4f2a91c");
 Check(j.row("friends-their-code")->value == "4f2a91c" && !j.row("friends-send")->enabled, "Send request is open with a code of seven symbols");
 // The page is left and opened again: no code is kept.
 j.h.Press(MenuInput::Back); j.h.shell.Navigation().Push("friends-add"); j.h.Frame(0, 3);
 Check(j.row("friends-their-code")->value.empty(), "Add a friend kept the last code");
 j.select("friends-their-code");
 j.type("4f2a-91cx");
 const auto typed = j.row("friends-their-code")->value;
 Check(typed == "4f2a-91cx" && j.row("friends-send")->enabled, "Send request is not open with a code of eight symbols");
 j.select("friends-send");
 const auto requests = j.sent(Kind::Request);
 Check(requests.size() == 1 && requests[0]->code == typed && requests[0]->card.empty() && requests[0]->bridgeId == "brg_1", "Send request did not send the code as typed");
 Check(j.screen() == "friends-add", "Add a friend went back before the request went through");
 // The bridge refused it: the page stays, so the code can be corrected.
 j.view().busy = true; j.h.Frame(0, 2);
 j.view().busy = false; ++j.view().state.finished; ++j.view().refusals; j.view().refusal = "friend_code_unknown"; j.view().refusalText = "No player has that friend code."; j.h.Frame(0, 3);
 Check(j.screen() == "friends-add", "Add a friend went back on a refused request");
 // Sent again, it goes through.
 j.view().refusal.clear(); j.h.Frame(0, 2);
 j.select("friends-send");
 Check(j.sent(Kind::Request).size() == 2, "Send request could not be sent again");
 j.view().busy = true; j.h.Frame(0, 2);
 j.view().busy = false; ++j.view().state.finished; j.h.Frame(0, 3);
 Check(j.screen() == "friends", "Add a friend did not go back to Friends once the request went through");
 // The next visit starts with no code.
 j.h.shell.Navigation().Push("friends-add"); j.h.Frame(0, 3);
 Check(j.row("friends-their-code")->value.empty(), "Add a friend kept the last code");
 // A new code asks first.
 j.select("friends-new-code");
 Check(j.h.shell.Navigation().Confirming() && j.sent(Kind::NewCode).empty(), "New code did not ask first");
}

// Appear offline is a switch Select flips; it sends the opposite of what the state says.
void AppearJourney() {
 Social j;
 j.open();
 j.select("friends-appear-offline");
 Check(j.sent(Kind::AppearOffline).size() == 1 && j.sent(Kind::AppearOffline)[0]->flag, "Appear offline did not send On from Off");
 auto state = FullState();
 state->appearOffline = true;
 j.view().busy = false;
 j.Give(state);
 j.h.Frame(0, 3);
 Check(j.row("friends-appear-offline")->value == loc::T("common.on"), "Appear offline does not read On");
 j.select("friends-appear-offline");
 Check(j.sent(Kind::AppearOffline).size() == 2 && !j.sent(Kind::AppearOffline)[1]->flag, "Appear offline did not send Off from On");
}

// Sent and Blocked list what they say; Select asks Cancel request and Unblock, and sends it.
void PagesJourney() {
 Social j;
 j.open();
 j.select("friends-sent");
 Check(j.screen() == "friends-sent" && j.row("sent:sq_1") && j.row("sent:sq_1")->label == "Player 14" && j.row("sent:sq_1")->value == loc::Tf("friends.sent_ago", loc::Tf("friends.days_ago", 2)), "Sent does not list the request and when it was sent");
 j.select("sent:sq_1");
 Check(j.choosing() && j.row("sent:sq_1")->choices.size() == 1 && j.row("sent:sq_1")->choices[0].label == loc::T("friends.cancel_request"), "Select on a sent request did not ask Cancel request");
 j.h.Press(MenuInput::Select);
 Check(j.sent(Kind::Cancel).size() == 1 && j.sent(Kind::Cancel)[0]->id == "sq_1", "Cancel request did not send Cancel");
 j.h.Press(MenuInput::Back);
 Check(j.screen() == "friends", "Back from Sent did not return to Friends");
 j.view().busy = false; ++j.view().state.finished; j.h.Frame(0, 3);
 j.select("friends-blocked");
 Check(j.screen() == "friends-blocked" && j.row("blocked:bl_1") && j.row("blocked:bl_1")->label == "Player 16", "Blocked does not list the player");
 j.select("blocked:bl_1");
 j.h.Press(MenuInput::Select);
 Check(j.sent(Kind::Unblock).size() == 1 && j.sent(Kind::Unblock)[0]->id == "bl_1", "Unblock did not send Unblock");
}

// Each state without a list is one card: no account (Connect Discord, focused), no friends on this service, loading, unreachable.
void StatesJourney() {
 {
  auto none = std::make_shared<nf::State>();
  none->kind = nf::State::Kind::NoAccount;
  Social j(none);
  j.open();
  Check(Ids(j) == std::vector<std::string>({"discord-connect"}) && j.h.shell.Navigation().Focus() == "discord-connect", "Without a Discord account the tab is not Connect Discord, focused");
  j.h.Press(MenuInput::Select);
  Check(j.screen() == "discord-connect", "Connect Discord did not open from Friends");
 }
 {
  Social j(std::make_shared<nf::State>());
  j.open();
  Check(Ids(j) == std::vector<std::string>({"friends-note"}) && j.rows[0].label == loc::T("friends.unavailable"), "A service without friends is not one card");
 }
 {
  Social j(std::make_shared<nf::State>());
  j.Give(std::make_shared<nf::State>(), false);
  j.view().state.loading = true; j.h.Frame(0, 2);
  j.open();
  Check(Ids(j) == std::vector<std::string>({"friends-add", "friends-note"}) && !j.rows[0].enabled && j.rows[1].label == loc::T("friends.loading"), "A first read in flight is not Add a friend and a line");
  j.view().state.loading = false; j.view().state.error = "bridge_unreachable"; j.h.Frame(0, 3);
  Check(Ids(j) == std::vector<std::string>({"friends-note"}) && j.rows[0].label == loc::T("friends.unreachable"), "A first read that failed is not one card");
 }
 {
  auto empty = std::make_shared<nf::State>();
  empty->kind = nf::State::Kind::Listed; empty->code = "4F2A-91CX";
  Social j(empty);
  j.open();
  Check(Ids(j) == std::vector<std::string>({"friends-add", "friends-note", "friends-appear-offline"}) && j.rows[1].label == loc::T("friends.empty") && j.h.shell.Navigation().Focus() == "friends-add", "An empty list is not Add a friend, focused, and a line");
 }
}

// A Join that asks first (the player is in a room or a game) opens its question over whatever shows: Stay answers no,
// leaving and joining answers yes after asking; the question closes with the runtime's.
void VisitQuestionJourney() {
 Social j;
 j.open();
 auto& visit = j.view().visit;
 visit.active = visit.confirm = true; visit.revision = 7; visit.name = "Player 3";
 j.h.Frame(0, 3);
 Check(j.screen() == "friends-visit" && j.row("friends-visit-stay") && j.row("friends-visit-go") && j.row("friends-visit-go")->label == loc::Tf("friends.visit_join", "Player 3"), "A visit that asks first did not open its question");
 j.select("friends-visit-stay");
 Check(j.sent(Kind::AnswerVisit).size() == 1 && !j.sent(Kind::AnswerVisit)[0]->flag && j.sent(Kind::AnswerVisit)[0]->revision == 7, "Stay here did not answer no for the revision shown");
 visit.confirm = visit.active = false; j.h.Frame(0, 3);
 Check(j.screen() == "friends", "The question stayed open after the runtime ended it");
 // A watch names the friend, and Leave and watch asks again before it says yes.
 visit.active = visit.confirm = visit.watch = true; visit.revision = 8; j.h.Frame(0, 3);
 Check(j.screen() == "friends-visit" && j.row("friends-visit-go")->label == loc::Tf("friends.visit_watch", "Player 3"), "A watch does not say it watches");
 j.select("friends-visit-go");
 Check(j.h.shell.Navigation().Confirming() && j.sent(Kind::AnswerVisit).size() == 1, "Leaving for a friend's room did not ask again");
 j.h.Press(MenuInput::Right); j.h.Press(MenuInput::Select);
 Check(j.sent(Kind::AnswerVisit).size() == 2 && j.sent(Kind::AnswerVisit)[1]->flag && j.sent(Kind::AnswerVisit)[1]->revision == 8, "Leave and go did not answer yes for the revision shown");
}

// Settings, Online, Friends: Appear offline flips, the friend code asks before a new one and Turn off friends asks before it
// deletes; the tab's cadence runs while the panel shows, and without an account or a service the rows say why they are off.
void SettingsJourney() {
 Social j;
 j.open("tab-online/friends");
 Check(Ids(j) == std::vector<std::string>({"friends-appear-offline", "friends-new-code", "friends-turn-off"}), "The Friends panel is not Appear offline, the friend code and Turn off friends");
 Check(j.sent(Kind::Visit).size() == 1, "The Friends panel did not keep the friends fresh");
 Check(j.row("friends-new-code")->value == "4F2A-91CX" && j.row("friends-appear-offline")->value == loc::T("common.off") &&
  j.row("friends-turn-off")->detail == loc::T("friends.turn_off_detail"), "The Friends panel does not show the code, the switch and what Turn off deletes");
 j.select("friends-appear-offline");
 Check(j.sent(Kind::AppearOffline).size() == 1 && j.sent(Kind::AppearOffline)[0]->flag, "Appear offline did not send on");
 ++j.view().state.finished; j.h.Frame(0, 3);
 j.select("friends-new-code");
 Check(j.h.shell.Navigation().Confirming() && j.sent(Kind::NewCode).empty(), "A new code was made without asking");
 j.h.Press(MenuInput::Right); j.h.Press(MenuInput::Select);
 Check(j.sent(Kind::NewCode).size() == 1, "Confirming did not ask for a new code");
 // The bridge answers: the next action may go.
 ++j.view().state.finished; j.h.Frame(0, 3);
 j.select("friends-turn-off");
 Check(j.h.shell.Navigation().Confirming() && j.sent(Kind::Forget).empty(), "Friends were turned off without asking");
 j.h.Press(MenuInput::Right); j.h.Press(MenuInput::Select);
 Check(j.sent(Kind::Forget).size() == 1, "Confirming did not turn friends off");
 // No Discord account: every row is off and says so.
 auto none = std::make_shared<nf::State>();
 none->kind = nf::State::Kind::NoAccount;
 j.Give(none);
 Check(!j.row("friends-appear-offline")->enabled && !j.row("friends-new-code")->enabled && !j.row("friends-turn-off")->enabled &&
  j.row("friends-turn-off")->detail == loc::T("friends.no_account"), "Without a Discord account the Friends panel rows are not off with its words");
 // A service without friends says that.
 j.Give(std::make_shared<nf::State>());
 Check(!j.row("friends-turn-off")->enabled && j.row("friends-turn-off")->detail == loc::T("friends.unavailable"), "A service without friends did not say so");
}
}

int main() {
 try {
  VisitJourney(); RowsJourney(); ActionsJourney(); MoreJourney(); RequestJourney(); AddJourney(); AppearJourney(); PagesJourney(); StatesJourney(); VisitQuestionJourney(); SettingsJourney();
  std::cout << "Friends journeys passed.\n";
  return 0;
 } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
