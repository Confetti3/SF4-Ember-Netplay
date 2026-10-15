// The Ready timeout: at a waiting table with one fighter ready, the other
// leaves the seat after ReadyTimeoutMs without a room action, and the room
// says who (Event::Kind::ReadyTimeout).
#include "room_authority_support.hxx"

// The events are exactly one Ready timeout, of `member` at `table`.
static bool TimedOut(const std::vector<Event>& events, MemberId member, std::uint8_t table) {
	return events.size() == 1 && events[0].kind == Event::Kind::ReadyTimeout && events[0].member == member && events[0].table == table;
}

static void TestReadyTimeout() {
	RoomAuthority authority("Ready timeout", 4, 1);
	const auto p1 = Join(authority, 0, true), p2 = Join(authority, 1), queued = Join(authority, 2);
	for (const auto member : {p1, p2, queued})
		CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	authority.AdvanceTime(1000);
	CHECK(authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Ready)).accepted);
	CHECK(!authority.HasDueTimerTransition(90900));
	CHECK(authority.AdvanceTime(90900).empty());
	CHECK(authority.SnapshotView().tables[0].p2 == p2);
	CHECK(authority.HasDueTimerTransition(91000));
	CHECK(TimedOut(authority.AdvanceTime(91000), p2, 0));
	const auto& table = authority.SnapshotView().tables[0];
	CHECK(table.p1 == p1 && table.p2 == queued && table.queue.empty());
	CHECK(!table.ready[0] && !table.ready[1]);
	const auto* standing = FindMember(authority.SnapshotView(), p2);
	CHECK(standing && standing->status == MemberStatus::Idle && standing->table == -1);
	CHECK(!WatchesByChoice(table, p2));
}

static void TestReadyTimeoutActivity() {
	for (const bool fighterChange : {false, true}) {
		RoomAuthority authority("Active fighter", 4, 1);
		const auto p1 = Join(authority, 0, true), p2 = Join(authority, 1);
		for (const auto member : {p1, p2})
			CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
		CHECK(authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Ready)).accepted);
		authority.AdvanceTime(60000);
		if (fighterChange) CHECK(authority.SetMemberFighter(p2, 3)); // first shared fighter retains Ready
		else {
			auto action = TableAction(authority, p2, 0, ActionKind::Chat);
			action.text = "Still here";
			CHECK(authority.Apply(p2, action).accepted);
		}
		CHECK(authority.SnapshotView().tables[0].ready[0]);
		CHECK(authority.SnapshotFor(p1).tables[0].readyRemainingMs == ReadyTimeoutMs);
		CHECK(!authority.HasDueTimerTransition(90000));
		CHECK(authority.AdvanceTime(149999).empty());
		CHECK(authority.HasDueTimerTransition(150000));
		CHECK(TimedOut(authority.AdvanceTime(150000), p2, 0));
		CHECK(authority.SnapshotView().tables[0].p2 == 0);
	}
	// Ready starts the clock even if the other fighter has been idle longer.
	RoomAuthority authority("Later Ready", 4, 1);
	const auto p1 = Join(authority, 0, true), p2 = Join(authority, 1);
	for (const auto member : {p1, p2})
		CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	authority.AdvanceTime(200000);
	CHECK(authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Ready)).accepted);
	CHECK(authority.SnapshotFor(p2).tables[0].readyRemainingMs == ReadyTimeoutMs);
	authority.AdvanceTime(260000);
	auto chat = TableAction(authority, p1, 0, ActionKind::Chat); chat.text = "Waiting";
	CHECK(authority.Apply(p1, chat).accepted); // the ready fighter's activity does not restart it
	CHECK(authority.HasDueTimerTransition(290000));
}

static void TestReadyTimeoutRecoveryAndWire() {
	RoomAuthority source("Recovered Ready", 4, 1);
	const auto p1 = Join(source, 0, true), p2 = Join(source, 1);
	for (const auto member : {p1, p2})
		CHECK(source.Apply(member, TableAction(source, member, 0, ActionKind::Queue)).accepted);
	CHECK(source.Apply(p1, TableAction(source, p1, 0, ActionKind::Ready)).accepted);
	CHECK(source.SnapshotFor(p2).tables[0].readyRemainingMs == ReadyTimeoutMs);
	source.AdvanceTime(60000);
	const auto sent = source.SnapshotFor(p2);
	CHECK(sent.tables[0].readyRemainingMs == 30000);
	CHECK(source.SnapshotView().tables[0].readyRemainingMs == 0);
	nlohmann::json wire = sent;
	CHECK(wire["tables"][0]["ready_ms"] == 30000);
	CHECK(wire.get<Snapshot>().tables[0].readyRemainingMs == 30000);
	CHECK(!wire["tables"][1].contains("ready_ms"));
	wire["tables"][0].erase("ready_ms");
	CHECK(wire.get<Snapshot>().tables[0].readyRemainingMs == 0);
	wire["tables"][0]["unknown_future_key"] = 17;
	CHECK(wire.get<Snapshot>().tables[0].p2 == p2);
	const auto live = source.Checkpoint();
	CHECK(!live["snapshot"]["tables"][0].contains("ready_ms"));
	source.PauseForRecovery();
	CHECK(source.SnapshotFor(p1).tables[0].readyRemainingMs == 30000);
	for (const auto& checkpoint : {live, source.Checkpoint()}) {
		for (const auto resumeAt : {500ull, 1000000ull}) {
			RoomAuthority restored("Replica");
			CHECK(restored.RestoreCheckpoint(nlohmann::json::parse(checkpoint.dump())));
			restored.PauseForRecovery();
			CHECK(!restored.HasDueTimerTransition(resumeAt));
			restored.ResumeRecovery(resumeAt);
			CHECK(restored.SnapshotFor(p2).tables[0].readyRemainingMs == 30000);
			// A lower receiving clock is rebased to the retained age, as for the hold.
			const auto rebased = (std::max)(resumeAt, 60000ull);
			CHECK(restored.AdvanceTime(rebased).empty());
			CHECK(restored.AdvanceTime(rebased + 29999).empty());
			const auto timedOut = restored.AdvanceTime(rebased + 30000);
			CHECK(TimedOut(timedOut, p2, 0));
			// The event crosses the wire as it left.
			CHECK(TimedOut({nlohmann::json(timedOut.front()).get<Event>()}, p2, 0));
			CHECK(restored.SnapshotView().tables[0].p2 == 0);
			CHECK(restored.SnapshotFor(p1).tables[0].readyRemainingMs == 0);
			CHECK(!nlohmann::json(restored.SnapshotFor(p1))["tables"][0].contains("ready_ms"));
		}
	}
	// Healthy passive replicas age deadlines without applying their effects.
	source.AdvancePausedTimers(10000);
	CHECK(source.SnapshotFor(p2).tables[0].readyRemainingMs == 20000);
}

static void TestReadyTimeoutExclusions() {
	RoomAuthority base("Excluded Ready", 8, 1);
	const auto p1 = Join(base, 0, true), p2 = Join(base, 1);
	for (const auto member : {p1, p2})
		CHECK(base.Apply(member, TableAction(base, member, 0, ActionKind::Queue)).accepted);
	// Neither ready, then only one seat filled (Ready itself refuses an empty pair).
	CHECK(base.AdvanceTime(ReadyTimeoutMs * 2).empty());
	CHECK(base.SnapshotFor(p1).tables[0].readyRemainingMs == 0);
	CHECK(base.Apply(p2, TableAction(base, p2, 0, ActionKind::Unqueue)).accepted);
	CHECK(base.AdvanceTime(ReadyTimeoutMs * 3).empty());
	CHECK(base.SnapshotView().tables[0].p1 == p1);
	CHECK(base.Apply(p2, TableAction(base, p2, 0, ActionKind::Queue)).accepted);
	CHECK(base.Apply(p1, TableAction(base, p1, 0, ActionKind::Ready)).accepted);
	CHECK(base.Apply(p2, TableAction(base, p2, 0, ActionKind::Ready)).accepted);
	CHECK(base.SnapshotFor(p1).tables[0].readyRemainingMs == 0);
	CHECK(base.AdvanceTime(ReadyTimeoutMs * 4).empty()); // both Ready, phase Ready
	CHECK(base.BeginMatch(0, p1, p2).accepted);
	CHECK(base.AdvanceTime(ReadyTimeoutMs * 5).empty()); // Playing
	const auto generation = base.SnapshotView().tables[0].matchGeneration;
	for (const auto member : {p1, p2}) {
		auto report = TableAction(base, member, 0, ActionKind::RecordResult);
		report.matchGeneration = generation; report.result = member == p1 ? MatchResult::P1Win : MatchResult::P2Win;
		CHECK(base.Apply(member, report).accepted);
	}
	CHECK(base.SnapshotView().tables[0].phase == TablePhase::Paused);
	CHECK(base.SnapshotFor(p2).tables[0].readyRemainingMs == 0);
	CHECK(base.AdvanceTime(ReadyTimeoutMs * 6).empty()); // Paused
	CHECK(base.EndMatch(0, generation, MatchResult::Abort).accepted);
	auto firstAck = TableAction(base, p1, 0, ActionKind::AcknowledgeTerminal); firstAck.matchGeneration = generation;
	CHECK(base.Apply(p1, firstAck).accepted);
	CHECK(base.Apply(p1, TableAction(base, p1, 0, ActionKind::Ready)).accepted);
	CHECK(base.SnapshotFor(p2).terminalPending[0]);
	CHECK(base.SnapshotFor(p2).tables[0].readyRemainingMs == 0);
	CHECK(!base.HasDueTimerTransition(ReadyTimeoutMs * 7));
	CHECK(base.AdvanceTime(ReadyTimeoutMs * 7).empty());
	CHECK(base.SnapshotView().tables[0].p2 == p2);
	for (const auto member : {p1, p2}) {
		auto ack = TableAction(base, member, 0, ActionKind::AcknowledgeTerminal); ack.matchGeneration = generation;
		CHECK(base.Apply(member, ack).accepted);
	}
	CHECK(base.SnapshotFor(p2).tables[0].readyRemainingMs == ReadyTimeoutMs);
	CHECK(base.Apply(p1, TableAction(base, p1, 0, ActionKind::Close)).accepted);
	CHECK(!base.HasDueTimerTransition(ReadyTimeoutMs * 9));
	CHECK(base.AdvanceTime(ReadyTimeoutMs * 9).empty());

	RoomAuthority bound("Tournament Ready", 4, 1);
	const std::string endpointA(64, 'a'), endpointB(64, 'b');
	const auto a = bound.Join("A", {"room", endpointA}, true).snapshot.members.back().id;
	TournamentBinding binding;
	binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
	binding.assignmentGeneration = binding.bindingRevision = 1; binding.gamesToWin = 2;
	binding.fighters[0] = {endpointA, "emb1_aaaa"}; binding.fighters[1] = {endpointB, "emb1_bbbb"};
	CHECK(bound.BindTournament(binding).accepted);
	const auto b = bound.Join("B", {"room", endpointB}).snapshot.members.back().id;
	CHECK(bound.Apply(a, TableAction(bound, a, 0, ActionKind::Ready)).accepted);
	CHECK(!bound.HasDueTimerTransition(ReadyTimeoutMs * 2));
	CHECK(bound.AdvanceTime(ReadyTimeoutMs * 2).empty());
	CHECK(bound.SnapshotView().tables[0].p1 == a && bound.SnapshotView().tables[0].p2 == b);
	CHECK(bound.SnapshotFor(b).tables[0].readyRemainingMs == 0);
}

static void TestReadyTimeoutMemberReceipt() {
	RoomAuthority authority("Returning fighter", 8, 1);
	const auto p1 = Join(authority, 0, true), p2 = Join(authority, 1), returning = Join(authority, 2), opponent = Join(authority, 3);
	for (const auto member : {p1, p2})
		CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	CHECK(authority.Apply(returning, TableAction(authority, returning, 0, ActionKind::Watch)).accepted);
	for (const auto member : {p1, p2})
		CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	CHECK(authority.BeginMatch(0, p1, p2).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	for (const auto member : {opponent, returning})
		CHECK(authority.Apply(member, TableAction(authority, member, 1, ActionKind::Queue)).accepted);
	CHECK(authority.Apply(opponent, TableAction(authority, opponent, 1, ActionKind::Ready)).accepted);
	CHECK(authority.EndMatch(0, generation, MatchResult::Draw).accepted);
	CHECK(authority.SnapshotFor(returning).localTerminalPending);
	CHECK(authority.SnapshotFor(returning).tables[1].readyRemainingMs == 0);
	CHECK(!authority.HasDueTimerTransition(ReadyTimeoutMs * 2));
	CHECK(authority.AdvanceTime(ReadyTimeoutMs * 2).empty());
	CHECK(authority.SnapshotView().tables[1].p2 == returning);
}

int main() {
	TestReadyTimeout();
	TestReadyTimeoutActivity();
	TestReadyTimeoutRecoveryAndWire();
	TestReadyTimeoutExclusions();
	TestReadyTimeoutMemberReceipt();
	if (failures) return 1;
	std::puts("RoomReadyTimeout test passed");
	return 0;
}
