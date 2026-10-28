// First-to-N sets and seat rotation at a room table.
#include "room_authority_support.hxx"

#include <vector>

static void SetRules(RoomAuthority& authority, MemberId host, SetFormat format, RotationMode rotation) {
	Action rules = TableAction(authority, host, 0, ActionKind::SetRules);
	rules.rules.format = format;
	rules.rules.rotation = rotation;
	EXPECT(authority.Apply(host, rules).accepted);
}

static void QueueAll(RoomAuthority& authority, std::initializer_list<MemberId> members) {
	for (const auto member : members) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
}

// Both seated fighters ready, the game runs, ends with `result`, and every
// recipient acknowledges it. Returns the game's generation.
static std::uint64_t Play(RoomAuthority& authority, MatchResult result) {
	const auto seated = authority.SnapshotView().tables[0];
	for (const auto member : {seated.p1, seated.p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.BeginMatch(0, seated.p1, seated.p2).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	EXPECT(authority.EndMatch(0, generation, result).accepted);
	for (const auto member : authority.TerminalMembers(0, generation)) {
		Action acknowledgment = TableAction(authority, member, 0, ActionKind::AcknowledgeTerminal);
		acknowledgment.matchGeneration = generation;
		EXPECT(authority.Apply(member, acknowledgment).accepted);
	}
	return generation;
}

static MemberStatus StatusOf(const RoomAuthority& authority, MemberId member) {
	for (const auto& value : authority.SnapshotView().members) if (value.id == member) return value.status;
	return MemberStatus::Idle;
}

static bool Queued(const Table& table, MemberId member) {
	return std::find(table.queue.begin(), table.queue.end(), member) != table.queue.end();
}

// King of the hill: the winner keeps the seat, the loser goes to the back of the
// queue and the next queued player sits down. A spectator who never queued is
// never given a seat.
static void TestWinnerStays() {
	RoomAuthority authority("Hill", 8, 1);
	const MemberId host = Join(authority, 0, true);
	const MemberId a = Join(authority, 1), b = Join(authority, 2), c = Join(authority, 3), watcher = Join(authority, 4);
	QueueAll(authority, {a, b, c});
	EXPECT(authority.Apply(watcher, TableAction(authority, watcher, 0, ActionKind::Watch)).accepted);
	SetRules(authority, host, SetFormat::Ft2, RotationMode::WinnerStays);
	Play(authority, MatchResult::P1Win);
	Play(authority, MatchResult::P2Win);
	{
		const auto& t = authority.SnapshotView().tables[0];
		EXPECT(t.p1 == a && t.p2 == b && t.score[0] == 1 && t.score[1] == 1 && t.lastSet.generation == 0);
	}
	const auto decider = Play(authority, MatchResult::P1Win);
	{
		const auto& t = authority.SnapshotView().tables[0];
		EXPECT(t.p1 == a && t.p2 == c);
		EXPECT(t.queue == std::vector<MemberId>{b});
		EXPECT(t.score[0] == 0 && t.score[1] == 0);
		EXPECT(t.lastSet.generation == decider && t.lastSet.p1 == a && t.lastSet.p2 == b);
		EXPECT(t.lastSet.score[0] == 2 && t.lastSet.score[1] == 1 && t.lastSet.Winner() == a && t.lastSet.Loser() == b);
		EXPECT(t.streakHolder == a && t.streak == 1);
		EXPECT(!Queued(t, watcher) && t.p1 != watcher && t.p2 != watcher);
		EXPECT(t.phase == TablePhase::Waiting);
	}
	EXPECT(StatusOf(authority, b) == MemberStatus::Queued);
	EXPECT(StatusOf(authority, c) == MemberStatus::Seated);
	// The king wins again: the challenger goes to the back and b is up next.
	Play(authority, MatchResult::P1Win);
	Play(authority, MatchResult::P1Win);
	{
		const auto& t = authority.SnapshotView().tables[0];
		EXPECT(t.p1 == a && t.p2 == b && t.queue == std::vector<MemberId>{c});
		EXPECT(t.streakHolder == a && t.streak == 2);
	}
	// The challenger takes the hill: the streak moves with the win.
	Play(authority, MatchResult::P2Win);
	Play(authority, MatchResult::P2Win);
	{
		const auto& t = authority.SnapshotView().tables[0];
		EXPECT(t.p1 == c && t.p2 == b && t.queue == std::vector<MemberId>{a});
		EXPECT(t.streakHolder == b && t.streak == 1);
	}
	EXPECT(!Queued(authority.SnapshotView().tables[0], watcher));
}

static void TestLoserStaysAndBothRotate() {
	{
		RoomAuthority authority("Loser", 8, 1);
		const MemberId host = Join(authority, 0, true);
		const MemberId a = Join(authority, 1), b = Join(authority, 2), c = Join(authority, 3);
		QueueAll(authority, {a, b, c});
		SetRules(authority, host, SetFormat::Ft1, RotationMode::LoserStays);
		Play(authority, MatchResult::P1Win);
		const auto& t = authority.SnapshotView().tables[0];
		EXPECT(t.p1 == c && t.p2 == b && t.queue == std::vector<MemberId>{a});
		// The winner left the seat, so there is no streak to carry.
		EXPECT(t.streakHolder == 0 && t.streak == 0);
		EXPECT(t.lastSet.Winner() == a);
	}
	{
		RoomAuthority authority("Both", 8, 1);
		const MemberId host = Join(authority, 0, true);
		const MemberId a = Join(authority, 1), b = Join(authority, 2), c = Join(authority, 3), d = Join(authority, 4);
		QueueAll(authority, {a, b, c, d});
		SetRules(authority, host, SetFormat::Ft1, RotationMode::BothRotate);
		Play(authority, MatchResult::P2Win);
		const auto& t = authority.SnapshotView().tables[0];
		EXPECT(t.p1 == c && t.p2 == d);
		// Winner first, then loser.
		EXPECT((t.queue == std::vector<MemberId>{b, a}));
	}
	{
		// Both rotate with one player queued: the winner is next in line after them.
		RoomAuthority authority("Short", 8, 1);
		const MemberId host = Join(authority, 0, true);
		const MemberId a = Join(authority, 1), b = Join(authority, 2), c = Join(authority, 3);
		QueueAll(authority, {a, b, c});
		SetRules(authority, host, SetFormat::Ft1, RotationMode::BothRotate);
		Play(authority, MatchResult::P1Win);
		const auto& t = authority.SnapshotView().tables[0];
		EXPECT(t.p1 == c && t.p2 == a && t.queue == std::vector<MemberId>{b});
	}
}

// With nobody queued the same two start a new set. Draws never end one, and an
// open-ended table never rotates.
static void TestEmptyQueueDrawsAndNoSetLength() {
	RoomAuthority authority("Pair", 8, 1);
	const MemberId host = Join(authority, 0, true);
	const MemberId a = Join(authority, 1), b = Join(authority, 2);
	QueueAll(authority, {a, b});
	SetRules(authority, host, SetFormat::Ft1, RotationMode::WinnerStays);
	Play(authority, MatchResult::Draw);
	EXPECT(authority.SnapshotView().tables[0].lastSet.generation == 0);
	Play(authority, MatchResult::Abort);
	EXPECT(authority.SnapshotView().tables[0].lastSet.generation == 0);
	const auto decider = Play(authority, MatchResult::P2Win);
	{
		const auto& t = authority.SnapshotView().tables[0];
		EXPECT(t.p1 == a && t.p2 == b && t.queue.empty());
		EXPECT(t.score[0] == 0 && t.score[1] == 0 && t.lastSet.generation == decider && t.lastSet.Winner() == b);
		EXPECT(t.streakHolder == b && t.streak == 1);
	}
	Play(authority, MatchResult::P2Win);
	EXPECT(authority.SnapshotView().tables[0].streak == 2);
	SetRules(authority, host, SetFormat::Unlimited, RotationMode::WinnerStays);
	for (int game = 0; game < 3; ++game) Play(authority, MatchResult::P1Win);
	const auto& t = authority.SnapshotView().tables[0];
	EXPECT(t.p1 == a && t.p2 == b && t.score[0] == 3 && t.streak == 0);
}

// A different set length or rotation starts a new set; other rules keep it.
static void TestRulesChangeStartsANewSet() {
	RoomAuthority authority("Rules", 8, 1);
	const MemberId host = Join(authority, 0, true);
	const MemberId a = Join(authority, 1), b = Join(authority, 2);
	QueueAll(authority, {a, b});
	SetRules(authority, host, SetFormat::Ft3, RotationMode::WinnerStays);
	Play(authority, MatchResult::P1Win);
	Action rounds = TableAction(authority, host, 0, ActionKind::SetRules);
	rounds.rules = authority.SnapshotView().tables[0].rules;
	rounds.rules.roundCount = 5;
	EXPECT(authority.Apply(host, rounds).accepted);
	EXPECT(authority.SnapshotView().tables[0].score[0] == 1);
	SetRules(authority, host, SetFormat::Ft2, RotationMode::WinnerStays);
	EXPECT(authority.SnapshotView().tables[0].score[0] == 0);
	EXPECT(authority.SnapshotView().tables[0].rules.format == SetFormat::Ft2);
	// Every first to 1 through 10 is a set length (best of 7 included); 11 is not.
	SetRules(authority, host, SetFormat::Ft4, RotationMode::WinnerStays);
	SetRules(authority, host, SetFormat::Ft10, RotationMode::WinnerStays);
	EXPECT(authority.SnapshotView().tables[0].rules.format == SetFormat::Ft10);
	{
		Action longer = TableAction(authority, host, 0, ActionKind::SetRules);
		longer.rules.format = static_cast<SetFormat>(MaxSetLength + 1);
		EXPECT(!authority.Apply(host, longer).accepted);
		EXPECT(authority.SnapshotView().tables[0].rules.format == SetFormat::Ft10);
	}
	SetRules(authority, host, SetFormat::Ft2, RotationMode::LoserStays);
	EXPECT(authority.SnapshotView().tables[0].rules.rotation == RotationMode::LoserStays);
	// Rules a room was created with are kept, not reset to an open table.
	Rules defaults;
	defaults.format = SetFormat::Ft5;
	defaults.rotation = RotationMode::BothRotate;
	RoomAuthority created("Created", 8, 1, defaults);
	EXPECT(created.SnapshotView().tables[1].rules.format == SetFormat::Ft5);
	EXPECT(created.SnapshotView().tables[1].rules.rotation == RotationMode::BothRotate);
}

// The fighter rotated out still owns the game's receipt: a retried result is
// answered from it and its acknowledgement still counts.
static void TestRotatedFighterKeepsItsReceipt() {
	RoomAuthority authority("Receipt", 8, 1);
	const MemberId host = Join(authority, 0, true);
	const MemberId a = Join(authority, 1), b = Join(authority, 2), c = Join(authority, 3);
	QueueAll(authority, {a, b, c});
	SetRules(authority, host, SetFormat::Ft1, RotationMode::WinnerStays);
	for (const auto member : {a, b}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.BeginMatch(0, a, b).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	for (const auto member : {a, b}) {
		Action report = TableAction(authority, member, 0, ActionKind::RecordResult);
		report.matchGeneration = generation;
		report.result = MatchResult::P1Win;
		EXPECT(authority.Apply(member, report).accepted);
	}
	EXPECT(authority.SnapshotView().tables[0].p2 == c);
	Action retry = TableAction(authority, b, 0, ActionKind::RecordResult);
	retry.matchGeneration = generation;
	retry.result = MatchResult::P1Win;
	retry.actionId += 50;
	const auto replay = authority.Apply(b, retry);
	EXPECT(replay.accepted && replay.terminalReplay);
	EXPECT(authority.SnapshotView().tables[0].lastSet.score[0] == 1);
	const auto acknowledge = [&](MemberId member) {
		Action acknowledgment = TableAction(authority, member, 0, ActionKind::AcknowledgeTerminal);
		acknowledgment.matchGeneration = generation;
		EXPECT(authority.Apply(member, acknowledgment).accepted);
	};
	// The next game waits for both of the last game's fighters, the one who
	// left the seat included.
	acknowledge(a); acknowledge(c);
	EXPECT(authority.Apply(a, TableAction(authority, a, 0, ActionKind::Ready)).accepted);
	EXPECT(!authority.Apply(c, TableAction(authority, c, 0, ActionKind::Ready)).accepted);
	acknowledge(b);
	EXPECT(authority.Apply(c, TableAction(authority, c, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.BeginMatch(0, a, c).accepted);
	// The rotated fighter waits in the queue and watches the next game.
	const auto& t = authority.SnapshotView().tables[0];
	EXPECT(Queued(t, b) && std::find(t.spectators.begin(), t.spectators.end(), b) != t.spectators.end());
}

// The set record and streak survive a checkpoint and the wire, and a member
// who left is dropped from them.
static void TestSetHistoryOnTheWire() {
	RoomAuthority authority("Wire", 8, 1);
	const MemberId host = Join(authority, 0, true);
	const MemberId a = Join(authority, 1), b = Join(authority, 2), c = Join(authority, 3);
	QueueAll(authority, {a, b, c});
	SetRules(authority, host, SetFormat::Ft1, RotationMode::WinnerStays);
	const auto decider = Play(authority, MatchResult::P1Win);
	RoomAuthority restored("Restored");
	EXPECT(restored.RestoreCheckpoint(authority.Checkpoint()));
	{
		const auto& t = restored.SnapshotView().tables[0];
		EXPECT(t.lastSet.generation == decider && t.lastSet.p1 == a && t.lastSet.p2 == b && t.lastSet.score[0] == 1);
		EXPECT(t.streakHolder == a && t.streak == 1 && t.rules.format == SetFormat::Ft1);
	}
	const nlohmann::json wire = authority.SnapshotView();
	EXPECT(wire.at("protocol_version").get<int>() == static_cast<int>(ProtocolVersion));
	Snapshot read = wire.get<Snapshot>();
	EXPECT(read.tables[0].lastSet.Winner() == a && read.tables[0].streak == 1);
	auto departed = wire;
	auto& members = departed["members"];
	members.erase(std::remove_if(members.begin(), members.end(), [&](const nlohmann::json& m) { return m.at("id").get<MemberId>() == a; }), members.end());
	departed["tables"][0]["p1"] = std::uint64_t(0);
	read = departed.get<Snapshot>();
	EXPECT(read.tables[0].lastSet.p1 == 0 && read.tables[0].lastSet.p2 == b);
	EXPECT(read.tables[0].streakHolder == 0 && read.tables[0].streak == 0);
	// Leaving the room clears the record and ends the streak.
	EXPECT(authority.Leave(a).accepted);
	const auto& t = authority.SnapshotView().tables[0];
	EXPECT(t.lastSet.p1 == 0 && t.streakHolder == 0 && t.streak == 0);
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	try {
		TestWinnerStays();
		TestLoserStaysAndBothRotate();
		TestEmptyQueueDrawsAndNoSetLength();
		TestRulesChangeStartsANewSet();
		TestRotatedFighterKeepsItsReceipt();
		TestSetHistoryOnTheWire();
	} catch (const std::exception& error) {
		std::printf("room sets threw: %s\n", error.what());
		return 1;
	}
	if (TEST_FAILURES()) std::printf("%d room set check(s) failed\n", TEST_FAILURES());
	else std::printf("room sets: ok\n");
	return TEST_EXIT_CODE();
}
