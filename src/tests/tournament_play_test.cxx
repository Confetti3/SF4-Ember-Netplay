// The tournament play state machine: claiming, hosting or joining the match's
// room, binding it, fetching each game's permit, and reporting games.
#include "../netplay/TournamentPlay.hxx"
#include "../netplay/OwedAbort.hxx"

#include <cstdio>
#include <string>

using namespace sf4e;
using namespace sf4e::netplay::tournament;
using Kind = Output::Kind;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

static const std::string RoomA(32, 'a');
static const std::string RoomB(32, 'b');

static Binding MakeBinding(const std::string& roomId, int slot, std::uint64_t revision = 1) {
	Binding binding;
	binding.roomId = roomId;
	binding.localSlot = slot;
	binding.room.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
	binding.room.assignmentGeneration = 1;
	binding.room.bindingRevision = revision;
	binding.room.gamesToWin = 2;
	binding.room.fighters[0] = {std::string(64, 'a'), "emb1_aaaa"};
	binding.room.fighters[1] = {std::string(64, 'b'), "emb1_bbbb"};
	return binding;
}

static ClaimReply Room(const std::string& roomId, std::optional<Binding> binding) {
	ClaimReply reply;
	reply.role = ClaimReply::Role::Room;
	reply.roomId = roomId;
	reply.invitation = "sf4e3:" + roomId;
	reply.binding = std::move(binding);
	return reply;
}

static std::size_t Count(const std::vector<Output>& outputs, Kind kind) {
	std::size_t count = 0;
	for (const auto& output : outputs) count += output.kind == kind;
	return count;
}

static const Output* Find(const std::vector<Output>& outputs, Kind kind) {
	for (const auto& output : outputs) if (output.kind == kind) return &output;
	return nullptr;
}

static RoomView Joined(const std::string& roomId, const room::Snapshot* snapshot, bool writable = true) {
	RoomView view;
	view.joined = true;
	view.roomId = roomId;
	view.invitation = "sf4e3:" + roomId;
	view.authorityWritable = writable;
	view.snapshot = snapshot;
	return view;
}

static RoomView Opening() {
	RoomView view;
	view.opening = true;
	return view;
}

// The join of the room the bridge named goes out. A match acts only on rooms
// it opened, so a test puts it in the match's room this way.
static void JoinNamed(TournamentPlay& play, std::uint64_t now) {
	CHECK(Count(play.Tick(now, {}), Kind::Join) == 1);
}

// The first fighter to claim hosts, publishes its room with the lease, and,
// leading the room, binds it once.
static void TestHostPublishesAndBinds() {
	TournamentPlay play;
	play.Start("brg_x", "emt_x", 0);
	auto out = play.Tick(0, {});
	CHECK(Count(out, Kind::Claim) == 1);
	CHECK(play.Tick(1, {}).empty());
	ClaimReply lease;
	lease.role = ClaimReply::Role::Host;
	lease.leaseId = "lse_1";
	lease.fence = "1";
	play.OnRoom(Kind::Claim, lease, 100);
	out = play.Tick(200, {});
	CHECK(Count(out, Kind::Host) == 1 && play.GetPhase() == Phase::Opening);
	RoomView opening;
	opening.opening = true;
	CHECK(play.Tick(300, opening).empty());
	room::Snapshot snapshot;
	out = play.Tick(400, Joined(RoomA, &snapshot));
	const auto* publish = Find(out, Kind::Publish);
	CHECK(publish && publish->roomId == RoomA && publish->leaseId == "lse_1" && publish->fence == "1");
	// One publish in flight at a time.
	CHECK(Count(play.Tick(500, Joined(RoomA, &snapshot)), Kind::Publish) == 0);
	play.OnRoom(Kind::Publish, Room(RoomA, MakeBinding(RoomA, 0)), 600);
	out = play.Tick(700, Joined(RoomA, &snapshot));
	const auto* bind = Find(out, Kind::Bind);
	CHECK(bind && bind->binding.bindingRevision == 1 && play.GetPhase() == Phase::InRoom);
	snapshot.tournament = MakeBinding(RoomA, 0).room;
	CHECK(Count(play.Tick(800, Joined(RoomA, &snapshot)), Kind::Bind) == 0);
	// A newer binding is applied too.
	play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 0, 2)), 900);
	CHECK(Count(play.Tick(1000, Joined(RoomA, &snapshot)), Kind::Bind) == 1);
	// The room's renewed invitation goes to the bridge, without a lease.
	auto renewed = Joined(RoomA, &snapshot);
	renewed.invitation = "sf4e3:renewed";
	const auto refresh = play.Tick(1100, renewed);
	const auto* again = Find(refresh, Kind::Publish);
	CHECK(again && again->invitation == "sf4e3:renewed" && again->leaseId.empty());
}

// A casual game under way at the match's table holds the room's first
// binding back, so the game does not offer it, and the room builds no
// checkpoint for a binding it would refuse, until that game ends. It is
// offered on the first tick after.
static void TestACasualGameHoldsTheFirstBinding() {
	TournamentPlay play;
	play.Start("brg_x", "emt_x", 0);
	play.Tick(0, {});
	ClaimReply lease;
	lease.role = ClaimReply::Role::Host;
	lease.leaseId = "lse_1";
	lease.fence = "1";
	play.OnRoom(Kind::Claim, lease, 100);
	play.Tick(200, {});
	room::Snapshot snapshot;
	play.Tick(400, Joined(RoomA, &snapshot));
	play.OnRoom(Kind::Publish, Room(RoomA, MakeBinding(RoomA, 0)), 600);
	auto& table = snapshot.tables[room::TournamentTable];
	std::uint64_t now = 700;
	for (const auto phase : {room::TablePhase::Ready, room::TablePhase::Playing, room::TablePhase::Paused}) {
		table.phase = phase;
		for (int tick = 0; tick < 4; ++tick, now += 700) CHECK(Count(play.Tick(now, Joined(RoomA, &snapshot)), Kind::Bind) == 0);
	}
	table.phase = room::TablePhase::Idle;
	CHECK(Count(play.Tick(now, Joined(RoomA, &snapshot)), Kind::Bind) == 1);
	// A newer binding for a room already bound is still offered mid-game: the
	// room keeps the score played and only swaps the fighters' endpoints.
	snapshot.tournament = MakeBinding(RoomA, 0).room;
	table.phase = room::TablePhase::Playing;
	play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 0, 2)), now + 100);
	CHECK(Count(play.Tick(now + 200, Joined(RoomA, &snapshot)), Kind::Bind) == 1);
}

// The second fighter waits, then joins the room the bridge names; it does not
// bind the room while another game leads it.
static void TestGuestJoins() {
	TournamentPlay play;
	play.Start("brg_x", "emt_x", 0);
	play.Tick(0, {});
	ClaimReply wait;
	wait.role = ClaimReply::Role::Wait;
	wait.retryAfterMs = 2000;
	play.OnRoom(Kind::Claim, wait, 0);
	CHECK(Count(play.Tick(1000, {}), Kind::Claim) == 0);
	CHECK(play.WaitingForOpponent());
	CHECK(Count(play.Tick(2000, {}), Kind::Claim) == 1);
	play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 1)), 2100);
	auto out = play.Tick(2200, {});
	const auto* join = Find(out, Kind::Join);
	CHECK(join && join->invitation == "sf4e3:" + RoomA);
	room::Snapshot snapshot;
	CHECK(Count(play.Tick(2300, Joined(RoomA, &snapshot, false)), Kind::Bind) == 0);
	CHECK(play.GetPhase() == Phase::InRoom);
}

// A room this game hosted while the lease moved on is left for the match's room.
static void TestLeaseMoved() {
	TournamentPlay play;
	play.Start("brg_x", "emt_x", 0);
	play.Tick(0, {});
	ClaimReply lease;
	lease.role = ClaimReply::Role::Host;
	lease.leaseId = "lse_1";
	lease.fence = "1";
	play.OnRoom(Kind::Claim, lease, 50);
	CHECK(Count(play.Tick(60, {}), Kind::Host) == 1);
	room::Snapshot snapshot;
	play.Tick(70, Opening());
	play.Tick(80, Joined(RoomA, &snapshot));
	play.OnRoom(Kind::Claim, Room(RoomB, std::nullopt), 100);
	auto out = play.Tick(200, Joined(RoomA, &snapshot));
	CHECK(Count(out, Kind::Leave) == 1 && Count(out, Kind::Join) == 0);
	CHECK(Count(play.Tick(300, Joined(RoomA, &snapshot)), Kind::Leave) == 0);
	out = play.Tick(400, {});
	CHECK(Count(out, Kind::Join) == 1);
}

// A room the player opened or joined by hand is the player's own: whatever the
// bridge answers and however the match ends, it is never left, joined over or
// published. The match waits, and opens its room once the player has left.
static void TestNeverActsOnAForeignRoom() {
	static const std::string HandRoom(32, 'c');
	room::Snapshot snapshot;
	const auto hand = Joined(HandRoom, &snapshot);
	// The claim names the match's room while the player sits in theirs (the hole
	// that left a hand-opened room).
	TournamentPlay named;
	named.Start("brg_x", "emt_x", 0);
	named.Tick(0, {});
	named.OnRoom(Kind::Claim, Room(RoomB, MakeBinding(RoomB, 0)), 100);
	for (std::uint64_t now = 200; now < 100000; now += 5000) {
		const auto out = named.Tick(now, hand);
		CHECK(Count(out, Kind::Leave) == 0 && Count(out, Kind::Join) == 0 && Count(out, Kind::Bind) == 0);
	}
	CHECK(named.GetPhase() == Phase::Claiming);
	CHECK(Count(named.Tick(100000, {}), Kind::Join) == 1);
	// Holding the lease, it neither hosts over the player's room nor publishes it.
	TournamentPlay leased;
	leased.Start("brg_x", "emt_x", 0);
	leased.Tick(0, {});
	ClaimReply lease;
	lease.role = ClaimReply::Role::Host;
	lease.leaseId = "lse_1";
	leased.OnRoom(Kind::Claim, lease, 100);
	auto out = leased.Tick(200, hand);
	CHECK(Count(out, Kind::Host) == 0 && Count(out, Kind::Publish) == 0 && Count(out, Kind::Leave) == 0);
	// Ended in any way, it leaves nothing of the player's.
	for (const char* code : {"stale_revision", "not_found"}) {
		TournamentPlay ended;
		ended.Start("brg_x", "emt_x", 0);
		ended.Tick(0, {});
		ended.OnRoom(Kind::Claim, Room(RoomB, std::nullopt), 100);
		ended.Tick(200, hand);
		ended.OnFailure(Kind::Claim, code, 300);
		CHECK(Count(ended.Tick(400, hand), Kind::Leave) == 0);
		CHECK(Count(ended.Tick(400 + FinalBattleHoldMs, hand), Kind::Leave) == 0);
	}
	TournamentPlay stopped;
	stopped.Start("brg_x", "emt_x", 0);
	stopped.Tick(0, {});
	stopped.OnRoom(Kind::Claim, Room(RoomB, std::nullopt), 100);
	stopped.Stop();
	CHECK(Count(stopped.Tick(200, hand), Kind::Leave) == 0);
	// A join that failed is not under way: the room that opens next is not the match's.
	TournamentPlay failed;
	failed.Start("brg_x", "emt_x", 0);
	failed.Tick(0, {});
	failed.OnRoom(Kind::Claim, Room(RoomB, std::nullopt), 100);
	JoinNamed(failed, 200);
	failed.Tick(300, {});
	failed.Abandon("helper_lost");
	CHECK(Count(failed.Tick(400, Opening()), Kind::Leave) == 0);
}

// The permit for a reserved game is fetched, told to the room until the
// room shows it, and the game reported once it is decided.
static void TestPermitsAndReports() {
	TournamentPlay play;
	play.Start("brg_x", "emt_x", 0);
	play.Tick(0, {});
	play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 1)), 0);
	JoinNamed(play, 50);
	room::Snapshot snapshot;
	snapshot.tournament = MakeBinding(RoomA, 1).room;
	auto& table = snapshot.tables[room::TournamentTable];
	table.phase = room::TablePhase::Ready;
	table.permitGeneration = 7;
	auto out = play.Tick(100, Joined(RoomA, &snapshot, false));
	const auto* prepare = Find(out, Kind::Prepare);
	CHECK(prepare && prepare->generation == 7 && play.WaitingForPermit());
	CHECK(Count(play.Tick(200, Joined(RoomA, &snapshot, false)), Kind::Prepare) == 0);
	PrepareReply pending;
	pending.retryAfterMs = 2000;
	play.OnPrepare(pending, 300);
	CHECK(Count(play.Tick(1000, Joined(RoomA, &snapshot, false)), Kind::Prepare) == 0);
	CHECK(Count(play.Tick(2300, Joined(RoomA, &snapshot, false)), Kind::Prepare) == 1);
	PrepareReply permitted;
	permitted.permitted = true;
	permitted.generation = 7;
	permitted.permitId = "per_x";
	permitted.startWindowMs = 90000;
	play.OnPrepare(permitted, 2400);
	out = play.Tick(2500, Joined(RoomA, &snapshot, false));
	// The room hears the permit's start window with it.
	const auto* ready = Find(out, Kind::PermitReady);
	CHECK(ready && ready->generation == 7 && ready->permitId == "per_x" && ready->startWindowMs == 90000);
	CHECK(Count(play.Tick(3000, Joined(RoomA, &snapshot, false)), Kind::PermitReady) == 0);
	CHECK(Count(play.Tick(4600, Joined(RoomA, &snapshot, false)), Kind::PermitReady) == 1);
	table.permits[1] = "per_x";
	CHECK(Count(play.Tick(7000, Joined(RoomA, &snapshot, false)), Kind::PermitReady) == 0);
	// The game starts and ends.
	table.phase = room::TablePhase::Playing;
	table.matchGeneration = 7;
	table.permitGeneration = 0;
	table.permits = {};
	CHECK(Count(play.Tick(8000, Joined(RoomA, &snapshot, false)), Kind::Report) == 0);
	play.OnTerminal(7, room::MatchResult::P2Win, 900, 899);
	play.OnTerminal(7, room::MatchResult::P2Win, 900, 899);
	out = play.Tick(9000, Joined(RoomA, &snapshot, false));
	const auto* report = Find(out, Kind::Report);
	CHECK(Count(out, Kind::Report) == 1 && report->result == "p2_win" && report->captureFrame == 900 && report->confirmedFrame == 899);
	// A game this fighter held no permit for is not reported.
	play.OnTerminal(8, room::MatchResult::P1Win, 1, 1);
	CHECK(Count(play.Tick(9100, Joined(RoomA, &snapshot, false)), Kind::Report) == 0);
}

// A permit for a start that was called off is reported cancelled.
static void TestCancelledStart() {
	TournamentPlay play;
	play.Start("brg_x", "emt_x", 0);
	play.Tick(0, {});
	play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 0)), 0);
	JoinNamed(play, 50);
	room::Snapshot snapshot;
	snapshot.tournament = MakeBinding(RoomA, 0).room;
	auto& table = snapshot.tables[room::TournamentTable];
	table.phase = room::TablePhase::Ready;
	table.permitGeneration = 5;
	play.Tick(100, Joined(RoomA, &snapshot, false));
	PrepareReply permitted;
	permitted.permitted = true;
	permitted.generation = 5;
	permitted.permitId = "per_y";
	play.OnPrepare(permitted, 200);
	play.Tick(300, Joined(RoomA, &snapshot, false));
	// The hold timed out: the table is back to waiting and generation 5 is gone.
	table.phase = room::TablePhase::Waiting;
	table.permitGeneration = 0;
	auto out = play.Tick(400, Joined(RoomA, &snapshot, false));
	const auto* cancel = Find(out, Kind::Report);
	CHECK(cancel && cancel->generation == 5 && cancel->result == "cancel" && cancel->captureFrame == 0);
	CHECK(Count(play.Tick(500, Joined(RoomA, &snapshot, false)), Kind::Report) == 0);
}

// A finished match ends play; leaving the room and the helper's copy follow.
static void TestFinishedAndFailed() {
	TournamentPlay play;
	play.Start("brg_x", "emt_x", 0);
	play.Tick(0, {});
	play.OnRoom(Kind::Claim, Room(RoomA, std::nullopt), 0);
	JoinNamed(play, 100);
	play.OnFailure(Kind::Claim, "bridge_unreachable", 200);
	CHECK(play.GetPhase() == Phase::Opening);
	CHECK(Count(play.Tick(5200, Opening()), Kind::Claim) == 1);
	play.OnFailure(Kind::Claim, "stale_revision", 5300);
	CHECK(play.GetPhase() == Phase::Finished && play.Reason() == "stale_revision");
	// Its join is still under way: that is left.
	auto out = play.Tick(5400, Opening());
	CHECK(Count(out, Kind::Leave) == 1 && Count(out, Kind::Forget) == 1);
	CHECK(play.Tick(5500, {}).empty());
	TournamentPlay other;
	other.Start("brg_x", "emt_x", 0);
	other.Tick(0, {});
	other.OnFailure(Kind::Claim, "not_found", 1);
	CHECK(other.GetPhase() == Phase::Failed);
	CHECK(Count(other.Tick(2, {}), Kind::Leave) == 0);
	// Losing the helper gives the match up with its reason; once ended, it stays ended.
	TournamentPlay lost;
	lost.Start("brg_x", "emt_x", 0);
	lost.Tick(0, {});
	lost.OnRoom(Kind::Claim, Room(RoomA, std::nullopt), 0);
	JoinNamed(lost, 50);
	lost.Abandon("helper_lost");
	CHECK(lost.GetPhase() == Phase::Failed && lost.Reason() == "helper_lost");
	CHECK(Count(lost.Tick(100, Opening()), Kind::Leave) == 1);
	other.Abandon("helper_lost");
	CHECK(other.Reason() == "not_found");
}

// The deciding game completes the match on the bridge at the KO, so a claim
// can answer stale_revision while that game's battle still shows the result.
// The room is left once the battle has closed by itself, so the result stays
// on screen and Ember's menu comes back after it; a battle that never closes
// is left all the same. Stop and giving the match up leave at once.
static void TestFinishedWaitsForTheLastBattle() {
	room::Snapshot snapshot;
	const auto inRoom = [&](TournamentPlay& play) {
		play.Start("brg_x", "emt_x", 0);
		play.Tick(0, {});
		play.OnRoom(Kind::Claim, Room(RoomA, std::nullopt), 0);
		JoinNamed(play, 50);
		play.Tick(100, Joined(RoomA, &snapshot, false));
		CHECK(play.GetPhase() == Phase::InRoom);
	};
	auto fighting = Joined(RoomA, &snapshot, false);
	fighting.fighting = true;
	const auto closed = Joined(RoomA, &snapshot, false);

	TournamentPlay play;
	inRoom(play);
	play.OnFailure(Kind::Claim, "stale_revision", 1000);
	CHECK(play.GetPhase() == Phase::Finished && play.Reason() == "stale_revision");
	auto out = play.Tick(1100, fighting);
	CHECK(Count(out, Kind::Leave) == 0 && Count(out, Kind::Forget) == 1);
	CHECK(Count(play.Tick(30000, fighting), Kind::Leave) == 0);
	CHECK(Count(play.Tick(30100, closed), Kind::Leave) == 1);
	CHECK(play.Tick(30200, closed).empty());

	TournamentPlay stuck;
	inRoom(stuck);
	stuck.OnFailure(Kind::Claim, "stale_revision", 1000);
	CHECK(Count(stuck.Tick(1100, fighting), Kind::Leave) == 0);
	CHECK(Count(stuck.Tick(1100 + FinalBattleHoldMs - 1, fighting), Kind::Leave) == 0);
	CHECK(Count(stuck.Tick(1100 + FinalBattleHoldMs, fighting), Kind::Leave) == 1);
	CHECK(Count(stuck.Tick(1200 + FinalBattleHoldMs, fighting), Kind::Leave) == 0);

	TournamentPlay stopped;
	inRoom(stopped);
	stopped.OnFailure(Kind::Claim, "stale_revision", 1000);
	CHECK(Count(stopped.Tick(1100, fighting), Kind::Leave) == 0);
	stopped.Stop();
	CHECK(Count(stopped.Tick(1200, fighting), Kind::Leave) == 1 && stopped.GetPhase() == Phase::Idle);
	CHECK(Count(stopped.Tick(1300, closed), Kind::Leave) == 0);

	TournamentPlay abandoned;
	inRoom(abandoned);
	abandoned.Abandon("helper_lost");
	CHECK(Count(abandoned.Tick(1000, fighting), Kind::Leave) == 1);
	TournamentPlay refused;
	inRoom(refused);
	refused.OnFailure(Kind::Claim, "not_found", 1000);
	CHECK(Count(refused.Tick(1100, fighting), Kind::Leave) == 1);
}

// An aborted game is reported once, whether the room ended it or this
// fighter's game reports it because its room did not. A match whose last
// report was the abort of a game that lost its link ended for the lost
// connection. Any later report, a game played out or a start called off,
// finishes it as usual, and so does an abort for another reason.
static void TestConnectionLost() {
	room::Snapshot snapshot;
	snapshot.tournament = MakeBinding(RoomA, 0).room;
	snapshot.tables[room::TournamentTable].phase = room::TablePhase::Ready;
	snapshot.tables[room::TournamentTable].permitGeneration = 3;
	const auto permit = [](std::uint64_t generation) {
		PrepareReply permitted;
		permitted.permitted = true;
		permitted.generation = generation;
		permitted.permitId = "per_" + std::to_string(generation);
		return permitted;
	};
	const auto bound = [&](TournamentPlay& play) {
		play.Start("brg_x", "emt_x", 0);
		play.Tick(0, {});
		play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 0)), 0);
		JoinNamed(play, 50);
		play.Tick(100, Joined(RoomA, &snapshot, false));
		play.OnPrepare(permit(3), 200);
	};

	TournamentPlay lost;
	bound(lost);
	lost.OnConnectionLost(3);
	lost.OnTerminal(3, room::MatchResult::Abort, 900, 899);
	lost.OnTerminal(3, room::MatchResult::Abort, 0, 0);
	lost.OnTerminal(3, room::MatchResult::P1Win, 10, 9);
	auto out = lost.Tick(300, Joined(RoomA, &snapshot, false));
	const auto* report = Find(out, Kind::Report);
	CHECK(Count(out, Kind::Report) == 1 && report->generation == 3 && report->result == "abort" &&
		report->captureFrame == 0 && report->confirmedFrame == 0);
	lost.OnReported(3);
	lost.OnFailure(Kind::Claim, "stale_revision", 400);
	CHECK(lost.GetPhase() == Phase::Finished && lost.Reason() == ConnectionLostReason);

	// An abort with the link up (a game this PC could not take).
	TournamentPlay rejected;
	bound(rejected);
	rejected.OnConnectionLost(2);
	rejected.OnTerminal(3, room::MatchResult::Abort, 0, 0);
	rejected.OnFailure(Kind::Claim, "stale_revision", 400);
	CHECK(rejected.GetPhase() == Phase::Finished && rejected.Reason() == "stale_revision");

	TournamentPlay replayed;
	bound(replayed);
	replayed.OnConnectionLost(3);
	replayed.OnTerminal(3, room::MatchResult::Abort, 0, 0);
	replayed.OnPrepare(permit(4), 300);
	replayed.OnTerminal(4, room::MatchResult::P2Win, 10, 9);
	replayed.OnFailure(Kind::Claim, "stale_revision", 400);
	CHECK(replayed.GetPhase() == Phase::Finished && replayed.Reason() == "stale_revision");

	// The next game's start called off: its cancel is the last report.
	TournamentPlay calledOff;
	bound(calledOff);
	calledOff.OnConnectionLost(3);
	calledOff.OnTerminal(3, room::MatchResult::Abort, 0, 0);
	CHECK(Count(calledOff.Tick(300, Joined(RoomA, &snapshot, false)), Kind::Report) == 1);
	calledOff.OnReported(3);
	calledOff.OnPrepare(permit(4), 400);
	room::Snapshot moved = snapshot;
	moved.tables[room::TournamentTable].phase = room::TablePhase::Waiting;
	moved.tables[room::TournamentTable].permitGeneration = 0;
	const auto cancelled = calledOff.Tick(500, Joined(RoomA, &moved, false));
	const auto* cancel = Find(cancelled, Kind::Report);
	CHECK(cancel && cancel->generation == 4 && cancel->result == "cancel");
	calledOff.OnFailure(Kind::Claim, "stale_revision", 600);
	CHECK(calledOff.GetPhase() == Phase::Finished && calledOff.Reason() == "stale_revision");
}

// The abort a fighter owes the bridge for a game whose connection was lost:
// the room's end decides the game whenever it comes within the wait, and so
// does a result this PC confirmed; an abort queued to a leader that is gone,
// or a room whose control stays lost, is reported once the bound passes.
static void TestOwedAbort() {
	using Step = OwedAbort::Step;
	OwedAbort::View healthy;
	healthy.healthy = true;
	healthy.playing = true;

	OwedAbort committed;
	committed.Owe(3, 1000);
	CHECK(committed.Next(healthy, 1000) == Step::Wait);
	CHECK(committed.Next(healthy, 1000 + OwedAbort::WaitMs - 1) == Step::Wait);
	auto ended = healthy;
	ended.ended = true;
	CHECK(committed.Next(ended, 1000 + OwedAbort::WaitMs - 1) == Step::Drop && committed.Generation() == 0);
	CHECK(committed.Next(healthy, 1000 + 2 * OwedAbort::WaitMs) == Step::Wait);

	OwedAbort confirmed;
	confirmed.Owe(3, 1000);
	auto captured = healthy;
	captured.captured = true;
	captured.healthy = false;
	CHECK(confirmed.Next(captured, 1000 + OwedAbort::WaitMs) == Step::Drop);

	OwedAbort stuck;
	stuck.Owe(3, 1000);
	CHECK(stuck.Next(healthy, 1000 + OwedAbort::WaitMs - 1) == Step::Wait);
	CHECK(stuck.Next(healthy, 1000 + OwedAbort::WaitMs) == Step::Report && stuck.Generation() == 0);
	CHECK(stuck.Next(healthy, 1000 + 2 * OwedAbort::WaitMs) == Step::Wait);

	// A control that comes back starts the count again.
	OwedAbort lost;
	lost.Owe(3, 1000);
	auto down = healthy;
	down.healthy = false;
	CHECK(lost.Next(down, 2000) == Step::Wait);
	CHECK(lost.Next(healthy, 3000) == Step::Wait);
	CHECK(lost.Next(down, 4000) == Step::Wait);
	CHECK(lost.Next(down, 4000 + OwedAbort::ControlLostMs - 1) == Step::Wait);
	CHECK(lost.Next(down, 4000 + OwedAbort::ControlLostMs) == Step::Report);

	OwedAbort stopped;
	stopped.Owe(3, 1000);
	auto idle = healthy;
	idle.playing = false;
	CHECK(stopped.Next(idle, 1000 + OwedAbort::WaitMs) == Step::Drop);
}

// The helper forgets the match only once every report has its answer; a
// lease lost while opening is never published; and only a room this game
// opened is published at all.
static void TestOrderingAndLeases() {
	TournamentPlay play;
	play.Start("brg_x", "emt_x", 0);
	play.Tick(0, {});
	play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 0)), 0);
	JoinNamed(play, 50);
	room::Snapshot snapshot;
	snapshot.tournament = MakeBinding(RoomA, 0).room;
	auto& table = snapshot.tables[room::TournamentTable];
	table.phase = room::TablePhase::Ready;
	table.permitGeneration = 3;
	play.Tick(100, Joined(RoomA, &snapshot, false));
	PrepareReply permitted;
	permitted.permitted = true;
	permitted.generation = 3;
	permitted.permitId = "per_z";
	play.OnPrepare(permitted, 200);
	play.OnTerminal(3, room::MatchResult::P1Win, 10, 9);
	CHECK(Count(play.Tick(300, Joined(RoomA, &snapshot, false)), Kind::Report) == 1);
	play.OnFailure(Kind::Claim, "stale_revision", 400);
	auto out = play.Tick(500, Joined(RoomA, &snapshot, false));
	CHECK(Count(out, Kind::Leave) == 1 && Count(out, Kind::Forget) == 0);
	play.OnReported(3);
	CHECK(Count(play.Tick(600, {}), Kind::Forget) == 1);

	TournamentPlay late;
	late.Start("brg_x", "emt_x", 0);
	late.Tick(0, {});
	ClaimReply lease;
	lease.role = ClaimReply::Role::Host;
	lease.leaseId = "lse_1";
	lease.fence = "1";
	late.OnRoom(Kind::Claim, lease, 0);
	CHECK(Count(late.Tick(100, {}), Kind::Host) == 1);
	ClaimReply wait;
	wait.role = ClaimReply::Role::Wait;
	late.OnRoom(Kind::Claim, wait, 200);
	CHECK(Count(late.Tick(300, Joined(RoomA, &snapshot)), Kind::Publish) == 0);

	TournamentPlay casual;
	casual.Start("brg_x", "emt_x", 0);
	casual.Tick(0, {});
	casual.OnRoom(Kind::Claim, lease, 0);
	// Already sitting in a room it did not open for the match.
	CHECK(Count(casual.Tick(100, Joined(RoomB, &snapshot)), Kind::Publish) == 0);

	// A game that ends just before the player stops: its report still goes
	// first, and Forget waits for its answer.
	TournamentPlay stopping;
	stopping.Start("brg_x", "emt_x", 0);
	stopping.Tick(0, {});
	stopping.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 0)), 0);
	JoinNamed(stopping, 50);
	stopping.Tick(100, Joined(RoomA, &snapshot, false));
	stopping.OnPrepare(permitted, 200);
	stopping.OnTerminal(3, room::MatchResult::P2Win, 10, 9);
	stopping.Stop();
	out = stopping.Tick(300, {});
	CHECK(Count(out, Kind::Report) == 1 && Count(out, Kind::Forget) == 0);
	stopping.OnReported(3);
	CHECK(Count(stopping.Tick(400, {}), Kind::Forget) == 1);

	// A helper busy for a moment (another identity task, a full queue): the
	// same report goes again, no further game is prepared meanwhile, and once
	// saved the match goes on.
	TournamentPlay busy;
	busy.Start("brg_x", "emt_x", 0);
	busy.Tick(0, {});
	busy.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 0)), 0);
	JoinNamed(busy, 50);
	busy.Tick(100, Joined(RoomA, &snapshot, false));
	busy.OnPrepare(permitted, 200);
	busy.OnTerminal(3, room::MatchResult::P1Win, 10, 9);
	CHECK(Count(busy.Tick(300, Joined(RoomA, &snapshot, false)), Kind::Report) == 1);
	CHECK(busy.SavingReports());
	busy.OnReportFailed(3, "identity_busy", 400);
	room::Snapshot next = snapshot;
	next.tables[room::TournamentTable].permitGeneration = 4;
	out = busy.Tick(500, Joined(RoomA, &next, false));
	CHECK(Count(out, Kind::Report) == 0 && Count(out, Kind::Prepare) == 0);
	out = busy.Tick(400 + RetryMs, Joined(RoomA, &next, false));
	const auto* again = Find(out, Kind::Report);
	CHECK(again && again->generation == 3 && again->result == "p1_win" && again->captureFrame == 10);
	CHECK(Count(out, Kind::Prepare) == 0);
	busy.OnReported(3);
	CHECK(!busy.SavingReports() && busy.GetPhase() == Phase::InRoom);
	CHECK(Count(busy.Tick(500 + RetryMs, Joined(RoomA, &next, false)), Kind::Prepare) == 1);

	// A report the helper never saves stops the match once its tries run out:
	// it leaves the room and starts no further game.
	TournamentPlay lost;
	lost.Start("brg_x", "emt_x", 0);
	lost.Tick(0, {});
	lost.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 0)), 0);
	JoinNamed(lost, 50);
	lost.Tick(100, Joined(RoomA, &snapshot, false));
	lost.OnPrepare(permitted, 200);
	lost.OnTerminal(3, room::MatchResult::P1Win, 10, 9);
	std::uint64_t now = 300;
	CHECK(Count(lost.Tick(now, Joined(RoomA, &snapshot, false)), Kind::Report) == 1);
	for (int attempt = 1; attempt < MaxReportAttempts; ++attempt) {
		lost.OnReportFailed(3, "report_not_saved", now);
		CHECK(lost.GetPhase() == Phase::InRoom);
		now += RetryMs;
		CHECK(Count(lost.Tick(now, Joined(RoomA, &snapshot, false)), Kind::Report) == 1);
	}
	lost.OnReportFailed(3, "report_not_saved", now);
	CHECK(lost.GetPhase() == Phase::Failed && lost.Reason() == "report_not_saved" && !lost.SavingReports());
	out = lost.Tick(now + 100, Joined(RoomA, &snapshot, false));
	CHECK(Count(out, Kind::Leave) == 1 && Count(out, Kind::Prepare) == 0 && Count(out, Kind::Claim) == 0 &&
		Count(out, Kind::Report) == 0 && Count(out, Kind::Forget) == 1);

	// A report the helper refuses outright (it holds no permit for the game)
	// cannot succeed on another try.
	TournamentPlay refused;
	refused.Start("brg_x", "emt_x", 0);
	refused.Tick(0, {});
	refused.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 0)), 0);
	JoinNamed(refused, 50);
	refused.Tick(100, Joined(RoomA, &snapshot, false));
	refused.OnPrepare(permitted, 200);
	refused.OnTerminal(3, room::MatchResult::P1Win, 10, 9);
	CHECK(Count(refused.Tick(300, Joined(RoomA, &snapshot, false)), Kind::Report) == 1);
	refused.OnReportFailed(3, "unknown_permit", 400);
	CHECK(refused.GetPhase() == Phase::Failed && refused.Reason() == "report_not_saved");
}

// Each claim says whether this fighter pressed Ready at the match's table, so
// the bridge can tell the tournament who has not. A change is claimed at once,
// at most every ReadyClaimSpacingMs, but not ahead of a failed claim's retry.
static void TestReadyGoesWithTheClaim() {
	TournamentPlay play;
	play.Start("brg_x", "emt_x", 0);
	// The Ready a tick's claim says, if it claims.
	const auto claimed = [](const std::vector<Output>& out) {
		const auto* claim = Find(out, Kind::Claim);
		return claim ? std::optional<bool>(claim->ready) : std::nullopt;
	};
	const auto first = claimed(play.Tick(0, {}));
	CHECK(first && !*first);
	play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 1)), 100);
	JoinNamed(play, 150);
	room::Snapshot snapshot;
	snapshot.tournament = MakeBinding(RoomA, 1).room;
	snapshot.localMember = 2;
	room::Member self;
	self.id = 2;
	self.table = static_cast<std::int8_t>(room::TournamentTable);
	self.seat = 1;
	snapshot.members.push_back(self);
	auto& table = snapshot.tables[room::TournamentTable];
	table.p2 = 2;
	const auto view = Joined(RoomA, &snapshot, false);
	CHECK(!claimed(play.Tick(200, view)));

	// Ready goes out ReadyClaimSpacingMs after the last claim, not 10 s on.
	table.ready[1] = true;
	CHECK(!claimed(play.Tick(1000, view)));
	const auto ready = claimed(play.Tick(2000, view));
	CHECK(ready && *ready);
	play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 1)), 2100);
	CHECK(!claimed(play.Tick(3000, view)));
	// Taking it back goes out as soon too.
	table.ready[1] = false;
	CHECK(!claimed(play.Tick(3500, view)));
	const auto unready = claimed(play.Tick(4000, view));
	CHECK(unready && !*unready);
	play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 1)), 4100);

	// Ready only counts in this fighter's own seat at the match's table.
	table.ready[0] = true;
	CHECK(!claimed(play.Tick(7000, view)));
	snapshot.members[0].seat = -1;
	table.ready[1] = true;
	CHECK(!claimed(play.Tick(7000, view)));
	snapshot.members[0].seat = 1;
	snapshot.tournament = room::TournamentBinding();
	CHECK(!claimed(play.Tick(7000, view)));
	snapshot.tournament = MakeBinding(RoomA, 1).room;
	CHECK(!claimed(play.Tick(7000, Joined(RoomB, &snapshot, false))));
	const auto again = claimed(play.Tick(7000, view));
	CHECK(again && *again);

	// A failed claim's retry is not brought forward.
	play.OnFailure(Kind::Claim, "service_unavailable", 7100);
	table.ready[1] = false;
	CHECK(!claimed(play.Tick(9200, view)));
	const auto retried = claimed(play.Tick(12100, view));
	CHECK(retried && !*retried);
}


// The match's room cannot be joined twice running (its host closed the game,
// or left): this fighter hosts a new room and publishes it in that one's
// place. The bridge's claims still name the old room until then, and are not
// joined again. Refused, because the other fighter replaced it first or a
// game is undecided, the room the bridge names next is joined and the hosted
// one left.
static void TestGoneHostIsReplaced() {
	const auto failTwice = [](TournamentPlay& play) {
		play.Start("brg_x", "emt_x", 0);
		play.Tick(0, {});
		play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 1)), 100);
		CHECK(Count(play.Tick(200, {}), Kind::Join) == 1);
		play.Tick(300, Opening());
		// The first failed join is tried again after the open timeout.
		CHECK(Count(play.Tick(400, {}), Kind::Host) == 0);
		CHECK(Count(play.Tick(OpenTimeoutMs + 200, {}), Kind::Join) == 1);
		play.Tick(OpenTimeoutMs + 300, Opening());
		CHECK(Count(play.Tick(OpenTimeoutMs + 400, {}), Kind::Host) == 1);
	};
	TournamentPlay play;
	failTwice(play);
	const std::uint64_t at = OpenTimeoutMs + 400;
	// A claim still naming the old room does not bring its join back.
	play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 1)), at + 10);
	CHECK(Count(play.Tick(at + 20, Opening()), Kind::Join) == 0);
	room::Snapshot snapshot;
	auto out = play.Tick(at + 30, Joined(RoomB, &snapshot));
	const auto* publish = Find(out, Kind::Publish);
	CHECK(publish && publish->roomId == RoomB && publish->replaces == RoomA && publish->leaseId.empty() && publish->fence.empty());
	play.OnRoom(Kind::Publish, Room(RoomB, MakeBinding(RoomB, 1)), at + 40);
	play.Tick(at + 50, Joined(RoomB, &snapshot));
	CHECK(play.GetPhase() == Phase::InRoom);

	TournamentPlay refused;
	failTwice(refused);
	refused.Tick(at + 20, Opening());
	CHECK(Count(refused.Tick(at + 30, Joined(RoomB, &snapshot)), Kind::Publish) == 1);
	refused.OnFailure(Kind::Publish, "lease_conflict", at + 40);
	const std::string RoomC(32, 'c');
	refused.OnRoom(Kind::Claim, Room(RoomC, MakeBinding(RoomC, 1)), at + 50);
	out = refused.Tick(at + 60, Joined(RoomB, &snapshot));
	CHECK(Count(out, Kind::Leave) == 1 && Count(out, Kind::Publish) == 0);
	out = refused.Tick(at + 70, {});
	const auto* join = Find(out, Kind::Join);
	CHECK(join && join->invitation == "sf4e3:" + RoomC);

	// Joins that each take the whole open timeout to fail: the tick that sees
	// the second fail hosts the replacement and sends no third join with it.
	TournamentPlay slow;
	slow.Start("brg_x", "emt_x", 0);
	slow.Tick(0, {});
	slow.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 1)), 100);
	CHECK(Count(slow.Tick(200, {}), Kind::Join) == 1);
	slow.Tick(300, Opening());
	CHECK(Count(slow.Tick(OpenTimeoutMs + 300, {}), Kind::Join) == 1);
	slow.Tick(OpenTimeoutMs + 400, Opening());
	out = slow.Tick(2 * OpenTimeoutMs + 400, {});
	CHECK(Count(out, Kind::Host) == 1 && Count(out, Kind::Join) == 0);

	// One failed join is not enough: the room is joined again, not replaced.
	TournamentPlay once;
	once.Start("brg_x", "emt_x", 0);
	once.Tick(0, {});
	once.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 1)), 100);
	once.Tick(200, {});
	once.Tick(300, Opening());
	once.Tick(400, {});
	once.Tick(OpenTimeoutMs + 200, {});
	once.Tick(OpenTimeoutMs + 300, Opening());
	out = once.Tick(OpenTimeoutMs + 400, Joined(RoomA, &snapshot));
	CHECK(Count(out, Kind::Host) == 0 && once.GetPhase() == Phase::InRoom);
}

// A room after another numbers its games from 1 again: once the match moves
// to a replacement room, the last room's settled game 1 neither stands in for
// the new game 1's permit nor keeps its result from being reported.
static void TestReplacementRoomStartsItsGamesAgain() {
	TournamentPlay play;
	play.Start("brg_x", "emt_x", 0);
	play.Tick(0, {});
	play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 1)), 0);
	JoinNamed(play, 50);
	room::Snapshot first;
	first.tournament = MakeBinding(RoomA, 1).room;
	auto& table = first.tables[room::TournamentTable];
	table.phase = room::TablePhase::Ready;
	table.permitGeneration = 1;
	CHECK(Count(play.Tick(100, Joined(RoomA, &first, false)), Kind::Prepare) == 1);
	PrepareReply permitted;
	permitted.permitted = true;
	permitted.generation = 1;
	permitted.permitId = "per_a";
	permitted.startWindowMs = 90000;
	play.OnPrepare(permitted, 200);
	table.permits[1] = "per_a";
	table.phase = room::TablePhase::Playing;
	table.matchGeneration = 1;
	table.permitGeneration = 0;
	play.Tick(300, Joined(RoomA, &first, false));
	play.OnTerminal(1, room::MatchResult::P1Win, 10, 9);
	CHECK(Count(play.Tick(400, Joined(RoomA, &first, false)), Kind::Report) == 1);
	play.OnReported(1);
	// The other fighter replaced the room: the bridge names B, a new assignment.
	auto moved = MakeBinding(RoomB, 1);
	moved.room.assignmentGeneration = 2;
	play.OnRoom(Kind::Claim, Room(RoomB, moved), 500);
	CHECK(Count(play.Tick(600, Joined(RoomA, &first, false)), Kind::Leave) == 1);
	CHECK(Count(play.Tick(700, {}), Kind::Join) == 1);
	room::Snapshot second;
	second.tournament = moved.room;
	auto& next = second.tables[room::TournamentTable];
	next.phase = room::TablePhase::Ready;
	next.permitGeneration = 1;
	auto out = play.Tick(800, Joined(RoomB, &second, false));
	CHECK(Count(out, Kind::Prepare) == 1 && Count(out, Kind::PermitReady) == 0);
	permitted.permitId = "per_b";
	play.OnPrepare(permitted, 900);
	out = play.Tick(1000, Joined(RoomB, &second, false));
	const auto* ready = Find(out, Kind::PermitReady);
	CHECK(ready && ready->permitId == "per_b");
	next.permits[1] = "per_b";
	next.phase = room::TablePhase::Playing;
	next.matchGeneration = 1;
	next.permitGeneration = 0;
	play.Tick(1100, Joined(RoomB, &second, false));
	play.OnTerminal(1, room::MatchResult::P2Win, 20, 19);
	out = play.Tick(1200, Joined(RoomB, &second, false));
	const auto* report = Find(out, Kind::Report);
	CHECK(report && report->generation == 1 && report->result == "p2_win");

	// A prepare out when the room changed answers for the last room: dropped.
	TournamentPlay late;
	late.Start("brg_x", "emt_x", 0);
	late.Tick(0, {});
	late.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 1)), 0);
	JoinNamed(late, 50);
	room::Snapshot waiting;
	waiting.tournament = MakeBinding(RoomA, 1).room;
	waiting.tables[room::TournamentTable].phase = room::TablePhase::Ready;
	waiting.tables[room::TournamentTable].permitGeneration = 1;
	CHECK(Count(late.Tick(100, Joined(RoomA, &waiting, false)), Kind::Prepare) == 1);
	late.OnRoom(Kind::Claim, Room(RoomB, moved), 150);
	permitted.permitId = "per_old";
	late.OnPrepare(permitted, 200);
	late.Tick(300, Joined(RoomA, &waiting, false));
	late.Tick(400, {});
	room::Snapshot fresh;
	fresh.tournament = moved.room;
	fresh.tables[room::TournamentTable].phase = room::TablePhase::Ready;
	fresh.tables[room::TournamentTable].permitGeneration = 1;
	out = late.Tick(500, Joined(RoomB, &fresh, false));
	CHECK(Count(out, Kind::Prepare) == 1 && Count(out, Kind::PermitReady) == 0);
}
int main() {
	TestHostPublishesAndBinds();
	TestACasualGameHoldsTheFirstBinding();
	TestGuestJoins();
	TestLeaseMoved();
	TestNeverActsOnAForeignRoom();
	TestPermitsAndReports();
	TestCancelledStart();
	TestFinishedAndFailed();
	TestFinishedWaitsForTheLastBattle();
	TestConnectionLost();
	TestOwedAbort();
	TestOrderingAndLeases();
	TestReadyGoesWithTheClaim();
	TestGoneHostIsReplaced();
	TestReplacementRoomStartsItsGamesAgain();
	if (failures) std::printf("%d failure(s)\n", failures);
	else std::printf("tournament play tests passed\n");
	return failures ? 1 : 0;
}
