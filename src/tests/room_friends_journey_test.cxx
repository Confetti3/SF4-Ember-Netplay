// Friends from a public room and from a toast (docs/design/PROFILE.md, "Another player's card" and "Toasts"): a member's
// popout offers Player card and Add friend by their relation, the page offers the relation row, Block and the rest, Add
// friend sends the card token with the member's room name, a request waiting is Accept, a member with no card has
// neither and a private room offers none; a request or an invite that comes is a toast whose Accept or Join sends its
// commands without opening a screen.
#include "public_rooms_journey_support.hxx"
#include "../ui/FriendsPanel.hxx"
#include "../ui/PostMatchFriend.hxx"
#include "../ui/RoomInvite.hxx"
#include <map>
#include <memory>
namespace {
namespace nf = sf4e::netplay::friends;
using sf4e::netplay::levels::Relation;
constexpr std::uint64_t Now = 1800000000, Hour = 3600, Day = 24 * Hour;

// A public room of three: this player (1), Player 2 with a card the bridge looked up, and Player 3 with none; and the
// friends the runtime has answered.
struct RoomJourney : Journey {
 explicit RoomJourney(Relation relation = Relation::None, bool listed = true, bool read = true) {
  h.Screen("home"); h.Frame(0, 2); answerIdentityUntilIdle();
  h.view.unixNow = Now;
  JoinedRoom(h, 10);
  h.view.session.generation.room = 1; h.view.session.control = netplay::Health::Healthy;
  auto& s = h.view.room;
  s.host = 1; s.capacity = 8; s.name = "Player 5's room"; s.serverOwned = listed;
  for (room::MemberId id : {1, 2, 3}) {
   room::Member m; m.id = id; m.name = "Player " + std::to_string(id); m.mainFighter = 4;
   if (id == 2) m.card = CardToken::Parsed("c1.player_2");
   s.members.push_back(m);
  }
  auto cards = std::make_shared<netplay::levels::CardMap>();
  netplay::levels::Card card; card.relation = relation; card.account = {120, 0}; card.fighters = {{4, 200, 1}};
  (*cards)["c1.player_2"] = card;
  h.view.memberCards = cards;
  auto state = std::make_shared<nf::State>();
  state->kind = nf::State::Kind::Listed; state->code = "4F2A-91CX";
  if (read) Give(std::move(state));
 }
 nf::View& view() { return h.view.friends; }
 void Give(std::shared_ptr<nf::State> state) {
  auto& f = view();
  f.bridge = "brg_1"; f.state.value = std::move(state); f.state.loaded = true; ++f.state.revision; ++f.state.finished; f.canInvite = true;
  h.Frame(0, 2);
 }
 template <class Command> std::vector<const Command*> sent() const {
  std::vector<const Command*> out;
  for (const auto& a : h.actions) if (const auto* f = As<Command>(a)) out.push_back(f);
  return out;
 }
 std::string screen() { return h.shell.Navigation().Screen(); }
 // Player 2's popout open on the Members list.
 const MenuEntry* popout(const char* member = "member-2") { h.Screen("room-members"); h.Choose(member); return row(member); }
 // The member's page, which Player card no longer opens (it opens their profile): Kick asks on it first, and Back puts that question away.
 void page(const char* member = "member-2") {
  const auto* entry = popout(member);
  std::size_t kick = 0;
  while (entry && kick < entry->choices.size() && entry->choices[kick].id != "kick") ++kick;
  for (std::size_t i = 0; i < kick; ++i) h.Press(MenuInput::Down);
  h.Press(MenuInput::Select); h.Frame(0, 2);
  h.Press(MenuInput::Back);
 }
 static bool offers(const MenuEntry* entry, const char* id) {
  return entry && std::any_of(entry->choices.begin(), entry->choices.end(), [&](const MenuChoice& c) { return c.id == id; });
 }
};

std::vector<std::string> Choices(const MenuEntry* entry) {
 std::vector<std::string> ids;
 if (entry) for (const auto& c : entry->choices) ids.push_back(c.id);
 return ids;
}

// The popout: Player card first, then Add friend or the relation it is in, then the rest; a member with no card has no
// friend choice and a private room's members have none.
// The words of the toasts said so far, newest first, as Notifications lists them (it takes the player off the screen they were on).
std::string Said(RoomJourney& j) {
 j.h.Screen("notifications"); j.h.Frame(0, 2);
 std::string all;
 for (const auto& r : j.rows) if (r.id.compare(0, 6, "toast-") == 0) all += r.detail + "|";
 return all;
}

void PopoutJourney() {
 {
  RoomJourney j;
  const auto* member = j.popout();
  Check(Choices(member) == std::vector<std::string>({"player-card", "friend-request", "mute", "steam-profile", "kick", "transfer-host"}), "A member's popout is not Player card, Add friend, Mute, Steam profile, Kick and Make host");
  Check(member->choices[0].label == loc::T("room.member_card") && member->choices[1].label == loc::T("friends.add_member") && member->choices[1].enabled, "Add friend is not offered to a member who is no friend yet");
  Check(Choices(j.popout("member-3")) == std::vector<std::string>({"player-card", "mute", "steam-profile", "kick", "transfer-host"}), "A member with no card is offered Add friend");
 }
 const struct { Relation relation; const char* label; bool enabled; } states[] = {
  {Relation::Sent, "friends.request_sent", false}, {Relation::Received, "friends.accept_member", true},
  {Relation::Friend, "friends.are_friends", false}, {Relation::Blocked, "friends.blocked_member", false}};
 for (const auto& state : states) {
  RoomJourney j(state.relation);
  const auto* member = j.popout();
  Check(j.offers(member, "friend-request") && member->choices[1].label == loc::T(state.label) && member->choices[1].enabled == state.enabled, "The popout's friend choice does not say the member's relation");
 }
 RoomJourney priv(Relation::None, false);
 Check(!RoomJourney::offers(priv.popout(), "friend-request"), "A private room offered Add friend");
}

// The member's page (Kick and Make host ask on it): the relation row, Mute, Steam profile, Kick, Make host, Block. Each relation says its state;
// Add friend and Accept request send the same request; the other states cannot be pressed.
void PageJourney() {
 RoomJourney j;
 j.page();
 Check(j.screen() == "room-member", "Kick did not open the member's page");
 const std::vector<std::string> ids = {"friend-request", "mute", "steam-profile", "kick", "transfer-host", "friend-block"};
 std::vector<std::string> shown;
 for (const auto& r : j.rows) shown.push_back(r.id);
 Check(shown == ids, "The member's page is not the relation row, Mute, Steam profile, Kick, Make host and Block");
 j.h.Choose("friend-request");
 const auto requests = j.sent<commands::FriendRequest>();
 Check(requests.size() == 1 && requests[0]->bridge == "brg_1" && requests[0]->card == "c1.player_2" && requests[0]->label == "Player 2", "Add friend did not send a request with the card token and the room name");
 Check(j.screen() == "room-member", "Add friend left the page");
 // What went through is said once.
 auto& f = j.view();
 ++f.state.finished; j.h.Frame(0, 3);
 Check(Said(j) == std::string(loc::Tf("friends.request_sent_to", "Player 2")) + "|", "A request that went through did not say who it was sent to");
}

void RelationRowsJourney() {
 const struct { Relation relation; const char* label; const char* detail; bool enabled; } states[] = {
  {Relation::None, "friends.add_member", nullptr, true}, {Relation::Sent, "friends.request_sent", nullptr, false},
  {Relation::Received, "friends.accept_member", nullptr, true}, {Relation::Friend, "friends.are_friends", nullptr, false},
  {Relation::Blocked, "friends.blocked_member", "friends.blocked_member_detail", false}};
 for (const auto& state : states) {
  RoomJourney j(state.relation);
  j.page();
  const auto* relation = j.row("friend-request");
  Check(relation && relation->label == loc::T(state.label) && relation->enabled == state.enabled && relation->detail == (state.detail ? loc::T(state.detail) : ""), "The page's relation row does not say the member's relation");
  Check((j.row("friend-block") != nullptr) == (state.relation != Relation::Blocked), "Block is offered to a blocked member, or not to another");
  j.h.Choose("friend-request");
  Check((j.sent<commands::FriendRequest>().size() == 1) == state.enabled, "A relation row that says a state was sent, or Add friend was not");
 }
 // A request waiting is accepted by sending the request back.
 // One journey at a time: each harness's interface loads its fonts into its own context.
 {
  RoomJourney received(Relation::Received);
  received.page(); received.h.Choose("friend-request");
  Check(received.sent<commands::FriendRequest>().size() == 1 && received.sent<commands::FriendRequest>()[0]->card == "c1.player_2", "Accept request did not send the request to the member");
 }
 // A member with no card, and a private room, leave the relation and Block out.
 {
  RoomJourney none;
  none.page("member-3");
  Check(none.screen() == "room-member" && !none.row("friend-request") && !none.row("friend-block"), "A member with no card has a friend row or Block");
 }
 {
  RoomJourney priv(Relation::None, false);
  priv.page();
  Check(!priv.row("friend-request") && !priv.row("friend-block") && priv.row("mute"), "A private room's member page offers a friend row or Block");
 }
}

// Block asks first and then sends the card token with the room name.
void BlockJourney() {
 RoomJourney j;
 j.page();
 j.h.Choose("friend-block");
 Check(j.h.shell.Navigation().Confirming() && j.sent<commands::FriendBlock>().empty(), "Block did not ask first");
 j.h.Press(MenuInput::Right); j.h.Press(MenuInput::Select);
 const auto blocks = j.sent<commands::FriendBlock>();
 Check(blocks.size() == 1 && blocks[0]->card == "c1.player_2" && blocks[0]->label == "Player 2" && blocks[0]->bridge == "brg_1", "Block did not send the card token and the room name");
}

// A friend's request or invite that comes after the list was read is a toast with buttons; the one already waiting at the
// first read is not.
std::shared_ptr<nf::State> WithRequest(bool request, bool invite) {
 auto state = std::make_shared<nf::State>();
 state->kind = nf::State::Kind::Listed; state->code = "4F2A-91CX";
 if (request) { nf::Incoming in; in.id = "rq_1"; in.name = "Player 9"; in.sentAt = Now - Hour; in.games = 6; in.lastGameAt = Now - Day; state->incoming = {in}; }
 nf::Friend host; host.id = "fr_3"; host.name = "Player 3"; host.presence.kind = nf::Presence::Kind::InRoom; host.presence.hasRoom = true;
 host.presence.room.id = std::string(32, 'a'); host.presence.room.name = "Player 3's room"; host.presence.room.joinable = true;
 state->friends = {host};
 if (invite) { nf::Invite iv; iv.id = "iv_1"; iv.friendId = "fr_3"; iv.room.value = host.presence.room.id; iv.expiresAt = Now + Hour; state->invites = {iv}; }
 return state;
}

void ToastJourney() {
 // The request already there when the list was first read is not a toast.
 {
  RoomJourney j(Relation::None, true, false); j.h.Screen("home");
  j.Give(WithRequest(true, false));
  Check(Said(j).empty(), "A request waiting at the first read was said as a toast");
 }
 // A request that comes later is: Accept sends accept without opening a screen, and is gone; Later sends nothing.
 {
  RoomJourney j; j.h.Screen("home");
  j.Give(WithRequest(true, false));
  j.h.Press(MenuInput::Select); j.h.Frame(0, 2);
  const auto accepts = j.sent<commands::FriendAccept>();
  Check(accepts.size() == 1 && accepts[0]->request == "rq_1" && accepts[0]->bridge == "brg_1", "Accept on the request toast did not send accept for the request");
  Check(j.screen() == "home", "Accept on the request toast changed the screen");
  j.h.Frame(0, 60);
  Check(j.sent<commands::FriendAccept>().size() == 1, "The request toast said itself again");
  // While the request waits its Notifications row stands for the toast; answered, the toast's words stay there.
  j.h.Screen("notifications"); j.h.Frame(0, 2);
  Check(j.row("request:rq_1") && Said(j).empty(), "A request waiting was listed as its toast too");
  auto& f = j.view();
  f.state.value = WithRequest(false, false); ++f.state.finished; ++f.state.revision; j.h.Frame(0, 2);
  Check(Said(j) == std::string(loc::Tf("friends.now_friends", "Player 9")) + "|" + loc::Tf("friends.toast_request", "Player 9") + "\n" + loc::Tf("friends.played_you", 6, loc::Tf("friends.days_ago", 1)) + "|",
   "A new request did not say who wants to be friends and whether they played, or its Accept did not say it went through");
 }
 {
  RoomJourney j; j.h.Screen("home");
  j.Give(WithRequest(true, false));
  const auto before = j.sent<commands::FriendAccept>().size();
  j.h.Press(MenuInput::Back); j.h.Frame(0, 2);
  Check(j.sent<commands::FriendAccept>().size() == before && j.screen() == "home", "Later on the request toast sent accept or changed the screen");
  // The request toast has three buttons, Accept, View and Later: Select is the first and Back the last, so Back above was Later.
 }
 // An invite: Join takes it and then drops it; Dismiss only drops it. Neither opens a screen.
 {
  RoomJourney j; j.h.Screen("home");
  j.Give(WithRequest(false, true));
  j.h.Press(MenuInput::Select); j.h.Frame(0, 2);
  std::vector<std::string> kinds;
  for (const auto& a : j.h.actions) {
   if (const auto* take = As<commands::FriendTakeInvite>(a)) { if (take->invite == "iv_1") kinds.push_back("take"); }
   else if (const auto* dismiss = As<commands::FriendDismiss>(a)) { if (dismiss->invite == "iv_1") kinds.push_back("dismiss"); }
  }
  Check(kinds == std::vector<std::string>({"take", "dismiss"}), "Join on the invite toast did not take the invite and then drop it");
  Check(j.screen() == "home", "Join on the invite toast changed the screen");
  // Dropped, the invite leaves the list, and its toast's words stay in Notifications.
  auto& f = j.view();
  f.state.value = WithRequest(false, false); ++f.state.finished; ++f.state.revision; j.h.Frame(0, 2);
  Check(Said(j) == std::string(loc::Tf("friends.toast_invite", "Player 3")) + "\nPlayer 3's room|", "A new invite did not say who invites and to which room");
 }
 {
  RoomJourney j; j.h.Screen("home");
  j.Give(WithRequest(false, true));
  j.h.Press(MenuInput::Back); j.h.Frame(0, 2);
  Check(j.sent<commands::FriendTakeInvite>().empty() && j.sent<commands::FriendDismiss>().size() == 1 && j.sent<commands::FriendDismiss>()[0]->invite == "iv_1", "Dismiss on the invite toast did not only drop the invite");
 }
 // An invite that ran out is not said.
 {
  RoomJourney j; j.h.Screen("home");
  auto late = WithRequest(false, true);
  late->invites[0].expiresAt = Now - 1;
  j.Give(late);
  Check(Said(j).empty(), "An invite that had run out was said as a toast");
 }
}

// This player (1) and Player 2 seated at table 1 for match 5; the frames while a game plays, with Ember hidden; the
// WIN of that game, which opens the prompt over it; and the game played on in place, which ends that prompt.
struct Rematches {
 RoomJourney& j;
 room::Table& t;
 explicit Rematches(RoomJourney& journey) : j(journey), t(journey.h.view.room.tables[0]) {
  for (auto& m : j.h.view.room.members) if (m.id == 1 || m.id == 2) { m.table = 0; m.seat = static_cast<std::int8_t>(m.id - 1); }
  t.p1 = 1; t.p2 = 2; t.matchGeneration = 5; t.phase = room::TablePhase::Waiting;
 }
 void Hidden(int frames, float seconds = 1.f) {
  for (int i = 0; i < frames; ++i) {
   j.h.imgui.Use(); ImGui::GetIO().DeltaTime = seconds; ImGui::NewFrame();
   j.h.shell.Background(j.h.view, j.h.view.room, [&](ShellAction a) { j.h.actions.push_back(a); return true; });
   ImGui::Render();
  }
 }
 void Plays() { j.h.view.session.match = netplay::MatchState::Playing; j.h.view.atMainMenu = false; t.phase = room::TablePhase::Playing; j.h.view.matchResult = false; }
 void Win() { j.h.view.foughtGame = {0, t.matchGeneration, room::MatchResult::P1Win}; j.h.view.matchResult = true; j.h.open = true; j.h.Frame(); }
 void OnInPlace() { j.h.view.matchResult = false; ++j.h.view.gamesInPlace; }
 void Over() { j.h.view.matchResult = false; j.h.view.session.match = netplay::MatchState::PostMatch; j.h.view.atMainMenu = true; t.phase = room::TablePhase::Waiting; j.h.open = true; }
};

// Add friend pressed in the room just before the next game: the answer comes while the game plays and Ember is hidden,
// and the words are said as the prompt over its WIN shows. Ember hid them again at once (both chose Rematch, the battle
// went on), so they are said once more on the room's own screen.
void AnsweredWhileHiddenJourney() {
 RoomJourney j;
 Rematches r(j);
 j.page();
 j.h.Choose("friend-request");
 Check(j.sent<commands::FriendRequest>().size() == 1, "Add friend sent no request");
 auto& f = j.view();
 const std::string words = loc::Tf("friends.request_sent_to", "Player 2");
 r.Plays();
 r.Hidden(2);
 f.busy = true; r.Hidden(2);
 // Answered long before Ember shows again: said as it shows.
 f.busy = false; ++f.state.finished; ++f.state.revision; r.Hidden(120);
 r.Win();
 Check(j.screen() == "post-match", "The WIN did not open the prompt");
 const auto up = [&] {
  for (const auto* card : j.h.shell.Notices().Cards(ImGui::GetTime())) if (card->text == words) return true;
  return false;
 };
 Check(up(), "A request answered while Ember was hidden was not said as the prompt showed");
 r.OnInPlace(); r.Hidden(60);
 r.Over(); j.h.Frame(0, 3);
 Check(j.screen() == "room-member", "The room did not show again on the member's page after the set");
 Check(up(), "Words Ember hid at once were not shown again in the room");
 Check(Said(j) == words + "|", "Words shown again were listed twice");
}

// A request that arrives while the prompt over a WIN shows: its toast shows there without taking the prompt's keys
// (Select answers Rematch), and is said once more, with its keys, when the room shows; Accept then says who is a friend.
void RequestOverPromptJourney() {
 RoomJourney j;
 Rematches r(j);
 j.h.Screen("room");
 r.Plays(); r.Hidden(30);
 r.Win();
 Check(j.screen() == "post-match", "The WIN did not open the prompt");
 j.Give(WithRequest(true, false));
 const auto pressed = j.h.actions.size();
 j.h.Press(MenuInput::Select);
 const bool rematch = std::any_of(j.h.actions.begin() + static_cast<std::ptrdiff_t>(pressed), j.h.actions.end(), [](const ShellAction& a) { return ReadyIs(a, true); });
 Check(j.sent<commands::FriendAccept>().empty() && rematch, "Select over the prompt answered the request's toast and not Rematch");
 r.OnInPlace(); r.Hidden(60);
 r.Over(); j.h.Screen("room-members"); j.h.Frame(0, 2);
 Check(j.screen() == "room-members", "The prompt stayed after the set");
 j.h.Press(MenuInput::Select);
 const auto accepts = j.sent<commands::FriendAccept>();
 Check(accepts.size() == 1 && accepts[0]->request == "rq_1", "The request's toast was not said again with its keys once the room showed");
 auto& f = j.view();
 auto after = WithRequest(false, false);
 f.state.value = after; ++f.state.finished; ++f.state.revision; j.h.Frame(0, 2);
 const std::string request = loc::Tf("friends.toast_request", "Player 9") + "\n" + loc::Tf("friends.played_you", 6, loc::Tf("friends.days_ago", 1));
 Check(Said(j) == std::string(loc::Tf("friends.now_friends", "Player 9")) + "|" + request + "|",
  "Accept on the request's toast did not say who is now a friend, or the toast shown again was listed twice");
}

// Player card in a member's popout opens their profile page: by the card token their ticket carried, shown with their name,
// and by their name and flag in a room with no tokens; Back returns to the Members list. It asks the runtime once.
void PlayerCardJourney() {
 namespace pr = netplay::profiles;
 {
  RoomJourney j;
  j.popout();
  j.h.Press(MenuInput::Select);
  j.h.Frame(0, 2);
  Check(j.screen() == "player-card", "Player card did not open their profile");
  const auto opens = j.sent<commands::ProfileOpen>();
  Check(opens.size() == 1 && opens[0]->bridge == "brg_1" && opens[0]->key == pr::Key::OfCard("c1.player_2") && opens[0]->known.name == "Player 2", "A member's profile was not asked for by their card token and name");
  j.h.Press(MenuInput::Back);
  Check(j.screen() == "room-members", "Back from a member's profile did not return to the Members list");
 }
 {
  // No token (a private room, a member with no card): by name.
  RoomJourney j;
  j.popout("member-3");
  j.h.Press(MenuInput::Select);
  j.h.Frame(0, 2);
  const auto opens = j.sent<commands::ProfileOpen>();
  Check(j.screen() == "player-card" && opens.size() == 1 && opens[0]->key == pr::Key::OfName("Player 3") && opens[0]->known.name == "Player 3", "A member with no token was not asked for by name");
 }
}

// After a set the opponent's row ends in Add friend: View sends the member's request (card token, room name) from anywhere in
// the prompt and the box turns to Request sent at once; the focus stays on Rematch, nothing is answered (no Ready) and the prompt
// stays; a refusal brings the button back; A on it presses it, as the D-pad reaches it. Accept request, Request sent, Friends,
// a block and a room with no card each say what they are, and only the first two are buttons.
void PostMatchFriendJourney() {
 const std::string row = ui::post_match_friend::RowId;
 std::map<std::string, std::string> words;
 {
  RoomJourney j(Relation::None);
  Rematches r(j);
  SetMenuCardTextProbe([&](const std::string& id, const std::string& text) { words[id] = text; });
  j.h.Screen("room");
  r.Plays(); r.Hidden(30);
  r.Win();
  Check(j.screen() == "post-match" && j.h.shell.Navigation().Focus() == "again", "The WIN did not open the prompt on Rematch");
  const auto* add = j.row(row);
  Check(add && add->wide && add->enabled && add->label == loc::T("friends.add_member") && add->detail == loc::Tf("postmatch.add_friend.detail", "Player 2"), "The prompt has no Add friend for a stranger from a public room");
  Check(j.rows.front().id == "again" && j.rows.back().id == row, "Add friend is not the footer after the prompt's buttons");
  // View sends it from the prompt, which stays as it was.
  j.h.Press(MenuInput::Chat);
  const auto requests = j.sent<commands::FriendRequest>();
  Check(requests.size() == 1 && requests[0]->bridge == "brg_1" && requests[0]->card == "c1.player_2" && requests[0]->label == "Player 2" && !requests[0]->mutual, "View did not send a request with the card token and the room name");
  Check(j.screen() == "post-match" && j.h.shell.Navigation().Focus() == "again", "View moved the focus or left the prompt");
  Check(j.h.shell.PostMatchAnswer().word == room::PostMatchWord::None && j.sent<commands::Ready>().empty(), "Add friend answered the rematch");
  // The box is the same box, flat: Request sent, and not a row.
  Check(!j.row(row) && words[row] == loc::T("friends.request_sent"), "The box did not turn to Request sent as the request went out");
  j.h.Press(MenuInput::Chat);
  Check(j.sent<commands::FriendRequest>().size() == 1, "A second View sent another request");
  // A refusal comes back as the usual words and the button returns; A on it presses it, and the focus goes back to Rematch.
  auto& f = j.view();
  f.busy = true; j.h.Frame(0, 2);
  f.busy = false; ++f.state.finished; ++f.refusals; f.refusal = "request_limit"; f.refusalText = "Wait a while before asking again."; j.h.Frame(0, 3);
  Check(j.row(row) != nullptr, "A refusal did not bring Add friend back");
  f.refusal.clear();
  j.h.shell.Navigation().Focus(row, j.rows); j.h.Frame();
  j.h.Press(MenuInput::Select);
  Check(j.sent<commands::FriendRequest>().size() == 2, "A on Add friend did not send the request");
  Check(j.h.shell.Navigation().Focus() == "again" && j.screen() == "post-match" && j.h.shell.PostMatchAnswer().word == room::PostMatchWord::None, "Add friend pressed with A left the focus on its row or answered the rematch");
  SetMenuCardTextProbe({});
 }
 // They asked first: Accept request sends the same request, which the bridge makes friendship of.
 {
  RoomJourney j(Relation::Received);
  Rematches r(j);
  j.h.Screen("room");
  r.Plays(); r.Hidden(30);
  r.Win();
  const auto* accept = j.row(row);
  Check(accept && accept->label == loc::T("friends.accept_member") && accept->detail == loc::Tf("postmatch.accept_friend.detail", "Player 2"), "A request waiting is not Accept request");
  j.h.Press(MenuInput::Chat);
  const auto requests = j.sent<commands::FriendRequest>();
  Check(requests.size() == 1 && requests[0]->mutual && j.h.shell.Navigation().Focus() == "again", "View on Accept request did not send a mutual request, or moved the focus");
 }
 // The states that are no button: a flat box with its word, nothing to press, and View sends nothing.
 const struct { Relation relation; const char* label; } flat[] = {{Relation::Sent, "friends.request_sent"}, {Relation::Friend, "friends.are_friends"}};
 for (const auto& state : flat) {
  RoomJourney j(state.relation);
  Rematches r(j);
  words.clear();
  SetMenuCardTextProbe([&](const std::string& id, const std::string& text) { words[id] = text; });
  j.h.Screen("room");
  r.Plays(); r.Hidden(30);
  r.Win();
  Check(!j.row(row) && words[row] == loc::T(state.label), "A relation that is no button is a row, or the box does not say it");
  j.h.Press(MenuInput::Chat);
  Check(j.sent<commands::FriendRequest>().empty() && j.h.shell.Navigation().Focus() == "again", "View sent a request to a friend or a player already asked");
  SetMenuCardTextProbe({});
 }
 // Nothing for a player who blocked, or a room with no card token (a private room).
 {
  RoomJourney blocked(Relation::Blocked);
  Rematches r(blocked);
  words.clear();
  SetMenuCardTextProbe([&](const std::string& id, const std::string& text) { words[id] = text; });
  blocked.h.Screen("room");
  r.Plays(); r.Hidden(30);
  r.Win();
  Check(!blocked.row(row) && words.count(row) == 0, "A blocked player has an Add friend box");
  SetMenuCardTextProbe({});
 }
 {
  RoomJourney priv(Relation::None, false);
  priv.h.view.room.members[1].card = CardToken();
  Rematches r(priv);
  words.clear();
  SetMenuCardTextProbe([&](const std::string& id, const std::string& text) { words[id] = text; });
  priv.h.Screen("room");
  r.Plays(); r.Hidden(30);
  r.Win();
  Check(priv.screen() == "post-match" && !priv.row(row) && words.count(row) == 0, "A room with no card tokens has an Add friend box");
  priv.h.Press(MenuInput::Chat);
  Check(priv.sent<commands::FriendRequest>().empty(), "View sent a request in a room with no card tokens");
  SetMenuCardTextProbe({});
 }
}

// What a toast is followed by: Later is an answer, and nothing said over a free menu is said again.
void ToastAnsweredJourney() {
 RoomJourney j; j.h.Screen("home");
 Rematches r(j);
 j.Give(WithRequest(true, false));
 j.h.Press(MenuInput::Back);
 r.Plays(); r.Hidden(30);
 r.Over(); j.h.Frame(0, 3);
 // Answered on another PC meanwhile: gone from the list, its toast's words stay in Notifications, said once.
 auto& f = j.view();
 f.state.value = WithRequest(false, false); ++f.state.finished; ++f.state.revision; j.h.Frame(0, 2);
 const std::string request = loc::Tf("friends.toast_request", "Player 9") + "\n" + loc::Tf("friends.played_you", 6, loc::Tf("friends.days_ago", 1));
 Check(Said(j) == request + "|", "A request's toast answered Later was said again");
}
}

// Notifications lists what waits from friends for as long as it waits: a request the first read already held (which no
// toast said), each once even when its toast was said, an invite still good and no other; A answers as the Friends tab
// does, and the row goes with the request.
std::size_t ToastRows(const RoomJourney& j) {
 return static_cast<std::size_t>(std::count_if(j.rows.begin(), j.rows.end(), [](const MenuEntry& r) { return r.id.compare(0, 6, "toast-") == 0; }));
}
void NotificationsJourney() {
 {
  RoomJourney j(Relation::None, true, false); j.h.Screen("home");
  j.Give(WithRequest(true, false));
  j.h.Screen("notifications"); j.h.Frame(0, 2);
  const auto* row = j.row("request:rq_1");
  Check(row && row->label == loc::Tf("friends.toast_request", "Player 9") && row->hint == loc::T("friends.accept") && row->detail == loc::Tf("friends.played_you", 6, loc::Tf("friends.days_ago", 1)),
   "A request waiting at the first read is not listed in Notifications as it asks, with its games");
  Check(ToastRows(j) == 0, "A request no toast said was listed as a toast too");
  Check(!j.sent<commands::FriendsVisit>().empty(), "Notifications did not keep the friends fresh");
  j.h.Choose("request:rq_1");
  const auto accepts = j.sent<commands::FriendAccept>();
  Check(accepts.size() == 1 && accepts[0]->request == "rq_1" && accepts[0]->bridge == "brg_1", "A on a request's row in Notifications did not send the Friends tab's Accept");
  auto& f = j.view();
  f.state.value = WithRequest(false, false); ++f.state.finished; ++f.state.revision; j.h.Frame(0, 3);
  Check(!j.row("request:rq_1"), "An accepted request stayed in Notifications");
  bool said = false;
  for (const auto& r : j.rows) said = said || r.detail == loc::Tf("friends.now_friends", "Player 9");
  Check(said, "Accept from Notifications did not say who is now a friend");
 }
 {
  // Toasted on Home, then listed once: the request's row stands for its toast, which keeps no buttons once answered.
  RoomJourney j; j.h.Screen("home");
  j.Give(WithRequest(true, false));
  j.h.Screen("notifications"); j.h.Frame(0, 2);
  Check(j.row("request:rq_1") && ToastRows(j) == 0, "A request that was toasted is listed twice, or not at all");
  auto& f = j.view();
  f.state.value = WithRequest(false, false); ++f.state.finished; ++f.state.revision; j.h.Frame(0, 3);
  Check(!j.row("request:rq_1") && ToastRows(j) == 1, "A request gone did not leave its toast's words in Notifications");
  const auto toast = std::find_if(j.rows.begin(), j.rows.end(), [](const MenuEntry& r) { return r.id.compare(0, 6, "toast-") == 0; });
  Check(toast != j.rows.end() && toast->choices.empty(), "A request gone still offers its toast's Accept");
 }
 {
  // An invite still good is listed with Join, which takes it and then drops it; one run out is not listed.
  RoomJourney j; j.h.Screen("home");
  auto late = WithRequest(false, true);
  late->invites[0].expiresAt = Now - 1;
  j.Give(late);
  j.h.Screen("notifications"); j.h.Frame(0, 2);
  Check(!j.row("invite:iv_1"), "An invite that ran out was listed in Notifications");
  j.Give(WithRequest(false, true));
  j.h.Screen("notifications"); j.h.Frame(0, 2);
  const auto* row = j.row("invite:iv_1");
  Check(row && row->label == loc::Tf("friends.toast_invite", "Player 3") && row->hint == loc::T("toast.join"), "An invite waiting is not listed in Notifications with Join");
  j.h.Choose("invite:iv_1");
  Check(j.sent<commands::FriendTakeInvite>().size() == 1 && j.sent<commands::FriendTakeInvite>()[0]->invite == "iv_1" &&
   j.sent<commands::FriendDismiss>().size() == 1 && j.sent<commands::FriendDismiss>()[0]->invite == "iv_1", "Join on an invite's row did not take the invite and then drop it");
 }
 {
  // Friends off: nothing listed there, and no words about it; the empty list says so as ever.
  RoomJourney j(Relation::None, true, false); j.h.Screen("home");
  auto off = WithRequest(true, true);
  off->kind = nf::State::Kind::Off;
  j.Give(off);
  j.h.Screen("notifications"); j.h.Frame(0, 2);
  // Friends are still offered (the screen says how to turn them on): their row stands beside the empty list's.
  Check(j.rows.size() == 2 && j.rows[0].id == "notifications-friends" && j.rows[1].id == "toasts-none", "Notifications listed something but the Friends row, or an error, with friends off");
 }
 {
  // The Friends row follows what waits and opens the Friends screen; Back returns to Notifications.
  RoomJourney j(Relation::None, true, false); j.h.Screen("home");
  j.Give(WithRequest(true, true));
  j.h.Screen("notifications"); j.h.Frame(0, 2);
  std::vector<std::string> ids;
  for (const auto& r : j.rows) ids.push_back(r.id);
  Check(ids == std::vector<std::string>({"request:rq_1", "invite:iv_1", "notifications-friends"}), "Notifications does not list the Friends row after the request and the invite");
  const auto* row = j.row("notifications-friends");
  Check(row && row->label == loc::T("home.friends") && row->detail == loc::T("notifications.friends_detail"), "Notifications' Friends row is not worded as Friends");
  j.h.Choose("notifications-friends");
  Check(j.screen() == "friends", "Notifications' Friends row did not open the Friends screen");
  j.h.Press(MenuInput::Back);
  Check(j.screen() == "notifications", "Back from the Friends screen did not return to Notifications");
 }
 {
  // Nothing waits: the row is there all the same.
  RoomJourney j; j.h.Screen("home");
  j.Give(WithRequest(false, false));
  j.h.Screen("notifications"); j.h.Frame(0, 2);
  Check(j.row("notifications-friends"), "Notifications has no Friends row while nothing waits");
 }
}

// Friends from the room's More menu: after Room settings, and Back returns to the room.
void MoreFriendsJourney() {
 RoomJourney j;
 j.h.Screen("room"); j.h.Frame(0, 3);
 const auto* more = j.row("room-more");
 Check(more, "The room has no More");
 std::size_t admin = more->choices.size(), friends = admin;
 for (std::size_t i = 0; i < more->choices.size(); ++i) { if (more->choices[i].id == "room-admin") admin = i; if (more->choices[i].id == "room-friends") friends = i; }
 Check(friends < more->choices.size() && friends == admin + 1 && more->choices[friends].enabled && more->choices[friends].label == loc::T("home.friends") &&
  more->choices[friends].detail == loc::T("room.v2.friends_detail"), "More does not offer Friends right after the room's settings");
 j.h.Choose("room-more");
 for (std::size_t i = 0; i < friends; ++i) j.h.Press(MenuInput::Down);
 j.h.Press(MenuInput::Select);
 Check(j.screen() == "friends", "More's Friends did not open the Friends screen");
 // The tab's own X, Y and Delete still work over a room.
 j.Give(WithRequest(true, false));
 j.h.Press(MenuInput::Fighter);
 Check(j.screen() == "friends-add", "X on the Friends screen opened from a room did not open Find a player");
 j.h.Press(MenuInput::Back);
 j.h.Press(MenuInput::Back);
 Check(j.screen() == "room", "Back from Friends did not return to the room");
}

// A friend as the bridge lists them: online, in a room or offline; `played` is when they last played this player.
nf::Friend Pal(const char* name, nf::Presence::Kind kind, std::uint64_t played = 0) {
 nf::Friend f; f.id = std::string("fr_") + name; f.name = name; f.presence.kind = kind;
 f.hasRecord = played != 0; f.record.lastAt = played;
 return f;
}
std::shared_ptr<nf::State> Roster(std::vector<nf::Friend> friends, nf::State::Kind kind = nf::State::Kind::Listed) {
 auto state = std::make_shared<nf::State>();
 state->kind = kind; state->code = "4F2A-91CX"; state->friends = std::move(friends);
 return state;
}
// A choice of the Invite button's menu, as a player makes it: open the button, step down to the choice, select.
void PickInvite(RoomJourney& j, const char* choice) {
 const auto* invite = j.row("invite");
 Check(invite, "The room has no Invite button");
 std::size_t at = 0;
 while (at < invite->choices.size() && invite->choices[at].id != choice) ++at;
 Check(at < invite->choices.size(), "Invite does not offer the choice");
 j.h.Choose("invite");
 for (std::size_t i = 0; i < at; ++i) j.h.Press(MenuInput::Down);
 j.h.Press(MenuInput::Select); j.h.Frame(0, 2);
}
// A private room of three, its invitation made and its friends read.
std::unique_ptr<RoomJourney> PrivateRoom(std::shared_ptr<nf::State> state) {
 auto j = std::make_unique<RoomJourney>(Relation::None, false, false);
 j->h.view.invitation = "sf4e3:invitation";
 j->h.Screen("room"); j->h.Frame(0, 2);
 if (state) j->Give(std::move(state));
 return j;
}

// The Invite button of a private room replaces the two copy buttons: the friends who can be invited now, most recently played
// first and at most six, More friends when others were left out or none is shown, then Copy short link and Copy invitation.
void InviteJourney() {
 using Kind = nf::Presence::Kind;
 // The header says what the room admits: its friends, unless it is Hidden or a match's, a public room being listed.
 {
  room::Snapshot here;
  Check(std::string(ui::room_invite::AccessKey(here)) == "room.v2.friends_can_join", "A private room open to friends does not say friends can join");
  here.friendsHidden = true;
  Check(std::string(ui::room_invite::AccessKey(here)) == "room.v2.invitation_only", "A Hidden room does not say invitation only");
  here.friendsHidden = false; here.tournament.matchId = "emt_1";
  Check(std::string(ui::room_invite::AccessKey(here)) == "room.v2.invitation_only", "A match room does not say invitation only");
  here.tournament = room::TournamentBinding(); here.serverOwned = true;
  Check(std::string(ui::room_invite::AccessKey(here)) == "room.v2.listed", "A public room does not say it is listed");
 }
 // Online friends in this game, the one played with last first; an offline friend and one in another game are not listed.
 {
  auto other = Pal("Other", Kind::Online); other.presence.game = "sfv";
  auto j = PrivateRoom(Roster({Pal("Amy", Kind::Online, 50), Pal("Zed", Kind::InRoom, 100), Pal("Away", Kind::Offline, 200), other}));
  const auto* invite = j->row("invite");
  Check(invite && invite->label == loc::T("friends.invite") && invite->popout && invite->enabled, "A private room has no Invite button");
  Check(!j->row("copy") && !j->row("copy-short"), "The bar still has the two copy buttons");
  Check(Choices(invite) == std::vector<std::string>({"friend:fr_Zed", "friend:fr_Amy", "copy-short", "copy"}), "Invite is not the friends played last, Copy short link and Copy invitation");
  Check(invite->choices[0].label == "Zed" && invite->choices[0].enabled && invite->choices[2].label == loc::T("room.copy_short_invitation") && invite->choices[3].label == loc::T("room.copy_invitation"), "Invite does not name the friends and the copies");
  // Choosing a friend sends the invite the Friends screen sends, and says so once it went through.
  PickInvite(*j, "friend:fr_Zed");
  const auto invites = j->sent<commands::FriendInvite>();
  Check(invites.size() == 1 && invites[0]->bridge == "brg_1" && invites[0]->friendId == "fr_Zed", "A friend chosen from Invite was not sent an invite");
  Check(j->screen() == "room", "Inviting a friend left the room");
  ++j->view().state.finished; j->h.Frame(0, 3);
  Check(Said(*j) == std::string(loc::Tf("friends.invite_sent", "Zed")) + "|", "An invite that went through did not say who it was sent to");
 }
 // More than six: six, then More friends, which opens the Friends screen as More's Friends does.
 {
  std::vector<nf::Friend> many;
  for (int i = 1; i <= 8; ++i) many.push_back(Pal(("Pal" + std::to_string(i)).c_str(), Kind::Online));
  auto j = PrivateRoom(Roster(many));
  Check(Choices(j->row("invite")) == std::vector<std::string>({"friend:fr_Pal1", "friend:fr_Pal2", "friend:fr_Pal3", "friend:fr_Pal4", "friend:fr_Pal5", "friend:fr_Pal6", "room-friends", "copy-short", "copy"}),
   "Invite does not stop at six friends and offer More friends");
  PickInvite(*j, "room-friends");
  Check(j->screen() == "friends", "More friends did not open the Friends screen");
 }
 // None to invite: More friends leads on, so the menu has a way to add some.
 {
  auto j = PrivateRoom(Roster({Pal("Away", Kind::Offline, 5)}));
  Check(Choices(j->row("invite")) == std::vector<std::string>({"room-friends", "copy-short", "copy"}), "Invite with no friend to invite is not More friends and the copies");
 }
 // A room that cannot be offered lists no friend and says why, in the cause's own words; the copies stay.
 const struct { nf::OfferBlock block; const char* words; } blocked[] = {
  {nf::OfferBlock::Locked, "friends.nothing_to_offer.locked"}, {nf::OfferBlock::Full, "friends.nothing_to_offer.full"}, {nf::OfferBlock::Other, "friends.nothing_to_offer"}};
 for (const auto& cause : blocked) {
  auto j = PrivateRoom(Roster({Pal("Amy", Kind::Online)}));
  j->view().canInvite = false; j->view().cannotInvite = cause.block; j->h.Frame(0, 2);
  const auto* invite = j->row("invite");
  Check(Choices(invite) == std::vector<std::string>({"invite-why", "copy-short", "copy"}), "Invite for a room that cannot be offered is not its reason and the copies");
  Check(!invite->choices[0].enabled && invite->choices[0].detail == loc::T(cause.words) && invite->choices[1].enabled && invite->choices[2].enabled && invite->enabled, "The reason is not a disabled line in the cause's words");
 }
 // No Discord account, friends turned off, or nothing read: the two copies and no reason.
 for (const auto kind : {nf::State::Kind::NoAccount, nf::State::Kind::Off, nf::State::Kind::Unavailable}) {
  auto j = PrivateRoom(Roster({}, kind));
  j->view().canInvite = false; j->h.Frame(0, 2);
  Check(Choices(j->row("invite")) == std::vector<std::string>({"copy-short", "copy"}), "Invite without friends is not just the two copies");
 }
 Check(Choices(PrivateRoom(nullptr)->row("invite")) == std::vector<std::string>({"copy-short", "copy"}), "Invite before the friends are read is not just the two copies");
 // The copies are the ones the bar had: Copy invitation puts the invitation on the clipboard, Copy short link asks for the link.
 {
  auto j = PrivateRoom(nullptr);
  auto& platform = ImGui::GetPlatformIO();
  platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) { g_clipboard = text ? text : ""; };
  platform.Platform_GetClipboardTextFn = [](ImGuiContext*) { return g_clipboard.c_str(); };
  PickInvite(*j, "copy");
  Check(g_clipboard == "sf4e3:invitation", "Copy invitation did not put the invitation on the clipboard");
  const auto asked = j->h.actions.size();
  PickInvite(*j, "copy-short");
  Check(j->h.actions.size() == asked + 1 && Is<commands::ShortInvitation>(j->h.actions.back()), "Copy short link did not ask for the link");
  platform.Platform_SetClipboardTextFn = nullptr; platform.Platform_GetClipboardTextFn = nullptr;
 }
}

int main() {
 try {
  PopoutJourney(); PlayerCardJourney(); PageJourney(); RelationRowsJourney(); BlockJourney(); ToastJourney();
  AnsweredWhileHiddenJourney(); RequestOverPromptJourney(); PostMatchFriendJourney(); ToastAnsweredJourney(); NotificationsJourney(); MoreFriendsJourney(); InviteJourney();
  std::cout << "Room friends journeys passed.\n";
  return 0;
 } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
