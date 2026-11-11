// The runtime's friends requests (netplay/FriendRequests.hxx): one request at a
// time, the cadence of syncs, a failed request that keeps the state, a service
// without friends, a change of service or Ember ID, requests the helper lost;
// and the exact requests the helper is sent (netplay/FriendWire.hxx).
#include "../netplay/FriendRequests.hxx"
#include "../netplay/FriendWire.hxx"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>
#include "test_support.hxx"

using namespace sf4e::netplay::friends;
using nlohmann::json;
using Kind = Command::Kind;

namespace {
State Listed(const std::string& code) {
	State state;
	state.kind = State::Kind::Listed;
	state.code = code;
	return state;
}

// A service and Ember ID, and a first sync that has been answered at `at`.
Requests Synced(std::uint64_t at) {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	EXPECT(requests.SyncDue(at));
	requests.SentSync(1, at);
	requests.Finish("", Listed("AAAA-AAAA"), at);
	return requests;
}

// Nothing is asked before a service is named, and then a sync is due at once.
void FirstSync() {
	Requests requests;
	EXPECT(!requests.SyncDue(0) && !requests.MayAct());
	requests.Scope("brg_1", "emb1_a");
	EXPECT(requests.SyncDue(0) && requests.MayAct() && !requests.Busy());
	requests.SentSync(10, 100);
	EXPECT(!requests.SyncDue(100) && !requests.MayAct() && requests.Awaiting(10) && !requests.Awaiting(11) && !requests.Awaiting(0));
	EXPECT(requests.Published().loading && !requests.Published().loaded);
	// A sync in flight is not an action: nothing is Busy until one is asked.
	EXPECT(!requests.Busy() && !requests.Acting());
	requests.Finish("", Listed("AAAA-AAAA"), 300);
	const auto& answered = requests.Published();
	EXPECT(!answered.loading && answered.loaded && answered.value->code == "AAAA-AAAA" && answered.revision == 1 && answered.finished == 1 && answered.installedMs == 300);
}

// The game's own cadence is 30 s, the tab's 10 s, and no two syncs are less than 5 s apart.
void Cadence() {
	auto requests = Synced(1000);
	EXPECT(!requests.SyncDue(1000 + Requests::IdleSyncMs - 1) && requests.SyncDue(1000 + Requests::IdleSyncMs));
	// The tab opening asks at once, but not within the gap.
	requests.Visit(1000 + 2000);
	EXPECT(!requests.SyncDue(1000 + Requests::MinGapMs - 1) && requests.SyncDue(1000 + Requests::MinGapMs));
	requests.SentSync(2, 6000);
	requests.Finish("", Listed("AAAA-AAAA"), 6100);
	EXPECT(requests.Visiting() && !requests.SyncDue(6100 + Requests::ShownSyncMs - 1) && requests.SyncDue(6100 + Requests::ShownSyncMs));
	// Closing the tab goes back to the game's own.
	requests.Leave();
	EXPECT(!requests.Visiting());
	requests.SentSync(3, 20000);
	requests.Finish("", Listed("AAAA-AAAA"), 20000);
	EXPECT(!requests.SyncDue(20000 + Requests::IdleSyncMs - 1) && requests.SyncDue(20000 + Requests::IdleSyncMs));
	// A tab nobody said closed stops being one after a while.
	requests.Visit(30000);
	requests.Expire(30000 + Requests::ShownLapseMs - 1);
	EXPECT(requests.Visiting());
	requests.Expire(30000 + Requests::ShownLapseMs);
	EXPECT(!requests.Visiting());
}

// A change of the player's own presence asks at once, once; the same presence asks nothing.
void PresenceChange() {
	auto requests = Synced(1000);
	requests.NotePresence(0);
	EXPECT(!requests.SyncDue(1000 + Requests::MinGapMs));
	requests.NotePresence(7);
	EXPECT(!requests.SyncDue(1000 + Requests::MinGapMs - 1) && requests.SyncDue(1000 + Requests::MinGapMs));
	requests.SentSync(2, 6000);
	// A change while the sync is in flight is asked for as soon as it ends.
	requests.NotePresence(8);
	requests.Finish("", Listed("AAAA-AAAA"), 6500);
	EXPECT(requests.SyncDue(6000 + Requests::MinGapMs));
	requests.SentSync(3, 11000);
	requests.Finish("", Listed("AAAA-AAAA"), 11000);
	requests.NotePresence(8);
	EXPECT(!requests.SyncDue(11000 + Requests::MinGapMs));
}

// One request is in flight; one action asked meanwhile is held and goes after it.
void OneAtATime() {
	auto requests = Synced(1000);
	requests.SentSync(2, 40000);
	const auto accept = Command::Accept("brg_1", "rq_a");
	EXPECT(!requests.MayAct() && requests.Hold(accept) && requests.Busy());
	EXPECT(!requests.Hold(Command::Decline("brg_1", "rq_b", true)));
	// Held, no sync is due ahead of it.
	EXPECT(!requests.SyncDue(1000000) && !requests.TakeHeld());
	requests.Finish("", Listed("AAAA-AAAA"), 40100);
	auto held = requests.TakeHeld();
	EXPECT(held && held->kind == Kind::Accept && held->id == "rq_a" && !requests.TakeHeld() && !requests.Busy());
	EXPECT(requests.MayAct());
	requests.SentAction(3, Kind::Accept, 40200);
	EXPECT(requests.Busy() && requests.Acting() && *requests.Acting() == Kind::Accept && !requests.MayAct());
	EXPECT(requests.Awaiting(3) && !requests.SyncDue(1000000));
}

// An action's answer is the state; a sync follows it shortly, and never inside the gap.
void ActionAnswers() {
	auto requests = Synced(1000);
	requests.SentSync(2, 30000);
	requests.Finish("", Listed("AAAA-AAAA"), 30100);
	requests.SentAction(3, Kind::NewCode, 31000);
	requests.Finish("", Listed("BBBB-BBBB"), 31200);
	EXPECT(requests.Published().value->code == "BBBB-BBBB" && requests.Published().revision == 3 && requests.Refusal().empty());
	EXPECT(!requests.Acting() && !requests.SyncDue(31200 + Requests::SettleMs - 1) && requests.SyncDue(31200 + Requests::SettleMs));
	// The action's own settle never brings a sync inside the gap of the last one.
	requests.SentSync(4, 40000);
	requests.Finish("", Listed("BBBB-BBBB"), 40000);
	requests.SentAction(5, Kind::Accept, 40100);
	requests.Finish("", Listed("BBBB-BBBB"), 40200);
	EXPECT(!requests.SyncDue(40000 + Requests::MinGapMs - 1) && requests.SyncDue(40000 + Requests::MinGapMs));
}

// A refusal is kept, with the state, until the next action starts.
void Refusals() {
	auto requests = Synced(1000);
	requests.SentAction(2, Kind::Request, 2000);
	requests.Finish("unknown_code", std::nullopt, 2100);
	EXPECT(requests.Refusal() == "unknown_code" && requests.Refusals() == 1 && !requests.Busy());
	EXPECT(requests.Published().value->code == "AAAA-AAAA" && requests.Published().revision == 1 && requests.Published().error == "unknown_code");
	requests.SentAction(3, Kind::Request, 3000);
	EXPECT(requests.Refusal().empty() && requests.Refusals() == 1);
	requests.Finish("request_limit", std::nullopt, 3100);
	EXPECT(requests.Refusal() == "request_limit" && requests.Refusals() == 2);
	requests.SentAction(4, Kind::Remove, 4000);
	requests.Finish("", Listed("AAAA-AAAA"), 4100);
	EXPECT(requests.Refusal().empty() && requests.Refusals() == 2);
	// A refused sync is not an action's refusal; it only keeps its code.
	requests.SentSync(5, 50000);
	requests.Finish("rate_limited", std::nullopt, 50100);
	EXPECT(requests.Refusal().empty() && requests.Published().error == "rate_limited" && requests.Published().value->code == "AAAA-AAAA");
	EXPECT(!requests.SyncDue(50100 + Requests::IdleSyncMs - 1) && requests.SyncDue(50100 + Requests::IdleSyncMs));
}

// A service without friends and a player without an account are states, and stop the syncs until the tab opens.
void NoFriends() {
	for (const char* code : {Requests::Unavailable, Requests::NoAccount}) {
		Requests requests;
		requests.Scope("brg_1", "emb1_a");
		requests.SentSync(1, 0);
		requests.Finish(code, std::nullopt, 10);
		const bool unavailable = std::string(code) == Requests::Unavailable;
		EXPECT(requests.Published().loaded && requests.Published().revision == 1 &&
			requests.Published().value->kind == (unavailable ? State::Kind::Unavailable : State::Kind::NoAccount));
		EXPECT(!requests.SyncDue(10000000));
		requests.NotePresence(3);
		EXPECT(!requests.SyncDue(10000000));
		requests.Visit(20000);
		EXPECT(requests.SyncDue(20000));
		requests.SentSync(2, 25000);
		requests.Finish("", Listed("AAAA-AAAA"), 25100);
		EXPECT(requests.Published().value->kind == State::Kind::Listed && !requests.SyncDue(25100 + Requests::ShownSyncMs - 1) &&
			requests.SyncDue(25100 + Requests::ShownSyncMs));
	}
	// An action's refusal for the same reason says so too.
	Requests requests = Synced(0);
	requests.SentAction(2, Kind::AppearOffline, 100);
	requests.Finish(Requests::NoAccount, std::nullopt, 200);
	EXPECT(requests.Published().value->kind == State::Kind::NoAccount && requests.Refusal() == Requests::NoAccount && !requests.SyncDue(10000000));
}

// Turning friends off stops the syncs, which would make the account again, until the tab opens.
void Forgetting() {
	auto requests = Synced(1000);
	requests.SentAction(2, Kind::Forget, 2000);
	requests.Finish("", Listed("CCCC-CCCC"), 2100);
	EXPECT(!requests.SyncDue(10000000));
	requests.Visit(3000000);
	EXPECT(requests.SyncDue(3000000));
}

// Another service or Ember ID forgets everything of the last one; the counts carry on.
void ScopeChange() {
	auto requests = Synced(1000);
	requests.SentAction(2, Kind::Request, 2000);
	requests.Finish("own_card", std::nullopt, 2100);
	requests.SentSync(3, 40000);
	requests.Hold(Command::Accept("brg_1", "rq_a"));
	requests.Scope("brg_1", "emb1_a");
	EXPECT(requests.Awaiting(3) && requests.Published().loaded);
	requests.Scope("brg_1", "emb1_b");
	const auto& answered = requests.Published();
	EXPECT(!answered.loaded && !answered.loading && answered.value->kind == State::Kind::Unavailable && answered.revision == 1 && answered.finished == 2);
	EXPECT(!requests.Awaiting(3) && !requests.Busy() && requests.Refusal().empty() && requests.MayAct() && requests.SyncDue(45000));
	requests.SentSync(4, 45000);
	requests.Finish("", Listed("DDDD-DDDD"), 45010);
	EXPECT(answered.revision == 2 && answered.finished == 3);
	// The answer of the retired request is nobody's.
	EXPECT(!requests.Awaiting(3));
}

// A request the helper lost ends after a minute; one it could not take ends now.
void LostRequests() {
	Requests requests;
	requests.Scope("brg_1", "emb1_a");
	requests.SentSync(1, 0);
	requests.Expire(Requests::GiveUpMs - 1);
	EXPECT(requests.Awaiting(1));
	requests.Expire(Requests::GiveUpMs);
	EXPECT(!requests.Awaiting(1) && requests.Published().error == "unavailable" && !requests.Published().loading && requests.MayAct());
	requests.SentAction(0, Kind::NewCode, 70000);
	EXPECT(requests.Refusal() == "helper_unavailable" && !requests.Busy() && requests.Published().error == "helper_unavailable");
	requests.SentSync(0, 80000);
	EXPECT(requests.Published().error == "helper_unavailable" && requests.MayAct() && !requests.SyncDue(80001));
}

void Commands() {
	const auto request = Command::RequestCard("brg_1", "c1.abc", "Player 1's room");
	EXPECT(request.IsAction() && request.ByCard() && request.Valid() && request.card == "c1.abc" && request.label == "Player 1's room");
	const auto typed = Command::RequestCode("brg_1", "4f2a 91cx");
	EXPECT(typed.IsAction() && !typed.ByCard() && typed.code == "4f2a 91cx");
	for (const auto& action : {Command::Accept("b", "i"), Command::Decline("b", "i", true), Command::Cancel("b", "i"), Command::Remove("b", "i", false),
			Command::BlockCard("b", "c1.a", "n"), Command::Unblock("b", "i"), Command::NewCode("b"), Command::AppearOffline("b", true), Command::Forget("b"),
			Command::Invite("b", "i"), Command::Dismiss("b", "i")})
		EXPECT(action.IsAction());
	for (const auto& local : {Command::Visit("b"), Command::Leave("b"), Command::Join("b", "i"), Command::Watch("b", "i"), Command::TakeInvite("b", "i"),
			Command::AnswerVisit("b", 3, true)})
		EXPECT(!local.IsAction());
	const auto answer = Command::AnswerVisit("b", 3, true);
	EXPECT(answer.revision == 3 && answer.flag && Command::Remove("b", "i", true).flag && Command::AppearOffline("b", true).flag);
	Command big = Command::Accept("b", std::string(65, 'x'));
	EXPECT(!big.Valid());
	big = Command::RequestCode("b", std::string(33, 'x'));
	EXPECT(!big.Valid());
	big = Command::Visit(std::string(257, 'b'));
	EXPECT(!big.Valid() && Command::Visit(std::string(256, 'b')).Valid());
	EXPECT(Command::Visit("brg").Bytes() == 3 && Command::RequestCard("b", "cc", "lll").Bytes() == 6);
}

const std::string FriendId = "fr_11111111-1111-4111-8111-111111111111";
const std::string RequestId = "rq_11111111-1111-4111-8111-111111111111";
const std::string RoomId = "0f0123456789abcdef0123456789abcd";
const std::string SecretText = "emd1:" + std::string(122, 'A');

// The bytes the helper is sent are the shapes its own tests read.
void Wire() {
	const Offer none;
	const auto act = [&](const Command& command, const Offer& offer = Offer()) { return ActionRequest(command, offer); };
	const auto wrapped = [](json action) { return json{{"op", "friends_act"}, {"bridge_id", "brg_1"}, {"act", std::move(action)}}; };
	EXPECT(act(Command::RequestCard("brg_1", "c1.abc", "Player 1's room")) == wrapped({{"action", "request"}, {"card", "c1.abc"}, {"label", "Player 1's room"}}));
	EXPECT(act(Command::RequestCode("brg_1", "4F2A-91CX")) == wrapped({{"action", "request"}, {"code", "4F2A-91CX"}}));
	EXPECT(act(Command::Accept("brg_1", RequestId)) == wrapped({{"action", "accept"}, {"request", RequestId}}));
	EXPECT(act(Command::Decline("brg_1", RequestId, false)) == wrapped({{"action", "decline"}, {"request", RequestId}}));
	EXPECT(act(Command::Decline("brg_1", RequestId, true)) == wrapped({{"action", "decline"}, {"request", RequestId}, {"block", true}}));
	EXPECT(act(Command::Cancel("brg_1", RequestId)) == wrapped({{"action", "cancel"}, {"request", RequestId}}));
	EXPECT(act(Command::Remove("brg_1", FriendId, false)) == wrapped({{"action", "remove"}, {"friend", FriendId}}));
	EXPECT(act(Command::Remove("brg_1", FriendId, true)) == wrapped({{"action", "remove"}, {"friend", FriendId}, {"block", true}}));
	EXPECT(act(Command::BlockCard("brg_1", "c1.abc", "Player 2")) == wrapped({{"action", "block"}, {"card", "c1.abc"}, {"label", "Player 2"}}));
	EXPECT(act(Command::Unblock("brg_1", "bl_1")) == wrapped({{"action", "unblock"}, {"block", "bl_1"}}));
	EXPECT(act(Command::NewCode("brg_1")) == wrapped({{"action", "new_code"}}));
	EXPECT(act(Command::AppearOffline("brg_1", true)) == wrapped({{"action", "appear_offline"}, {"on", true}}));
	EXPECT(act(Command::AppearOffline("brg_1", false)) == wrapped({{"action", "appear_offline"}, {"on", false}}));
	EXPECT(act(Command::Forget("brg_1")) == wrapped({{"action", "forget"}}));
	EXPECT(act(Command::Dismiss("brg_1", "iv_1")) == wrapped({{"action", "dismiss"}, {"invite", "iv_1"}}));
	Offer publicRoom; publicRoom.kind = Offer::Kind::PublicRoom; publicRoom.value = RoomId;
	Offer privateRoom; privateRoom.kind = Offer::Kind::PrivateRoom; privateRoom.value = SecretText;
	EXPECT(act(Command::Invite("brg_1", FriendId), publicRoom) == wrapped({{"action", "invite"}, {"friend", FriendId}, {"room", {{"room_id", RoomId}}}}));
	EXPECT(act(Command::Invite("brg_1", FriendId), privateRoom) == wrapped({{"action", "invite"}, {"friend", FriendId}, {"room", {{"secret", SecretText}}}}));
	// Nothing to offer is not sent; a command that is not an action is not a request.
	EXPECT(act(Command::Invite("brg_1", FriendId), none).is_null() && act(Command::Join("brg_1", FriendId)).is_null() &&
		act(Command::Visit("brg_1")).is_null() && act(Command::AnswerVisit("brg_1", 1, true)).is_null());

	Report menus, offline_play, closing, fighting, private_room, training;
	offline_play.kind = Report::Kind::OfflinePlay;
	closing.kind = Report::Kind::Offline;
	fighting.kind = Report::Kind::InRoom; fighting.roomId = RoomId; fighting.status = Status::Fighting; fighting.table = 0;
	private_room.kind = Report::Kind::InRoom; private_room.status = Status::Queued;
	training.kind = Report::Kind::InRoom; training.roomId = RoomId; training.status = Status::Training;
	EXPECT(menus.ToJson() == json({{"activity", "menus"}}) && offline_play.ToJson() == json({{"activity", "offline_play"}}) &&
		closing.ToJson() == json({{"activity", "offline"}}));
	EXPECT(fighting.ToJson() == json({{"activity", "room"}, {"room", {{"room_id", RoomId}, {"status", "fighting"}, {"table", 0}}}}));
	EXPECT(private_room.ToJson() == json({{"activity", "room"}, {"room", {{"private", true}, {"status", "queued"}}}}));
	EXPECT(training.ToJson() == json({{"activity", "room"}, {"room", {{"room_id", RoomId}, {"status", "training"}}}}));

	Listing listing;
	listing.name = "Player 1"; listing.country = "GB"; listing.main = 3;
	EXPECT(SyncRequest("brg_1", "b1", listing, fighting) == json({{"op", "friends_sync"}, {"bridge_id", "brg_1"}, {"sync", {
		{"build_id", "b1"}, {"listing", {{"name", "Player 1"}, {"country", "GB"}}}, {"main", 3},
		{"presence", {{"activity", "room"}, {"room", {{"room_id", RoomId}, {"status", "fighting"}, {"table", 0}}}}}}}}));
	listing.country.clear(); listing.main = -1;
	EXPECT(SyncRequest("brg_1", "b1", listing, menus) == json({{"op", "friends_sync"}, {"bridge_id", "brg_1"}, {"sync", {
		{"build_id", "b1"}, {"listing", {{"name", "Player 1"}}}, {"presence", {{"activity", "menus"}}}}}}));
}
}

int main() {
	FirstSync();
	Cadence();
	PresenceChange();
	OneAtATime();
	ActionAnswers();
	Refusals();
	NoFriends();
	Forgetting();
	ScopeChange();
	LostRequests();
	Commands();
	Wire();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	return TEST_EXIT_CODE();
}
