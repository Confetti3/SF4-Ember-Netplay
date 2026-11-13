// The runtime's friends requests (netplay/FriendRequests.hxx): one request at a
// time, the cadence of syncs, a failed request that keeps the state, a service
// without friends, a change of service or Ember ID, requests the helper lost;
// and the exact requests the helper is sent (netplay/FriendWire.hxx).
#include "../netplay/FriendRequests.hxx"
#include "../netplay/FriendWire.hxx"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>
#include "test_support.hxx"

using namespace sf4e::netplay::friends;
using namespace sf4e::commands;
using nlohmann::json;

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
	requests.Finish(requests.BeginSync(at), "", Listed("AAAA-AAAA"), at);
	return requests;
}

// An answer of a sync sent at `sentAt`, came at `answeredAt`.
void Sync(Requests& requests, std::uint64_t sentAt, std::uint64_t answeredAt, const std::string& code = "AAAA-AAAA") {
	requests.Finish(requests.BeginSync(sentAt), "", Listed(code), answeredAt);
}

// Nothing is asked before a service is named, and then a sync is due at once.
void FirstSync() {
	Requests requests;
	EXPECT(!requests.SyncDue(0) && !requests.MayAct());
	requests.Scope("brg_1", "emb1_a");
	EXPECT(requests.SyncDue(0) && requests.MayAct() && !requests.Busy());
	const auto flight = requests.BeginSync(100);
	EXPECT(!requests.SyncDue(100) && !requests.MayAct() && requests.Awaiting(flight) && !requests.Awaiting(flight + 1) && !requests.Awaiting(0));
	EXPECT(requests.Published().loading && !requests.Published().loaded);
	// A sync in flight is not an action: nothing is Busy until one is asked.
	EXPECT(!requests.Busy());
	requests.Finish(flight, "", Listed("AAAA-AAAA"), 300);
	const auto& answered = requests.Published();
	EXPECT(!answered.loading && answered.loaded && answered.value->code == "AAAA-AAAA" && answered.revision == 1 && answered.finished == 1 && answered.installedMs == 300);
	// The answer of a flight that ended is nobody's.
	requests.Finish(flight, "", Listed("BBBB-BBBB"), 400);
	EXPECT(requests.Published().value->code == "AAAA-AAAA" && requests.Published().finished == 1);
}

// The game's own cadence is 30 s, the tab's 10 s, and no two syncs are less than 5 s apart.
void Cadence() {
	auto requests = Synced(1000);
	EXPECT(!requests.SyncDue(1000 + Requests::IdleSyncMs - 1) && requests.SyncDue(1000 + Requests::IdleSyncMs));
	// The tab opening asks at once, but not within the gap.
	requests.Visit(1000 + 2000);
	EXPECT(!requests.SyncDue(1000 + Requests::MinGapMs - 1) && requests.SyncDue(1000 + Requests::MinGapMs));
	Sync(requests, 6000, 6100);
	EXPECT(requests.Visiting() && !requests.SyncDue(6100 + Requests::ShownSyncMs - 1) && requests.SyncDue(6100 + Requests::ShownSyncMs));
	// Closing the tab goes back to the game's own.
	requests.Leave();
	EXPECT(!requests.Visiting());
	Sync(requests, 20000, 20000);
	EXPECT(!requests.SyncDue(20000 + Requests::IdleSyncMs - 1) && requests.SyncDue(20000 + Requests::IdleSyncMs));
	// A tab nobody said closed stops being one after a while.
	requests.Visit(30000);
	requests.LapseVisit(30000 + Requests::ShownLapseMs - 1);
	EXPECT(requests.Visiting());
	requests.LapseVisit(30000 + Requests::ShownLapseMs);
	EXPECT(!requests.Visiting());
}

// A change of the player's own presence asks at once, once; the same presence asks nothing.
void PresenceChange() {
	auto requests = Synced(1000);
	requests.NotePresence(0);
	EXPECT(!requests.SyncDue(1000 + Requests::MinGapMs));
	requests.NotePresence(7);
	EXPECT(!requests.SyncDue(1000 + Requests::MinGapMs - 1) && requests.SyncDue(1000 + Requests::MinGapMs));
	const auto flight = requests.BeginSync(6000);
	// A change while the sync is in flight is asked for as soon as it ends.
	requests.NotePresence(8);
	requests.Finish(flight, "", Listed("AAAA-AAAA"), 6500);
	EXPECT(requests.SyncDue(6000 + Requests::MinGapMs));
	Sync(requests, 11000, 11000);
	requests.NotePresence(8);
	EXPECT(!requests.SyncDue(11000 + Requests::MinGapMs));
}

// One request is in flight; one action asked meanwhile is held and goes after it.
void OneAtATime() {
	auto requests = Synced(1000);
	const auto flight = requests.BeginSync(40000);
	const FriendAccept accept{"brg_1", "rq_a"};
	EXPECT(!requests.MayAct() && requests.Hold(accept) && requests.Busy());
	EXPECT(!requests.Hold(FriendDecline{"brg_1", "rq_b", true}));
	// Held, no sync is due ahead of it.
	EXPECT(!requests.SyncDue(1000000) && !requests.TakeHeld());
	requests.Finish(flight, "", Listed("AAAA-AAAA"), 40100);
	auto held = requests.TakeHeld();
	EXPECT(held && std::holds_alternative<FriendAccept>(*held) && std::get<FriendAccept>(*held).request == "rq_a" && !requests.TakeHeld() && !requests.Busy());
	EXPECT(requests.MayAct());
	const auto action = requests.BeginAction();
	EXPECT(requests.Busy() && !requests.MayAct());
	EXPECT(requests.Awaiting(action) && !requests.SyncDue(1000000));
}

// An action's answer is the state; a sync follows it shortly, and never inside the gap.
void ActionAnswers() {
	auto requests = Synced(1000);
	// The last sync is well before the action, so only the settle decides when the next is due.
	Sync(requests, 20000, 20100);
	requests.Finish(requests.BeginAction(), "", Listed("BBBB-BBBB"), 31200);
	EXPECT(requests.Published().value->code == "BBBB-BBBB" && requests.Published().revision == 3 && requests.Refusal().empty());
	EXPECT(!requests.Busy() && !requests.SyncDue(31200 + Requests::SettleMs - 1) && requests.SyncDue(31200 + Requests::SettleMs));
	// The action's own settle never brings a sync inside the gap of the last one.
	Sync(requests, 40000, 40000, "BBBB-BBBB");
	requests.Finish(requests.BeginAction(), "", Listed("BBBB-BBBB"), 40200);
	EXPECT(!requests.SyncDue(40000 + Requests::MinGapMs - 1) && requests.SyncDue(40000 + Requests::MinGapMs));
}

// A refusal is kept, with the state, until the next action starts.
void Refusals() {
	auto requests = Synced(1000);
	requests.Finish(requests.BeginAction(), "unknown_code", std::nullopt, 2100);
	EXPECT(requests.Refusal() == "unknown_code" && requests.Refusals() == 1 && !requests.Busy());
	EXPECT(requests.Published().value->code == "AAAA-AAAA" && requests.Published().revision == 1 && requests.Published().error == "unknown_code");
	const auto next = requests.BeginAction();
	EXPECT(requests.Refusal().empty() && requests.Refusals() == 1);
	requests.Finish(next, "request_limit", std::nullopt, 3100);
	EXPECT(requests.Refusal() == "request_limit" && requests.Refusals() == 2);
	requests.Finish(requests.BeginAction(), "", Listed("AAAA-AAAA"), 4100);
	EXPECT(requests.Refusal().empty() && requests.Refusals() == 2);
	// A refused sync is not an action's refusal; it only keeps its code.
	requests.Finish(requests.BeginSync(50000), "rate_limited", std::nullopt, 50100);
	EXPECT(requests.Refusal().empty() && requests.Published().error == "rate_limited" && requests.Published().value->code == "AAAA-AAAA");
	EXPECT(!requests.SyncDue(50100 + Requests::IdleSyncMs - 1) && requests.SyncDue(50100 + Requests::IdleSyncMs));
	// An action the runtime could not send is counted like a refusal, without a flight.
	requests.Refuse("nothing_to_offer");
	EXPECT(requests.Refusal() == "nothing_to_offer" && requests.Refusals() == 3 && !requests.Busy() && requests.MayAct());
}

// A service without friends and a player without an account are states, and stop the syncs until the tab opens.
void NoFriends() {
	for (const char* code : {Requests::Unavailable, Requests::NoAccount}) {
		Requests requests;
		requests.Scope("brg_1", "emb1_a");
		requests.Finish(requests.BeginSync(0), code, std::nullopt, 10);
		const bool unavailable = std::string(code) == Requests::Unavailable;
		EXPECT(requests.Published().loaded && requests.Published().revision == 1 &&
			requests.Published().value->kind == (unavailable ? State::Kind::Unavailable : State::Kind::NoAccount));
		EXPECT(!requests.SyncDue(10000000));
		requests.NotePresence(3);
		EXPECT(!requests.SyncDue(10000000));
		requests.Visit(20000);
		EXPECT(requests.SyncDue(20000));
		Sync(requests, 25000, 25100);
		EXPECT(requests.Published().value->kind == State::Kind::Listed && !requests.SyncDue(25100 + Requests::ShownSyncMs - 1) &&
			requests.SyncDue(25100 + Requests::ShownSyncMs));
	}
	// An action's refusal for the same reason says so too.
	Requests requests = Synced(0);
	requests.Finish(requests.BeginAction(), Requests::NoAccount, std::nullopt, 200);
	EXPECT(requests.Published().value->kind == State::Kind::NoAccount && requests.Refusal() == Requests::NoAccount && !requests.SyncDue(10000000));
}

// Friends the bridge says are off are a state, to a sync or to any action: the syncs stop, nothing is refused, and
// only a state (Turn on's answer) ends it, however the scope is set meanwhile.
void TurningOff() {
	// The Turn off's own answer.
	auto requests = Synced(1000);
	requests.Finish(requests.BeginAction(), Requests::Off, std::nullopt, 2100);
	EXPECT(requests.Published().value->kind == State::Kind::Off && requests.Published().error.empty() && requests.Refusal().empty() && requests.Refusals() == 0);
	EXPECT(!requests.SyncDue(10000000) && !requests.Syncing());
	// Not the tab opening, nor the presence changing, asks again.
	requests.Visit(3000000);
	requests.NotePresence(9);
	EXPECT(!requests.SyncDue(3000000) && requests.Published().value->kind == State::Kind::Off);
	// Nor a change of scope away and back to the same account, as when the room object is gone for a moment.
	requests.Scope({}, {});
	EXPECT(requests.Published().value->kind == State::Kind::Unavailable);
	requests.Scope("brg_1", "emb1_a");
	EXPECT(requests.Published().value->kind == State::Kind::Off && !requests.SyncDue(20000000));
	// Another account is its own: it syncs, and may be off or on.
	requests.Scope("brg_1", "emb1_b");
	EXPECT(requests.Published().value->kind == State::Kind::Unavailable && requests.SyncDue(0));
	requests.Scope("brg_1", "emb1_a");
	EXPECT(requests.Published().value->kind == State::Kind::Off);
	// A Turn on whose answer a change of scope drops: back on the account, the
	// bridge is asked again rather than Off shown from memory.
	{
		auto dropped = requests;
		dropped.TurningOn();
		dropped.BeginAction();
		dropped.Scope("brg_1", "emb1_b");
		dropped.Scope("brg_1", "emb1_a");
		EXPECT(dropped.Published().value->kind == State::Kind::Unavailable && dropped.SyncDue(0));
	}
	// Turn on is an action whose answer is a state.
	EXPECT(requests.MayAct());
	requests.Finish(requests.BeginAction(), "", Listed("CCCC-CCCC"), 4000000);
	EXPECT(requests.Published().value->kind == State::Kind::Listed && requests.Published().value->code == "CCCC-CCCC" && requests.Syncing());
	EXPECT(requests.SyncDue(4000000 + Requests::SettleMs));
	// And they stay on when the scope is set again.
	requests.Scope({}, {});
	requests.Scope("brg_1", "emb1_a");
	EXPECT(requests.Published().value->kind == State::Kind::Unavailable && requests.SyncDue(0));

	// Any request may find them off: a sync at the next launch, an action made on a list that was current.
	Requests launch;
	launch.Scope("brg_1", "emb1_a");
	launch.Finish(launch.BeginSync(0), Requests::Off, std::nullopt, 10);
	EXPECT(launch.Published().value->kind == State::Kind::Off && !launch.SyncDue(10000000) && launch.Published().error.empty());
	auto acting = Synced(0);
	acting.Finish(acting.BeginAction(), Requests::Off, std::nullopt, 100);
	EXPECT(acting.Published().value->kind == State::Kind::Off && acting.Refusals() == 0 && !acting.SyncDue(10000000));
}

// Another service or Ember ID forgets everything of the last one; the counts carry on.
void ScopeChange() {
	auto requests = Synced(1000);
	requests.Finish(requests.BeginAction(), "own_card", std::nullopt, 2100);
	const auto stale = requests.BeginSync(40000);
	requests.Hold(FriendAccept{"brg_1", "rq_a"});
	EXPECT(!requests.Scope("brg_1", "emb1_a"));
	EXPECT(requests.Awaiting(stale) && requests.Published().loaded && requests.Busy());
	// The action held is dropped with the scope, and refused so that whoever asked is told.
	EXPECT(requests.Scope("brg_1", "emb1_b"));
	const auto& answered = requests.Published();
	EXPECT(!answered.loaded && !answered.loading && answered.value->kind == State::Kind::Unavailable && answered.revision == 1 && answered.finished == 2);
	EXPECT(!requests.Awaiting(stale) && !requests.Busy() && requests.MayAct() && requests.SyncDue(45000));
	EXPECT(requests.Refusal() == Requests::Dropped && requests.Refusals() == 2);
	Sync(requests, 45000, 45010, "DDDD-DDDD");
	EXPECT(answered.revision == 2 && answered.finished == 3);
	// The answer of the retired request is nobody's.
	requests.Finish(stale, "", Listed("EEEE-EEEE"), 46000);
	EXPECT(answered.revision == 2 && requests.Published().value->code == "DDDD-DDDD");
	// An action in flight is dropped and refused alike; a change with nothing asked refuses nothing.
	const auto lost = requests.BeginAction();
	EXPECT(requests.Scope("brg_2", "emb1_b") && !requests.Awaiting(lost) && !requests.Busy());
	EXPECT(requests.Refusal() == Requests::Dropped && requests.Refusals() == 3);
	requests.Finish(lost, "", Listed("FFFF-FFFF"), 47000);
	EXPECT(requests.Refusals() == 3 && requests.Published().value->kind == State::Kind::Unavailable);
	const auto syncing = requests.BeginSync(50000);
	EXPECT(!requests.Scope("brg_1", "emb1_b") && !requests.Awaiting(syncing));
	EXPECT(requests.Refusal().empty() && requests.Refusals() == 3);
}

// What is an action is a property of its type, and the commands state their bounds.
void Commands() {
	static_assert(IsFriendAction<FriendRequest> && IsFriendAction<FriendRequestByCode> && IsFriendAction<FriendAccept> && IsFriendAction<FriendDecline> &&
		IsFriendAction<FriendCancel> && IsFriendAction<FriendRemove> && IsFriendAction<FriendBlock> && IsFriendAction<FriendUnblock> &&
		IsFriendAction<FriendNewCode> && IsFriendAction<FriendAppearOffline> && IsFriendAction<FriendTurnOff> && IsFriendAction<FriendTurnOn> &&
		IsFriendAction<FriendInvite> && IsFriendAction<FriendDismiss>, "the bridge's actions");
	static_assert(!IsFriendAction<FriendsVisit> && !IsFriendAction<FriendsLeave> && !IsFriendAction<FriendJoin> && !IsFriendAction<FriendWatch> &&
		!IsFriendAction<FriendTakeInvite> && !IsFriendAction<FriendAnswerVisit>, "going to a friend asks the bridge nothing");
	static_assert(IsFriendVisit<FriendJoin> && IsFriendVisit<FriendsVisit> && !IsFriendVisit<FriendAccept>, "the others");
	FriendAccept longId{"b", std::string(65, 'x')};
	FriendRequestByCode longCode{"b", std::string(33, 'x')};
	FriendsVisit longBridge{std::string(257, 'b')}, maxBridge{std::string(256, 'b')};
	EXPECT(!longId.Valid() && !longCode.Valid() && !longBridge.Valid() && maxBridge.Valid());
	FriendsVisit short_{"brg"};
	FriendRequest card{"b", "cc", "lll"};
	EXPECT(short_.Bytes() == 3 && card.Bytes() == 6);
	EXPECT(HasBounds<FriendJoin> && HasBounds<FriendTurnOn> && BridgeOf(FriendAction(FriendNewCode{"brg_1"})) == std::string("brg_1"));
}

const std::string FriendId = "fr_11111111-1111-4111-8111-111111111111";
const std::string RequestId = "rq_11111111-1111-4111-8111-111111111111";
const std::string RoomId = "0f0123456789abcdef0123456789abcd";
const std::string SecretText = "emd1:" + std::string(122, 'A');

// The bytes the helper is sent are the shapes its own tests read.
void Wire() {
	const Offer none;
	const auto act = [&](const FriendAction& action, const Offer& offer = Offer()) { return ActionRequest(action, offer); };
	const auto wrapped = [](json action) { return json{{"op", "friends_act"}, {"bridge_id", "brg_1"}, {"act", std::move(action)}}; };
	EXPECT(act(FriendRequest{"brg_1", "c1.abc", "Player 1's room"}) == wrapped({{"action", "request"}, {"card", "c1.abc"}, {"label", "Player 1's room"}}));
	EXPECT(act(FriendRequestByCode{"brg_1", "4F2A-91CX"}) == wrapped({{"action", "request"}, {"code", "4F2A-91CX"}}));
	EXPECT(act(FriendAccept{"brg_1", RequestId}) == wrapped({{"action", "accept"}, {"request", RequestId}}));
	EXPECT(act(FriendDecline{"brg_1", RequestId, false}) == wrapped({{"action", "decline"}, {"request", RequestId}}));
	EXPECT(act(FriendDecline{"brg_1", RequestId, true}) == wrapped({{"action", "decline"}, {"request", RequestId}, {"block", true}}));
	EXPECT(act(FriendCancel{"brg_1", RequestId}) == wrapped({{"action", "cancel"}, {"request", RequestId}}));
	EXPECT(act(FriendRemove{"brg_1", FriendId, false}) == wrapped({{"action", "remove"}, {"friend", FriendId}}));
	EXPECT(act(FriendRemove{"brg_1", FriendId, true}) == wrapped({{"action", "remove"}, {"friend", FriendId}, {"block", true}}));
	EXPECT(act(FriendBlock{"brg_1", "c1.abc", "Player 2"}) == wrapped({{"action", "block"}, {"card", "c1.abc"}, {"label", "Player 2"}}));
	EXPECT(act(FriendUnblock{"brg_1", "bl_1"}) == wrapped({{"action", "unblock"}, {"block", "bl_1"}}));
	EXPECT(act(FriendNewCode{"brg_1"}) == wrapped({{"action", "new_code"}}));
	EXPECT(act(FriendAppearOffline{"brg_1", true}) == wrapped({{"action", "appear_offline"}, {"on", true}}));
	EXPECT(act(FriendAppearOffline{"brg_1", false}) == wrapped({{"action", "appear_offline"}, {"on", false}}));
	EXPECT(act(FriendTurnOff{"brg_1"}) == wrapped({{"action", "forget"}}));
	EXPECT(act(FriendTurnOn{"brg_1"}) == wrapped({{"action", "turn_on"}}));
	EXPECT(act(FriendDismiss{"brg_1", "iv_1"}) == wrapped({{"action", "dismiss"}, {"invite", "iv_1"}}));
	Offer publicRoom; publicRoom.kind = Offer::Kind::PublicRoom; publicRoom.value = RoomId;
	Offer privateRoom; privateRoom.kind = Offer::Kind::PrivateRoom; privateRoom.value = SecretText;
	EXPECT(act(FriendInvite{"brg_1", FriendId}, publicRoom) == wrapped({{"action", "invite"}, {"friend", FriendId}, {"room", {{"room_id", RoomId}}}}));
	EXPECT(act(FriendInvite{"brg_1", FriendId}, privateRoom) == wrapped({{"action", "invite"}, {"friend", FriendId}, {"room", {{"secret", SecretText}}}}));
	// Nothing to offer is not sent.
	EXPECT(act(FriendInvite{"brg_1", FriendId}, none).is_null());

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
	ScopeChange();
	TurningOff();
	Commands();
	Wire();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	return TEST_EXIT_CODE();
}
