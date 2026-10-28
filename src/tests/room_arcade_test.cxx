// Arcade and King of the hill rooms: sixteen one-player cabinets; whoever sits
// down plays the player who has waited longest (Snapshot::arcade).
#include "room_authority_support.hxx"

#include <vector>

static RoomAuthority Arcade(RoomType type = ArcadeRoom, SetFormat format = SetFormat::Ft1) {
	Rules rules;
	rules.format = format;
	return RoomAuthority("Arcade", 16, 1, rules, false, DelayPolicy::Shared, type);
}

// Queue names a cabinet: table + seat * TableCount.
static void Sit(RoomAuthority& authority, MemberId member, int cabinet, bool accepted = true) {
	Action sit = TableAction(authority, member, static_cast<std::uint8_t>(cabinet % TableCount), ActionKind::Queue);
	sit.seat = static_cast<std::int8_t>(cabinet / TableCount);
	CHECK(authority.Apply(member, sit).accepted == accepted);
}

static int TableOf(const RoomAuthority& authority, MemberId member) {
	for (const auto& t : authority.SnapshotView().tables) if (t.p1 == member || t.p2 == member) return t.id;
	return -1;
}
static bool Paired(const RoomAuthority& authority, MemberId a, MemberId b) {
	const int table = TableOf(authority, a);
	if (table < 0) return false;
	const auto& t = authority.SnapshotView().tables[table];
	return (t.p1 == a && t.p2 == b) || (t.p1 == b && t.p2 == a);
}
static bool Waits(const RoomAuthority& authority, MemberId member) { return Paired(authority, member, 0); }

static Result Choose(RoomAuthority& authority, MemberId member, ActionKind kind) {
	return authority.Apply(member, TableAction(authority, member, static_cast<std::uint8_t>(TableOf(authority, member)), kind));
}

// Both fighters at `member`'s table ready, play one game that `result`
// decides, and every recipient acknowledges it.
static void Play(RoomAuthority& authority, MemberId member, MatchResult result) {
	const auto table = static_cast<std::uint8_t>(TableOf(authority, member));
	const auto seated = authority.SnapshotView().tables[table];
	for (const auto fighter : {seated.p1, seated.p2}) CHECK(authority.Apply(fighter, TableAction(authority, fighter, table, ActionKind::Ready)).accepted);
	CHECK(authority.BeginMatch(table, seated.p1, seated.p2).accepted);
	const auto generation = authority.SnapshotView().tables[table].matchGeneration;
	CHECK(authority.EndMatch(table, generation, result).accepted);
	for (const auto recipient : authority.TerminalMembers(table, generation)) {
		Action acknowledgment = TableAction(authority, recipient, table, ActionKind::AcknowledgeTerminal);
		acknowledgment.matchGeneration = generation;
		CHECK(authority.Apply(recipient, acknowledgment).accepted);
	}
}
// `winner` beats their opponent in a one-game set.
static void Beat(RoomAuthority& authority, MemberId winner) {
	const auto& t = authority.SnapshotView().tables[TableOf(authority, winner)];
	Play(authority, winner, t.p1 == winner ? MatchResult::P1Win : MatchResult::P2Win);
}

// Any free cabinet; the next to sit anywhere plays the one who waits, and a
// taken cabinet is refused. Sixteen members fill all eight tables.
static void TestFirstComeFirstPaired() {
	auto authority = Arcade();
	std::vector<MemberId> members;
	for (int i = 0; i < 16; ++i) members.push_back(Join(authority, i, i == 0));
	Sit(authority, members[0], 3);
	CHECK(Waits(authority, members[0]) && authority.SnapshotView().cabinets[3] == members[0]);
	Sit(authority, members[1], 3, false);
	Sit(authority, members[1], 12);
	CHECK(Paired(authority, members[0], members[1]) && StatusOf(authority, members[1]) == MemberStatus::Seated);
	int cabinet = 15;
	for (int i = 2; i < 16; ++i, --cabinet) { if (cabinet == 12) --cabinet; if (cabinet == 3) --cabinet; Sit(authority, members[i], cabinet); }
	for (int i = 0; i < 16; i += 2) CHECK(Paired(authority, members[i], members[i + 1]));
	for (const auto& t : authority.SnapshotView().tables) CHECK(t.phase == TablePhase::Waiting && t.queue.empty());
}

// After a set nobody leaves: Ready waits for both Rematches, then the pair
// plays again.
static void TestRematchBoth() {
	auto authority = Arcade();
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1);
	Sit(authority, a, 0); Sit(authority, b, 9);
	Beat(authority, a);
	const auto table = TableOf(authority, a);
	CHECK(authority.SnapshotView().tables[table].deciding && Paired(authority, a, b));
	CHECK(Choose(authority, a, ActionKind::Ready).reason == RejectReason::WrongPhase);
	CHECK(Choose(authority, a, ActionKind::Rematch).accepted);
	CHECK(authority.SnapshotView().tables[table].deciding && authority.SnapshotView().tables[table].rematch[0]);
	CHECK(Choose(authority, b, ActionKind::Rematch).accepted);
	CHECK(!authority.SnapshotView().tables[table].deciding && Paired(authority, a, b));
	CHECK(Choose(authority, a, ActionKind::Ready).accepted);
	const auto wire = nlohmann::json(authority.SnapshotView());
	CHECK(!wire.at("tables")[table].contains("rematch"));
}

// One Rematch and one New opponent part the pair: each plays whoever waited
// longest, or waits for the next to sit, never with the other again while
// someone else waits.
static void TestRematchOneRepairs() {
	auto authority = Arcade();
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1), c = Join(authority, 2), d = Join(authority, 3), e = Join(authority, 4);
	Sit(authority, a, 0); Sit(authority, b, 5); Sit(authority, c, 7);
	Beat(authority, a);
	CHECK(Choose(authority, a, ActionKind::Rematch).accepted);
	CHECK(authority.SnapshotView().tables[TableOf(authority, a)].deciding);
	CHECK(Choose(authority, b, ActionKind::NewOpponent).accepted);
	CHECK(Paired(authority, b, c) && Waits(authority, a));
	CHECK(Choose(authority, a, ActionKind::Rematch).reason == RejectReason::WrongPhase);
	// Nobody else waits: both wait, apart, and the next two to sit play them
	// in the order they began to wait.
	Beat(authority, b);
	CHECK(Choose(authority, c, ActionKind::NewOpponent).accepted);
	CHECK(Paired(authority, c, a) && Waits(authority, b));
	Sit(authority, d, 15);
	CHECK(Paired(authority, d, b));
	Beat(authority, d);
	CHECK(Choose(authority, d, ActionKind::NewOpponent).accepted);
	CHECK(Waits(authority, d) && Waits(authority, b) && !Paired(authority, d, b));
	Sit(authority, e, 8);
	CHECK(Paired(authority, e, d) && Waits(authority, b));
	// Standing up leaves the cabinet; the opponent waits again.
	CHECK(Choose(authority, e, ActionKind::Unqueue).accepted);
	CHECK(authority.SnapshotView().cabinets[8] == 0 && TableOf(authority, e) < 0 && Paired(authority, d, b));
}

// King of the hill: the winner stays and plays the longest waiting, the loser
// waits; streaks count per cabinet, and the top slot goes to the longest one
// until someone beats that streak or beats its holder. A tie keeps it.
static void TestKingStreaksAndCrown() {
	auto authority = Arcade(KingRoom);
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1), c = Join(authority, 2), d = Join(authority, 3);
	Sit(authority, a, 0); Sit(authority, b, 1); Sit(authority, c, 2); Sit(authority, d, 3);
	const auto& room = authority.SnapshotView();
	Beat(authority, a);
	CHECK(room.streaks[0] == 1 && room.topSlot == 0 && Waits(authority, a) && Waits(authority, b));
	CHECK(!room.tables[TableOf(authority, a)].deciding);
	Beat(authority, c);
	CHECK(room.streaks[2] == 1 && room.topSlot == 0 && Paired(authority, c, a) && Paired(authority, d, b));
	Beat(authority, c);
	CHECK(room.streaks[2] == 2 && room.streaks[0] == 0 && room.topSlot == 2);
	Beat(authority, d);
	CHECK(room.streaks[3] == 1 && room.topSlot == 2 && Paired(authority, c, d));
	Beat(authority, d);
	CHECK(room.streaks[2] == 0 && room.streaks[3] == 2 && room.topSlot == 3);
	const auto wire = nlohmann::json(room);
	CHECK(wire.at("arcade") == 2 && wire.at("top_slot") == 3 && wire.get<Snapshot>().streaks[3] == 2);
	CHECK(authority.Leave(d).accepted && room.topSlot < 0 && room.cabinets[3] == 0);
}

// King of the hill with two players: the loser challenges again at once,
// the winner's streak going on.
static void TestKingTwoPlayers() {
	auto authority = Arcade(KingRoom);
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1);
	Sit(authority, a, 0); Sit(authority, b, 8);
	const auto& room = authority.SnapshotView();
	Beat(authority, a);
	CHECK(room.streaks[0] == 1 && room.streaks[8] == 0 && Paired(authority, a, b));
	const auto& t = room.tables[TableOf(authority, a)];
	CHECK(t.phase == TablePhase::Waiting && t.score[0] == 0 && t.score[1] == 0 && !t.deciding);
	Beat(authority, a);
	CHECK(room.streaks[0] == 2 && room.topSlot == 0 && Paired(authority, a, b));
	Beat(authority, b);
	CHECK(room.streaks[0] == 0 && room.streaks[8] == 1 && room.topSlot == 8 && Paired(authority, a, b));
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

// The room's type travels with its state; a tournament's Arcade room is
// refused, and so is a queue in one.
static void TestArcadeOnTheWire() {
	auto authority = Arcade();
	const MemberId a = Join(authority, 0, true);
	Sit(authority, a, 4);
	const auto wire = nlohmann::json(authority.SnapshotView());
	CHECK(wire.at("arcade") == 1 && wire.get<Snapshot>().cabinets[4] == a);
	CHECK(!nlohmann::json(RoomAuthority("Classic").SnapshotView()).contains("arcade"));
	RoomAuthority restored("Restore");
	CHECK(restored.RestoreCheckpoint(authority.Checkpoint()) && restored.SnapshotView().arcade == ArcadeRoom && restored.SnapshotView().cabinets[4] == a);
	const auto refuses = [](nlohmann::json state) {
		try { (void)state.get<Snapshot>(); } catch (const std::exception&) { return true; }
		return false;
	};
	auto queued = wire;
	queued["tables"][3]["queue"] = nlohmann::json::array({a});
	auto stranger = wire;
	stranger["cabinets"][9] = 99;
	CHECK(refuses(queued) && refuses(stranger));
	TournamentBinding binding;
	binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
	binding.assignmentGeneration = 1;
	binding.bindingRevision = 1;
	binding.gamesToWin = 2;
	binding.fighters[0] = {std::string(64, 'a'), "emb1_aaaa"};
	binding.fighters[1] = {std::string(64, 'b'), "emb1_bbbb"};
	CHECK(authority.BindTournament(binding).reason == RejectReason::Unauthorized);
}

// A public room may be either Arcade type: its creator, alone in it, sets the
// type and every table's rules in one action.
static void TestPublicArcade() {
	RoomAuthority authority("Public", 16, 1);
	CHECK(authority.SetServerOwned());
	MemberProfile profile;
	profile.admission.account = "ember-a";
	const auto joined = authority.Join("Creator", Peer(1), false, profile);
	CHECK(joined.accepted);
	const MemberId host = joined.snapshot.members.back().id;
	Action type = TableAction(authority, host, 0, ActionKind::SetRoomType);
	type.roomType = KingRoom;
	type.rules.format = SetFormat::Ft2;
	CHECK(nlohmann::json(type).get<Action>().roomType == KingRoom);
	CHECK(authority.Apply(host, type).accepted);
	const auto& room = authority.SnapshotView();
	CHECK(room.arcade == KingRoom && room.tables[7].rules.format == SetFormat::Ft2);
	CHECK(nlohmann::json(room).get<Snapshot>().arcade == KingRoom);
	type = TableAction(authority, host, 0, ActionKind::SetRoomType);
	type.roomType = ArcadeRoom;
	CHECK(!authority.Apply(host, type).accepted);
	Sit(authority, host, 6);
	CHECK(authority.Leave(host).accepted && room.arcade == ClassicRoom);
	auto arcade = Arcade();
	CHECK(arcade.SetServerOwned());
}

// An Arcade room may also be a Practice room.
static void TestArcadePracticeRoom() {
	RoomAuthority authority("Both", 8, 1, Rules(), true, DelayPolicy::Shared, ArcadeRoom);
	const MemberId a = Join(authority, 0, true), b = Join(authority, 1);
	Sit(authority, a, 0); Sit(authority, b, 2);
	CHECK(authority.SnapshotView().practice && Paired(authority, a, b));
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	try {
		TestFirstComeFirstPaired();
		TestRematchBoth();
		TestRematchOneRepairs();
		TestKingStreaksAndCrown();
	TestKingTwoPlayers();
		TestRulesAreTheRooms();
		TestArcadeOnTheWire();
		TestPublicArcade();
		TestArcadePracticeRoom();
	} catch (const std::exception& error) {
		std::printf("room arcade threw: %s\n", error.what());
		return 1;
	}
	if (TEST_FAILURES()) std::printf("%d room arcade check(s) failed\n", TEST_FAILURES());
	else std::printf("room arcade: ok\n");
	return TEST_FAILURES() ? 1 : 0;
}
