// The tournament play state machine: claiming, hosting or joining the match's
// room, binding it, fetching each game's permit, and reporting games.
#include "../netplay/TournamentPlay.hxx"

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
	play.OnRoom(Kind::Claim, Room(RoomB, std::nullopt), 100);
	room::Snapshot snapshot;
	auto out = play.Tick(200, Joined(RoomA, &snapshot));
	CHECK(Count(out, Kind::Leave) == 1 && Count(out, Kind::Join) == 0);
	CHECK(Count(play.Tick(300, Joined(RoomA, &snapshot)), Kind::Leave) == 0);
	out = play.Tick(400, {});
	CHECK(Count(out, Kind::Join) == 1);
}

// The permit for a reserved game is fetched, told to the room until the
// room shows it, and the game reported once it is decided.
static void TestPermitsAndReports() {
	TournamentPlay play;
	play.Start("brg_x", "emt_x", 0);
	play.Tick(0, {});
	play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 1)), 0);
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
	play.OnPrepare(permitted, 2400);
	out = play.Tick(2500, Joined(RoomA, &snapshot, false));
	const auto* ready = Find(out, Kind::PermitReady);
	CHECK(ready && ready->generation == 7 && ready->permitId == "per_x");
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
	play.Tick(100, {});
	play.OnFailure(Kind::Claim, "bridge_unreachable", 200);
	CHECK(play.GetPhase() == Phase::Opening);
	CHECK(Count(play.Tick(5200, {}), Kind::Claim) == 1);
	play.OnFailure(Kind::Claim, "stale_revision", 5300);
	CHECK(play.GetPhase() == Phase::Finished && play.Reason() == "stale_revision");
	auto out = play.Tick(5400, {});
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
	lost.Abandon("helper_lost");
	CHECK(lost.GetPhase() == Phase::Failed && lost.Reason() == "helper_lost");
	CHECK(Count(lost.Tick(100, {}), Kind::Leave) == 1);
	other.Abandon("helper_lost");
	CHECK(other.Reason() == "not_found");
}

// The helper forgets the match only once every report has its answer; a
// lease lost while opening is never published; and only a room this game
// opened is published at all.
static void TestOrderingAndLeases() {
	TournamentPlay play;
	play.Start("brg_x", "emt_x", 0);
	play.Tick(0, {});
	play.OnRoom(Kind::Claim, Room(RoomA, MakeBinding(RoomA, 0)), 0);
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
	auto out = play.Tick(500, {});
	CHECK(Count(out, Kind::Leave) == 1 && Count(out, Kind::Forget) == 0);
	play.OnReported();
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
	stopping.Tick(100, Joined(RoomA, &snapshot, false));
	stopping.OnPrepare(permitted, 200);
	stopping.OnTerminal(3, room::MatchResult::P2Win, 10, 9);
	stopping.Stop();
	out = stopping.Tick(300, {});
	CHECK(Count(out, Kind::Report) == 1 && Count(out, Kind::Forget) == 0);
	stopping.OnReported();
	CHECK(Count(stopping.Tick(400, {}), Kind::Forget) == 1);
}

int main() {
	TestHostPublishesAndBinds();
	TestGuestJoins();
	TestLeaseMoved();
	TestPermitsAndReports();
	TestCancelledStart();
	TestFinishedAndFailed();
	TestOrderingAndLeases();
	if (failures) std::printf("%d failure(s)\n", failures);
	else std::printf("tournament play tests passed\n");
	return failures ? 1 : 0;
}
