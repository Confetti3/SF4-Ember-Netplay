// Watching a tournament match, as the player sees it: a match link for a match
// that is not theirs offers Watch, the matches screen says how the watch is
// going and can stop it, a watcher in the match's room sees no seat chooser,
// no chat box and no host powers, and nobody offers the match room's invitation.
#include "identity_journey_support.hxx"
#include "../ui/ChatTranscript.hxx"
#include "../ui/MenuProbes.hxx"
#include "../ui/RoomControls.hxx"
#include "../ui/RoomFeedback.hxx"
#include <map>
namespace {
using namespace sf4e;
using netplay::tournament::Command;
using netplay::tournament::WatchNotice;
using netplay::tournament::WatchPhase;

std::string Endpoint(char digit) { return std::string(64, digit); }

// A player with an Ember ID and one trusted service, on the matches screen.
struct Watching : Journey {
 ui::Tone tone = ui::Tone::Success;
 Watching() {
  SetMenuStatusProbe([this](const char* text, ui::Tone shown) { status = text; tone = shown; });
  view.identity.known = true;
  ready();
  view.identity.bridges = {{"brg_1", "https://bridge.example", "Example"}};
  Screen("tournament-matches");
  for (int i = 0; i < 4 && !sent().empty(); ++i) answer();
  view.tournament.list.bridge = "brg_1";
  netplay::tournament::Assignment mine;
  mine.matchId = "emt_1"; mine.state = "ready"; mine.profile = "ember-room-v1"; mine.slot = 0; mine.gamesToWin = 2;
  view.tournament.list.items = {mine};
  Frame();
 }
 // A link to `match` on `bridge`, and the list refresh it asks for answered without it.
 void OpenLink(const char* bridge, const char* match) {
  auto& t = view.tournament;
  t.link.bridge = bridge; t.link.match = match; ++t.link.sequence;
  Frame(0, 3);
 }
 void RefreshWithout() {
  ++view.tournament.list.finished;
  Frame(0, 3);
 }
};

// A match link for a match the player is not in offers Watch on its row, focused;
// a match they are in offers Play; Watch goes out naming the service and match.
void WatchRow() {
 Watching h;
 h.OpenLink("brg_1", "emt_1");
 h.RefreshWithout();
 Check(h.row("tm-play:emt_1") && !h.has("tm-watch:emt_1"), "A match the player plays offered Watch");
 h.OpenLink("brg_1", "emt_9");
 Check(h.played().back()->op == Command::Op::Refresh && h.played().back()->bridgeId == "brg_1", "A link did not refresh the matches");
 for (int i = 0; i < 4; ++i) h.answer();
 h.RefreshWithout();
 Check(h.status == loc::T("tournament.failure.not_yours"), "A match that is not the player's was not said");
 Check(h.tone == ui::Tone::Neutral, "A match offered for watching was reported as an error");
 const auto* watch = h.row("tm-watch:emt_9");
 Check(watch && watch->enabled && watch->label == loc::T("tournament.watch.row") && watch->hint == loc::T("room.choice.watch"),
  "A match that is not the player's has no Watch row");
 Check(watch->detail == loc::T("tournament.watch.row_detail"), "The Watch row does not say what it does");
 Check(h.shell.Navigation().Focus() == "tm-watch:emt_9", "The match a link opened is not selected");
 // The rows that were there keep their order: the player's matches, then the Watch row, then Refresh.
 Check(h.index("tm-play:emt_1") < h.index("tm-watch:emt_9") && h.index("tm-watch:emt_9") < h.index("tm-refresh"), "The Watch row is out of place");
 const auto before = h.played().size();
 h.Choose("tm-watch:emt_9");
 Check(h.played().size() == before + 1 && h.played().back()->op == Command::Op::Watch && h.played().back()->bridgeId == "brg_1" &&
  h.played().back()->matchId == "emt_9", "Watch did not name its service and match");
 // Nothing plays or watches a second match while one is watched, and the row says so.
 h.view.tournament.watch.phase = WatchPhase::Asking; h.view.tournament.watch.matchId = "emt_9"; h.view.tournament.watch.bridgeId = "brg_1";
 h.Frame();
 Check(!h.row("tm-watch:emt_9")->enabled && !h.row("tm-play:emt_1")->enabled, "A match can be played or watched while one is watched");
}

// Watch looks at the player again when pressed: a row older than the screen, for a
// player with no ID, in a room, already watching or playing, or whose service is
// gone, sends nothing; a match that can be watched sends Watch.
void WatchRechecks() {
 IdentityPanel panel;
 ShellView v;
 MenuNavigation nav;
 std::vector<ShellAction> sent;
 double now = 0;
 const auto submit = [&](ShellAction a) { sent.push_back(std::move(a)); return true; };
 v.identity.known = true; v.identity.state = "ready";
 v.identity.bridges = {{"brg_1", "https://bridge.example", "Example"}};
 panel.OpenMatch("brg_1", "emt_9");
 v.tournament.list.bridge = "brg_1";
 // The list is asked for, and comes back without the match.
 const auto asked = [&] {
  return std::any_of(sent.begin(), sent.end(), [](const ShellAction& s) { return s.tournament.op == Command::Op::Refresh; });
 };
 for (int i = 0; i < 6 && !asked(); ++i) panel.Update(v, "tournament-matches", submit, now += 1);
 Check(asked(), "The match link did not ask for the list");
 ++v.tournament.list.finished;
 for (int i = 0; i < 3; ++i) panel.Update(v, "tournament-matches", submit, now += 1);
 const auto press = [&](const char* id) {
  MenuAction a; a.kind = MenuAction::Activate; a.id = id;
  panel.Activate(a, v, nav);
  sent.clear();
  panel.Update(v, "tournament-matches", submit, now += 1);
  return std::any_of(sent.begin(), sent.end(), [](const ShellAction& s) { return s.tournament.op == Command::Op::Watch; });
 };
 Check(!press("tm-watch:emt_1"), "Watch went out for a match the player was not offered");
 v.identity.state = "locked"; Check(!press("tm-watch:emt_9"), "Watch went out with no Ember ID ready");
 v.identity.state = "ready";
 v.session.room = netplay::RoomState::Joined; Check(!press("tm-watch:emt_9"), "Watch went out while the player is in a room");
 v.session.room = netplay::RoomState::Idle;
 v.tournament.phase = netplay::tournament::Phase::InRoom; Check(!press("tm-watch:emt_9"), "Watch went out while a match is played");
 v.tournament.phase = netplay::tournament::Phase::Idle;
 v.tournament.watch.phase = WatchPhase::Watching; Check(!press("tm-watch:emt_9"), "Watch went out while a match is watched");
 v.tournament.watch.phase = WatchPhase::Idle;
 v.identity.bridges.clear(); Check(!press("tm-watch:emt_9"), "Watch went out for a service the player no longer trusts");
 v.identity.bridges = {{"brg_1", "https://bridge.example", "Example"}};
 Check(press("tm-watch:emt_9"), "Watch did not go out for a match that can be watched");
 const auto went = std::find_if(sent.begin(), sent.end(), [](const ShellAction& s) { return s.tournament.op == Command::Op::Watch; });
 Check(went != sent.end() && went->tournament.matchId == "emt_9" && went->tournament.bridgeId == "brg_1", "Watch did not name its service and match");
}

// A match link to a service that is not trusted shows the Watch row with why it cannot be used.
void UntrustedWatch() {
 Watching h;
 h.OpenLink("brg_9", "emt_9");
 for (int i = 0; i < 8; ++i) h.answer();
 const auto* watch = h.row("tm-watch:emt_9");
 Check(h.status == loc::T("tournament.failure.link_service"), "A link to an untrusted service did not say so");
 Check(watch && watch->info && watch->detail == loc::T("tournament.watch.untrusted_bridge"),
  "A match on an untrusted service does not say why it cannot be watched");
}

// Without an Ember ID a match link asks for one: the Ember ID screen's Create
// says watching needs it, and an ordinary visit to it does not.
void WatchNeedsAnId() {
 {
  Journey h;
  h.view.identity.known = true; h.view.identity.state = "disabled";
  h.Screen("identity");
  Check(h.row("id-enable") && h.row("id-enable")->detail == loc::T("identity.enable_detail"), "Create changed its words for a player with no link");
 }
 Journey h;
 h.view.identity.known = true; h.view.identity.state = "disabled";
 h.view.identity.bridges = {{"brg_1", "https://bridge.example", "Example"}};
 h.Screen("home");
 h.view.tournament.link.bridge = "brg_1"; h.view.tournament.link.match = "emt_9"; h.view.tournament.link.sequence = 1;
 h.Frame(0, 3);
 Check(h.shell.Navigation().Screen() == "tournament-matches", "A match link did not open the matches screen");
 Check(h.row("identity") && h.row("identity")->detail == loc::T("tournament.watch.needs_id"), "The matches screen does not send a player with no ID to make one");
 h.Choose("identity");
 Check(h.shell.Navigation().Screen() == "identity" && h.row("id-enable") && h.row("id-enable")->detail == loc::T("tournament.watch.needs_id"),
  "Create does not say watching needs an Ember ID");
}

// What watching is doing, in the matches screen's words, and Stop.
void WatchStates() {
 Watching h;
 auto& t = h.view.tournament;
 t.watch.bridgeId = "brg_1"; t.watch.matchId = "emt_9";
 // The two players, as the room names them once it is joined.
 h.view.room.roomEpoch = 3; h.view.room.localMember = 1;
 for (int i = 0; i < 4; ++i) h.view.room.tables[i].id = static_cast<std::uint8_t>(i);
 room::Member watcher; watcher.id = 1; watcher.name = "Watcher 1";
 room::Member first; first.id = 2; first.name = "Player 1";
 room::Member second; second.id = 3; second.name = "Player 2";
 h.view.room.members = {watcher, first, second};
 h.view.room.tables[0].p1 = 2; h.view.room.tables[0].p2 = 3;
 struct Case { WatchPhase phase; bool full; const char* reason; std::string line; bool stop; };
 const Case cases[] = {
  {WatchPhase::Asking, false, "", loc::T("tournament.watch.state.asking"), true},
  {WatchPhase::Waiting, false, "", loc::T("tournament.watch.state.waiting_room"), true},
  {WatchPhase::Waiting, true, "", loc::T("tournament.watch.state.full"), true},
  {WatchPhase::Joining, false, "", loc::T("tournament.watch.state.joining"), true},
  {WatchPhase::Watching, false, "", loc::Tf("tournament.watch.state.watching", "Player 1", "Player 2"), true},
  {WatchPhase::Finished, false, "finished", loc::T("tournament.finished_detail"), false},
  {WatchPhase::Refused, false, "not_allowed", loc::T("tournament.watch.refused.not_allowed"), false},
  {WatchPhase::Refused, false, "revoked", loc::T("tournament.watch.refused.revoked"), false},
  {WatchPhase::Refused, false, "fighter", loc::T("tournament.watch.refused.fighter"), false},
  {WatchPhase::Refused, false, "incompatible_build", loc::T("tournament.watch.refused.build"), false},
  {WatchPhase::Refused, false, "watch_unsupported", loc::T("tournament.watch.refused.unsupported"), false},
  {WatchPhase::Refused, false, "not_found", loc::T("tournament.failure.not_found"), false},
  {WatchPhase::Refused, false, "helper_lost", loc::T("tournament.failure.helper"), false},
 };
 for (const auto& expected : cases) {
  t.watch.phase = expected.phase; t.watch.full = expected.full; t.watch.reason = expected.reason;
  h.Frame();
  const auto* row = h.row("tm-watching");
  Check(row && row->value == expected.line, "The watch is not shown in its words");
  Check(row->label == loc::T("tournament.current") && row->detail == loc::T("tournament.watch.row_detail"), "The watch row has no label or detail");
  Check(h.has("tm-stop") == expected.stop, "Stop watching was offered at the wrong time, or not at all");
  if (expected.stop) Check(h.row("tm-stop")->label == loc::T("room.stop_watching") && h.row("tm-stop")->detail == loc::T("tournament.watch.stop_detail"),
   "Stop watching does not say what it does");
 }
 t.watch.phase = WatchPhase::Watching;
 h.Frame();
 const auto before = h.played().size();
 h.Choose("tm-stop"); h.Press(MenuInput::Right); h.Press(MenuInput::Select);
 Check(h.played().size() > before && h.played().back()->op == Command::Op::Stop, "Stop watching did not stop");
 // A watch that never began shows nothing.
 t.watch = {};
 h.Frame();
 Check(!h.has("tm-watching") && !h.has("tm-stop"), "A watch that never began is shown");
}

// How a joined room ended for a watcher is said once, wherever the player is.
void WatchNotices() {
 Journey h;
 h.view.identity.known = true; h.ready();
 h.Screen("home");
 h.Frame(0, 2);
 auto& watch = h.view.tournament.watch;
 const struct { WatchNotice notice; const char* key; } cases[] = {
  {WatchNotice::Finished, "tournament.watch.finished_notice"},
  {WatchNotice::Removed, "tournament.watch.removed_notice"},
  {WatchNotice::RoomClosed, "tournament.watch.room_closed_notice"},
 };
 std::uint64_t sequence = 0;
 for (const auto& expected : cases) {
  watch.notice = expected.notice; watch.noticeSequence = ++sequence;
  h.Frame(0, 2);
  Check(h.status == loc::T(expected.key), "A watcher's end was not said");
  // Not again on a later frame, once it has lapsed.
  h.Frame(0, 3, 30.f);
  Check(h.status != loc::T(expected.key), "A watcher's end was said again");
 }
 // A frame with the same count says nothing.
 h.Frame(0, 2);
 Check(h.status != loc::T("tournament.watch.room_closed_notice"), "A watcher's end was repeated for the same count");
}

// A room bound to a tournament match with a watcher in it.
struct Bound : Journey {
 Bound(bool localIsWatcher) {
  view.session.generation.room = 1; view.session.room = netplay::RoomState::Joined; view.session.control = netplay::Health::Healthy;
  view.room.roomEpoch = 10; view.room.localMember = localIsWatcher ? 3 : 1; view.room.host = 1; view.room.capacity = 8;
  view.room.name = "Match"; view.room.revision = 3;
  for (int i = 0; i < 4; ++i) { view.room.tables[i].id = static_cast<std::uint8_t>(i); view.room.tables[i].revision = 7; }
  const auto member = [&](room::MemberId id, const char* name, char digit) {
   room::Member m; m.id = id; m.name = name; m.connection = {"room", Endpoint(digit)};
   return m;
  };
  auto first = member(1, "Player 1", 'a'); first.table = 0; first.seat = 0; first.host = true; first.status = room::MemberStatus::Seated;
  auto second = member(2, "Player 2", 'b'); second.table = 0; second.seat = 1; second.status = room::MemberStatus::Seated;
  auto watcher = member(3, "Watcher 1", '1'); watcher.table = 0; watcher.status = room::MemberStatus::WatchingNext;
  view.room.members = {first, second, watcher};
  view.room.tables[0].p1 = 1; view.room.tables[0].p2 = 2; view.room.tables[0].spectators = {3};
  auto& match = view.room.tournament;
  match.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12"; match.assignmentGeneration = 1; match.bindingRevision = 1; match.gamesToWin = 2;
  match.fighters[0] = {Endpoint('a'), "emb1_player1"}; match.fighters[1] = {Endpoint('b'), "emb1_player2"};
  match.watchers = {{Endpoint('1'), "emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja"}};
 }
};

// A watcher sees its own status line, the fighters' seat chooser is gone for
// everyone, the watcher is tagged by name with its Ember ID, and it cannot run
// the room even holding the host field.
void WatcherRoom() {
 {
  Bound h(true);
  // The board's member cards, as drawn.
  std::map<std::string, std::string> cards;
  SetMenuCardTextProbe([&](const std::string& id, const std::string& text) { cards[id] = text; });
  h.Screen("room");
  h.Frame(0, 2);
  Check(h.status == loc::T("room.tournament_watching"), "A watcher's room does not say it is watching");
  Check(h.row("table-0") && h.row("table-0")->choices.empty(), "A bound room offers a watcher the seat chooser");
  Check(h.row("member-3") && h.row("member-3")->detail == loc::Tf("room.member_status", std::string(loc::T("room.member.watcher")) + " / j25zrhe6-pmdhvlja"),
   "A watcher is not tagged with its Ember ID");
  Check(h.row("member-1")->detail == loc::Tf("room.member_status_host", loc::T("room.status.seated")), "A fighter's row changed");
  const auto watcherLine = loc::Tf("room.member_status", std::string(loc::T("room.member.watcher")) + " / j25zrhe6-pmdhvlja");
  Check(cards["member-3"] == watcherLine, "A watcher's card is not tagged with its Ember ID");
  Check(cards["member-1"] == loc::Tf("room.board_status_host_slot", loc::T("room.status.seated"), 1), "A fighter's card changed");
  Check(!h.has("copy") && !h.has("copy-short"), "A bound room offers its invitation");
  // An identifier with no key shape is shown whole.
  Check(ShortEmberId("emb1_watcher1") == "emb1_watcher1" && ShortEmberId("emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja") == "j25zrhe6-pmdhvlja",
   "An Ember ID is not shortened as its fingerprint is");
  // The chat is for reading: no sending, whatever the room's state.
  Check(ChatGateOf(h.view) == ChatGate::ReadOnly && !ChatTypable(h.view), "A watcher can type in the chat");
  Check(!h.has("inline-chat") && !ImGui::GetIO().WantTextInput, "A tournament watcher has an inline editor");
  h.Choose("room-chat");
  Check(h.shell.Navigation().Screen() == "room-chat" && h.row("compose") && !h.row("compose")->enabled, "A watcher's chat offers a message to send");
  // Holding the host field with no fighter left, it has none of the host's rows.
  h.Screen("room");
  h.view.room.host = 3; h.view.room.members[0].host = false; h.view.room.members[2].host = true;
  cards.clear();
  h.Frame(0, 2);
  Check(cards["member-3"] == watcherLine, "A watcher holding the host field shows as HOST on its card");
  SetMenuCardTextProbe({});
  h.Choose("settings");
  Check(h.row("room-admin") && !h.row("room-admin")->enabled, "A watcher holding the host field can change the room through Settings");
  // Nor is it told it hosts when it leaves.
  Check(room_controls::LeaveRoomDetail(h.view).find(loc::T("room.leave.host")) == std::string::npos, "A watcher holding the host field is warned it hosts the room");
 }
 {
  // The chat does not announce a watcher that is left holding the host field.
  Bound h(true);
  ui::ChatTranscript transcript;
  std::vector<ui::RoomNotice> notices;
  transcript.Update(h.view.room, notices);
  const auto lines = transcript.Lines().size();
  h.view.room.host = 3; h.view.room.members[0].host = false; h.view.room.members[2].host = true;
  transcript.Update(h.view.room, notices);
  Check(transcript.Lines().size() == lines, "A watcher was announced as the new host");
 }
 {
  Bound h(false);
  h.Screen("room");
  h.Frame(0, 2);
  Check(h.status == loc::Tf("room.tournament_status_watched", 2, 1), "A fighter's room does not count its watchers");
  Check(h.row("table-0") && std::none_of(h.row("table-0")->choices.begin(),h.row("table-0")->choices.end(),[](const MenuChoice& c){return room_controls::FindSeatOption(c.id)!=nullptr;}), "A fighter is offered the seat chooser");
  Check(ChatGateOf(h.view) == ChatGate::Writable && ChatTypable(h.view), "A fighter cannot type in the chat");
  h.Choose("room-members");
  h.Choose("member-3");
  Check(h.row("transfer-host") && !h.row("transfer-host")->enabled, "A watcher can be handed the host");
  h.Press(MenuInput::Back);
  h.Choose("member-2");
  Check(h.row("transfer-host") && h.row("transfer-host")->enabled, "A fighter cannot be handed the host");
  // No watchers left: the fighters' line is as it was.
  h.Screen("room");
  h.view.room.tournament.watchers.clear();
  h.view.room.members.pop_back();
  h.view.room.tables[0].spectators.clear();
  h.Frame(0, 2);
  Check(h.status == loc::Tf("room.tournament_status", 2), "A tournament room without watchers changed its line");
 }
}

// The match room's invitation is never offered from the first claim, before
// the room's binding lands, and a casual room keeps its rows.
void NoInvitationForAMatchRoom() {
 Journey h;
 h.view.session.generation.room = 1; h.view.session.room = netplay::RoomState::Joined; h.view.session.control = netplay::Health::Healthy;
 h.view.room.roomEpoch = 10; h.view.room.localMember = 1; h.view.room.host = 1; h.view.room.capacity = 8; h.view.room.name = "Room";
 for (int i = 0; i < 4; ++i) { h.view.room.tables[i].id = static_cast<std::uint8_t>(i); h.view.room.tables[i].revision = 7; }
 room::Member local; local.id = 1; local.name = "Player 1"; local.host = true;
 h.view.room.members = {local};
 h.view.invitation = "sf4e3:invitation";
 h.Screen("room");
 h.Frame(0, 2);
 h.Choose("room-invite");
 Check(h.has("copy") && h.has("copy-short"), "A casual room lost its invitation rows");
 for (const auto phase : {netplay::tournament::Phase::Claiming, netplay::tournament::Phase::Opening, netplay::tournament::Phase::InRoom}) {
  h.view.tournament.phase = phase;
  h.Frame(0, 2);
  Check(!h.has("copy") && !h.has("copy-short"), "A match room offers its invitation before it is bound");
 }
 h.view.tournament.phase = netplay::tournament::Phase::Finished;
 h.Frame(0, 2);
 Check(h.has("copy"), "A finished match hides a casual room's invitation");
 h.view.tournament.watch.phase = WatchPhase::Watching;
 h.Frame(0, 2);
 Check(!h.has("copy") && !h.has("copy-short"), "A watched room offers its invitation");
}
}

int main() {
 try {
  WatchRow();
  WatchRechecks();
  UntrustedWatch();
  WatchNeedsAnId();
  WatchStates();
  WatchNotices();
  WatcherRoom();
  NoInvitationForAMatchRoom();
  std::cout << "watch journey tests passed\n";
  return 0;
 } catch (const std::exception& error) {
  std::cerr << "FAIL: " << error.what() << "\n";
  return 1;
 }
}
