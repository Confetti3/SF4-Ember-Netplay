#include "room_authority_support.hxx"
#include <limits>

using nlohmann::json;

static Result Wire(RoomAuthority& room, MemberId member, const Action& action) {
	return room.Apply(member, json::parse(json(action).dump()).get<Action>());
}
static void Recover(RoomAuthority& room) {
	const auto saved = json::parse(room.Checkpoint().dump());
	RoomAuthority restored("restored");
	CHECK(restored.RestoreCheckpoint(saved));
	room = std::move(restored);
}
static unsigned EndEvents(const Result& result) {
	return static_cast<unsigned>(std::count_if(result.events.begin(), result.events.end(),
		[](const Event& e) { return e.kind == Event::Kind::MatchEnded; }));
}
static MemberId JoinCapable(RoomAuthority& room, int index, bool host = false, bool capable = true) {
	MemberProfile profile;
	profile.continuousCapable = capable;
	const auto joined = room.Join("Player" + std::to_string(index), Peer(index), host, profile);
	CHECK(joined.accepted);
	return joined.accepted ? joined.snapshot.members.back().id : 0;
}

struct Fixture {
	RoomAuthority room;
	MemberId a = JoinCapable(room, 0, true), b = JoinCapable(room, 1), c = JoinCapable(room, 2), d = JoinCapable(room, 3), watcher = JoinCapable(room, 4);
	std::uint64_t generation = 0;
	Fixture(SetFormat format = SetFormat::Unlimited, RotationMode rotation = RotationMode::WinnerStays, DelayPolicy policy = DelayPolicy::Shared)
        : room("continuous",12,23,Rules{},false,policy) {
		auto rules = TableAction(room, a, 0, ActionKind::SetRules);
		rules.rules.format = format; rules.rules.rotation = rotation; rules.rules.continuousRematch = true;
		if (!(rules.rules == room.SnapshotView().tables[0].rules)) CHECK(Wire(room, a, rules).accepted);
		for (auto member : {a, b, c, d}) CHECK(Wire(room, member, TableAction(room, member, 0, ActionKind::Queue)).accepted);
		CHECK(Wire(room, watcher, TableAction(room, watcher, 0, ActionKind::Watch)).accepted);
		Start();
	}
	void Start() {
		const auto t = TableCopy();
		for (auto member : {t.p1, t.p2}) {
			auto ready = TableAction(room, member, 0, ActionKind::Ready); ready.continuous = true; ready.inputDelay = member==t.p1?1:5;
			CHECK(Wire(room, member, ready).accepted);
		}
		CHECK(room.BeginMatch(0, t.p1, t.p2).accepted);
		generation = TableCopy().matchGeneration;
		CHECK(TableCopy().continuous.enabled);
	}
	Table TableCopy() const { return room.SnapshotView().tables[0]; }
	Action Report(MemberId member, std::uint64_t game, MatchResult outcome) const {
		auto action = TableAction(room, member, 0, ActionKind::RecordContinuousResult);
		action.matchGeneration = generation; action.continuousGame = game; action.result = outcome; return action;
	}
	Action Finish(MemberId member) const {
		auto action = TableAction(room, member, 0, ActionKind::FinishContinuousMatch);
		action.matchGeneration = generation; action.continuousGame = TableCopy().continuous.games; return action;
	}
	void Game(MatchResult outcome, bool restore = true) {
		const auto before = TableCopy();
		const auto roster = room.MatchRoster(0);
		auto first = Report(before.p2, before.continuous.games + 1, outcome);
		CHECK(Wire(room, before.p2, first).accepted);
		CHECK(TableCopy().score[0] == before.score[0] && TableCopy().score[1] == before.score[1]);
		if (restore) Recover(room);
		CHECK(Wire(room, before.p2, first).reason == RejectReason::DuplicateResult);
		const auto second = Wire(room, before.p1, Report(before.p1, first.continuousGame, outcome));
		CHECK(second.accepted && EndEvents(second) == 0);
		CHECK(TableCopy().continuous.games == first.continuousGame);
		CHECK(TableCopy().matchGeneration == generation && TableCopy().phase == TablePhase::Playing);
		CHECK(TableCopy().p1 == before.p1 && TableCopy().p2 == before.p2 && room.MatchRoster(0) == roster);
		CHECK(!room.SnapshotFor(before.p1).localTerminalPending);
		if (restore) Recover(room);
		CHECK(Wire(room, before.p2, first).reason == RejectReason::DuplicateResult);
	}
};

static void SetsAndRotation() {
	for (auto rotation : {RotationMode::WinnerStays, RotationMode::LoserStays, RotationMode::BothRotate}) {
		Fixture f(SetFormat::Ft3, rotation);
		for (auto result : {MatchResult::P2Win, MatchResult::P1Win, MatchResult::P2Win,
			MatchResult::Draw, MatchResult::P1Win, MatchResult::P2Win}) f.Game(result);
		auto t = f.TableCopy();
		CHECK(t.score[0] == 2 && t.score[1] == 3 && t.continuous.games == 6);
		CHECK(t.lastSet.generation == 0 && t.p1 == f.a && t.p2 == f.b);
		CHECK(!Wire(f.room, f.a, f.Report(f.a, 7, MatchResult::P1Win)).accepted);
		auto first = f.Finish(f.b), second = f.Finish(f.a);
		CHECK(!Wire(f.room, f.watcher, first).accepted);
		CHECK(EndEvents(Wire(f.room, f.b, first)) == 0);
		CHECK(f.TableCopy().phase == TablePhase::Playing && f.TableCopy().continuous.finishMask == 2);
		Recover(f.room);
		const auto ended = Wire(f.room, f.a, second);
		CHECK(ended.accepted && EndEvents(ended) == 1);
		t = f.TableCopy();
		CHECK(t.continuous.closed && t.score[0] == 0 && t.score[1] == 0);
		CHECK(t.lastSet.generation == f.generation && t.lastSet.score[0] == 2 && t.lastSet.score[1] == 3);
		CHECK(t.lastSet.Winner() == f.b && t.lastSet.Loser() == f.a);
		if (rotation == RotationMode::WinnerStays) CHECK(t.p1 == f.c && t.p2 == f.b);
		if (rotation == RotationMode::LoserStays) CHECK(t.p1 == f.a && t.p2 == f.c);
		if (rotation == RotationMode::BothRotate) CHECK(t.p1 == f.c && t.p2 == f.d);
		CHECK(t.p1 != f.watcher && t.p2 != f.watcher);
		Recover(f.room);
		const auto checkpoint = f.room.Checkpoint();
		const auto replay = Wire(f.room, f.a, second);
		CHECK(replay.accepted && replay.terminalReplay && EndEvents(replay) == 1);
		CHECK(json(f.TableCopy()) == checkpoint.at("snapshot").at("tables").at(0));
		CHECK(!Wire(f.room, f.watcher, second).accepted);
		for(auto member:f.room.TerminalMembers(0,f.generation)) {
			auto ack=TableAction(f.room,member,0,ActionKind::AcknowledgeTerminal);ack.matchGeneration=f.generation;
			CHECK(Wire(f.room,member,ack).accepted);
		}
		const auto previousGeneration=f.generation;
		f.Start(); // the queued spectator can now be a fighter in a fresh set
		CHECK(f.generation!=previousGeneration&&f.TableCopy().continuous.games==0);
		CHECK(f.TableCopy().continuous.finishMask==0&&!f.TableCopy().continuous.closed);
		CHECK(f.TableCopy().score[0]==0&&f.TableCopy().score[1]==0);
		f.Game(MatchResult::P1Win);
		CHECK(f.TableCopy().score[0]==1&&f.TableCopy().score[1]==0);
	}
}

static void ExitAbortAndUnlimited() {
	Fixture f;
	// More than one diagnostic rematch and more than the wire's 15 epochs.
	for (unsigned n = 0; n < 40; ++n) f.Game(n % 2 ? MatchResult::P2Win : MatchResult::P1Win, false);
	CHECK(f.TableCopy().score[0] == 20 && f.TableCopy().score[1] == 20);
	CHECK(Wire(f.room, f.a, f.Finish(f.a)).accepted);
	CHECK(!Wire(f.room, f.b, f.Report(f.b, 41, MatchResult::P2Win)).accepted);
	CHECK(Wire(f.room, f.b, f.Finish(f.b)).accepted);
	CHECK(f.TableCopy().score[0] == 20 && f.TableCopy().score[1] == 20);
	for (auto member : f.room.TerminalMembers(0, f.generation)) {
		auto ack = TableAction(f.room, member, 0, ActionKind::AcknowledgeTerminal); ack.matchGeneration = f.generation;
		CHECK(Wire(f.room, member, ack).accepted);
	}
	f.Start();
	CHECK(f.TableCopy().continuous.games == 0 && f.TableCopy().continuous.initialScore[0] == 20);
	f.Game(MatchResult::P2Win);
	auto abort = TableAction(f.room, f.a, 0, ActionKind::AbortMatch); abort.matchGeneration = f.generation;
	CHECK(Wire(f.room, f.a, abort).accepted);
	CHECK(f.TableCopy().score[0] == 20 && f.TableCopy().score[1] == 21 && f.TableCopy().continuous.closed);
	Recover(f.room);
	// FT completion survives a disconnect between score commit and exit votes.
	Fixture ft(SetFormat::Ft1); ft.Game(MatchResult::P1Win);
	CHECK(ft.room.EndMatch(0, ft.generation, MatchResult::Abort).accepted);
	CHECK(ft.TableCopy().lastSet.Winner() == ft.a && ft.TableCopy().p2 == ft.c);
}

static void RulesChangeBetweenSessions() {
	Fixture f(SetFormat::Ft3);
	for (unsigned game = 0; game < 3; ++game) f.Game(MatchResult::P1Win);
	CHECK(Wire(f.room, f.a, f.Finish(f.a)).accepted);
	CHECK(EndEvents(Wire(f.room, f.b, f.Finish(f.b))) == 1);
	for (auto member : f.room.TerminalMembers(0, f.generation)) {
		auto ack = TableAction(f.room, member, 0, ActionKind::AcknowledgeTerminal);
		ack.matchGeneration = f.generation;
		CHECK(Wire(f.room, member, ack).accepted);
	}
	const auto priorGeneration = f.generation;
	const auto seats = f.TableCopy();
	auto oldReady = TableAction(f.room, seats.p1, 0, ActionKind::Ready);
	oldReady.continuous = true;
	CHECK(Wire(f.room, seats.p1, oldReady).accepted);
	CHECK(f.TableCopy().ready[0]);
	auto rules = TableAction(f.room, f.a, 0, ActionKind::SetRules);
	rules.rules = seats.rules;
	rules.rules.format = SetFormat::Unlimited;
	CHECK(Wire(f.room, f.a, rules).accepted);
	CHECK(f.TableCopy().rules.format == SetFormat::Unlimited);
	CHECK(!f.TableCopy().ready[0] && !f.TableCopy().ready[1]);
	CHECK(f.TableCopy().score[0] == 0 && f.TableCopy().score[1] == 0);
	Recover(f.room);
	f.Start();
	CHECK(f.generation != priorGeneration);
	CHECK(f.TableCopy().rules.format == SetFormat::Unlimited);
	CHECK(f.TableCopy().continuous.initialScore[0] == 0 && f.TableCopy().continuous.initialScore[1] == 0);
	for (unsigned game = 1; game <= 4; ++game) {
		f.Game(MatchResult::P1Win);
		CHECK(f.TableCopy().score[0] == game);
		CHECK(f.TableCopy().phase == TablePhase::Playing && !f.TableCopy().continuous.closed);
		CHECK(f.TableCopy().p1 == seats.p1 && f.TableCopy().p2 == seats.p2);
	}
	// Invalid edits cannot silently change the live battle's limit.
	const auto before = json(f.TableCopy());
	auto duringMatch = TableAction(f.room, f.a, 0, ActionKind::SetRules);
	duringMatch.rules = f.TableCopy().rules;
	duringMatch.rules.roundCount = 0;
	CHECK(f.room.Apply(f.a, duringMatch).reason == RejectReason::InvalidRules);
	CHECK(json(f.TableCopy()) == before);
	f.Game(MatchResult::P1Win);
	CHECK(f.TableCopy().score[0] == 5 && !f.TableCopy().continuous.closed);
}

static void LiveRulesCloseOneTable() {
	for (bool paused : {false, true}) {
		Fixture f(SetFormat::Ft3);
		const auto x = Join(f.room, 5), y = Join(f.room, 6);
		for (auto member : {x, y})
			CHECK(Wire(f.room, member, TableAction(f.room, member, 1, ActionKind::Queue)).accepted);
		for (auto member : {x, y}) {
			auto ready = TableAction(f.room, member, 1, ActionKind::Ready);
			ready.continuous = true;
			CHECK(Wire(f.room, member, ready).accepted);
		}
		CHECK(f.room.BeginMatch(1, x, y).accepted);
		f.Game(MatchResult::P2Win);
		if (paused) {
			CHECK(Wire(f.room, f.a, f.Report(f.a, 2, MatchResult::P1Win)).accepted);
			CHECK(Wire(f.room, f.b, f.Report(f.b, 2, MatchResult::P2Win)).accepted);
			CHECK(f.TableCopy().phase == TablePhase::Paused);
		}
		const auto before = f.TableCopy();
		const auto roster = f.room.MatchRoster(0);
		const auto otherTable = json(f.room.SnapshotView().tables[1]);
		const auto otherRoster = f.room.MatchRoster(1);
		// Even a host applying unchanged rules must leave a live game alone.
		auto equal = TableAction(f.room, f.a, 0, ActionKind::SetRules);
		equal.rules = before.rules;
		equal.actionId = 0;
		const auto unchanged = Wire(f.room, f.a, equal);
		CHECK(unchanged.accepted && EndEvents(unchanged) == 0);
		CHECK(json(f.TableCopy()) == json(before));
		auto edit = TableAction(f.room, f.a, 0, ActionKind::SetRules);
		edit.rules = before.rules;
		edit.rules.roundCount = 5;
		auto denied = edit;
		denied.actionId = TableAction(f.room, f.b, 0, ActionKind::SetRules).actionId;
		CHECK(Wire(f.room, f.b, denied).reason == RejectReason::NotHost);
		auto stale = edit;
		--stale.tableRevision;
		CHECK(Wire(f.room, f.a, stale).reason == RejectReason::StaleTable);
		CHECK(json(f.TableCopy()) == json(before));
		const auto changed = Wire(f.room, f.a, edit);
		CHECK(changed.accepted && EndEvents(changed) == 1);
		CHECK(json(changed.snapshot.tables[0]) == json(f.TableCopy()));
		for (const auto& event : changed.events) {
			CHECK(event.table == 0 && event.matchGeneration == f.generation);
			if (event.kind == Event::Kind::MatchEnded) CHECK(event.result == MatchResult::Cancel);
		}
		CHECK(f.TableCopy().phase == TablePhase::Waiting && f.TableCopy().continuous.closed);
		CHECK(!f.TableCopy().ready[0] && !f.TableCopy().ready[1] && !f.TableCopy().resultPending);
		CHECK(f.TableCopy().rules == edit.rules && f.TableCopy().score[0] == 0 && f.TableCopy().score[1] == 1);
		CHECK(f.TableCopy().p1 == before.p1 && f.TableCopy().p2 == before.p2);
		CHECK(f.room.MatchRoster(0).empty() && f.room.TerminalMembers(0, f.generation) == roster);
		CHECK(json(f.room.SnapshotView().tables[1]) == otherTable && f.room.MatchRoster(1) == otherRoster);
		for (auto member : roster) {
			const auto pending = f.room.PendingTerminalEvents(member);
			CHECK(pending.size() == 1 && pending[0].result == MatchResult::Cancel);
		}
		Recover(f.room);
		const auto committed = f.room.Checkpoint();
		const auto retry = Wire(f.room, f.a, edit);
		CHECK(retry.accepted && EndEvents(retry) == 0);
		CHECK(f.room.Checkpoint() == committed);
		CHECK(!Wire(f.room, f.a, f.Report(f.a, 2, MatchResult::P1Win)).accepted);
		const auto lateFinish = Wire(f.room, f.b, f.Finish(f.b));
		CHECK(lateFinish.accepted && lateFinish.terminalReplay && EndEvents(lateFinish) == 1);
		CHECK(!lateFinish.events.empty() && lateFinish.events.front().result == MatchResult::Cancel);
		CHECK(f.TableCopy().score[1] == 1);
	}
}

static void LiveSetLengthChangeAndLedgerPressure() {
	Fixture f(SetFormat::Ft3);
	f.Game(MatchResult::P1Win);
	const auto roster = f.room.MatchRoster(0);
	auto edit = TableAction(f.room, f.a, 0, ActionKind::SetRules);
	edit.rules = f.TableCopy().rules;
	edit.rules.format = SetFormat::Unlimited;
	const auto changed = Wire(f.room, f.a, edit);
	CHECK(changed.accepted && EndEvents(changed) == 1);
	CHECK(f.TableCopy().rules.format == SetFormat::Unlimited && f.TableCopy().continuous.closed);
	CHECK(f.TableCopy().score[0] == 0 && f.TableCopy().score[1] == 0);
	CHECK(f.room.TerminalMembers(0, f.generation) == roster);
	for (auto member : roster) {
		auto ack = TableAction(f.room, member, 0, ActionKind::AcknowledgeTerminal);
		ack.matchGeneration = f.generation;
		CHECK(Wire(f.room, member, ack).accepted);
	}
	Recover(f.room);
	f.Start();
	for (int game = 0; game < 4; ++game) f.Game(MatchResult::P2Win);
	CHECK(f.TableCopy().score[1] == 4 && !f.TableCopy().continuous.closed);

	// Exercise the persisted ledger's capacity boundary without exposing a
	// test-only mutation on the authority. Every historical receipt retains
	// the same authenticated recipient identities as the live roster.
	Fixture full(SetFormat::Ft3);
	auto checkpoint = full.room.Checkpoint();
	checkpoint["snapshot"]["tables"][0]["match_generation"] = std::uint64_t(65);
	checkpoint["next_match"] = std::uint64_t(66);
	const auto recipients = checkpoint.at("active_match_recipients").at(0);
	for (std::uint64_t generation = 1; generation <= 64; ++generation)
		checkpoint["terminal_receipts"].push_back({{"table", 0}, {"generation", generation},
			{"result", static_cast<int>(MatchResult::Cancel)}, {"recipients", recipients}});
	CHECK(full.room.RestoreCheckpoint(checkpoint));
	const auto intact = full.room.Checkpoint();
	auto blocked = TableAction(full.room, full.a, 0, ActionKind::SetRules);
	blocked.rules = full.TableCopy().rules;
	blocked.rules.format = SetFormat::Unlimited;
	const auto refused = Wire(full.room, full.a, blocked);
	CHECK(!refused.accepted && refused.reason == RejectReason::TerminalLedgerFull && refused.events.empty());
	CHECK(full.room.Checkpoint() == intact);
}

static void RejectionsAndRecovery() {
	Fixture f(SetFormat::Ft3);
	CHECK(!Wire(f.room, f.watcher, f.Report(f.watcher, 1, MatchResult::P1Win)).accepted);
	CHECK(!Wire(f.room, f.a, f.Report(f.a, 0, MatchResult::P1Win)).accepted);
	CHECK(!Wire(f.room, f.a, f.Report(f.a, 2, MatchResult::P1Win)).accepted);
	CHECK(!Wire(f.room, f.a, f.Report(f.a, (std::numeric_limits<std::uint64_t>::max)(), MatchResult::P1Win)).accepted);
	CHECK(!Wire(f.room, f.a, f.Report(f.a, 1, MatchResult::Abort)).accepted);
	auto wrong = f.Report(f.a, 1, MatchResult::P1Win); ++wrong.matchGeneration;
	CHECK(!Wire(f.room, f.a, wrong).accepted);
	wrong = f.Report(f.a, 1, MatchResult::P1Win); ++wrong.roomEpoch;
	CHECK(!Wire(f.room, f.a, wrong).accepted);
	wrong = f.Report(f.a, 1, MatchResult::P1Win); wrong.kind = ActionKind::RecordResult;
	CHECK(!Wire(f.room, f.a, wrong).accepted);
	CHECK(!f.room.EndMatch(0, f.generation, MatchResult::P1Win).accepted);
	f.Game(MatchResult::P2Win);
	CHECK(!Wire(f.room, f.b, f.Report(f.b, 1, MatchResult::P1Win)).accepted);
	CHECK(Wire(f.room, f.a, f.Report(f.a, 2, MatchResult::P1Win)).accepted);
	const auto disputed = Wire(f.room, f.b, f.Report(f.b, 2, MatchResult::P2Win));
	CHECK(disputed.accepted && EndEvents(disputed) == 0 && f.TableCopy().phase == TablePhase::Paused);
	CHECK(f.TableCopy().score[0] == 0 && f.TableCopy().score[1] == 1 && f.TableCopy().continuous.games == 1);
	Recover(f.room);
	CHECK(!Wire(f.room, f.a, f.Finish(f.a)).accepted);
	CHECK(f.room.EndMatch(0, f.generation, MatchResult::Cancel).accepted);
	CHECK(f.TableCopy().score[1] == 1);
	Fixture timed;
	timed.room.AdvanceTime(1000);
	CHECK(Wire(timed.room, timed.a, timed.Report(timed.a, 1, MatchResult::Draw)).accepted);
	timed.room.AdvanceTime(11000); timed.room.PauseForRecovery(); Recover(timed.room);
	timed.room.ResumeRecovery(500);
	const auto rebased = timed.room.Checkpoint().at("time").get<std::uint64_t>();
	timed.room.AdvanceTime(rebased + 19999); CHECK(timed.TableCopy().phase == TablePhase::Playing);
	timed.room.AdvanceTime(rebased + 20000); CHECK(timed.TableCopy().phase == TablePhase::Paused);
	CHECK(timed.TableCopy().continuous.games == 0);
}

static void IsolationAndValidation() {
	Fixture f;
	const auto x = Join(f.room, 5), y = Join(f.room, 6);
	for (auto member : {x, y})
		CHECK(Wire(f.room, member, TableAction(f.room, member, 1, ActionKind::Queue)).accepted);
	for (auto member : {x, y}) {
		auto ready = TableAction(f.room, member, 1, ActionKind::Ready); ready.continuous = true;
		CHECK(Wire(f.room, member, ready).accepted);
	}
	CHECK(f.room.BeginMatch(1, x, y).accepted);
	const auto table1 = json(f.room.SnapshotView().tables[1]);
	f.Game(MatchResult::Draw);
	CHECK(json(f.room.SnapshotView().tables[1]) == table1);
	CHECK(Wire(f.room, f.watcher, TableAction(f.room, f.watcher, 0, ActionKind::Unwatch)).accepted);
	f.Game(MatchResult::P1Win);
	CHECK(json(f.room.SnapshotView().tables[1]) == table1);
	const auto good = f.room.Checkpoint();
	for (int kind = 0; kind < 7; ++kind) {
		auto bad = good;
		auto& t = bad["snapshot"]["tables"][0]; auto& s = t["continuous_series"];
		switch (kind) {
		case 0: s["games"] = std::uint64_t(0); break;
		case 1: s["wins"][0] = std::uint64_t(999); break;
		case 2: t["continuous_ready"][0] = false; break;
		case 3: s["finish_mask"] = 4; break;
		case 4: s["closed"] = "yes"; break;
		case 5: t["result_pending"] = true; break;
		case 6: bad["snapshot"]["practice"] = true; break;
		}
		CHECK(!f.room.RestoreCheckpoint(bad));
		CHECK(f.room.Checkpoint() == good);
	}
	// Old snapshots/actions remain byte-for-byte free of the opt-in fields.
	RoomAuthority legacy("normal"); const auto a = Join(legacy, 0, true), b = Join(legacy, 1);
	CHECK(json(legacy.SnapshotView()).dump().find("continuous") == std::string::npos);
	for (auto member : {a, b}) CHECK(Wire(legacy, member, TableAction(legacy, member, 0, ActionKind::Queue)).accepted);
	auto ready = TableAction(legacy, a, 0, ActionKind::Ready); ready.continuous = true;
	CHECK(Wire(legacy, a, ready).accepted);
	CHECK(Wire(legacy, b, TableAction(legacy, b, 0, ActionKind::Ready)).accepted);
    CHECK(legacy.BeginMatch(0, a, b).accepted);
    const auto mixedGeneration=legacy.SnapshotView().tables[0].matchGeneration;
    CHECK(!legacy.SnapshotView().tables[0].continuous.enabled);
    CHECK(legacy.EndMatch(0,mixedGeneration,MatchResult::Draw).accepted);
    for(auto member:{a,b}) { auto ack=TableAction(legacy,member,0,ActionKind::AcknowledgeTerminal); ack.matchGeneration=mixedGeneration; CHECK(Wire(legacy,member,ack).accepted); }
	CHECK(Wire(legacy, a, TableAction(legacy, a, 0, ActionKind::Unready)).accepted);
	CHECK(!legacy.SnapshotView().tables[0].continuousReady[0]);
	for (auto member : {a, b}) CHECK(Wire(legacy, member, TableAction(legacy, member, 0, ActionKind::Ready)).accepted);
	CHECK(legacy.BeginMatch(0, a, b).accepted);
	CHECK(!legacy.SnapshotView().tables[0].continuous.enabled);
	CHECK(legacy.EndMatch(0, legacy.SnapshotView().tables[0].matchGeneration, MatchResult::P1Win).accepted);
	CHECK(legacy.SnapshotView().tables[0].score[0] == 1);
}

static void ContinuousOwnDelay() {
    for(auto policy:{DelayPolicy::Shared,DelayPolicy::Own}) {
        Fixture f(SetFormat::Ft3,RotationMode::WinnerStays,policy);
        const auto roster=f.room.MatchRoster(0);
        for(int game=0;game<2;++game) {
            f.Game(MatchResult::P2Win);
            CHECK(f.room.SnapshotView().delayPolicy==policy);
            CHECK(SeatMatchDelay(f.room.SnapshotView(),f.TableCopy(),0)==(policy==DelayPolicy::Own?1:5));
            CHECK(SeatMatchDelay(f.room.SnapshotView(),f.TableCopy(),1)==5);
            CHECK(f.room.MatchRoster(0)==roster);
        }
        CHECK(Wire(f.room,f.a,f.Finish(f.a)).accepted);
        CHECK(EndEvents(Wire(f.room,f.b,f.Finish(f.b)))==1);
        CHECK(f.TableCopy().score[1]==2);
    }
}
static void DrawFinishesNormally() {
    for (bool priorWin : {false, true}) {
        Fixture f(SetFormat::Ft3);
        if (priorWin) f.Game(MatchResult::P2Win);
        f.Game(MatchResult::Draw);
        const auto before = f.TableCopy();
        CHECK(Wire(f.room, f.a, f.Finish(f.a)).accepted);
        const auto ended = Wire(f.room, f.b, f.Finish(f.b));
        CHECK(ended.accepted && EndEvents(ended) == 1);
        for (const auto& event : ended.events)
            if (event.kind == Event::Kind::MatchEnded) CHECK(event.result == MatchResult::Draw);
        const auto after = f.TableCopy();
        CHECK(after.phase == TablePhase::Waiting && after.continuous.closed);
        CHECK(after.score[0] == before.score[0] && after.score[1] == before.score[1]);
        CHECK(after.p1 == f.a && after.p2 == f.b);
        CHECK(after.score[0] == 0 && after.score[1] == (priorWin ? 1u : 0u));
        Recover(f.room);
    }
}
int main() {
    DrawFinishesNormally();
    ContinuousOwnDelay();
	RulesChangeBetweenSessions();
	LiveRulesCloseOneTable();
	LiveSetLengthChangeAndLedgerPressure();
	SetsAndRotation(); ExitAbortAndUnlimited(); RejectionsAndRecovery(); IsolationAndValidation();
	std::printf("RoomContinuous: %d failures\n", failures);
	return failures ? 1 : 0;
}
