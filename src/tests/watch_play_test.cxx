// The watch state machine: asking the bridge for the match's room, joining it
// as an observer, asking on for as long as it watches, and leaving.
#include "../netplay/TournamentPlay.hxx"
#include "../netplay/WatchPlay.hxx"

#include <cstdio>
#include <string>
#include <vector>

using namespace sf4e::netplay::tournament;
using Kind = WatchOutput::Kind;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

static const std::string RoomA(32, 'a');
static const std::string RoomB(32, 'b');

static WatchReply Room(const std::string& roomId) {
	WatchReply reply;
	reply.role = WatchReply::Role::Room;
	reply.roomId = roomId;
	reply.invitation = "sf4e3:" + roomId;
	return reply;
}

static WatchReply Waiting(const std::string& reason, std::uint64_t retryAfterMs = 10000) {
	WatchReply reply;
	reply.reason = reason;
	reply.retryAfterMs = retryAfterMs;
	return reply;
}

static RoomView Joined(const std::string& roomId) {
	RoomView view;
	view.joined = true;
	view.roomId = roomId;
	return view;
}

static std::size_t Count(const std::vector<WatchOutput>& outputs, Kind kind) {
	std::size_t count = 0;
	for (const auto& output : outputs) count += output.kind == kind;
	return count;
}

static std::vector<WatchOutput> Tick(WatchPlay& play, std::uint64_t nowMs, const RoomView& view = {}) {
	return play.Tick(nowMs, view);
}

// It asks at once, and again only after the answer, every ten seconds or when
// the bridge says; a full match waits with its reason and a place ends it.
static void TestAsking() {
	WatchPlay play;
	CHECK(Tick(play, 0).empty() && play.GetPhase() == WatchPhase::Idle);
	play.Start("brg_1", "emt_1", 100);
	CHECK(play.GetPhase() == WatchPhase::Asking && play.BridgeId() == "brg_1" && play.MatchId() == "emt_1");
	CHECK(Count(Tick(play, 100), Kind::Watch) == 1);
	// Nothing more while the answer is out.
	CHECK(Tick(play, 100 + 60000).empty());
	play.OnReply(Waiting("not_bound"), 200);
	CHECK(play.GetPhase() == WatchPhase::Waiting && !play.Full());
	CHECK(Tick(play, 200 + WatchIntervalMs - 1).empty());
	CHECK(Count(Tick(play, 200 + WatchIntervalMs), Kind::Watch) == 1);
	// A longer wait the bridge asks for is kept, and so is a full match's reason.
	play.OnReply(Waiting("full", 30000), 20000);
	CHECK(play.GetPhase() == WatchPhase::Waiting && play.Full());
	CHECK(Tick(play, 49999).empty() && Count(Tick(play, 50000), Kind::Watch) == 1);
	// A room answer clears it and asks again in ten seconds.
	play.OnReply(Room(RoomA), 50100);
	CHECK(!play.Full());
	CHECK(Count(Tick(play, 50100 + WatchIntervalMs - 1), Kind::Watch) == 0);
	CHECK(Count(Tick(play, 50100 + WatchIntervalMs), Kind::Watch) == 1);
	// Never sooner than a second, whatever the bridge says.
	WatchPlay quick;
	quick.Start("brg_1", "emt_1", 0);
	Tick(quick, 0);
	quick.OnReply(Waiting("no_room", 1), 100);
	CHECK(Tick(quick, 1099).empty() && Count(Tick(quick, 1100), Kind::Watch) == 1);
}

// A room answer is joined once, as an observer would: with its invitation, not
// again while the join is open, and the phase follows the room.
static void TestJoiningTheRoom() {
	WatchPlay play;
	play.Start("brg_1", "emt_1", 0);
	Tick(play, 0);
	play.OnReply(Room(RoomA), 10);
	auto outputs = Tick(play, 20);
	CHECK(Count(outputs, Kind::Join) == 1 && outputs[0].invitation == "sf4e3:" + RoomA);
	CHECK(play.GetPhase() == WatchPhase::Joining);
	RoomView opening;
	opening.opening = true;
	CHECK(Count(Tick(play, 30, opening), Kind::Join) == 0 && Count(Tick(play, 9000, opening), Kind::Join) == 0);
	CHECK(Tick(play, 9500, Joined(RoomA)).empty() && play.GetPhase() == WatchPhase::Watching);
	// It keeps asking while it watches, which keeps the watcher live.
	play.OnReply(Room(RoomA), 9500);
	CHECK(Count(Tick(play, 9500 + WatchIntervalMs, Joined(RoomA)), Kind::Watch) == 1 && play.GetPhase() == WatchPhase::Watching);
}

// A join the room refused, or whose handshake closed because the bind that
// names the watcher has not landed, is tried again after ten seconds.
static void TestJoinIsRetried() {
	WatchPlay play;
	play.Start("brg_1", "emt_1", 0);
	Tick(play, 0);
	play.OnReply(Room(RoomA), 10);
	CHECK(Count(Tick(play, 1000), Kind::Join) == 1);
	// Closed `not admitted`: not in a room, nothing opening.
	CHECK(Count(Tick(play, 1000 + WatchIntervalMs - 1), Kind::Join) == 0);
	CHECK(Count(Tick(play, 1000 + WatchIntervalMs), Kind::Join) == 1);
	CHECK(Count(Tick(play, 1000 + WatchIntervalMs + 1), Kind::Join) == 0);
}

// A replaced room: the watcher leaves the old one and joins the new one.
static void TestFollowsAReplacedRoom() {
	WatchPlay play;
	play.Start("brg_1", "emt_1", 0);
	Tick(play, 0);
	play.OnReply(Room(RoomA), 10);
	Tick(play, 20);
	Tick(play, 30, Joined(RoomA));
	CHECK(play.GetPhase() == WatchPhase::Watching);
	Tick(play, WatchIntervalMs + 10, Joined(RoomA));
	play.OnReply(Room(RoomB), WatchIntervalMs + 20);
	auto outputs = Tick(play, WatchIntervalMs + 30, Joined(RoomA));
	CHECK(Count(outputs, Kind::Leave) == 1 && Count(outputs, Kind::Join) == 0);
	CHECK(Count(Tick(play, WatchIntervalMs + 40, Joined(RoomA)), Kind::Leave) == 0);
	outputs = Tick(play, WatchIntervalMs + 50);
	CHECK(Count(outputs, Kind::Join) == 1 && outputs[0].invitation == "sf4e3:" + RoomB);
	Tick(play, WatchIntervalMs + 60, Joined(RoomB));
	CHECK(play.GetPhase() == WatchPhase::Watching);
}

// Sent away by a rebind it asks again at once; a revocation ends the watch
// and says so, a new room is joined.
static void TestKickedAsksAtOnce() {
	WatchPlay play;
	play.Start("brg_1", "emt_1", 0);
	Tick(play, 0);
	play.OnReply(Room(RoomA), 10);
	Tick(play, 20);
	Tick(play, 30, Joined(RoomA));
	// Not for a second: its next ask is nine seconds off.
	CHECK(Tick(play, 100, Joined(RoomA)).empty());
	play.OnRoomEnded(WatchRoomEnd::Removed, 200);
	CHECK(play.GetPhase() == WatchPhase::Waiting && play.Notice() == WatchNotice::None);
	// The room it left is not joined again before the bridge says.
	auto outputs = Tick(play, 200);
	CHECK(Count(outputs, Kind::Watch) == 1 && Count(outputs, Kind::Join) == 0 && Count(outputs, Kind::Leave) == 0);
	play.OnFailure("revoked", 300);
	CHECK(play.GetPhase() == WatchPhase::Refused && play.Reason() == "revoked");
	CHECK(play.Notice() == WatchNotice::Removed && play.NoticeSequence() == 1);
	CHECK(Tick(play, 100000).empty() && !play.Active());
	// Sent away and then readmitted, it joins what the bridge names.
	play.Start("brg_1", "emt_1", 0);
	Tick(play, 0);
	play.OnReply(Room(RoomA), 10);
	Tick(play, 20);
	Tick(play, 30, Joined(RoomA));
	play.OnRoomEnded(WatchRoomEnd::Removed, 200);
	Tick(play, 200);
	play.OnReply(Room(RoomA), 250);
	CHECK(Count(Tick(play, 260), Kind::Join) == 1);
}

// A closed room is said once, and the bridge says what comes next.
static void TestRoomClosed() {
	WatchPlay play;
	play.Start("brg_1", "emt_1", 0);
	Tick(play, 0);
	play.OnReply(Room(RoomA), 10);
	Tick(play, 20);
	Tick(play, 30, Joined(RoomA));
	play.OnRoomEnded(WatchRoomEnd::Closed, 500);
	CHECK(play.Notice() == WatchNotice::RoomClosed && play.NoticeSequence() == 1 && play.Active());
	CHECK(Count(Tick(play, 500), Kind::Watch) == 1);
	// Nothing under way, nothing to say.
	WatchPlay idle;
	idle.OnRoomEnded(WatchRoomEnd::Closed, 5);
	CHECK(idle.Notice() == WatchNotice::None && idle.GetPhase() == WatchPhase::Idle);
}

// Stopping tells the bridge, then leaves the room; one that never asked says
// nothing, and a late answer after a stop changes nothing.
static void TestStop() {
	WatchPlay play;
	play.Start("brg_1", "emt_1", 0);
	Tick(play, 0);
	play.OnReply(Room(RoomA), 10);
	Tick(play, 20);
	Tick(play, 30, Joined(RoomA));
	play.Stop();
	auto outputs = Tick(play, 40, Joined(RoomA));
	CHECK(outputs.size() == 2 && outputs[0].kind == Kind::Unwatch && outputs[1].kind == Kind::Leave);
	CHECK(play.GetPhase() == WatchPhase::Idle && Tick(play, 100000).empty());
	play.OnReply(Room(RoomB), 100);
	play.OnFailure("revoked", 100);
	CHECK(play.GetPhase() == WatchPhase::Idle && Tick(play, 200000).empty());
	// Before the first ask there is nothing to tell the bridge, and no room to leave.
	WatchPlay early;
	early.Start("brg_1", "emt_1", 0);
	early.Stop();
	CHECK(Tick(early, 10).empty());
	// Waiting for a room: it tells the bridge but has no room to leave.
	WatchPlay waiting;
	waiting.Start("brg_1", "emt_1", 0);
	Tick(waiting, 0);
	waiting.OnReply(Waiting("no_room"), 10);
	waiting.Stop();
	outputs = Tick(waiting, 20);
	CHECK(outputs.size() == 1 && outputs[0].kind == Kind::Unwatch);
	// Stopping twice says it once.
	waiting.Stop();
	CHECK(Tick(waiting, 30).empty());
}

// Refused for good, a watcher stops; for a moment, it asks again.
static void TestFailures() {
	for (const char* code : {"not_allowed", "revoked", "fighter", "incompatible_build", "watch_unsupported", "not_found",
		"unsupported_rules", "forbidden", "bridge_not_approved"}) {
		WatchPlay play;
		play.Start("brg_1", "emt_1", 0);
		Tick(play, 0);
		play.OnFailure(code, 10);
		CHECK(play.GetPhase() == WatchPhase::Refused && play.Reason() == code && !play.Active());
		// Never in the room, so there is nothing to leave and nothing to tell the bridge.
		CHECK(Tick(play, 20).empty() && play.Notice() == WatchNotice::None);
	}
	for (const char* code : {"helper_unavailable", "rate_limited", "bridge_invalid_response", "unavailable", "bridge_unreachable"}) {
		WatchPlay play;
		play.Start("brg_1", "emt_1", 0);
		Tick(play, 0);
		play.OnFailure(code, 100);
		CHECK(play.Active() && Tick(play, 100 + WatchRetryMs - 1).empty());
		CHECK(Count(Tick(play, 100 + WatchRetryMs), Kind::Watch) == 1);
	}
	// Refused in the room leaves it at once.
	WatchPlay play;
	play.Start("brg_1", "emt_1", 0);
	Tick(play, 0);
	play.OnReply(Room(RoomA), 10);
	Tick(play, 20);
	Tick(play, 30, Joined(RoomA));
	play.OnFailure("not_allowed", 40);
	CHECK(play.Notice() == WatchNotice::Removed);
	const auto outputs = Tick(play, 50, Joined(RoomA));
	CHECK(outputs.size() == 1 && outputs[0].kind == Kind::Leave);
	// The helper gone ends the watch the same way, without a notice.
	WatchPlay lost;
	lost.Start("brg_1", "emt_1", 0);
	lost.Abandon("helper_lost");
	CHECK(lost.GetPhase() == WatchPhase::Refused && lost.Reason() == "helper_lost" && lost.Notice() == WatchNotice::None);
	WatchPlay idle;
	idle.Abandon("helper_lost");
	CHECK(idle.GetPhase() == WatchPhase::Idle);
}

// A match that is over is left once its last battle closes, or after the hold.
static void TestFinished() {
	for (const bool closes : {true, false}) {
		WatchPlay play;
		play.Start("brg_1", "emt_1", 0);
		Tick(play, 0);
		play.OnReply(Room(RoomA), 10);
		Tick(play, 20);
		Tick(play, 30, Joined(RoomA));
		play.OnFailure("finished", 40);
		CHECK(play.GetPhase() == WatchPhase::Finished && play.Notice() == WatchNotice::Finished && !play.Active());
		auto fighting = Joined(RoomA);
		fighting.fighting = true;
		CHECK(Tick(play, 50, fighting).empty());
		CHECK(Tick(play, 50 + FinalBattleHoldMs - 1, fighting).empty());
		if (closes) {
			CHECK(Count(Tick(play, 1000, Joined(RoomA)), Kind::Leave) == 1);
		} else {
			CHECK(Count(Tick(play, 50 + FinalBattleHoldMs, fighting), Kind::Leave) == 1);
		}
		CHECK(Tick(play, 200000, fighting).empty());
	}
	// Stopping a finished watch leaves at once, and does not tell a bridge that is done.
	WatchPlay play;
	play.Start("brg_1", "emt_1", 0);
	Tick(play, 0);
	play.OnReply(Room(RoomA), 10);
	Tick(play, 20);
	Tick(play, 30, Joined(RoomA));
	play.OnFailure("stale_revision", 40);
	CHECK(play.Reason() == "stale_revision" && play.GetPhase() == WatchPhase::Finished);
	play.Stop();
	const auto outputs = Tick(play, 50, Joined(RoomA));
	CHECK(outputs.size() == 1 && outputs[0].kind == Kind::Leave && play.GetPhase() == WatchPhase::Idle);
	// A match over before the watcher was ever in the room has no notice.
	WatchPlay early;
	early.Start("brg_1", "emt_1", 0);
	Tick(early, 0);
	early.OnFailure("finished", 10);
	CHECK(early.GetPhase() == WatchPhase::Finished && early.Notice() == WatchNotice::None && Tick(early, 20).empty());
}

// A new watch forgets the last one's room, notice and phase.
static void TestRestart() {
	WatchPlay play;
	play.Start("brg_1", "emt_1", 0);
	Tick(play, 0);
	play.OnReply(Room(RoomA), 10);
	Tick(play, 20);
	Tick(play, 30, Joined(RoomA));
	play.OnRoomEnded(WatchRoomEnd::Closed, 40);
	play.Stop();
	Tick(play, 50, Joined(RoomA));
	play.Start("brg_2", "emt_2", 1000);
	CHECK(play.GetPhase() == WatchPhase::Asking && play.BridgeId() == "brg_2" && play.MatchId() == "emt_2" && play.Notice() == WatchNotice::None);
	const auto outputs = Tick(play, 1000);
	CHECK(outputs.size() == 1 && outputs[0].kind == Kind::Watch);
}

// A room the player opened or joined by hand is not the match's. Whatever the
// bridge answers, and however the watch ends, it is not left, joined over or
// taken for the match's room; the watch ends and says why.
static const std::string HandRoom(32, 'c');

static RoomView Opening() {
	RoomView view;
	view.opening = true;
	return view;
}

static void TestNeverActsOnAForeignRoom() {
	// The sequence that tore a hand-made room down: a watch waits (no room yet),
	// the player hosts a room of their own, and the bridge then names the match's.
	WatchPlay play;
	play.Start("brg_1", "emt_1", 0);
	Tick(play, 0);
	play.OnReply(Waiting("no_room"), 10);
	CHECK(play.GetPhase() == WatchPhase::Waiting);
	CHECK(Tick(play, 20, Opening()).empty());
	// The room opens with guests in it. Nothing is said to the runtime but the bridge, once.
	auto outputs = Tick(play, 30, Joined(HandRoom));
	CHECK(Count(outputs, Kind::Leave) == 0 && Count(outputs, Kind::Join) == 0 && Count(outputs, Kind::Unwatch) == 1);
	CHECK(play.GetPhase() == WatchPhase::Refused && play.Reason() == "other_room" && !play.Active());
	// A late answer does not bring it back, and the room is never left.
	play.OnReply(Room(RoomA), 40);
	for (std::uint64_t now = 50; now < 200000; now += 5000) {
		outputs = Tick(play, now, Joined(HandRoom));
		CHECK(Count(outputs, Kind::Leave) == 0 && Count(outputs, Kind::Join) == 0 && Count(outputs, Kind::Watch) == 0);
	}
	// The same when the answer names the room before the hand-made one is seen.
	WatchPlay answered;
	answered.Start("brg_1", "emt_1", 0);
	Tick(answered, 0);
	answered.OnReply(Room(RoomA), 10);
	outputs = Tick(answered, 15, Joined(HandRoom));
	CHECK(Count(outputs, Kind::Leave) == 0 && Count(outputs, Kind::Join) == 0 && answered.GetPhase() == WatchPhase::Refused);
	CHECK(Tick(answered, 20, Joined(HandRoom)).empty());
	// Stopping a watch that is waiting, or has a room named but not joined, leaves nothing of the player's.
	for (const bool named : {false, true}) {
		WatchPlay stopped;
		stopped.Start("brg_1", "emt_1", 0);
		Tick(stopped, 0);
		stopped.OnReply(named ? Room(RoomA) : Waiting("not_bound"), 10);
		stopped.Stop();
		outputs = Tick(stopped, 20, Joined(HandRoom));
		CHECK(Count(outputs, Kind::Leave) == 0 && Count(outputs, Kind::Join) == 0);
		CHECK(Tick(stopped, 30, Joined(HandRoom)).empty());
	}
	// A watch that ended by itself (revoked while it waited to retry a join) leaves nothing either.
	WatchPlay revoked;
	revoked.Start("brg_1", "emt_1", 0);
	Tick(revoked, 0);
	revoked.OnReply(Room(RoomA), 10);
	CHECK(Count(Tick(revoked, 20), Kind::Join) == 1);
	revoked.OnFailure("revoked", 30);
	CHECK(Count(Tick(revoked, 40, Joined(HandRoom)), Kind::Leave) == 0);
	// What it did join it still leaves: stopped in the match's room, stopped while its join is
	// under way, and the match's old room once the bridge names another.
	WatchPlay own;
	own.Start("brg_1", "emt_1", 0);
	Tick(own, 0);
	own.OnReply(Room(RoomA), 10);
	Tick(own, 20);
	Tick(own, 30, Joined(RoomA));
	own.Stop();
	CHECK(Count(Tick(own, 40, Joined(RoomA)), Kind::Leave) == 1);
	WatchPlay joining;
	joining.Start("brg_1", "emt_1", 0);
	Tick(joining, 0);
	joining.OnReply(Room(RoomA), 10);
	Tick(joining, 20);
	joining.Stop();
	CHECK(Count(Tick(joining, 30, Opening()), Kind::Leave) == 1);
	// A join long abandoned (the handshake closed, the retry not yet due) is not one under way.
	WatchPlay stale;
	stale.Start("brg_1", "emt_1", 0);
	Tick(stale, 0);
	stale.OnReply(Room(RoomA), 10);
	Tick(stale, 20);
	stale.Stop();
	CHECK(Count(Tick(stale, 20 + WatchIntervalMs, Opening()), Kind::Leave) == 0);
}

int main() {
	TestAsking();
	TestJoiningTheRoom();
	TestJoinIsRetried();
	TestFollowsAReplacedRoom();
	TestKickedAsksAtOnce();
	TestRoomClosed();
	TestStop();
	TestFailures();
	TestFinished();
	TestRestart();
	TestNeverActsOnAForeignRoom();
	if (failures) std::printf("%d failure(s)\n", failures);
	else std::printf("watch play tests passed\n");
	return failures ? 1 : 0;
}
