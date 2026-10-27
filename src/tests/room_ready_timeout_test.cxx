// The Ready timeout: at a waiting table with one fighter ready, the other
// leaves the seat after ReadyTimeoutMs without a room action, and the room
// says who (Event::Kind::ReadyTimeout).
#include "room_authority_support.hxx"
#include <limits>

// The result dispute window of RoomModelDetail.hxx, which tests do not include.
constexpr std::uint64_t ResultDisputeTimeoutMs = 30000;

// The events are exactly one Ready timeout, of `member` at `table`.
static bool TimedOut(const std::vector<Event>& events, MemberId member, std::uint8_t table) {
	return events.size() == 1 && events[0].kind == Event::Kind::ReadyTimeout && events[0].member == member && events[0].table == table;
}

static void TestReadyTimeout() {
	RoomAuthority authority("Ready timeout", 4, 1);
	const auto p1 = Join(authority, 0, true), p2 = Join(authority, 1), queued = Join(authority, 2);
	for (const auto member : {p1, p2, queued})
		EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	authority.AdvanceTime(1000);
	EXPECT(authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Ready)).accepted);
	EXPECT(!authority.HasDueTimerTransition(90900));
	EXPECT(authority.AdvanceTime(90900).empty());
	EXPECT(authority.SnapshotView().tables[0].p2 == p2);
	EXPECT(authority.HasDueTimerTransition(91000));
	EXPECT(TimedOut(authority.AdvanceTime(91000), p2, 0));
	const auto& table = authority.SnapshotView().tables[0];
	EXPECT(table.p1 == p1 && table.p2 == queued && table.queue.empty());
	EXPECT(!table.ready[0] && !table.ready[1]);
	const auto* standing = FindMember(authority.SnapshotView(), p2);
	EXPECT(standing && standing->status == MemberStatus::Idle && standing->table == -1);
	EXPECT(!WatchesByChoice(table, p2));
}

// NextDueMs is the time left until HasDueTimerTransition turns true: zero
// exactly when it is true, and a deadline that is true from then on.
static const auto NoTimer = (std::numeric_limits<std::uint64_t>::max)();

static void TestNextDueMs() {
	RoomAuthority idle("No timers", 4, 1);
	const auto lone = Join(idle, 0, true);
	EXPECT(idle.NextDueMs(0) == NoTimer && idle.NextDueMs(1000000) == NoTimer);
	EXPECT(idle.Apply(lone, TableAction(idle, lone, 0, ActionKind::Queue)).accepted);
	EXPECT(idle.NextDueMs(5000) == NoTimer); // seated alone, nobody ready

	// A Ready at 1000 ms starts the opponent's 90 s clock.
	RoomAuthority authority("Next deadline", 4, 1);
	const auto p1 = Join(authority, 0, true), p2 = Join(authority, 1);
	for (const auto member : {p1, p2})
		EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	authority.AdvanceTime(1000);
	EXPECT(authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.NextDueMs(1000) == ReadyTimeoutMs);
	EXPECT(authority.NextDueMs(50000) == ReadyTimeoutMs - 49000);
	EXPECT(authority.NextDueMs(0) == ReadyTimeoutMs); // a backward clock input leaves the room's own clock
	for (const std::uint64_t now : {1000ull, 5000ull, 90999ull, 91000ull, 95000ull})
		EXPECT(authority.HasDueTimerTransition(now) == (authority.NextDueMs(now) == 0));
	const auto left = authority.NextDueMs(1000);
	EXPECT(!authority.HasDueTimerTransition(1000 + left - 1) && authority.HasDueTimerTransition(1000 + left));
	EXPECT(TimedOut(authority.AdvanceTime(1000 + left), p2, 0));
	EXPECT(authority.NextDueMs(1000 + left) == NoTimer); // the seat cleared, nobody waits

	// A paused room holds ages, so what is left does not move with the clock.
	RoomAuthority source("Paused", 4, 1);
	const auto a = Join(source, 0, true), b = Join(source, 1);
	for (const auto member : {a, b})
		EXPECT(source.Apply(member, TableAction(source, member, 0, ActionKind::Queue)).accepted);
	EXPECT(source.Apply(a, TableAction(source, a, 0, ActionKind::Ready)).accepted);
	source.AdvanceTime(60000);
	EXPECT(source.NextDueMs(60000) == 30000);
	source.PauseForRecovery();
	EXPECT(source.NextDueMs(0) == 30000 && source.NextDueMs(500000) == 30000);
	EXPECT(!source.HasDueTimerTransition(500000));
	source.AdvancePausedTimers(29999);
	EXPECT(source.NextDueMs(0) == 1);
	source.AdvancePausedTimers(1);
	EXPECT(source.NextDueMs(0) == 0 && source.HasDueTimerTransition(0));
}

// A game's result that nobody settles is disputed after ResultDisputeTimeoutMs,
// and the paused table that follows runs no timer.
static void TestNextDueMsResultDispute() {
	RoomAuthority authority("Dispute", 8, 1);
	const auto host = Join(authority, 0, true);
	const auto p1 = Join(authority, 1), p2 = Join(authority, 2);
	(void)host;
	for (const auto member : {p1, p2})
		EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	const auto seats = authority.SnapshotView().tables[0];
	for (const auto member : {seats.p1, seats.p2})
		EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.BeginMatch(0, seats.p1, seats.p2).accepted);
	EXPECT(authority.NextDueMs(0) == NoTimer); // playing, no result yet
	auto finished = TableAction(authority, p1, 0, ActionKind::MatchFinished);
	finished.matchGeneration = authority.SnapshotView().tables[0].matchGeneration;
	EXPECT(authority.Apply(p1, finished).accepted);
	authority.AdvanceTime(10000);
	EXPECT(authority.NextDueMs(10000) == ResultDisputeTimeoutMs - 10000);
	for (const std::uint64_t now : {10000ull, 29999ull, 30000ull, 45000ull})
		EXPECT(authority.HasDueTimerTransition(now) == (authority.NextDueMs(now) == 0));
	EXPECT(!authority.AdvanceTime(ResultDisputeTimeoutMs).empty());
	EXPECT(authority.SnapshotView().tables[0].phase == TablePhase::Paused);
	EXPECT(authority.NextDueMs(60000) == NoTimer && !authority.HasDueTimerTransition(60000));
}

static void TestReadyTimeoutActivity() {
	for (const bool fighterChange : {false, true}) {
		RoomAuthority authority("Active fighter", 4, 1);
		const auto p1 = Join(authority, 0, true), p2 = Join(authority, 1);
		for (const auto member : {p1, p2})
			EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
		EXPECT(authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Ready)).accepted);
		authority.AdvanceTime(60000);
		if (fighterChange) EXPECT(authority.SetMemberFighter(p2, 3)); // first shared fighter retains Ready
		else {
			auto action = TableAction(authority, p2, 0, ActionKind::Chat);
			action.text = "Still here";
			EXPECT(authority.Apply(p2, action).accepted);
		}
		EXPECT(authority.SnapshotView().tables[0].ready[0]);
		EXPECT(authority.SnapshotFor(p1).tables[0].readyRemainingMs == ReadyTimeoutMs);
		EXPECT(!authority.HasDueTimerTransition(90000));
		EXPECT(authority.AdvanceTime(149999).empty());
		EXPECT(authority.HasDueTimerTransition(150000));
		EXPECT(TimedOut(authority.AdvanceTime(150000), p2, 0));
		EXPECT(authority.SnapshotView().tables[0].p2 == 0);
	}
	// Ready starts the clock even if the other fighter has been idle longer.
	RoomAuthority authority("Later Ready", 4, 1);
	const auto p1 = Join(authority, 0, true), p2 = Join(authority, 1);
	for (const auto member : {p1, p2})
		EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	authority.AdvanceTime(200000);
	EXPECT(authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.SnapshotFor(p2).tables[0].readyRemainingMs == ReadyTimeoutMs);
	authority.AdvanceTime(260000);
	auto chat = TableAction(authority, p1, 0, ActionKind::Chat); chat.text = "Waiting";
	EXPECT(authority.Apply(p1, chat).accepted); // the ready fighter's activity does not restart it
	EXPECT(authority.HasDueTimerTransition(290000));
}

static void TestReadyTimeoutRecoveryAndWire() {
	RoomAuthority source("Recovered Ready", 4, 1);
	const auto p1 = Join(source, 0, true), p2 = Join(source, 1);
	for (const auto member : {p1, p2})
		EXPECT(source.Apply(member, TableAction(source, member, 0, ActionKind::Queue)).accepted);
	EXPECT(source.Apply(p1, TableAction(source, p1, 0, ActionKind::Ready)).accepted);
	EXPECT(source.SnapshotFor(p2).tables[0].readyRemainingMs == ReadyTimeoutMs);
	source.AdvanceTime(60000);
	const auto sent = source.SnapshotFor(p2);
	EXPECT(sent.tables[0].readyRemainingMs == 30000);
	EXPECT(source.SnapshotView().tables[0].readyRemainingMs == 0);
	nlohmann::json wire = sent;
	EXPECT(wire["tables"][0]["ready_ms"] == 30000);
	EXPECT(wire.get<Snapshot>().tables[0].readyRemainingMs == 30000);
	EXPECT(!wire["tables"][1].contains("ready_ms"));
	wire["tables"][0].erase("ready_ms");
	EXPECT(wire.get<Snapshot>().tables[0].readyRemainingMs == 0);
	wire["tables"][0]["unknown_future_key"] = 17;
	EXPECT(wire.get<Snapshot>().tables[0].p2 == p2);
	const auto live = source.Checkpoint();
	EXPECT(!live["snapshot"]["tables"][0].contains("ready_ms"));
	source.PauseForRecovery();
	EXPECT(source.SnapshotFor(p1).tables[0].readyRemainingMs == 30000);
	for (const auto& checkpoint : {live, source.Checkpoint()}) {
		for (const auto resumeAt : {500ull, 1000000ull}) {
			RoomAuthority restored("Replica");
			EXPECT(restored.RestoreCheckpoint(nlohmann::json::parse(checkpoint.dump())));
			restored.PauseForRecovery();
			EXPECT(!restored.HasDueTimerTransition(resumeAt));
			restored.ResumeRecovery(resumeAt);
			EXPECT(restored.SnapshotFor(p2).tables[0].readyRemainingMs == 30000);
			// A lower receiving clock is rebased to the retained age, as for the hold.
			const auto rebased = (std::max)(resumeAt, 60000ull);
			EXPECT(restored.AdvanceTime(rebased).empty());
			EXPECT(restored.AdvanceTime(rebased + 29999).empty());
			const auto timedOut = restored.AdvanceTime(rebased + 30000);
			EXPECT(TimedOut(timedOut, p2, 0));
			// The event crosses the wire as it left.
			EXPECT(TimedOut({nlohmann::json(timedOut.front()).get<Event>()}, p2, 0));
			EXPECT(restored.SnapshotView().tables[0].p2 == 0);
			EXPECT(restored.SnapshotFor(p1).tables[0].readyRemainingMs == 0);
			EXPECT(!nlohmann::json(restored.SnapshotFor(p1))["tables"][0].contains("ready_ms"));
		}
	}
	// Healthy passive replicas age deadlines without applying their effects.
	source.AdvancePausedTimers(10000);
	EXPECT(source.SnapshotFor(p2).tables[0].readyRemainingMs == 20000);
}

static void TestReadyTimeoutExclusions() {
	RoomAuthority base("Excluded Ready", 8, 1);
	const auto p1 = Join(base, 0, true), p2 = Join(base, 1);
	for (const auto member : {p1, p2})
		EXPECT(base.Apply(member, TableAction(base, member, 0, ActionKind::Queue)).accepted);
	// Neither ready, then only one seat filled (Ready itself refuses an empty pair).
	EXPECT(base.AdvanceTime(ReadyTimeoutMs * 2).empty());
	EXPECT(base.SnapshotFor(p1).tables[0].readyRemainingMs == 0);
	EXPECT(base.Apply(p2, TableAction(base, p2, 0, ActionKind::Unqueue)).accepted);
	EXPECT(base.AdvanceTime(ReadyTimeoutMs * 3).empty());
	EXPECT(base.SnapshotView().tables[0].p1 == p1);
	EXPECT(base.Apply(p2, TableAction(base, p2, 0, ActionKind::Queue)).accepted);
	EXPECT(base.Apply(p1, TableAction(base, p1, 0, ActionKind::Ready)).accepted);
	EXPECT(base.Apply(p2, TableAction(base, p2, 0, ActionKind::Ready)).accepted);
	EXPECT(base.SnapshotFor(p1).tables[0].readyRemainingMs == 0);
	EXPECT(base.AdvanceTime(ReadyTimeoutMs * 4).empty()); // both Ready, phase Ready
	EXPECT(base.BeginMatch(0, p1, p2).accepted);
	EXPECT(base.AdvanceTime(ReadyTimeoutMs * 5).empty()); // Playing
	const auto generation = base.SnapshotView().tables[0].matchGeneration;
	for (const auto member : {p1, p2}) {
		auto report = TableAction(base, member, 0, ActionKind::RecordResult);
		report.matchGeneration = generation; report.result = member == p1 ? MatchResult::P1Win : MatchResult::P2Win;
		EXPECT(base.Apply(member, report).accepted);
	}
	EXPECT(base.SnapshotView().tables[0].phase == TablePhase::Paused);
	EXPECT(base.SnapshotFor(p2).tables[0].readyRemainingMs == 0);
	EXPECT(base.AdvanceTime(ReadyTimeoutMs * 6).empty()); // Paused
	EXPECT(base.EndMatch(0, generation, MatchResult::Abort).accepted);
	auto firstAck = TableAction(base, p1, 0, ActionKind::AcknowledgeTerminal); firstAck.matchGeneration = generation;
	EXPECT(base.Apply(p1, firstAck).accepted);
	EXPECT(base.Apply(p1, TableAction(base, p1, 0, ActionKind::Ready)).accepted);
	EXPECT(base.SnapshotFor(p2).terminalPending[0]);
	EXPECT(base.SnapshotFor(p2).tables[0].readyRemainingMs == 0);
	EXPECT(!base.HasDueTimerTransition(ReadyTimeoutMs * 7));
	EXPECT(base.AdvanceTime(ReadyTimeoutMs * 7).empty());
	EXPECT(base.SnapshotView().tables[0].p2 == p2);
	for (const auto member : {p1, p2}) {
		auto ack = TableAction(base, member, 0, ActionKind::AcknowledgeTerminal); ack.matchGeneration = generation;
		EXPECT(base.Apply(member, ack).accepted);
	}
	EXPECT(base.SnapshotFor(p2).tables[0].readyRemainingMs == ReadyTimeoutMs);
	EXPECT(base.Apply(p1, TableAction(base, p1, 0, ActionKind::Close)).accepted);
	EXPECT(!base.HasDueTimerTransition(ReadyTimeoutMs * 9));
	EXPECT(base.AdvanceTime(ReadyTimeoutMs * 9).empty());

	RoomAuthority bound("Tournament Ready", 4, 1);
	const std::string endpointA(64, 'a'), endpointB(64, 'b');
	const auto a = bound.Join("A", {"room", endpointA}, true).snapshot.members.back().id;
	TournamentBinding binding;
	binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
	binding.assignmentGeneration = binding.bindingRevision = 1; binding.gamesToWin = 2;
	binding.fighters[0] = {endpointA, "emb1_aaaa"}; binding.fighters[1] = {endpointB, "emb1_bbbb"};
	EXPECT(bound.BindTournament(binding).accepted);
	const auto b = bound.Join("B", {"room", endpointB}).snapshot.members.back().id;
	EXPECT(bound.Apply(a, TableAction(bound, a, 0, ActionKind::Ready)).accepted);
	EXPECT(!bound.HasDueTimerTransition(ReadyTimeoutMs * 2));
	EXPECT(bound.AdvanceTime(ReadyTimeoutMs * 2).empty());
	EXPECT(bound.SnapshotView().tables[0].p1 == a && bound.SnapshotView().tables[0].p2 == b);
	EXPECT(bound.SnapshotFor(b).tables[0].readyRemainingMs == 0);
}

static void TestReadyTimeoutMemberReceipt() {
	RoomAuthority authority("Returning fighter", 8, 1);
	const auto p1 = Join(authority, 0, true), p2 = Join(authority, 1), returning = Join(authority, 2), opponent = Join(authority, 3);
	for (const auto member : {p1, p2})
		EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	EXPECT(authority.Apply(returning, TableAction(authority, returning, 0, ActionKind::Watch)).accepted);
	for (const auto member : {p1, p2})
		EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.BeginMatch(0, p1, p2).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	for (const auto member : {opponent, returning})
		EXPECT(authority.Apply(member, TableAction(authority, member, 1, ActionKind::Queue)).accepted);
	EXPECT(authority.Apply(opponent, TableAction(authority, opponent, 1, ActionKind::Ready)).accepted);
	EXPECT(authority.EndMatch(0, generation, MatchResult::Draw).accepted);
	EXPECT(authority.SnapshotFor(returning).localTerminalPending);
	EXPECT(authority.SnapshotFor(returning).tables[1].readyRemainingMs == 0);
	EXPECT(!authority.HasDueTimerTransition(ReadyTimeoutMs * 2));
	EXPECT(authority.AdvanceTime(ReadyTimeoutMs * 2).empty());
	EXPECT(authority.SnapshotView().tables[1].p2 == returning);
}

int main() {
	TestReadyTimeout();
	TestNextDueMs();
	TestNextDueMsResultDispute();
	TestReadyTimeoutActivity();
	TestReadyTimeoutRecoveryAndWire();
	TestReadyTimeoutExclusions();
	TestReadyTimeoutMemberReceipt();
	if (TEST_FAILURES()) return 1;
	std::puts("RoomReadyTimeout test passed");
	return 0;
}
