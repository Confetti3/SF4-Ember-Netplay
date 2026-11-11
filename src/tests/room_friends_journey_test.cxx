// Friends from a public room and from a toast (docs/design/PROFILE.md, "Another player's card" and "Toasts"): a member's
// popout offers Player card and Add friend by their relation, the page offers the relation row, Block and the rest, Add
// friend sends the card token with the member's room name, a request waiting is Accept, a member with no card has
// neither and a private room offers none; a request or an invite that comes is a toast whose Accept or Join sends its
// commands without opening a screen.
#include "public_rooms_journey_support.hxx"
#include "../ui/FriendsPanel.hxx"
#include <memory>
namespace {
namespace nf = sf4e::netplay::friends;
using Kind = nf::Command::Kind;
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
 std::vector<const nf::Command*> sent(Kind kind) const {
  std::vector<const nf::Command*> out;
  for (const auto& a : h.actions) if (const auto* f = As<commands::Friends>(a)) if (f->friends.kind == kind) out.push_back(&f->friends);
  return out;
 }
 std::string screen() { return h.shell.Navigation().Screen(); }
 // Player 2's popout open on the Members list.
 const MenuEntry* popout(const char* member = "member-2") { h.Screen("room-members"); h.Choose(member); return row(member); }
 // Player 2's page, by Player card.
 void page() { popout(); h.Press(MenuInput::Select); h.Frame(0, 2); }
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

// Player card opens the page: the relation row, Mute, Steam profile, Kick, Make host, Block. Each relation says its state;
// Add friend and Accept request send the same request; the other states cannot be pressed.
void PageJourney() {
 RoomJourney j;
 j.page();
 Check(j.screen() == "room-member", "Player card did not open the member's page");
 const std::vector<std::string> ids = {"friend-request", "mute", "steam-profile", "kick", "transfer-host", "friend-block"};
 std::vector<std::string> shown;
 for (const auto& r : j.rows) shown.push_back(r.id);
 Check(shown == ids, "The member's page is not the relation row, Mute, Steam profile, Kick, Make host and Block");
 j.h.Choose("friend-request");
 const auto requests = j.sent(Kind::Request);
 Check(requests.size() == 1 && requests[0]->bridgeId == "brg_1" && requests[0]->card == "c1.player_2" && requests[0]->label == "Player 2" && requests[0]->code.empty(), "Add friend did not send a request with the card token and the room name");
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
  Check((j.sent(Kind::Request).size() == 1) == state.enabled, "A relation row that says a state was sent, or Add friend was not");
 }
 // A request waiting is accepted by sending the request back.
 RoomJourney received(Relation::Received);
 received.page(); received.h.Choose("friend-request");
 Check(received.sent(Kind::Request).size() == 1 && received.sent(Kind::Request)[0]->card == "c1.player_2", "Accept request did not send the request to the member");
 // A member with no card, and a private room, leave the relation and Block out.
 RoomJourney none;
 none.h.Screen("room-members"); none.h.Choose("member-3"); none.h.Press(MenuInput::Select); none.h.Frame(0, 2);
 Check(none.screen() == "room-member" && !none.row("friend-request") && !none.row("friend-block"), "A member with no card has a friend row or Block");
 RoomJourney priv(Relation::None, false);
 priv.page();
 Check(!priv.row("friend-request") && !priv.row("friend-block") && priv.row("mute"), "A private room's member page offers a friend row or Block");
}

// Block asks first and then sends the card token with the room name.
void BlockJourney() {
 RoomJourney j;
 j.page();
 j.h.Choose("friend-block");
 Check(j.h.shell.Navigation().Confirming() && j.sent(Kind::Block).empty(), "Block did not ask first");
 j.h.Press(MenuInput::Right); j.h.Press(MenuInput::Select);
 const auto blocks = j.sent(Kind::Block);
 Check(blocks.size() == 1 && blocks[0]->card == "c1.player_2" && blocks[0]->label == "Player 2" && blocks[0]->bridgeId == "brg_1", "Block did not send the card token and the room name");
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
  const auto accepts = j.sent(Kind::Accept);
  Check(accepts.size() == 1 && accepts[0]->id == "rq_1" && accepts[0]->bridgeId == "brg_1", "Accept on the request toast did not send accept for the request");
  Check(j.screen() == "home", "Accept on the request toast changed the screen");
  j.h.Frame(0, 60);
  Check(j.sent(Kind::Accept).size() == 1, "The request toast said itself again");
  Check(Said(j) == std::string(loc::Tf("friends.toast_request", "Player 9")) + "\n" + loc::Tf("friends.played_you", 6, loc::Tf("friends.days_ago", 1)) + "|", "A new request did not say who wants to be friends and whether they played");
 }
 {
  RoomJourney j; j.h.Screen("home");
  j.Give(WithRequest(true, false));
  const auto before = j.sent(Kind::Accept).size();
  j.h.Press(MenuInput::Back); j.h.Frame(0, 2);
  Check(j.sent(Kind::Accept).size() == before && j.screen() == "home", "Later on the request toast sent accept or changed the screen");
 }
 // An invite: Join takes it and then drops it; Dismiss only drops it. Neither opens a screen.
 {
  RoomJourney j; j.h.Screen("home");
  j.Give(WithRequest(false, true));
  j.h.Press(MenuInput::Select); j.h.Frame(0, 2);
  std::vector<Kind> kinds;
  for (const auto& a : j.h.actions) if (const auto* f = As<commands::Friends>(a)) if (f->friends.id == "iv_1") kinds.push_back(f->friends.kind);
  Check(kinds == std::vector<Kind>({Kind::TakeInvite, Kind::Dismiss}), "Join on the invite toast did not take the invite and then drop it");
  Check(j.screen() == "home", "Join on the invite toast changed the screen");
  Check(Said(j) == std::string(loc::Tf("friends.toast_invite", "Player 3")) + "\nPlayer 3's room|", "A new invite did not say who invites and to which room");
 }
 {
  RoomJourney j; j.h.Screen("home");
  j.Give(WithRequest(false, true));
  j.h.Press(MenuInput::Back); j.h.Frame(0, 2);
  Check(j.sent(Kind::TakeInvite).empty() && j.sent(Kind::Dismiss).size() == 1 && j.sent(Kind::Dismiss)[0]->id == "iv_1", "Dismiss on the invite toast did not only drop the invite");
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
}

int main() {
 try {
  PopoutJourney(); PageJourney(); RelationRowsJourney(); BlockJourney(); ToastJourney();
  std::cout << "Room friends journeys passed.\n";
  return 0;
 } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
