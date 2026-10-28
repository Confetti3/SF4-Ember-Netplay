#include "room_authority_support.hxx"

static void TestCommittedDepartureCheckpoint() {
    RoomAuthority source("Departure", 16, 92);
    const auto host = Join(source, 0, true), guest = Join(source, 1);
    const auto leave = TableAction(source, guest, 0, ActionKind::Leave);
    EXPECT(leave.actionId != 0 && source.Apply(guest, leave).accepted);
    EXPECT(source.SnapshotFor(guest).localMember == 0);
    RoomAuthority replica("Replica");
    EXPECT(replica.RestoreCheckpoint(nlohmann::json::parse(source.Checkpoint().dump())));
    EXPECT(replica.SnapshotView().members.size() == 1 && replica.SnapshotView().host == host);
    EXPECT(source.Apply(host, TableAction(source, host, 0, ActionKind::Leave)).accepted);
    EXPECT(replica.RestoreCheckpoint(nlohmann::json::parse(source.Checkpoint().dump())));
    EXPECT(replica.SnapshotView().members.empty());
}

static void TestAuthorityRecovery() {
    RoomAuthority original("Recovery", 16, 91);
    auto host = Join(original, 0, true), p1 = Join(original, 1), p2 = Join(original, 2);
    auto kicked = Join(original, 3);
    Action ban = TableAction(original, host, 0, ActionKind::Kick); ban.target = kicked;
    EXPECT(original.Apply(host, ban).accepted);
    for (auto member : {p1, p2}) EXPECT(original.Apply(member, TableAction(original, member, 0, ActionKind::Queue)).accepted);
    for (auto member : {p1, p2}) EXPECT(original.Apply(member, TableAction(original, member, 0, ActionKind::Ready)).accepted);
    EXPECT(original.BeginMatch(0, p1, p2).accepted);
    original.AdvanceTime(1000);
    Action report = TableAction(original, p1, 0, ActionKind::RecordResult);
    report.matchGeneration = original.SnapshotView().tables[0].matchGeneration;
    report.result = MatchResult::P1Win;
    EXPECT(original.Apply(p1, report).accepted);
    const auto checkpoint = nlohmann::json::parse(original.Checkpoint().dump());
    RoomAuthority restored("Unchanged");
    EXPECT(restored.RestoreCheckpoint(checkpoint));
    EXPECT(restored.Checkpoint() == checkpoint);
	const auto duplicateReport = restored.Apply(p1, report);
	EXPECT(!duplicateReport.accepted && duplicateReport.reason == RejectReason::DuplicateResult);
    EXPECT(!restored.Join("New name", Peer(3)).accepted);
    EXPECT(!restored.TransferHost(p2, p1).accepted);
    EXPECT(restored.TransferHost(host, p1).accepted);
    EXPECT(restored.Leave(host).accepted && !restored.SnapshotView().closed);
    EXPECT(restored.SnapshotView().tables[0].matchGeneration == report.matchGeneration);
    auto second = TableAction(restored, p2, 0, ActionKind::RecordResult);
    second.matchGeneration = report.matchGeneration; second.result = report.result;
    EXPECT(restored.Apply(p2, second).accepted);
    EXPECT(restored.SnapshotView().tables[0].score[0] == 1);
    restored.Apply(p2, second);
    EXPECT(restored.SnapshotView().tables[0].score[0] == 1);
    EXPECT(Join(restored, 4) > kicked);
    const auto unchanged = restored.Checkpoint();
    for (const auto* key : {"version", "next_member", "next_match", "actions", "reporters"}) {
        auto invalid = checkpoint; invalid[key] = "invalid";
        EXPECT(!restored.RestoreCheckpoint(invalid)); EXPECT(restored.Checkpoint() == unchanged);
    }
    RoomAuthority timed("Deadline"); EXPECT(timed.RestoreCheckpoint(checkpoint));
    EXPECT(timed.AdvanceTime(30999).empty());
    EXPECT(!timed.AdvanceTime(31000).empty());
}

static void TestRelativeTimerRecovery() {
	RoomAuthority source("Relative timers", 8, 22);
	Join(source, 0, true);
	const auto p1 = Join(source, 1);
	const auto p2 = Join(source, 2);
	EXPECT(source.Apply(p1, TableAction(source, p1, 0, ActionKind::Queue)).accepted);
	EXPECT(source.Apply(p2, TableAction(source, p2, 0, ActionKind::Queue)).accepted);
	EXPECT(source.Apply(p1, TableAction(source, p1, 0, ActionKind::Ready)).accepted);
	EXPECT(source.Apply(p2, TableAction(source, p2, 0, ActionKind::Ready)).accepted);
	EXPECT(source.BeginMatch(0, p1, p2).accepted);
	Action finished = TableAction(source, p1, 0, ActionKind::MatchFinished);
	finished.matchGeneration = source.SnapshotView().tables[0].matchGeneration;
	EXPECT(source.Apply(p1, finished).accepted);
	EXPECT(source.AdvanceTime(9000).empty());
	auto chat=TableAction(source,p1,0,ActionKind::Chat);chat.text="Before recovery";
	EXPECT(source.Apply(p1,chat).accepted);
	EXPECT(source.AdvanceTime(10000).empty());
	source.PauseForRecovery();
	const auto checkpoint = source.Checkpoint();
	EXPECT(checkpoint.at("recovery_paused").get<bool>());
	EXPECT(checkpoint.at("result_age").at(0).get<std::uint64_t>() == 10000);
	EXPECT(checkpoint.at("result_since").at(0).get<std::uint64_t>() == 0);
	EXPECT(checkpoint.at("chat_times").at(0).at(1).get<std::uint64_t>() == 1000);
	RoomAuthority restored("Restored timers");
	EXPECT(restored.RestoreCheckpoint(checkpoint));
	EXPECT(restored.RecoveryPaused());
	// Forwarding an already paused checkpoint through another helper must
	// preserve durations even though its receiving monotonic clock is zero.
	EXPECT(restored.Checkpoint() == checkpoint);
	EXPECT(restored.RestoreCheckpoint(nlohmann::json::parse(restored.Checkpoint().dump())));
	EXPECT(restored.Checkpoint() == checkpoint);
	RoomAuthority healthyFollower("Healthy follower timers");
	EXPECT(healthyFollower.RestoreCheckpoint(checkpoint));
	healthyFollower.AdvancePausedTimers(20000);
	const auto agedCheckpoint = healthyFollower.Checkpoint();
	EXPECT(agedCheckpoint.at("result_age").at(0).get<std::uint64_t>() == 30000);
	EXPECT(agedCheckpoint.at("chat_times").at(0).at(1).get<std::uint64_t>() == 1000);
	EXPECT(!healthyFollower.AdvanceTime(500).empty());
	EXPECT(healthyFollower.SnapshotView().tables[0].phase == TablePhase::Paused);
	EXPECT(restored.AdvanceTime(500).empty()); // receiving clock is lower
	EXPECT(restored.AdvanceTime(29999).empty());
	EXPECT(!restored.AdvanceTime(30000).empty()); // age is retained across recovery
	EXPECT(restored.SnapshotView().tables[0].phase == TablePhase::Paused);
}

static void TestUnlimitedRematch() {
	RoomAuthority authority("Rotation", 8, 1);
	const MemberId host = Join(authority, 0, true);
	const MemberId p1 = Join(authority, 1);
	const MemberId p2 = Join(authority, 2);
	const MemberId p3 = Join(authority, 3);
	const MemberId p4 = Join(authority, 4);
	for (const auto member : {p1, p2, p3, p4}) {
		Action queue = TableAction(authority, member, 0, ActionKind::Queue);
		EXPECT(authority.Apply(member, queue).accepted);
	}
	Action rules = TableAction(authority, host, 0, ActionKind::SetRules);
	rules.rules.format = SetFormat::Unlimited; // no set length: the pair stays seated
	rules.rules.roundCount = 5; // a rules change that is not a new set
	EXPECT(authority.Apply(host, rules).accepted);
	const auto before = authority.SnapshotView().tables[0];
	for (int game = 1; game <= 8; ++game) {
		for (const auto member : {before.p1, before.p2}) {
			Action ready = TableAction(authority, member, 0, ActionKind::Ready);
			EXPECT(authority.Apply(member, ready).accepted);
		}
		EXPECT(authority.BeginMatch(0, before.p1, before.p2).accepted);
		const auto generation = authority.SnapshotView().tables[0].matchGeneration;
		EXPECT(authority.EndMatch(0, generation, MatchResult::P1Win).accepted);
		for (const auto member : authority.TerminalMembers(0, generation)) {
			Action acknowledgment = TableAction(authority, member, 0, ActionKind::AcknowledgeTerminal);
			acknowledgment.matchGeneration = generation;
			EXPECT(authority.Apply(member, acknowledgment).accepted);
		}
		const auto& after = authority.SnapshotView().tables[0];
		EXPECT(after.p1 == before.p1 && after.p2 == before.p2);
		EXPECT(after.phase == TablePhase::Waiting && !after.ready[0] && !after.ready[1]);
		EXPECT(after.score[0] == static_cast<std::uint32_t>(game) && after.score[1] == 0);
	}
	EXPECT(authority.SnapshotView().tables[0].queue.size() == 2);
}

// A seat that changes fighter takes back the other seat's Ready, as an
// Unready would, so the match cannot start against a matchup the other
// player never saw. Nothing else is taken back.
static void TestFighterChangeWithdrawsOpponentReady() {
	RoomAuthority authority("Matchup", 8, 1);
	Join(authority, 0, true);
	const MemberId p1 = Join(authority, 1);
	const MemberId p2 = Join(authority, 2);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	const auto seated = authority.SnapshotView().tables[0];
	bool withdrew = true;
	// A first fighter changes no matchup: the table revision stays.
	auto revision = authority.SnapshotView().tables[0].revision;
	EXPECT(authority.SetMemberFighter(seated.p1, 0, &withdrew) && !withdrew);
	EXPECT(authority.SetMemberFighter(seated.p2, 1, &withdrew) && !withdrew);
	EXPECT(authority.SnapshotView().tables[0].revision == revision);
	EXPECT(!authority.SetMemberFighter(seated.p2, 1, &withdrew) && !withdrew); // unchanged
	// p1 readies; p2 changes a fighter it showed: p1's Ready is taken back.
	Action ready = TableAction(authority, seated.p1, 0, ActionKind::Ready);
	ready.inputDelay = 2;
	EXPECT(authority.Apply(seated.p1, ready).accepted);
	revision = authority.SnapshotView().tables[0].revision;
	EXPECT(authority.SetMemberFighter(seated.p2, 3, &withdrew) && withdrew);
	{
		const auto& after = authority.SnapshotView().tables[0];
		EXPECT(!after.ready[0] && !after.ready[1] && after.phase == TablePhase::Waiting);
		EXPECT(after.revision > revision);
		EXPECT(!FindMember(authority.SnapshotView(), seated.p1)->delayLocked);
	}
	// A Ready pressed before the withdrawal carries the old table revision.
	Action late = ready; late.actionId += 7;
	EXPECT(authority.Apply(seated.p1, late).reason == RejectReason::StaleTable);
	// The race: p1 presses Ready against p2's fighter 3, then p2's change
	// to 4 arrives first with nobody ready. The Ready still cannot pass.
	Action raced = TableAction(authority, seated.p1, 0, ActionKind::Ready);
	EXPECT(authority.SetMemberFighter(seated.p2, 4, &withdrew) && !withdrew);
	EXPECT(authority.Apply(seated.p1, raced).reason == RejectReason::StaleTable);
	EXPECT(!authority.SnapshotView().tables[0].ready[0]);
	// A seat that is itself ready takes nothing back, the other direction
	// works the same way, and a member without a seat changes nothing.
	EXPECT(authority.Apply(seated.p2, TableAction(authority, seated.p2, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.SetMemberFighter(seated.p2, 5, &withdrew) && !withdrew);
	EXPECT(authority.SnapshotView().tables[0].ready[1]);
	EXPECT(authority.SetMemberFighter(seated.p1, 6, &withdrew) && withdrew);
	EXPECT(!authority.SnapshotView().tables[0].ready[1]);
	const MemberId bystander = Join(authority, 3);
	EXPECT(authority.SetMemberFighter(bystander, 7, &withdrew) && !withdrew);
	EXPECT(authority.SetMemberFighter(bystander, 8, &withdrew) && !withdrew);
}

// Idle time: counted from a member's last room action or join, reported in
// each sent snapshot, held at zero in a game, never in the room's own state.
static void TestIdleTimeIsStampedPerSnapshot() {
	RoomAuthority authority("Idle", 8, 1);
	const MemberId host = Join(authority, 0, true);
	const MemberId guest = Join(authority, 1);
	authority.AdvanceTime(500); // members joined at 0
	const auto idleOf = [&](MemberId member) {
		return FindMember(authority.SnapshotFor(host), member)->idleSeconds;
	};
	EXPECT(idleOf(guest) == 0);
	authority.AdvanceTime(500 + 125 * 1000);
	EXPECT(idleOf(guest) == 125 && idleOf(host) == 125);
	EXPECT(FindMember(authority.SnapshotCopy(), guest)->idleSeconds == 0);
	EXPECT(authority.Apply(guest, TableAction(authority, guest, 0, ActionKind::Queue)).accepted);
	EXPECT(idleOf(guest) == 0 && idleOf(host) == 125);
	// Sent as idle_s only when there is something to say.
	const auto sent = nlohmann::json(authority.SnapshotFor(host));
	bool sawHost = false, sawGuest = false;
	for (const auto& member : sent.at("members")) {
		if (member.at("id") == host) { sawHost = member.at("idle_s") == 125; }
		if (member.at("id") == guest) { sawGuest = !member.contains("idle_s"); }
	}
	EXPECT(sawHost && sawGuest);
	EXPECT(sent.get<Snapshot>().members.size() == 2);
	EXPECT(FindMember(sent.get<Snapshot>(), host)->idleSeconds == 125);
	// A member in a game stays active however long the game runs.
	const MemberId other = Join(authority, 2);
	EXPECT(authority.Apply(other, TableAction(authority, other, 0, ActionKind::Queue)).accepted);
	for (const auto member : {guest, other}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.BeginMatch(0, authority.SnapshotView().tables[0].p1, authority.SnapshotView().tables[0].p2).accepted);
	authority.AdvanceTime(500 + 600 * 1000);
	EXPECT(idleOf(guest) == 0 && idleOf(other) == 0 && idleOf(host) == 600);
	// Members who come and go leave no activity behind.
	for (int cycle = 0; cycle < 20; ++cycle) {
		const MemberId visitor = Join(authority, 10 + cycle);
		EXPECT(authority.Apply(visitor, TableAction(authority, visitor, 0, ActionKind::Leave)).accepted);
	}
	EXPECT(authority.TrackedActivity() == authority.SnapshotView().members.size());
	// A huge gap reads as the cap.
	authority.AdvanceTime(500 + 600 * 1000 + 30ull * 24 * 60 * 60 * 1000);
	EXPECT(idleOf(host) == MaximumIdleSeconds);
}

static void TestMatchFinishedAndSeatLifecycle() {
	RoomAuthority authority("Lifecycle", 8, 1);
	const MemberId host = Join(authority, 0, true);
	const MemberId p1 = Join(authority, 1);
	const MemberId p2 = Join(authority, 2);
	const MemberId p3 = Join(authority, 3);
	for (const auto member : {p1, p2, p3}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	const auto initial = authority.SnapshotView().tables[0];
	for (const auto member : {initial.p1, initial.p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.BeginMatch(0, initial.p1, initial.p2).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	EXPECT(!authority.HasDueTimerTransition(0)); // no pending result
	Action finished = TableAction(authority, p1, 0, ActionKind::MatchFinished);
	finished.matchGeneration = generation;
	EXPECT(authority.Apply(p1, finished).accepted);
	EXPECT(authority.SnapshotView().tables[0].phase == TablePhase::Playing);
	EXPECT(!authority.HasDueTimerTransition(0));
	EXPECT(authority.AdvanceTime(10000).empty());
	EXPECT(!authority.HasDueTimerTransition(9999)); // backward clock input
	EXPECT(!authority.HasDueTimerTransition(10000)); // equal clock input
	EXPECT(!authority.HasDueTimerTransition(29999)); // immediately before deadline
	EXPECT(authority.HasDueTimerTransition(30000)); // exact deadline
	const auto timeout = authority.AdvanceTime(30000);
	EXPECT(!timeout.empty() && authority.SnapshotView().tables[0].phase == TablePhase::Paused);
	EXPECT(authority.SnapshotView().tables[0].resultPending);
	EXPECT(!authority.HasDueTimerTransition(60000)); // paused but still pending
	const auto unresolved = authority.SnapshotView().tables[0];
	Action pausedRules = TableAction(authority, host, 0, ActionKind::SetRules);
	pausedRules.rules.roundTime = 300;
	EXPECT(!authority.Apply(host, pausedRules).accepted);
	const auto afterPausedRules = authority.SnapshotView().tables[0];
	EXPECT(afterPausedRules.phase == TablePhase::Paused && afterPausedRules.matchGeneration == unresolved.matchGeneration);
	EXPECT(afterPausedRules.resultPending == unresolved.resultPending && afterPausedRules.score[0] == unresolved.score[0] &&
		afterPausedRules.score[1] == unresolved.score[1]);
	Action staleCancel = TableAction(authority, host, 0, ActionKind::CancelResult);
	staleCancel.matchGeneration = generation + 1;
	EXPECT(!authority.Apply(host, staleCancel).accepted);
	Action cancel = staleCancel;
	cancel.matchGeneration = generation;
	EXPECT(authority.Apply(host, cancel).accepted);
	EXPECT(authority.SnapshotView().tables[0].phase == TablePhase::Waiting);

	// A waiting fighter can leave its seat and the next FIFO member fills it;
	// readiness and scores must not leak across that transition.
	const auto waiting = authority.SnapshotView().tables[0];
	Action unseat = TableAction(authority, waiting.p1, 0, ActionKind::Unqueue);
	EXPECT(authority.Apply(waiting.p1, unseat).accepted);
	const auto after = authority.SnapshotView().tables[0];
	EXPECT(after.p1 != waiting.p1 && after.p1 != 0 && !after.ready[0] && !after.ready[1]);
}

static void TestSnapshotBound() {
	RoomAuthority authority("Bounded room", 16, 1);
	const MemberId host = Join(authority, 0, true);
	for (int i = 1; i < 16; ++i) Join(authority, i);
	for (int i = 0; i < 100; ++i) {
		Action chat;
		chat.kind = ActionKind::Chat;
		chat.roomEpoch = authority.SnapshotView().roomEpoch;
		chat.text.assign(256, 'x');
		chat.actionId = static_cast<ActionId>(i + 1);
		EXPECT(authority.Apply(host, chat).accepted);
		authority.AdvanceTime(static_cast<std::uint64_t>(i + 1) * 1000);
	}
	EXPECT(nlohmann::json(authority.SnapshotView()).dump().size() < 65536);
}

static void TestDepartureRecoveryAndSnapshotValidation() {
	RoomAuthority authority("Departure", 8, 1);
	const MemberId host = Join(authority, 0, true);
	const MemberId p1 = Join(authority, 1);
	const MemberId p2 = Join(authority, 2);
	const MemberId p3 = Join(authority, 3);
	const MemberId p4 = Join(authority, 4);
	for (const auto member : {p1, p2, p3, p4}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	const auto seated = authority.SnapshotView().tables[0];
	for (const auto member : {seated.p1, seated.p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.BeginMatch(0, seated.p1, seated.p2).accepted);
	EXPECT(authority.SnapshotView().tables[0].spectators.size() == 2);

	// A watcher/queued member can disappear without touching the frozen fighter
	// pair. The next fighter departure aborts that generation and seats FIFO.
	EXPECT(authority.Leave(p3).accepted);
	EXPECT(authority.SnapshotView().tables[0].p1 == p1 && authority.SnapshotView().tables[0].p2 == p2);
	EXPECT(authority.SnapshotView().tables[0].phase == TablePhase::Playing);
	const auto leaving = authority.Leave(p2);
	EXPECT(leaving.accepted);
	EXPECT(std::any_of(leaving.events.begin(), leaving.events.end(), [](const Event& event) { return event.kind == Event::Kind::MatchEnded; }));
	const auto recovered = authority.SnapshotView().tables[0];
	EXPECT(recovered.p1 == p1 && recovered.p2 == p4 && recovered.phase == TablePhase::Waiting);
	EXPECT(recovered.score[0] == 0 && recovered.score[1] == 0 && !recovered.ready[0] && !recovered.ready[1]);
	RoomAuthority departedRestore("Active departure restore");
	EXPECT(departedRestore.RestoreCheckpoint(nlohmann::json::parse(authority.Checkpoint().dump())));
	EXPECT(departedRestore.PendingTerminalEvents(p1).size() == 1);

	// Pre-start spectators leaving a waiting table do not pause or strand it.
	const MemberId p5 = Join(authority, 5);
	Action watch = TableAction(authority, p5, 0, ActionKind::Watch);
	EXPECT(authority.Apply(p5, watch).accepted);
	EXPECT(authority.Leave(p5).accepted);
	EXPECT(authority.SnapshotView().tables[0].phase == TablePhase::Waiting);

	// Decoder bounds reject forged IDs/table slots rather than narrowing them
	// into a valid-looking snapshot.
	nlohmann::json encoded = authority.SnapshotView();
	auto badMember = encoded;
	badMember["members"][0]["name"] = std::string(33, 'x');
	bool threw = false;
	try { (void)badMember.get<Snapshot>(); } catch (const std::exception&) { threw = true; }
	EXPECT(threw);
	auto badTable = encoded;
	badTable["tables"][0]["id"] = 4;
	threw = false;
	try { (void)badTable.get<Snapshot>(); } catch (const std::exception&) { threw = true; }
	EXPECT(threw);
	auto badRoster = encoded;
	badRoster["tables"][0]["queue"] = nlohmann::json::array({999999999999ULL});
	threw = false;
	try { (void)badRoster.get<Snapshot>(); } catch (const std::exception&) { threw = true; }
	EXPECT(threw);

	(void)host;
}

static void TestProfileMain() {
    RoomAuthority authority("Profiles",16,70);
    EXPECT(!authority.Join("Invalid",Peer(0),true,{44}).accepted);
    EXPECT(!authority.Join("Invalid",Peer(0),true,{-2}).accepted);
    EXPECT(authority.Join("Main",Peer(0),true,{11,sf4e::NetworkLink::Wireless,sf4e::NatClass::Strict}).accepted);
    const auto host=authority.SnapshotView().host;
    EXPECT(authority.SetMemberFighter(host,2));
    const auto encoded=nlohmann::json(authority.SnapshotView()).dump();
    const auto decoded=nlohmann::json::parse(encoded).get<Snapshot>();
    EXPECT(decoded.members[0].mainFighter==11&&decoded.members[0].fighter==2);
    RoomAuthority restored("Restore");EXPECT(restored.RestoreCheckpoint(authority.Checkpoint()));
    EXPECT(restored.SnapshotView().members[0].mainFighter==11);
    EXPECT(decoded.members[0].link==sf4e::NetworkLink::Wireless&&restored.SnapshotView().members[0].link==sf4e::NetworkLink::Wireless);
    // An older peer omits the link, and a newer one may name a kind this build
    // does not know: both read as Unknown rather than refusing the member.
    auto member=nlohmann::json(decoded.members[0]);member.erase("main_fighter");member.erase("link");
    EXPECT(member.get<Member>().mainFighter==-1&&member.get<Member>().link==sf4e::NetworkLink::Unknown);
    member["link"]=7;EXPECT(member.get<Member>().link==sf4e::NetworkLink::Unknown);
    // The same holds for how the member's network treats a direct connection.
    EXPECT(decoded.members[0].nat==sf4e::NatClass::Strict&&restored.SnapshotView().members[0].nat==sf4e::NatClass::Strict);
    member.erase("nat");EXPECT(member.get<Member>().nat==sf4e::NatClass::Unknown);
    member["nat"]=9;EXPECT(member.get<Member>().nat==sf4e::NatClass::Unknown);
    member["nat"]=static_cast<int>(sf4e::NatClass::NoUdp);EXPECT(member.get<Member>().nat==sf4e::NatClass::NoUdp);
    EXPECT(authority.Join("Future",Peer(1),false,{-1,static_cast<sf4e::NetworkLink>(7)}).accepted);
    EXPECT(authority.SnapshotView().members.back().link==sf4e::NetworkLink::Unknown);
    EXPECT(authority.Join("FutureNat",Peer(2),false,{-1,sf4e::NetworkLink::Unknown,static_cast<sf4e::NatClass>(9)}).accepted);
    EXPECT(authority.SnapshotView().members.back().nat==sf4e::NatClass::Unknown);
    for(const nlohmann::json invalid:{nlohmann::json(-2),nlohmann::json(44),nlohmann::json(1.5),nlohmann::json(UINT64_MAX)}){
        member["main_fighter"]=invalid;bool rejected=false;
        try{member.get<Member>();}catch(...){rejected=true;}EXPECT(rejected);
    }
}

static void TestTerminalLifecycleGate() {
	RoomAuthority authority("Terminal lifecycle", 8, 310);
	Join(authority, 0, true);
	const auto p1 = Join(authority, 1);
	const auto p2 = Join(authority, 2);
	const auto spectator = Join(authority, 3);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	Action watch = TableAction(authority, spectator, 0, ActionKind::Watch);
	EXPECT(authority.Apply(spectator, watch).accepted);
	const auto seated = authority.SnapshotView().tables[0];
	for (const auto member : {seated.p1, seated.p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.BeginMatch(0, p1, p2).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	Action first = TableAction(authority, p1, 0, ActionKind::RecordResult);
	first.matchGeneration = generation; first.result = MatchResult::P1Win;
	EXPECT(authority.Apply(p1, first).accepted);
	Action second = TableAction(authority, p2, 0, ActionKind::RecordResult);
	second.matchGeneration = generation; second.result = MatchResult::P1Win;
	EXPECT(authority.Apply(p2, second).accepted);

	// A member cannot stage a new native generation while any of its frozen
	// spectator/fighter receipts still drain; this also prevents local outcome
	// state from being overwritten by a different table's terminal event.
	Action ready = TableAction(authority, p1, 0, ActionKind::Ready);
	EXPECT(!authority.Apply(p1, ready).accepted);
	ready = TableAction(authority, p2, 0, ActionKind::Ready);
	EXPECT(!authority.Apply(p2, ready).accepted);
	EXPECT(authority.PendingTerminalEvents(spectator).size() == 1);
	Action crossTableQueue = TableAction(authority, p1, 1, ActionKind::Queue);
	const auto crossTableResult = authority.Apply(p1, crossTableQueue);
	EXPECT(!crossTableResult.accepted && crossTableResult.reason == RejectReason::TerminalLedgerFull);

	// The frozen incarnation is the authorization boundary for the ACK. A
	// rebind of the same numeric member cannot release an older native slot.
	authority.SetMemberIncarnation(spectator, 2);
	Action spectatorAck = TableAction(authority, spectator, 0, ActionKind::AcknowledgeTerminal);
	spectatorAck.matchGeneration = generation;
	EXPECT(!authority.Apply(spectator, spectatorAck).accepted);
	authority.SetMemberIncarnation(spectator, 1);
	EXPECT(authority.Apply(spectator, spectatorAck).accepted);
	for (const auto member : {p1, p2}) {
		Action acknowledgment = TableAction(authority, member, 0, ActionKind::AcknowledgeTerminal);
		acknowledgment.matchGeneration = generation;
		EXPECT(authority.Apply(member, acknowledgment).accepted);
	}
	ready = TableAction(authority, p1, 0, ActionKind::Ready);
	EXPECT(authority.Apply(p1, ready).accepted);
	ready = TableAction(authority, p2, 0, ActionKind::Ready);
	EXPECT(authority.Apply(p2, ready).accepted);
	EXPECT(authority.BeginMatch(0, p1, p2).accepted);
}

// Diverged peers report different results, which pauses the table at once.
// Neither fighter can leave the seat or ready while Paused, so either fighter
// must be able to abandon it without the host. The abandon awards nothing and
// its receipt drains through the same terminal ACKs as any other ending.
static void TestFighterAbandonsDisputedResult() {
	RoomAuthority authority("Disputed result", 8, 311);
	const auto host = Join(authority, 0, true);
	const auto p1 = Join(authority, 1);
	const auto p2 = Join(authority, 2);
	const auto spectator = Join(authority, 3);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	EXPECT(authority.Apply(spectator, TableAction(authority, spectator, 0, ActionKind::Watch)).accepted);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.BeginMatch(0, p1, p2).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	Action first = TableAction(authority, p1, 0, ActionKind::RecordResult);
	first.matchGeneration = generation; first.result = MatchResult::P1Win;
	EXPECT(authority.Apply(p1, first).accepted);
	Action second = TableAction(authority, p2, 0, ActionKind::RecordResult);
	second.matchGeneration = generation; second.result = MatchResult::P2Win;
	const auto disputed = authority.Apply(p2, second);
	EXPECT(disputed.accepted && authority.SnapshotView().tables[0].phase == TablePhase::Paused);

	EXPECT(!authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Unqueue)).accepted);
	EXPECT(!authority.Apply(p2, TableAction(authority, p2, 0, ActionKind::Ready)).accepted);
	Action spectatorAbort = TableAction(authority, spectator, 0, ActionKind::AbortMatch);
	spectatorAbort.matchGeneration = generation;
	EXPECT(!authority.Apply(spectator, spectatorAbort).accepted);
	Action staleAbort = TableAction(authority, p2, 0, ActionKind::AbortMatch);
	staleAbort.matchGeneration = generation + 1;
	EXPECT(!authority.Apply(p2, staleAbort).accepted);

	Action abandon = TableAction(authority, p2, 0, ActionKind::AbortMatch);
	abandon.matchGeneration = generation;
	EXPECT(authority.Apply(p2, abandon).accepted);
	const auto released = authority.SnapshotView().tables[0];
	EXPECT(released.phase == TablePhase::Waiting && !released.resultPending);
	EXPECT(released.p1 == p1 && released.p2 == p2);
	EXPECT(released.score[0] == 0 && released.score[1] == 0);
	EXPECT(released.matchGeneration == generation);
	Action replayedAbandon = TableAction(authority, p1, 0, ActionKind::AbortMatch);
	replayedAbandon.matchGeneration = generation;
	EXPECT(authority.Apply(p1, replayedAbandon).accepted); // idempotent replay of the same terminal
	EXPECT(authority.SnapshotView().tables[0].score[0] == 0 && authority.SnapshotView().tables[0].score[1] == 0);

	// The table stays fenced until every frozen recipient acknowledges. (A new
	// action id: the replay above did not advance the revision.)
	Action earlyReady = TableAction(authority, p1, 0, ActionKind::Ready);
	++earlyReady.actionId;
	const auto earlyReadyResult = authority.Apply(p1, earlyReady);
	EXPECT(!earlyReadyResult.accepted && earlyReadyResult.reason == RejectReason::TerminalLedgerFull);
	for (const auto member : {p1, p2, spectator}) {
		Action acknowledgment = TableAction(authority, member, 0, ActionKind::AcknowledgeTerminal);
		acknowledgment.matchGeneration = generation;
		EXPECT(authority.Apply(member, acknowledgment).accepted);
	}
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	EXPECT(authority.BeginMatch(0, p1, p2).accepted);
	EXPECT(authority.SnapshotView().host == host);

	// A fighter can also leave the seat once the abandoned game has drained.
	const auto nextGeneration = authority.SnapshotView().tables[0].matchGeneration;
	Action nextAbort = TableAction(authority, p1, 0, ActionKind::AbortMatch);
	nextAbort.matchGeneration = nextGeneration;
	EXPECT(authority.Apply(p1, nextAbort).accepted);
	for (const auto member : {p1, p2, spectator}) {
		Action acknowledgment = TableAction(authority, member, 0, ActionKind::AcknowledgeTerminal);
		acknowledgment.matchGeneration = nextGeneration;
		EXPECT(authority.Apply(member, acknowledgment).accepted);
	}
	EXPECT(authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Unqueue)).accepted);
	EXPECT(authority.SnapshotView().tables[0].p1 != p1);
}

static void TestTerminalReceiptSurvivesCompaction() {
	// Explicit native retirement may remove a fighter from the mutable room
	// roster. The frozen endpoint/incarnation is still enough to restore the
	// durable terminal receipt from a later checkpoint.
	RoomAuthority departed("Retired fighter", 8, 204);
	Join(departed, 0, true);
	const auto departedP1 = Join(departed, 1), departedP2 = Join(departed, 2);
	for (const auto member : {departedP1, departedP2}) EXPECT(departed.Apply(member, TableAction(departed, member, 0, ActionKind::Queue)).accepted);
	const auto departedTable = departed.SnapshotView().tables[0];
	for (const auto member : {departedTable.p1, departedTable.p2}) EXPECT(departed.Apply(member, TableAction(departed, member, 0, ActionKind::Ready)).accepted);
	EXPECT(departed.BeginMatch(0, departedP1, departedP2).accepted);
	const auto departedGeneration = departed.SnapshotView().tables[0].matchGeneration;
	Action departedFirst = TableAction(departed, departedP1, 0, ActionKind::RecordResult);
	departedFirst.matchGeneration = departedGeneration; departedFirst.result = MatchResult::P1Win;
	EXPECT(departed.Apply(departedP1, departedFirst).accepted);
	Action departedSecond = TableAction(departed, departedP2, 0, ActionKind::RecordResult);
	departedSecond.matchGeneration = departedGeneration; departedSecond.result = MatchResult::P1Win;
	EXPECT(departed.Apply(departedP2, departedSecond).accepted);
	EXPECT(departed.Leave(departedP2).accepted);
	RoomAuthority departedRestored("Retired fighter restore");
	EXPECT(departedRestored.RestoreCheckpoint(nlohmann::json::parse(departed.Checkpoint().dump())));
	EXPECT(departedRestored.PendingTerminalEvents(departedP1).size() == 1);

	RoomAuthority source("Terminal receipt", 16, 203);
	Join(source, 0, true);
	const auto p1 = Join(source, 1);
	const auto p2 = Join(source, 2);
	for (const auto member : {p1, p2}) EXPECT(source.Apply(member, TableAction(source, member, 0, ActionKind::Queue)).accepted);
	const auto seated = source.SnapshotView().tables[0];
	for (const auto member : {seated.p1, seated.p2}) EXPECT(source.Apply(member, TableAction(source, member, 0, ActionKind::Ready)).accepted);
	EXPECT(source.BeginMatch(0, p1, p2).accepted);
	const auto generation = source.SnapshotView().tables[0].matchGeneration;
	Action first = TableAction(source, p1, 0, ActionKind::RecordResult);
	first.matchGeneration = generation;
	first.result = MatchResult::P1Win;
	EXPECT(source.Apply(p1, first).accepted);
	Action second = TableAction(source, p2, 0, ActionKind::RecordResult);
	second.matchGeneration = generation;
	second.result = MatchResult::P1Win;
	EXPECT(source.Apply(p2, second).accepted);
	EXPECT(source.SnapshotView().tables[0].phase == TablePhase::Waiting);
	EXPECT(source.Checkpoint().at("terminal_receipts").size() == 1);

	// Drive a different table through more than the generic journal's old
	// retention span. The completed fighter pair and outcome must stay in the
	// replicated checkpoint ledger while unrelated room actions continue.
	for (int i = 0; i < 320; ++i) {
		Action unrelated = TableAction(source, source.SnapshotView().host, 1, ActionKind::SetRules);
		unrelated.rules = source.SnapshotView().tables[1].rules;
		unrelated.rules.editionSelect = (i & 1) != 0;
		EXPECT(source.Apply(source.SnapshotView().host, unrelated).accepted);
	}
	const auto checkpoint = nlohmann::json::parse(source.Checkpoint().dump());
	RoomAuthority restored("Restored terminal receipt");
	EXPECT(restored.RestoreCheckpoint(checkpoint));
	EXPECT(restored.SnapshotView().tables[0].score[0] == 1);

	Action late = TableAction(restored, p2, 0, ActionKind::RecordResult);
	late.actionId = 900001;
	late.matchGeneration = generation;
	late.result = MatchResult::P1Win;
	const auto replay = restored.Apply(p2, late);
	EXPECT(replay.accepted && replay.terminalReplay);
	EXPECT(std::any_of(replay.events.begin(), replay.events.end(), [](const Event& event) {
		return event.kind == Event::Kind::MatchEnded && event.terminalReplay;
	}));
	EXPECT(restored.SnapshotView().tables[0].score[0] == 1);
	const auto replayCheckpoint = restored.Checkpoint();
	EXPECT(std::none_of(replayCheckpoint.at("terminal_receipts").at(0).at("recipients").begin(),
		replayCheckpoint.at("terminal_receipts").at(0).at("recipients").end(),
		[](const nlohmann::json& recipient) { return recipient.at("acknowledged").get<bool>(); }));

	late = TableAction(restored, p1, 0, ActionKind::RecordResult);
	late.actionId = 900002;
	late.matchGeneration = generation;
	late.result = MatchResult::P1Win;
	const auto duplicateReplay = restored.Apply(p1, late);
	EXPECT(duplicateReplay.accepted && duplicateReplay.terminalReplay);
	EXPECT(restored.SnapshotView().tables[0].score[0] == 1);
	for (const auto member : source.TerminalMembers(0, generation)) {
		Action acknowledgment = TableAction(source, member, 0, ActionKind::AcknowledgeTerminal);
		acknowledgment.matchGeneration = generation;
		EXPECT(source.Apply(member, acknowledgment).accepted);
	}

	// Cancel/abort teardown has the same durable replay path even when the
	// moderator is not one of the two frozen fighters.
	for (const auto member : {p1, p2}) EXPECT(source.Apply(member, TableAction(source, member, 0, ActionKind::Ready)).accepted);
	EXPECT(source.BeginMatch(0, p1, p2).accepted);
	const auto cancelledGeneration = source.SnapshotView().tables[0].matchGeneration;
	Action cancel = TableAction(source, source.SnapshotView().host, 0, ActionKind::CancelResult);
	cancel.matchGeneration = cancelledGeneration;
	EXPECT(source.Apply(source.SnapshotView().host, cancel).accepted);
	RoomAuthority cancelRestored("Restored cancel receipt");
	EXPECT(cancelRestored.RestoreCheckpoint(nlohmann::json::parse(source.Checkpoint().dump())));
	Action lateCancel = TableAction(cancelRestored, cancelRestored.SnapshotView().host, 0, ActionKind::CancelResult);
	lateCancel.actionId = 900003;
	lateCancel.matchGeneration = cancelledGeneration;
	const auto cancelReplay = cancelRestored.Apply(cancelRestored.SnapshotView().host, lateCancel);
	EXPECT(cancelReplay.accepted && cancelReplay.terminalReplay);

	// The first receipt was acknowledged, then pruned when this later
	// generation was stored. A lost action reply must still be recoverable only
	// for the same member incarnation and exact old table/generation key.
	Action lateOldAck = TableAction(source, p1, 0, ActionKind::AcknowledgeTerminal);
	lateOldAck.actionId = 900004; lateOldAck.matchGeneration = generation;
	EXPECT(source.Apply(p1, lateOldAck).accepted);
	Action unknownAck = lateOldAck;
	unknownAck.actionId = 900005; unknownAck.matchGeneration = generation + 100000;
	EXPECT(!source.Apply(p1, unknownAck).accepted);
}

// A spectator still retiring its session keeps its own receipt row but no
// longer holds the table: the fighters rematch at once, the spectator sits
// the new generation out, and it rejoins the one after its acknowledgment.
// A snapshot must stay readable by clients after someone who chatted leaves.
static void TestChatAfterSenderLeaves() {
	RoomAuthority authority("Chat departure", 8, 313);
	const auto host = Join(authority, 0, true);
	const auto guest = Join(authority, 1);
	Action chat = TableAction(authority, guest, 0, ActionKind::Chat);
	chat.text = "hello";
	EXPECT(authority.Apply(guest, chat).accepted);
	EXPECT(authority.Apply(guest, TableAction(authority, guest, 0, ActionKind::Leave)).accepted);
	Snapshot parsed;
	bool readable = true;
	try { nlohmann::json(authority.SnapshotFor(host)).get_to(parsed); } catch (const std::exception&) { readable = false; }
	EXPECT(readable && parsed.chat.empty());
	// A successor restores the room.
	RoomAuthority replica("Replica");
	EXPECT(replica.RestoreCheckpoint(nlohmann::json::parse(authority.Checkpoint().dump())));
	// State from an older owner that kept the departed sender's line still reads.
	auto legacy = nlohmann::json(authority.SnapshotFor(host));
	nlohmann::json orphan = nlohmann::json::object();
	orphan["sequence"] = std::uint64_t(1); orphan["sender"] = guest; orphan["text"] = "hello";
	legacy["chat"] = nlohmann::json::array();
	legacy["chat"].push_back(orphan);
	readable = true;
	try { legacy.get_to(parsed); } catch (const std::exception& error) { std::printf("legacy chat: %s\n", error.what()); readable = false; }
	EXPECT(readable && parsed.chat.empty());
}

// Typing is shown to the others for TypingMs, ended by a stop or a message, at
// most a start a second, moves no revision, and is never checkpointed.
static void TestTyping() {
	RoomAuthority authority("Typing", 8, 314);
	authority.AdvanceTime(1000);
	const auto host = Join(authority, 0, true), guest = Join(authority, 1);
	// Typing moves no revision, so TableAction's ids would repeat.
	std::uint64_t next = 100000;
	const auto say = [&](MemberId member, bool typing) {
		auto action = TableAction(authority, member, 0, ActionKind::Typing);
		action.typing = typing;
		action.actionId = ++next;
		return authority.Apply(member, action);
	};
	const auto shown = [&](MemberId member) { return FindMember(authority.SnapshotFor(host), member)->typingMs; };
	const auto revision = authority.SnapshotView().revision;
	CHECK(!shown(guest));
	CHECK(say(guest, true).accepted && shown(guest) == TypingMs && authority.SnapshotView().revision == revision);
	// Stamped per recipient: the room's own state and checkpoints keep none.
	CHECK(!FindMember(authority.SnapshotCopy(), guest)->typingMs);
	CHECK(authority.Checkpoint().dump().find("typing") == std::string::npos);
	authority.AdvanceTime(1500);
	CHECK(shown(guest) == TypingMs - 500 && !shown(host));
	// A second start within the second is refused; after it, it shows for TypingMs again.
	authority.AdvanceTime(1900);
	CHECK(say(guest, true).reason == RejectReason::InvalidChat && shown(guest) == TypingMs - 900);
	authority.AdvanceTime(2000);
	CHECK(say(guest, true).accepted && shown(guest) == TypingMs);
	// It runs out by itself.
	authority.AdvanceTime(2000 + TypingMs);
	CHECK(!shown(guest));
	// A stop ends it, and a stop with nothing to stop is refused, so neither can be sent over and over.
	CHECK(say(guest, false).reason == RejectReason::InvalidChat);
	CHECK(say(guest, true).accepted && say(guest, false).accepted && !shown(guest));
	CHECK(!say(guest, false).accepted);
	// A message from the member ends it; another member's does not.
	authority.AdvanceTime(9000);
	CHECK(say(guest, true).accepted && say(host, true).accepted);
	auto chat = TableAction(authority, guest, 0, ActionKind::Chat);
	chat.text = "hello";
	chat.actionId = ++next;
	CHECK(authority.Apply(guest, chat).accepted && !shown(guest) && shown(host));
	// On the wire: as "locked", written only when set, and capped rather than refused when a newer peer says more.
	auto action = TableAction(authority, guest, 0, ActionKind::Typing);
	action.typing = true;
	const nlohmann::json wire = action;
	CHECK(wire.at("locked").get<bool>());
	const Action back = wire.get<Action>();
	CHECK(back.kind == ActionKind::Typing && back.typing && !back.locked);
	nlohmann::json sent = authority.SnapshotFor(guest);
	int written = 0;
	for (auto& member : sent.at("members")) {
		written += member.contains("typing_ms");
		if (member.at("id") == host) member["typing_ms"] = TypingMs * 10;
	}
	CHECK(written == 1 && FindMember(sent.get<Snapshot>(), host)->typingMs == TypingMs);
	// An older peer's snapshot reads as nobody typing, and a kind past ours is still refused.
	for (auto& member : sent.at("members")) member.erase("typing_ms");
	CHECK(!FindMember(sent.get<Snapshot>(), host)->typingMs);
	nlohmann::json unknown = wire;
	unknown["kind"] = static_cast<int>(LastActionKind) + 1;
	bool refused = false;
	try { (void)unknown.get<Action>(); } catch (const std::exception&) { refused = true; }
	CHECK(refused);
	// Paused for recovery, nobody shows as typing, and a member who leaves takes it with them.
	authority.PauseForRecovery();
	CHECK(!shown(host));
	authority.ResumeRecovery(10000);
	CHECK(say(guest, true).accepted && authority.Leave(guest).accepted);
	CHECK(!say(guest, true).accepted);
}

static void TestSpectatorDoesNotHoldTable() {
	RoomAuthority authority("Spectator retirement", 8, 312);
	Join(authority, 0, true);
	const auto p1 = Join(authority, 1);
	const auto p2 = Join(authority, 2);
	const auto spectator = Join(authority, 3);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	EXPECT(authority.Apply(spectator, TableAction(authority, spectator, 0, ActionKind::Watch)).accepted);
	const auto begin = [&]() {
		for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
		EXPECT(authority.BeginMatch(0, p1, p2).accepted);
		return authority.SnapshotView().tables[0].matchGeneration;
	};
	const auto acknowledge = [&](MemberId member, std::uint64_t generation) {
		Action acknowledgment = TableAction(authority, member, 0, ActionKind::AcknowledgeTerminal);
		acknowledgment.matchGeneration = generation;
		return authority.Apply(member, acknowledgment).accepted;
	};

	const auto first = begin();
	EXPECT((authority.MatchRoster(0) == std::vector<MemberId>{p1, p2, spectator}));
	EXPECT(authority.EndMatch(0, first, MatchResult::P1Win).accepted);
	EXPECT(authority.MatchRoster(0).empty());
	EXPECT(authority.SnapshotFor(p1).terminalPending[0]);
	EXPECT(acknowledge(p1, first) && acknowledge(p2, first));
	// Only the spectator is still retiring generation one.
	EXPECT(!authority.SnapshotFor(p1).terminalPending[0]);
	EXPECT(authority.SnapshotFor(spectator).localTerminalPending && !authority.SnapshotFor(p1).localTerminalPending);

	const auto second = begin();
	EXPECT(second > first);
	EXPECT((authority.MatchRoster(0) == std::vector<MemberId>{p1, p2}));
	// The spectator stays in the room view, and its own gates still hold.
	const auto& table = authority.SnapshotView().tables[0];
	EXPECT(std::find(table.spectators.begin(), table.spectators.end(), spectator) != table.spectators.end());
	EXPECT(authority.PendingTerminalEvents(spectator).size() == 1);
	const auto crossTable = authority.Apply(spectator, TableAction(authority, spectator, 1, ActionKind::Watch));
	EXPECT(!crossTable.accepted && crossTable.reason == RejectReason::TerminalLedgerFull);

	// A successor restores the same frozen roster and receipt state.
	RoomAuthority replica("Replica");
	EXPECT(replica.RestoreCheckpoint(nlohmann::json::parse(authority.Checkpoint().dump())));
	EXPECT(replica.MatchRoster(0) == authority.MatchRoster(0));
	EXPECT(!replica.SnapshotFor(p1).terminalPending[0] && replica.SnapshotFor(spectator).localTerminalPending);

	EXPECT(authority.EndMatch(0, second, MatchResult::P2Win).accepted);
	// A late duplicate completion never awards a second score.
	EXPECT(!authority.EndMatch(0, second, MatchResult::P2Win).accepted);
	EXPECT(authority.SnapshotView().tables[0].score[0] == 1 && authority.SnapshotView().tables[0].score[1] == 1);
	// Generation two's receipt never included the spectator.
	EXPECT((authority.TerminalMembers(0, second) == std::vector<MemberId>{p1, p2}));
	EXPECT(!acknowledge(spectator, second));
	EXPECT(acknowledge(p1, second) && acknowledge(p2, second));

	// Once it acknowledges generation one, the spectator joins the next roster.
	EXPECT(acknowledge(spectator, first));
	EXPECT(!authority.SnapshotFor(spectator).localTerminalPending);
	const auto third = begin();
	EXPECT(third > second);
	EXPECT((authority.MatchRoster(0) == std::vector<MemberId>{p1, p2, spectator}));

	// Leaving is not admission. The member the room is waiting on is exactly
	// the one that needs to leave, so Unwatch stays open while its receipt is
	// outstanding, and Unqueue stays open for anyone else at the table.
	const auto queuer = Join(authority, 4);
	EXPECT(authority.Apply(queuer, TableAction(authority, queuer, 0, ActionKind::Queue)).accepted);
	EXPECT(authority.EndMatch(0, third, MatchResult::P1Win).accepted);
	EXPECT(authority.SnapshotFor(spectator).localTerminalPending);
	EXPECT(authority.SnapshotFor(queuer).terminalPending[0]);
	EXPECT(authority.Apply(spectator, TableAction(authority, spectator, 0, ActionKind::Unwatch)).accepted);
	EXPECT(authority.Apply(queuer, TableAction(authority, queuer, 0, ActionKind::Unqueue)).accepted);
	// Departure does not release the receipt, and admission is still fenced.
	EXPECT(authority.SnapshotFor(spectator).localTerminalPending);
	const auto rejoin = authority.Apply(spectator, TableAction(authority, spectator, 0, ActionKind::Watch));
	EXPECT(!rejoin.accepted && rejoin.reason == RejectReason::TerminalLedgerFull);
}

// The place, seat-opening and hold rules the room panel reads are the model's:
// they agree with what the authority does, and each hold names one cause.
static void TestPlaceAndSeatRules() {
	RoomAuthority authority("Places", 8, 9);
	Join(authority, 0, true);
	const auto first = Join(authority, 1), second = Join(authority, 2), queued = Join(authority, 3),
		watcher = Join(authority, 4), idle = Join(authority, 5);
	const auto act = [&](MemberId member, std::uint8_t table, ActionKind kind, int seat = -1) {
		auto action = TableAction(authority, member, table, kind);
		action.seat = static_cast<std::int8_t>(seat);
		return authority.Apply(member, action);
	};
	EXPECT(PlaceOf(authority.SnapshotView(), idle).kind == Place::Kind::None);
	EXPECT(SeatOpenNow(authority.SnapshotView().tables[1], 0) && SeatOpenNow(authority.SnapshotView().tables[1], 1));
	EXPECT(!SeatOpenNow(authority.SnapshotView().tables[1], -1) && !SeatOpenNow(authority.SnapshotView().tables[1], 2));
	EXPECT(act(first, 0, ActionKind::Queue).accepted && act(second, 0, ActionKind::Queue).accepted);
	EXPECT(act(queued, 0, ActionKind::Queue).accepted && act(watcher, 0, ActionKind::Watch).accepted);
	const auto& snapshot = authority.SnapshotView();
	const auto seatOf = PlaceOf(snapshot, first), secondSeat = PlaceOf(snapshot, second);
	EXPECT(seatOf.kind == Place::Kind::Seat && seatOf.table == 0 && seatOf.seat == 0);
	EXPECT(secondSeat.kind == Place::Kind::Seat && secondSeat.table == 0 && secondSeat.seat == 1);
	const auto waiting = PlaceOf(snapshot, queued);
	EXPECT(waiting.kind == Place::Kind::Queue && waiting.table == 0 && waiting.seat == -1);
	// Watching is not a place, since Queue and Watch both end it.
	EXPECT(PlaceOf(snapshot, watcher).kind == Place::Kind::None && PlaceOf(snapshot, 99).kind == Place::Kind::None);
	// The seat rule is the authority's: a full table has no open seat, and a
	// named seat behind a queue is refused.
	EXPECT(!SeatOpenNow(snapshot.tables[0], 0) && !SeatOpenNow(snapshot.tables[0], 1));
	EXPECT(!act(idle, 0, ActionKind::Queue, 0).accepted);
	EXPECT(act(idle, 1, ActionKind::Queue, 1).accepted);
	EXPECT(SeatOpenNow(authority.SnapshotView().tables[1], 0) && !SeatOpenNow(authority.SnapshotView().tables[1], 1));
	// A queued member's Watch at its own table is refused: the panel offers none.
	const auto refused = act(queued, 0, ActionKind::Watch);
	EXPECT(!refused.accepted && refused.reason == RejectReason::AlreadySeated);
	EXPECT(!WatchesByChoice(authority.SnapshotView().tables[0], queued));

	Table table;
	table.p1 = first; table.p2 = second; table.phase = TablePhase::Waiting;
	EXPECT(LeavingSeatHold(table, 0, false, false) == SeatHold::None && ChangingFighterHold(table, 0, false) == SeatHold::None);
	// This client's own game can outlast the table's Waiting phase.
	EXPECT(LeavingSeatHold(table, 0, true, false) == SeatHold::InProgress);
	EXPECT(LeavingSeatHold(table, 0, true, true) == SeatHold::InProgress);
	table.ready[0] = true;
	EXPECT(LeavingSeatHold(table, 0, false, false) == SeatHold::None);
	EXPECT(ChangingFighterHold(table, 0, false) == SeatHold::UnreadyFirst && ChangingFighterHold(table, 1, false) == SeatHold::None);
	EXPECT(LeavingSeatHold(table, 0, true, false) == SeatHold::UnreadyFirst);
	// A held start is a Ready that can still be taken back, not a game.
	table.ready[1] = true; table.phase = TablePhase::Ready; table.spectatorHold = true;
	EXPECT(LeavingSeatHold(table, 0, false, false) == SeatHold::UnreadyFirst && LeavingSeatHold(table, 1, false, false) == SeatHold::UnreadyFirst);
	EXPECT(ChangingFighterHold(table, 1, false) == SeatHold::UnreadyFirst);
	table.spectatorHold = false;
	EXPECT(LeavingSeatHold(table, 0, false, false) == SeatHold::InProgress && ChangingFighterHold(table, 0, false) == SeatHold::InProgress);
	table.phase = TablePhase::Playing;
	EXPECT(LeavingSeatHold(table, 0, false, false) == SeatHold::InProgress);
	EXPECT(LeavingSeatHold(table, 0, false, true) == SeatHold::AwaitingResult && ChangingFighterHold(table, 0, true) == SeatHold::AwaitingResult);
	table.resultPending = true;
	EXPECT(ChangingFighterHold(table, 0, false) == SeatHold::AwaitingResult);
	table.phase = TablePhase::Paused;
	EXPECT(LeavingSeatHold(table, 0, false, false) == SeatHold::Unresolved && ChangingFighterHold(table, 1, false) == SeatHold::Unresolved);

	Table fresh;
	EXPECT(!CostOfLeavingSeat(fresh));
	fresh.score[1] = 2;
	EXPECT(CostOfLeavingSeat(fresh).score && !CostOfLeavingSeat(fresh).handsOver);
	fresh.score[1] = 0; fresh.queue = {queued};
	EXPECT(!CostOfLeavingSeat(fresh).score && CostOfLeavingSeat(fresh).handsOver);
}

static void TestReadyDelaysShareTheHigherValue() {
	RoomAuthority authority("Delay", 4, 1);
	Join(authority, 0, true);
	const MemberId p1 = Join(authority, 1);
	const MemberId p2 = Join(authority, 2);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	const auto ready = [&](MemberId member, std::uint8_t delay) {
		Action action = TableAction(authority, member, 0, ActionKind::Ready);
		action.inputDelay = delay;
		EXPECT(authority.Apply(member, action).accepted);
	};
	// Each fighter keeps their own choice; the match uses the higher one.
	ready(p1, 1); ready(p2, 5);
	const auto& first = authority.SnapshotView().tables[0];
	EXPECT(first.inputDelay[0] == 1 && first.inputDelay[1] == 5);
	EXPECT(MatchDelay(first) == 5);
	EXPECT(authority.BeginMatch(0, p1, p2).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	EXPECT(authority.EndMatch(0, generation, MatchResult::P1Win).accepted);
	for (const auto member : authority.TerminalMembers(0, generation)) {
		Action acknowledgment = TableAction(authority, member, 0, ActionKind::AcknowledgeTerminal);
		acknowledgment.matchGeneration = generation;
		EXPECT(authority.Apply(member, acknowledgment).accepted);
	}
	// A rematch takes both fresh choices; the earlier higher value does not stick.
	ready(p1, 4); ready(p2, 2);
	EXPECT(MatchDelay(authority.SnapshotView().tables[0]) == 4);
}

// 0 frames is a delay a fighter can choose: Ready at 0 is recorded as 0, the
// match plays at 0 when both chose it, and a checkpoint keeps it.
static void TestReadyDelayZero() {
	RoomAuthority authority("Delay", 4, 1);
	Join(authority, 0, true);
	const MemberId p1 = Join(authority, 1);
	const MemberId p2 = Join(authority, 2);
	for (const auto member : {p1, p2}) EXPECT(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	for (const auto member : {p1, p2}) {
		Action action = TableAction(authority, member, 0, ActionKind::Ready);
		action.inputDelay = 0;
		EXPECT(authority.Apply(member, action).accepted);
	}
	const auto& table = authority.SnapshotView().tables[0];
	EXPECT(table.inputDelay[0] == 0 && table.inputDelay[1] == 0 && MatchDelay(table) == 0);
	EXPECT(FindMember(authority.SnapshotView(), p1)->frozenDelay == 0);
	RoomAuthority restored("Delay", 4, 1);
	EXPECT(restored.RestoreCheckpoint(nlohmann::json::parse(authority.Checkpoint().dump())));
	EXPECT(MatchDelay(restored.SnapshotView().tables[0]) == 0 && FindMember(restored.SnapshotView(), p2)->frozenDelay == 0);
	EXPECT(MatchDelay(0, 0) == 0 && MatchDelay(0, 3) == 3 && MatchDelay(3, 0) == 3);
}

// Auto's connection check names the seated pair at the table revision this
// PC sees, and waits until its committed copy of the room seats the same pair
// at the same revision. A spectator watching moves the revision; a copy that
// has not caught up with the seating, or has moved past it, is not yet clear.
static void TestProbePairCommitted() {
	RoomAuthority authority("Probe", 4, 1);
	Join(authority, 0, true);
	const MemberId p1 = Join(authority, 1);
	const MemberId p2 = Join(authority, 2);
	const MemberId watcher = Join(authority, 3);
	EXPECT(authority.Apply(p1, TableAction(authority, p1, 0, ActionKind::Queue)).accepted);
	const Table oneSeat = authority.SnapshotView().tables[0];
	EXPECT(!ProbePairCommitted(oneSeat, oneSeat));
	EXPECT(authority.Apply(p2, TableAction(authority, p2, 0, ActionKind::Queue)).accepted);
	const Table seated = authority.SnapshotView().tables[0];
	EXPECT(ProbePairCommitted(seated, seated));
	// The committed copy still shows the table before the second seat.
	EXPECT(!ProbePairCommitted(seated, oneSeat));
	EXPECT(authority.Apply(watcher, TableAction(authority, watcher, 0, ActionKind::Watch)).accepted);
	const Table watched = authority.SnapshotView().tables[0];
	EXPECT(watched.revision != seated.revision && watched.p1 == seated.p1 && watched.p2 == seated.p2);
	// Same pair, another revision either way: the check waits for both to agree.
	EXPECT(!ProbePairCommitted(seated, watched) && !ProbePairCommitted(watched, seated) && ProbePairCommitted(watched, watched));
}

// The host names a table, during a game too; the name crosses the wire and a
// checkpoint, an unnamed table carries no key, and nobody else can name one.
static void TestTableName() {
    RoomAuthority authority("Names", 16, 93);
    const auto host = Join(authority, 0, true), guest = Join(authority, 1);
    const auto named = [&](MemberId member, std::string text, std::uint64_t id) {
        auto action = TableAction(authority, member, 1, ActionKind::NameTable);
        action.text = std::move(text); action.actionId += id;
        return authority.Apply(member, action).accepted;
    };
    CHECK(named(host, "Beginners", 0) && authority.SnapshotView().tables[1].name == "Beginners");
    CHECK(!named(guest, "Mine", 10) && !named(host, std::string(MaximumTableNameBytes + 1, 'a'), 20) && !named(host, "Two\nlines", 30));
    CHECK(authority.SnapshotView().tables[1].name == "Beginners");
    const auto wire = nlohmann::json(authority.SnapshotFor(guest));
    CHECK(wire["tables"][1]["name"] == "Beginners" && !wire["tables"][0].contains("name"));
    CHECK(wire.get<Snapshot>().tables[1].name == "Beginners");
    RoomAuthority replica("Replica");
    CHECK(replica.RestoreCheckpoint(nlohmann::json::parse(authority.Checkpoint().dump())) && replica.SnapshotView().tables[1].name == "Beginners");
    CHECK(named(host, "", 40) && authority.SnapshotView().tables[1].name.empty());
}

int main() {
    TestTableName();
    TestProfileMain();
    TestReadyDelaysShareTheHigherValue();
    TestReadyDelayZero();
    TestProbePairCommitted();
    TestFighterChangeWithdrawsOpponentReady();
    TestIdleTimeIsStampedPerSnapshot();
	RoomAuthority authority("Test room", 16, 77);
	const MemberId host = Join(authority, 0, true);
	const MemberId guest = Join(authority, 1);
	EXPECT(authority.SnapshotView().host == host);
	EXPECT(authority.SnapshotView().members.size() == 2);

	// Four tables can seat independent pairs without touching each other's
	// revisions or membership state.
	for (int table = 0; table < 4; ++table) {
		const MemberId p1 = Join(authority, table * 2 + 2);
		const MemberId p2 = Join(authority, table * 2 + 3);
		for (const auto member : {p1, p2}) {
			Action queue = TableAction(authority, member, static_cast<std::uint8_t>(table), ActionKind::Queue);
			EXPECT(authority.Apply(member, queue).accepted);
		}
		const auto& snapshot = authority.SnapshotView();
		EXPECT(snapshot.tables[table].p1 != 0 && snapshot.tables[table].p2 != 0);
	}
	EXPECT(authority.SnapshotView().tables[0].p1 != 0);
	EXPECT(authority.SnapshotView().tables[1].p1 != 0);

	// A stale command for table zero is rejected only by table zero's
	// revision; activity on table one does not invalidate it.
	const auto beforeTableZero = authority.SnapshotView().tables[0].revision;
	const auto beforeRoomRevision = authority.SnapshotView().revision;
	Action watch = TableAction(authority, guest, 1, ActionKind::Watch);
	EXPECT(authority.Apply(guest, watch).accepted);
	EXPECT(authority.SnapshotView().tables[0].revision == beforeTableZero);
	EXPECT(authority.SnapshotView().revision > beforeRoomRevision);

	const auto table0 = authority.SnapshotView().tables[0];
	const MemberId p1 = table0.p1;
	const MemberId p2 = table0.p2;
	Action ready1 = TableAction(authority, p1, 0, ActionKind::Ready);
	EXPECT(authority.Apply(p1, ready1).accepted);
	Action ready2 = TableAction(authority, p2, 0, ActionKind::Ready);
	EXPECT(authority.Apply(p2, ready2).accepted);
	EXPECT(authority.SnapshotView().tables[0].phase == TablePhase::Ready);
	EXPECT(authority.BeginMatch(0, p1, p2).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	EXPECT(generation != 0);

	Action first = TableAction(authority, p1, 0, ActionKind::RecordResult);
	first.matchGeneration = generation;
	first.result = MatchResult::P1Win;
	EXPECT(authority.Apply(p1, first).accepted);
	Action duplicate = first;
	const auto duplicateResult = authority.Apply(p1, duplicate);
	EXPECT(!duplicateResult.accepted && duplicateResult.reason == RejectReason::DuplicateResult);
	EXPECT(authority.Apply(p2, [&]() { Action action = TableAction(authority, p2, 0, ActionKind::RecordResult); action.matchGeneration = generation; action.result = MatchResult::P2Win; return action; }()).accepted);
	EXPECT(authority.SnapshotView().tables[0].phase == TablePhase::Paused);
	Action cancel = TableAction(authority, host, 0, ActionKind::CancelResult);
	cancel.matchGeneration = generation;
	EXPECT(authority.Apply(host, cancel).accepted);

	// JSON round trips retain the explicit table and seat model.
    EXPECT(authority.SetMemberFighter(host,12));
    EXPECT(!authority.SetMemberFighter(host,44));
    EXPECT(!authority.SetMemberFighter(9999,0));
	nlohmann::json encoded = authority.SnapshotView();
	Snapshot decoded = encoded.get<Snapshot>();
	EXPECT(decoded.tables[0].id == 0 && decoded.tables[3].id == 3);
    EXPECT(decoded.members.front().fighter==12);
    auto oldMember=encoded["members"][0];oldMember.erase("fighter");
    EXPECT(oldMember.get<Member>().fighter==-1);
    auto badMember=encoded["members"][0];badMember["fighter"]=44;
    bool rejectedFighter=false;try{badMember.get<Member>();}catch(...){rejectedFighter=true;}
    EXPECT(rejectedFighter);

	// Kicked connection identity cannot rejoin under another display name.
	Action kick = TableAction(authority, host, 1, ActionKind::Kick);
	kick.revision = authority.SnapshotView().revision;
	kick.target = guest;
	EXPECT(authority.Apply(host, kick).accepted);
	const auto rejected = authority.Join("Guest renamed", Peer(1));
	EXPECT(!rejected.accepted && rejected.reason == RejectReason::MemberKicked);

	TestUnlimitedRematch();
	TestMatchFinishedAndSeatLifecycle();
	TestTerminalLifecycleGate();
	TestSpectatorDoesNotHoldTable();
	TestPlaceAndSeatRules();
	TestChatAfterSenderLeaves();
	TestTyping();
	TestFighterAbandonsDisputedResult();
	TestSnapshotBound();
	TestDepartureRecoveryAndSnapshotValidation();
    TestAuthorityRecovery();
    TestCommittedDepartureCheckpoint();
	TestRelativeTimerRecovery();
	TestTerminalReceiptSurvivesCompaction();

	if (TEST_FAILURES()) return 1;
	std::puts("RoomAuthority test passed");
	return 0;
}
