// An Arcade room's seating: eight tables, each a pair of cabinets facing each
// other (Snapshot::arcade).
#include "room_authority_support.hxx"

#include <vector>

static RoomAuthority Arcade() {
	return RoomAuthority("Arcade", 16, 1, Rules(), false, DelayPolicy::Shared, ArcadeFloor);
}

static void Sit(RoomAuthority& authority, MemberId member, std::uint8_t table = 0) {
	CHECK(authority.Apply(member, TableAction(authority, member, table, ActionKind::Queue)).accepted);
}

static bool Pair(const RoomAuthority& authority, std::uint8_t table, MemberId p1, MemberId p2) {
	const auto& t = authority.SnapshotView().tables[table];
	return t.p1 == p1 && t.p2 == p2;
}

// Both fighters at `table` ready, play one game that `result` decides, and
// every recipient acknowledges it.
static void Play(RoomAuthority& authority, std::uint8_t table, MatchResult result) {
	const auto seated = authority.SnapshotView().tables[table];
	for (const auto member : {seated.p1, seated.p2}) CHECK(authority.Apply(member, TableAction(authority, member, table, ActionKind::Ready)).accepted);
	CHECK(authority.BeginMatch(table, seated.p1, seated.p2).accepted);
	const auto generation = authority.SnapshotView().tables[table].matchGeneration;
	CHECK(authority.EndMatch(table, generation, result).accepted);
	for (const auto member : authority.TerminalMembers(table, generation)) {
		Action acknowledgment = TableAction(authority, member, table, ActionKind::AcknowledgeTerminal);
		acknowledgment.matchGeneration = generation;
		CHECK(authority.Apply(member, acknowledgment).accepted);
	}
}

static void SitAt(RoomAuthority& authority, MemberId member, std::uint8_t table, std::int8_t seat, bool accepted = true) {
	Action named = TableAction(authority, member, table, ActionKind::Queue);
	named.seat = seat;
	CHECK(authority.Apply(member, named).accepted == accepted);
}

// Sitting down without a cabinet puts a member opposite whoever waits alone,
// else at the first empty table; sixteen members fill all eight tables. A
// member may also sit at the cabinet they choose.
static void TestSittingDownPairsCabinets() {
	auto authority = Arcade();
	std::vector<MemberId> members;
	for (int i = 0; i < 16; ++i) members.push_back(Join(authority, i, i == 0));
	Sit(authority, members[0], 5);
	CHECK(Pair(authority, 0, members[0], 0) && StatusOf(authority, members[0]) == MemberStatus::Seated);
	for (std::size_t i = 1; i < members.size(); ++i) Sit(authority, members[i], 7);
	for (std::uint8_t table = 0; table < TableCount; ++table) {
		CHECK(Pair(authority, table, members[table * 2], members[table * 2 + 1]));
		CHECK(authority.SnapshotView().tables[table].phase == TablePhase::Waiting && authority.SnapshotView().tables[table].queue.empty());
	}
	CHECK(TablesInPlay(authority.SnapshotView()) == TableCount);
	auto chosen = Arcade();
	const MemberId a = Join(chosen, 0, true);
	SitAt(chosen, a, 6, 1);
	CHECK(Pair(chosen, 6, 0, a));
}

// The two cabinets play on after a set; the streak counts as at any table.
static void TestCabinetsRematch() {
	Rules rules;
	rules.format = SetFormat::Ft1;
	RoomAuthority authority("Sets", 16, 1, rules, false, DelayPolicy::Shared, ArcadeFloor);
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1), c = Join(authority, 2);
	for (const auto member : {a, b, c}) Sit(authority, member);
	Play(authority, 0, MatchResult::P1Win);
	Play(authority, 0, MatchResult::P1Win);
	const auto& t = authority.SnapshotView().tables[0];
	CHECK(Pair(authority, 0, a, b) && t.streakHolder == a && t.streak == 2 && t.phase == TablePhase::Waiting);
	CHECK(Pair(authority, 1, c, 0));
}

// To play someone else a member moves to the free cabinet opposite them,
// between games only. Nobody is moved for them: a cabinet left alone waits.
static void TestMovingCabinets() {
	auto authority = Arcade();
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1), c = Join(authority, 2), d = Join(authority, 3);
	for (const auto member : {a, b, c, d}) Sit(authority, member);
	CHECK(authority.Apply(b, TableAction(authority, b, 0, ActionKind::Unqueue)).accepted);
	CHECK(Pair(authority, 0, a, 0) && Pair(authority, 1, c, d));
	SitAt(authority, c, 0, 1);
	CHECK(Pair(authority, 0, a, c) && Pair(authority, 1, 0, d) && StatusOf(authority, c) == MemberStatus::Seated);
	SitAt(authority, d, 0, 0, false);
	for (const auto member : {a, c}) CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	CHECK(authority.BeginMatch(0, a, c).accepted);
	SitAt(authority, a, 1, 1, false);
	CHECK(Pair(authority, 0, a, c));
	CHECK(authority.Leave(d).accepted && Pair(authority, 1, 0, 0));
	Sit(authority, b);
	CHECK(Pair(authority, 1, b, 0));
}

// An Arcade room's rules are the room's: no table's can be changed.
static void TestRulesAreTheRooms() {
	auto authority = Arcade();
	const MemberId host = Join(authority, 0, true);
	Action rules = TableAction(authority, host, 2, ActionKind::SetRules);
	rules.rules = authority.SnapshotView().tables[2].rules;
	rules.rules.roundCount = 5;
	CHECK(authority.Apply(host, rules).reason == RejectReason::Unauthorized);
}

// A Classic room plays at its first four tables; an Arcade room at all eight.
static void TestTablesInPlay() {
	RoomAuthority classic("Classic");
	const MemberId host = Join(classic, 0, true);
	CHECK(TablesInPlay(classic.SnapshotView()) == ClassicTableCount);
	CHECK(classic.Apply(host, TableAction(classic, host, 4, ActionKind::Queue)).reason == RejectReason::UnknownTable);
	auto arcade = Arcade();
	const MemberId member = Join(arcade, 0, true);
	Sit(arcade, member, 7);
	CHECK(Pair(arcade, 0, member, 0));
}

// The room's kind travels with its state. A public or a tournament's Arcade
// room is refused, as a Practice one is, and so is a queue in one.
static void TestArcadeOnTheWire() {
	auto authority = Arcade();
	const MemberId a = Join(authority, 0, true);
	Sit(authority, a);
	const auto wire = nlohmann::json(authority.SnapshotView());
	CHECK(wire.at("arcade") == 1 && wire.get<Snapshot>().arcade);
	CHECK(!nlohmann::json(RoomAuthority("Classic").SnapshotView()).contains("arcade"));
	RoomAuthority restored("Restore");
	CHECK(restored.RestoreCheckpoint(authority.Checkpoint()) && restored.SnapshotView().arcade);
	const auto refuses = [](nlohmann::json state) {
		try { (void)state.get<Snapshot>(); } catch (const std::exception&) { return true; }
		return false;
	};
	auto serverOwned = wire;
	serverOwned["server_owned"] = true;
	auto queued = wire;
	queued["tables"][3]["queue"] = nlohmann::json::array({a});
	queued["tables"][0]["p1"] = 0;
	CHECK(refuses(serverOwned) && refuses(queued));
	CHECK(!Arcade().SetServerOwned());
	TournamentBinding binding;
	binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
	binding.assignmentGeneration = 1;
	binding.bindingRevision = 1;
	binding.gamesToWin = 2;
	binding.fighters[0] = {std::string(64, 'a'), "emb1_aaaa"};
	binding.fighters[1] = {std::string(64, 'b'), "emb1_bbbb"};
	const auto bound = authority.BindTournament(binding);
	CHECK(!bound.accepted && bound.reason == RejectReason::Unauthorized);
	RoomAuthority match("Match");
	CHECK(match.BindTournament(binding).accepted);
	auto tournament = wire;
	tournament["tournament"] = nlohmann::json(match.SnapshotView()).at("tournament");
	CHECK(refuses(tournament));
}

// An Arcade room may also be a Practice room: its games are Practice matches
// and the room seats its cabinets.
static void TestArcadePracticeRoom() {
	RoomAuthority authority("Both", 8, 1, Rules(), true, DelayPolicy::Shared, ArcadeFloor);
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1);
	Sit(authority, a);
	Sit(authority, b, 2);
	CHECK(authority.SnapshotView().practice && Pair(authority, 0, a, b));
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	try {
		TestSittingDownPairsCabinets();
		TestCabinetsRematch();
		TestMovingCabinets();
		TestRulesAreTheRooms();
		TestTablesInPlay();
		TestArcadeOnTheWire();
		TestArcadePracticeRoom();
	} catch (const std::exception& error) {
		std::printf("room arcade threw: %s\n", error.what());
		return 1;
	}
	if (failures) std::printf("%d room arcade check(s) failed\n", failures);
	else std::printf("room arcade: ok\n");
	return failures ? 1 : 0;
}
