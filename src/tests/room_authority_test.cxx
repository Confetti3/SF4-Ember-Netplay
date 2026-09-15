#include "../session/RoomModel.hxx"
#include "../session/RoomCommit.hxx"

#include <cstdio>
#include <cstdlib>
#include <algorithm>

#include <nlohmann/json.hpp>

using namespace sf4e::room;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

static ConnectionRef Peer(int index) {
	return ConnectionRef{"host", std::to_string(index)};
}

static Action TableAction(const RoomAuthority& authority, MemberId member, std::uint8_t table, ActionKind kind) {
	Action action;
	action.kind = kind;
	action.roomEpoch = authority.SnapshotView().roomEpoch;
	action.revision = authority.SnapshotView().revision;
	action.table = table;
	action.tableRevision = authority.SnapshotView().tables[table].revision;
	action.actionId = member * 1000 + authority.SnapshotView().revision + 1;
	return action;
}

static MemberId Join(RoomAuthority& authority, int index, bool host = false) {
	const auto result = authority.Join("Player" + std::to_string(index), Peer(index), host);
	CHECK(result.accepted);
	return result.accepted ? result.snapshot.members.back().id : 0;
}

static void TestCommittedDepartureCheckpoint() {
    RoomAuthority source("Departure", 16, 92);
    const auto host = Join(source, 0, true), guest = Join(source, 1);
    const auto leave = TableAction(source, guest, 0, ActionKind::Leave);
    CHECK(leave.actionId != 0 && source.Apply(guest, leave).accepted);
    CHECK(source.SnapshotFor(guest).localMember == 0);
    RoomAuthority replica("Replica");
    CHECK(replica.RestoreCheckpoint(nlohmann::json::parse(source.Checkpoint().dump())));
    CHECK(replica.SnapshotView().members.size() == 1 && replica.SnapshotView().host == host);
    CHECK(source.Apply(host, TableAction(source, host, 0, ActionKind::Leave)).accepted);
    CHECK(replica.RestoreCheckpoint(nlohmann::json::parse(source.Checkpoint().dump())));
    CHECK(replica.SnapshotView().members.empty());
}

static void TestAuthorityRecovery() {
    RoomAuthority original("Recovery", 16, 91);
    auto host = Join(original, 0, true), p1 = Join(original, 1), p2 = Join(original, 2);
    auto kicked = Join(original, 3);
    Action ban = TableAction(original, host, 0, ActionKind::Kick); ban.target = kicked;
    CHECK(original.Apply(host, ban).accepted);
    for (auto member : {p1, p2}) CHECK(original.Apply(member, TableAction(original, member, 0, ActionKind::Queue)).accepted);
    for (auto member : {p1, p2}) CHECK(original.Apply(member, TableAction(original, member, 0, ActionKind::Ready)).accepted);
    CHECK(original.BeginMatch(0, p1, p2).accepted);
    original.AdvanceTime(1000);
    Action report = TableAction(original, p1, 0, ActionKind::RecordResult);
    report.matchGeneration = original.SnapshotView().tables[0].matchGeneration;
    report.result = MatchResult::P1Win;
    CHECK(original.Apply(p1, report).accepted);
    const auto checkpoint = nlohmann::json::parse(original.Checkpoint().dump());
    RoomAuthority restored("Unchanged");
    CHECK(restored.RestoreCheckpoint(checkpoint));
    CHECK(restored.Checkpoint() == checkpoint);
	const auto duplicateReport = restored.Apply(p1, report);
	CHECK(!duplicateReport.accepted && duplicateReport.reason == RejectReason::DuplicateResult);
    CHECK(!restored.Join("New name", Peer(3)).accepted);
    CHECK(!restored.TransferHost(p2, p1).accepted);
    CHECK(restored.TransferHost(host, p1).accepted);
    CHECK(restored.Leave(host).accepted && !restored.SnapshotView().closed);
    CHECK(restored.SnapshotView().tables[0].matchGeneration == report.matchGeneration);
    auto second = TableAction(restored, p2, 0, ActionKind::RecordResult);
    second.matchGeneration = report.matchGeneration; second.result = report.result;
    CHECK(restored.Apply(p2, second).accepted);
    CHECK(restored.SnapshotView().tables[0].score[0] == 1);
    restored.Apply(p2, second);
    CHECK(restored.SnapshotView().tables[0].score[0] == 1);
    CHECK(Join(restored, 4) > kicked);
    const auto unchanged = restored.Checkpoint();
    for (const auto* key : {"version", "next_member", "next_match", "actions", "reporters"}) {
        auto invalid = checkpoint; invalid[key] = "invalid";
        CHECK(!restored.RestoreCheckpoint(invalid)); CHECK(restored.Checkpoint() == unchanged);
    }
    RoomAuthority timed("Deadline"); CHECK(timed.RestoreCheckpoint(checkpoint));
    CHECK(timed.AdvanceTime(30999).empty());
    CHECK(!timed.AdvanceTime(31000).empty());

    RoomCommit commit(restored); Result effects;
    commit.SetAuthority(1, true);
    auto rename = TableAction(restored, p1, 0, ActionKind::Rename); rename.text = "Committed only";
    CHECK(commit.Prepare(1, [&](RoomAuthority& candidate) { return candidate.Apply(p1, rename); }));
    const auto stale = commit.PendingToken();
    CHECK(restored.SnapshotView().name == "Recovery");
    commit.SetAuthority(2, true);
    CHECK(!commit.Commit(stale, effects) && restored.SnapshotView().name == "Recovery");
    CHECK(commit.Prepare(2, [&](RoomAuthority& candidate) { return candidate.Apply(p1, rename); }));
    const auto accepted = commit.PendingToken();
    CHECK(commit.Commit(accepted, effects) && restored.SnapshotView().name == "Committed only");
    CHECK(!commit.Commit(accepted, effects));
    commit.SetAuthority(2, false);
    CHECK(!commit.Prepare(3, [&](RoomAuthority& candidate) { return candidate.Apply(p1, rename); }));
}

static void TestRelativeTimerRecovery() {
	RoomAuthority source("Relative timers", 8, 22);
	Join(source, 0, true);
	const auto p1 = Join(source, 1);
	const auto p2 = Join(source, 2);
	CHECK(source.Apply(p1, TableAction(source, p1, 0, ActionKind::Queue)).accepted);
	CHECK(source.Apply(p2, TableAction(source, p2, 0, ActionKind::Queue)).accepted);
	CHECK(source.Apply(p1, TableAction(source, p1, 0, ActionKind::Ready)).accepted);
	CHECK(source.Apply(p2, TableAction(source, p2, 0, ActionKind::Ready)).accepted);
	CHECK(source.BeginMatch(0, p1, p2).accepted);
	Action finished = TableAction(source, p1, 0, ActionKind::MatchFinished);
	finished.matchGeneration = source.SnapshotView().tables[0].matchGeneration;
	CHECK(source.Apply(p1, finished).accepted);
	CHECK(source.AdvanceTime(9000).empty());
	auto chat=TableAction(source,p1,0,ActionKind::Chat);chat.text="Before recovery";
	CHECK(source.Apply(p1,chat).accepted);
	CHECK(source.AdvanceTime(10000).empty());
	source.PauseForRecovery();
	const auto checkpoint = source.Checkpoint();
	CHECK(checkpoint.at("recovery_paused").get<bool>());
	CHECK(checkpoint.at("result_age").at(0).get<std::uint64_t>() == 10000);
	CHECK(checkpoint.at("result_since").at(0).get<std::uint64_t>() == 0);
	CHECK(checkpoint.at("chat_times").at(0).at(1).get<std::uint64_t>() == 1000);
	RoomAuthority restored("Restored timers");
	CHECK(restored.RestoreCheckpoint(checkpoint));
	CHECK(restored.RecoveryPaused());
	// Forwarding an already paused checkpoint through another helper must
	// preserve durations even though its receiving monotonic clock is zero.
	CHECK(restored.Checkpoint() == checkpoint);
	CHECK(restored.RestoreCheckpoint(nlohmann::json::parse(restored.Checkpoint().dump())));
	CHECK(restored.Checkpoint() == checkpoint);
	RoomAuthority healthyFollower("Healthy follower timers");
	CHECK(healthyFollower.RestoreCheckpoint(checkpoint));
	healthyFollower.AdvancePausedTimers(20000);
	const auto agedCheckpoint = healthyFollower.Checkpoint();
	CHECK(agedCheckpoint.at("result_age").at(0).get<std::uint64_t>() == 30000);
	CHECK(agedCheckpoint.at("chat_times").at(0).at(1).get<std::uint64_t>() == 1000);
	CHECK(!healthyFollower.AdvanceTime(500).empty());
	CHECK(healthyFollower.SnapshotView().tables[0].phase == TablePhase::Paused);
	CHECK(restored.AdvanceTime(500).empty()); // receiving clock is lower
	CHECK(restored.AdvanceTime(29999).empty());
	CHECK(!restored.AdvanceTime(30000).empty()); // age is retained across recovery
	CHECK(restored.SnapshotView().tables[0].phase == TablePhase::Paused);
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
		CHECK(authority.Apply(member, queue).accepted);
	}
	Action rules = TableAction(authority, host, 0, ActionKind::SetRules);
	rules.rules.format = SetFormat::Ft1; // normalized to Unlimited by the room
	CHECK(authority.Apply(host, rules).accepted);
	const auto before = authority.SnapshotView().tables[0];
	for (int game = 1; game <= 8; ++game) {
		for (const auto member : {before.p1, before.p2}) {
			Action ready = TableAction(authority, member, 0, ActionKind::Ready);
			CHECK(authority.Apply(member, ready).accepted);
		}
		CHECK(authority.BeginMatch(0, before.p1, before.p2).accepted);
		const auto generation = authority.SnapshotView().tables[0].matchGeneration;
		CHECK(authority.EndMatch(0, generation, MatchResult::P1Win).accepted);
		for (const auto member : authority.TerminalMembers(0, generation)) {
			Action acknowledgment = TableAction(authority, member, 0, ActionKind::AcknowledgeTerminal);
			acknowledgment.matchGeneration = generation;
			CHECK(authority.Apply(member, acknowledgment).accepted);
		}
		const auto& after = authority.SnapshotView().tables[0];
		CHECK(after.p1 == before.p1 && after.p2 == before.p2);
		CHECK(after.phase == TablePhase::Waiting && !after.ready[0] && !after.ready[1]);
		CHECK(after.score[0] == static_cast<std::uint32_t>(game) && after.score[1] == 0);
	}
	CHECK(authority.SnapshotView().tables[0].queue.size() == 2);
}

static void TestMatchFinishedAndSeatLifecycle() {
	RoomAuthority authority("Lifecycle", 8, 1);
	const MemberId host = Join(authority, 0, true);
	const MemberId p1 = Join(authority, 1);
	const MemberId p2 = Join(authority, 2);
	const MemberId p3 = Join(authority, 3);
	for (const auto member : {p1, p2, p3}) CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	const auto initial = authority.SnapshotView().tables[0];
	for (const auto member : {initial.p1, initial.p2}) CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	CHECK(authority.BeginMatch(0, initial.p1, initial.p2).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	CHECK(!authority.HasDueTimerTransition(0)); // no pending result
	Action finished = TableAction(authority, p1, 0, ActionKind::MatchFinished);
	finished.matchGeneration = generation;
	CHECK(authority.Apply(p1, finished).accepted);
	CHECK(authority.SnapshotView().tables[0].phase == TablePhase::Playing);
	CHECK(!authority.HasDueTimerTransition(0));
	CHECK(authority.AdvanceTime(10000).empty());
	CHECK(!authority.HasDueTimerTransition(9999)); // backward clock input
	CHECK(!authority.HasDueTimerTransition(10000)); // equal clock input
	CHECK(!authority.HasDueTimerTransition(29999)); // immediately before deadline
	CHECK(authority.HasDueTimerTransition(30000)); // exact deadline
	const auto timeout = authority.AdvanceTime(30000);
	CHECK(!timeout.empty() && authority.SnapshotView().tables[0].phase == TablePhase::Paused);
	CHECK(authority.SnapshotView().tables[0].resultPending);
	CHECK(!authority.HasDueTimerTransition(60000)); // paused but still pending
	const auto unresolved = authority.SnapshotView().tables[0];
	Action pausedRules = TableAction(authority, host, 0, ActionKind::SetRules);
	pausedRules.rules.roundTime = 300;
	CHECK(!authority.Apply(host, pausedRules).accepted);
	const auto afterPausedRules = authority.SnapshotView().tables[0];
	CHECK(afterPausedRules.phase == TablePhase::Paused && afterPausedRules.matchGeneration == unresolved.matchGeneration);
	CHECK(afterPausedRules.resultPending == unresolved.resultPending && afterPausedRules.score[0] == unresolved.score[0] &&
		afterPausedRules.score[1] == unresolved.score[1]);
	Action staleCancel = TableAction(authority, host, 0, ActionKind::CancelResult);
	staleCancel.matchGeneration = generation + 1;
	CHECK(!authority.Apply(host, staleCancel).accepted);
	Action cancel = staleCancel;
	cancel.matchGeneration = generation;
	CHECK(authority.Apply(host, cancel).accepted);
	CHECK(authority.SnapshotView().tables[0].phase == TablePhase::Waiting);

	// A waiting fighter can leave its seat and the next FIFO member fills it;
	// readiness and scores must not leak across that transition.
	const auto waiting = authority.SnapshotView().tables[0];
	Action unseat = TableAction(authority, waiting.p1, 0, ActionKind::Unqueue);
	CHECK(authority.Apply(waiting.p1, unseat).accepted);
	const auto after = authority.SnapshotView().tables[0];
	CHECK(after.p1 != waiting.p1 && after.p1 != 0 && !after.ready[0] && !after.ready[1]);
}

static void TestQueueWatchAndReplay() {
	RoomAuthority authority("Roster", 8, 1);
	const MemberId host = Join(authority, 0, true);
	const MemberId p1 = Join(authority, 1);
	const MemberId p2 = Join(authority, 2);
	const MemberId p3 = Join(authority, 3);
	const MemberId p4 = Join(authority, 4);
	for (const auto member : {p1, p2, p3, p4}) CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	const auto queued = authority.SnapshotView().tables[0];
	for (const auto member : {queued.p1, queued.p2}) CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	CHECK(authority.BeginMatch(0, queued.p1, queued.p2).accepted);
	const auto playing = authority.SnapshotView().tables[0];
	CHECK(playing.queue.size() == 2 && playing.spectators.size() == 2);
	const auto generation = playing.matchGeneration;
	Action exactGeneration = TableAction(authority, p3, 0, ActionKind::Unwatch);
	exactGeneration.matchGeneration = generation;
	exactGeneration.tableRevision = playing.revision - 1; // stale UI revision, current native generation
	CHECK(authority.Apply(p3, exactGeneration).accepted);
	Action staleGeneration = TableAction(authority, p4, 0, ActionKind::Unwatch);
	staleGeneration.matchGeneration = generation + 1;
	CHECK(!authority.Apply(p4, staleGeneration).accepted);
	CHECK(authority.EndMatch(0, playing.matchGeneration, MatchResult::P1Win).accepted);
	const auto rematched = authority.SnapshotView().tables[0];
	CHECK(rematched.p1 == p1 && rematched.p2 == p2 && rematched.score[0] == 1);
	const auto terminalCheckpoint = authority.Checkpoint();
	CHECK(terminalCheckpoint.at("terminal_receipts").at(0).at("recipients").size() == 4);
	Action spectatorAck = TableAction(authority, p3, 0, ActionKind::AcknowledgeTerminal);
	spectatorAck.matchGeneration = playing.matchGeneration;
	CHECK(authority.Apply(p3, spectatorAck).accepted);
	CHECK(std::find(rematched.queue.begin(), rematched.queue.end(), p3) != rematched.queue.end());
	Action stop = TableAction(authority, p2, 0, ActionKind::Unqueue);
	CHECK(authority.Apply(p2, stop).accepted);
	const auto rotated = authority.SnapshotView().tables[0];
	CHECK(rotated.p2 == p3 && std::find(rotated.queue.begin(), rotated.queue.end(), p3) == rotated.queue.end());
	CHECK(rotated.score[0] == 0 && rotated.score[1] == 0);
	Action replay = TableAction(authority, host, 0, ActionKind::Chat);
	replay.text = "replay-safe";
	replay.actionId = 9001;
	const auto first = authority.Apply(host, replay);
	const auto revision = authority.SnapshotView().revision;
	const auto repeat = authority.Apply(host, replay);
	CHECK(first.accepted && repeat.accepted && authority.SnapshotView().revision == revision && repeat.events.empty());

	// A seated fighter cannot silently join another table as a watcher.
	Action watch = TableAction(authority, rotated.p2, 1, ActionKind::Watch);
	const auto before = authority.SnapshotCopy();
	CHECK(!authority.Apply(rotated.p2, watch).accepted);
	CHECK(authority.SnapshotView().revision == before.revision);
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
		CHECK(authority.Apply(host, chat).accepted);
		authority.AdvanceTime(static_cast<std::uint64_t>(i + 1) * 1000);
	}
	CHECK(nlohmann::json(authority.SnapshotView()).dump().size() < 65536);
}

static void TestDepartureRecoveryAndSnapshotValidation() {
	RoomAuthority authority("Departure", 8, 1);
	const MemberId host = Join(authority, 0, true);
	const MemberId p1 = Join(authority, 1);
	const MemberId p2 = Join(authority, 2);
	const MemberId p3 = Join(authority, 3);
	const MemberId p4 = Join(authority, 4);
	for (const auto member : {p1, p2, p3, p4}) CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	const auto seated = authority.SnapshotView().tables[0];
	for (const auto member : {seated.p1, seated.p2}) CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	CHECK(authority.BeginMatch(0, seated.p1, seated.p2).accepted);
	CHECK(authority.SnapshotView().tables[0].spectators.size() == 2);

	// A watcher/queued member can disappear without touching the frozen fighter
	// pair. The next fighter departure aborts that generation and seats FIFO.
	CHECK(authority.Leave(p3).accepted);
	CHECK(authority.SnapshotView().tables[0].p1 == p1 && authority.SnapshotView().tables[0].p2 == p2);
	CHECK(authority.SnapshotView().tables[0].phase == TablePhase::Playing);
	const auto leaving = authority.Leave(p2);
	CHECK(leaving.accepted);
	CHECK(std::any_of(leaving.events.begin(), leaving.events.end(), [](const Event& event) { return event.kind == Event::Kind::MatchEnded; }));
	const auto recovered = authority.SnapshotView().tables[0];
	CHECK(recovered.p1 == p1 && recovered.p2 == p4 && recovered.phase == TablePhase::Waiting);
	CHECK(recovered.score[0] == 0 && recovered.score[1] == 0 && !recovered.ready[0] && !recovered.ready[1]);
	RoomAuthority departedRestore("Active departure restore");
	CHECK(departedRestore.RestoreCheckpoint(nlohmann::json::parse(authority.Checkpoint().dump())));
	CHECK(departedRestore.PendingTerminalEvents(p1).size() == 1);

	// Pre-start spectators leaving a waiting table do not pause or strand it.
	const MemberId p5 = Join(authority, 5);
	Action watch = TableAction(authority, p5, 0, ActionKind::Watch);
	CHECK(authority.Apply(p5, watch).accepted);
	CHECK(authority.Leave(p5).accepted);
	CHECK(authority.SnapshotView().tables[0].phase == TablePhase::Waiting);

	// Decoder bounds reject forged IDs/table slots rather than narrowing them
	// into a valid-looking snapshot.
	nlohmann::json encoded = authority.SnapshotView();
	auto badMember = encoded;
	badMember["members"][0]["name"] = std::string(33, 'x');
	bool threw = false;
	try { (void)badMember.get<Snapshot>(); } catch (const std::exception&) { threw = true; }
	CHECK(threw);
	auto badTable = encoded;
	badTable["tables"][0]["id"] = 4;
	threw = false;
	try { (void)badTable.get<Snapshot>(); } catch (const std::exception&) { threw = true; }
	CHECK(threw);
	auto badRoster = encoded;
	badRoster["tables"][0]["queue"] = nlohmann::json::array({999999999999ULL});
	threw = false;
	try { (void)badRoster.get<Snapshot>(); } catch (const std::exception&) { threw = true; }
	CHECK(threw);

	(void)host;
}

static void TestProfileMain() {
    RoomAuthority authority("Profiles",16,70);
    CHECK(!authority.Join("Invalid",Peer(0),true,44).accepted);
    CHECK(!authority.Join("Invalid",Peer(0),true,-2).accepted);
    CHECK(authority.Join("Main",Peer(0),true,11).accepted);
    const auto host=authority.SnapshotView().host;
    CHECK(authority.SetMemberFighter(host,2));
    const auto encoded=nlohmann::json(authority.SnapshotView()).dump();
    const auto decoded=nlohmann::json::parse(encoded).get<Snapshot>();
    CHECK(decoded.members[0].mainFighter==11&&decoded.members[0].fighter==2);
    RoomAuthority restored("Restore");CHECK(restored.RestoreCheckpoint(authority.Checkpoint()));
    CHECK(restored.SnapshotView().members[0].mainFighter==11);
    auto member=nlohmann::json(decoded.members[0]);member.erase("main_fighter");
    CHECK(member.get<Member>().mainFighter==-1);
    for(const nlohmann::json invalid:{nlohmann::json(-2),nlohmann::json(44),nlohmann::json(1.5),nlohmann::json(UINT64_MAX)}){
        member["main_fighter"]=invalid;bool rejected=false;
        try{member.get<Member>();}catch(...){rejected=true;}CHECK(rejected);
    }
}

static void TestTerminalLifecycleGate() {
	RoomAuthority authority("Terminal lifecycle", 8, 310);
	Join(authority, 0, true);
	const auto p1 = Join(authority, 1);
	const auto p2 = Join(authority, 2);
	const auto spectator = Join(authority, 3);
	for (const auto member : {p1, p2}) CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	Action watch = TableAction(authority, spectator, 0, ActionKind::Watch);
	CHECK(authority.Apply(spectator, watch).accepted);
	const auto seated = authority.SnapshotView().tables[0];
	for (const auto member : {seated.p1, seated.p2}) CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
	CHECK(authority.BeginMatch(0, p1, p2).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	Action first = TableAction(authority, p1, 0, ActionKind::RecordResult);
	first.matchGeneration = generation; first.result = MatchResult::P1Win;
	CHECK(authority.Apply(p1, first).accepted);
	Action second = TableAction(authority, p2, 0, ActionKind::RecordResult);
	second.matchGeneration = generation; second.result = MatchResult::P1Win;
	CHECK(authority.Apply(p2, second).accepted);

	// A member cannot stage a new native generation while any of its frozen
	// spectator/fighter receipts still drain; this also prevents local outcome
	// state from being overwritten by a different table's terminal event.
	Action ready = TableAction(authority, p1, 0, ActionKind::Ready);
	CHECK(!authority.Apply(p1, ready).accepted);
	ready = TableAction(authority, p2, 0, ActionKind::Ready);
	CHECK(!authority.Apply(p2, ready).accepted);
	CHECK(authority.PendingTerminalEvents(spectator).size() == 1);
	Action crossTableQueue = TableAction(authority, p1, 1, ActionKind::Queue);
	const auto crossTableResult = authority.Apply(p1, crossTableQueue);
	CHECK(!crossTableResult.accepted && crossTableResult.reason == RejectReason::TerminalLedgerFull);

	// The frozen incarnation is the authorization boundary for the ACK. A
	// rebind of the same numeric member cannot release an older native slot.
	authority.SetMemberIncarnation(spectator, 2);
	Action spectatorAck = TableAction(authority, spectator, 0, ActionKind::AcknowledgeTerminal);
	spectatorAck.matchGeneration = generation;
	CHECK(!authority.Apply(spectator, spectatorAck).accepted);
	authority.SetMemberIncarnation(spectator, 1);
	CHECK(authority.Apply(spectator, spectatorAck).accepted);
	for (const auto member : {p1, p2}) {
		Action acknowledgment = TableAction(authority, member, 0, ActionKind::AcknowledgeTerminal);
		acknowledgment.matchGeneration = generation;
		CHECK(authority.Apply(member, acknowledgment).accepted);
	}
	ready = TableAction(authority, p1, 0, ActionKind::Ready);
	CHECK(authority.Apply(p1, ready).accepted);
	ready = TableAction(authority, p2, 0, ActionKind::Ready);
	CHECK(authority.Apply(p2, ready).accepted);
	CHECK(authority.BeginMatch(0, p1, p2).accepted);
}

static void TestTerminalReceiptSurvivesCompaction() {
	// Explicit native retirement may remove a fighter from the mutable room
	// roster. The frozen endpoint/incarnation is still enough to restore the
	// durable terminal receipt from a later checkpoint.
	RoomAuthority departed("Retired fighter", 8, 204);
	Join(departed, 0, true);
	const auto departedP1 = Join(departed, 1), departedP2 = Join(departed, 2);
	for (const auto member : {departedP1, departedP2}) CHECK(departed.Apply(member, TableAction(departed, member, 0, ActionKind::Queue)).accepted);
	const auto departedTable = departed.SnapshotView().tables[0];
	for (const auto member : {departedTable.p1, departedTable.p2}) CHECK(departed.Apply(member, TableAction(departed, member, 0, ActionKind::Ready)).accepted);
	CHECK(departed.BeginMatch(0, departedP1, departedP2).accepted);
	const auto departedGeneration = departed.SnapshotView().tables[0].matchGeneration;
	Action departedFirst = TableAction(departed, departedP1, 0, ActionKind::RecordResult);
	departedFirst.matchGeneration = departedGeneration; departedFirst.result = MatchResult::P1Win;
	CHECK(departed.Apply(departedP1, departedFirst).accepted);
	Action departedSecond = TableAction(departed, departedP2, 0, ActionKind::RecordResult);
	departedSecond.matchGeneration = departedGeneration; departedSecond.result = MatchResult::P1Win;
	CHECK(departed.Apply(departedP2, departedSecond).accepted);
	CHECK(departed.Leave(departedP2).accepted);
	RoomAuthority departedRestored("Retired fighter restore");
	CHECK(departedRestored.RestoreCheckpoint(nlohmann::json::parse(departed.Checkpoint().dump())));
	CHECK(departedRestored.PendingTerminalEvents(departedP1).size() == 1);

	RoomAuthority source("Terminal receipt", 16, 203);
	Join(source, 0, true);
	const auto p1 = Join(source, 1);
	const auto p2 = Join(source, 2);
	for (const auto member : {p1, p2}) CHECK(source.Apply(member, TableAction(source, member, 0, ActionKind::Queue)).accepted);
	const auto seated = source.SnapshotView().tables[0];
	for (const auto member : {seated.p1, seated.p2}) CHECK(source.Apply(member, TableAction(source, member, 0, ActionKind::Ready)).accepted);
	CHECK(source.BeginMatch(0, p1, p2).accepted);
	const auto generation = source.SnapshotView().tables[0].matchGeneration;
	Action first = TableAction(source, p1, 0, ActionKind::RecordResult);
	first.matchGeneration = generation;
	first.result = MatchResult::P1Win;
	CHECK(source.Apply(p1, first).accepted);
	Action second = TableAction(source, p2, 0, ActionKind::RecordResult);
	second.matchGeneration = generation;
	second.result = MatchResult::P1Win;
	CHECK(source.Apply(p2, second).accepted);
	CHECK(source.SnapshotView().tables[0].phase == TablePhase::Waiting);
	CHECK(source.Checkpoint().at("terminal_receipts").size() == 1);

	// Drive a different table through more than the generic journal's old
	// retention span. The completed fighter pair and outcome must stay in the
	// replicated checkpoint ledger while unrelated room actions continue.
	for (int i = 0; i < 320; ++i) {
		Action unrelated = TableAction(source, source.SnapshotView().host, 1, ActionKind::SetRules);
		unrelated.rules = source.SnapshotView().tables[1].rules;
		unrelated.rules.editionSelect = (i & 1) != 0;
		CHECK(source.Apply(source.SnapshotView().host, unrelated).accepted);
	}
	const auto checkpoint = nlohmann::json::parse(source.Checkpoint().dump());
	RoomAuthority restored("Restored terminal receipt");
	CHECK(restored.RestoreCheckpoint(checkpoint));
	CHECK(restored.SnapshotView().tables[0].score[0] == 1);

	Action late = TableAction(restored, p2, 0, ActionKind::RecordResult);
	late.actionId = 900001;
	late.matchGeneration = generation;
	late.result = MatchResult::P1Win;
	const auto replay = restored.Apply(p2, late);
	CHECK(replay.accepted && replay.terminalReplay);
	CHECK(std::any_of(replay.events.begin(), replay.events.end(), [](const Event& event) {
		return event.kind == Event::Kind::MatchEnded && event.terminalReplay;
	}));
	CHECK(restored.SnapshotView().tables[0].score[0] == 1);
	const auto replayCheckpoint = restored.Checkpoint();
	CHECK(std::none_of(replayCheckpoint.at("terminal_receipts").at(0).at("recipients").begin(),
		replayCheckpoint.at("terminal_receipts").at(0).at("recipients").end(),
		[](const nlohmann::json& recipient) { return recipient.at("acknowledged").get<bool>(); }));

	late = TableAction(restored, p1, 0, ActionKind::RecordResult);
	late.actionId = 900002;
	late.matchGeneration = generation;
	late.result = MatchResult::P1Win;
	const auto duplicateReplay = restored.Apply(p1, late);
	CHECK(duplicateReplay.accepted && duplicateReplay.terminalReplay);
	CHECK(restored.SnapshotView().tables[0].score[0] == 1);
	for (const auto member : source.TerminalMembers(0, generation)) {
		Action acknowledgment = TableAction(source, member, 0, ActionKind::AcknowledgeTerminal);
		acknowledgment.matchGeneration = generation;
		CHECK(source.Apply(member, acknowledgment).accepted);
	}

	// Cancel/abort teardown has the same durable replay path even when the
	// moderator is not one of the two frozen fighters.
	for (const auto member : {p1, p2}) CHECK(source.Apply(member, TableAction(source, member, 0, ActionKind::Ready)).accepted);
	CHECK(source.BeginMatch(0, p1, p2).accepted);
	const auto cancelledGeneration = source.SnapshotView().tables[0].matchGeneration;
	Action cancel = TableAction(source, source.SnapshotView().host, 0, ActionKind::CancelResult);
	cancel.matchGeneration = cancelledGeneration;
	CHECK(source.Apply(source.SnapshotView().host, cancel).accepted);
	RoomAuthority cancelRestored("Restored cancel receipt");
	CHECK(cancelRestored.RestoreCheckpoint(nlohmann::json::parse(source.Checkpoint().dump())));
	Action lateCancel = TableAction(cancelRestored, cancelRestored.SnapshotView().host, 0, ActionKind::CancelResult);
	lateCancel.actionId = 900003;
	lateCancel.matchGeneration = cancelledGeneration;
	const auto cancelReplay = cancelRestored.Apply(cancelRestored.SnapshotView().host, lateCancel);
	CHECK(cancelReplay.accepted && cancelReplay.terminalReplay);

	// The first receipt was acknowledged, then pruned when this later
	// generation was stored. A lost action reply must still be recoverable only
	// for the same member incarnation and exact old table/generation key.
	Action lateOldAck = TableAction(source, p1, 0, ActionKind::AcknowledgeTerminal);
	lateOldAck.actionId = 900004; lateOldAck.matchGeneration = generation;
	CHECK(source.Apply(p1, lateOldAck).accepted);
	Action unknownAck = lateOldAck;
	unknownAck.actionId = 900005; unknownAck.matchGeneration = generation + 100000;
	CHECK(!source.Apply(p1, unknownAck).accepted);
}

int main() {
    TestProfileMain();
	RoomAuthority authority("Test room", 16, 77);
	const MemberId host = Join(authority, 0, true);
	const MemberId guest = Join(authority, 1);
	CHECK(authority.SnapshotView().host == host);
	CHECK(authority.SnapshotView().members.size() == 2);

	// Four tables can seat independent pairs without touching each other's
	// revisions or membership state.
	for (int table = 0; table < 4; ++table) {
		const MemberId p1 = Join(authority, table * 2 + 2);
		const MemberId p2 = Join(authority, table * 2 + 3);
		for (const auto member : {p1, p2}) {
			Action queue = TableAction(authority, member, static_cast<std::uint8_t>(table), ActionKind::Queue);
			CHECK(authority.Apply(member, queue).accepted);
		}
		const auto& snapshot = authority.SnapshotView();
		CHECK(snapshot.tables[table].p1 != 0 && snapshot.tables[table].p2 != 0);
	}
	CHECK(authority.SnapshotView().tables[0].p1 != 0);
	CHECK(authority.SnapshotView().tables[1].p1 != 0);

	// A stale command for table zero is rejected only by table zero's
	// revision; activity on table one does not invalidate it.
	const auto beforeTableZero = authority.SnapshotView().tables[0].revision;
	const auto beforeRoomRevision = authority.SnapshotView().revision;
	Action watch = TableAction(authority, guest, 1, ActionKind::Watch);
	CHECK(authority.Apply(guest, watch).accepted);
	CHECK(authority.SnapshotView().tables[0].revision == beforeTableZero);
	CHECK(authority.SnapshotView().revision > beforeRoomRevision);

	const auto table0 = authority.SnapshotView().tables[0];
	const MemberId p1 = table0.p1;
	const MemberId p2 = table0.p2;
	Action ready1 = TableAction(authority, p1, 0, ActionKind::Ready);
	CHECK(authority.Apply(p1, ready1).accepted);
	Action ready2 = TableAction(authority, p2, 0, ActionKind::Ready);
	CHECK(authority.Apply(p2, ready2).accepted);
	CHECK(authority.SnapshotView().tables[0].phase == TablePhase::Ready);
	CHECK(authority.BeginMatch(0, p1, p2).accepted);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	CHECK(generation != 0);

	Action first = TableAction(authority, p1, 0, ActionKind::RecordResult);
	first.matchGeneration = generation;
	first.result = MatchResult::P1Win;
	CHECK(authority.Apply(p1, first).accepted);
	Action duplicate = first;
	const auto duplicateResult = authority.Apply(p1, duplicate);
	CHECK(!duplicateResult.accepted && duplicateResult.reason == RejectReason::DuplicateResult);
	CHECK(authority.Apply(p2, [&]() { Action action = TableAction(authority, p2, 0, ActionKind::RecordResult); action.matchGeneration = generation; action.result = MatchResult::P2Win; return action; }()).accepted);
	CHECK(authority.SnapshotView().tables[0].phase == TablePhase::Paused);
	Action cancel = TableAction(authority, host, 0, ActionKind::CancelResult);
	cancel.matchGeneration = generation;
	CHECK(authority.Apply(host, cancel).accepted);

	// JSON round trips retain the explicit table and seat model.
    CHECK(authority.SetMemberFighter(host,12));
    CHECK(!authority.SetMemberFighter(host,44));
    CHECK(!authority.SetMemberFighter(9999,0));
	nlohmann::json encoded = authority.SnapshotView();
	Snapshot decoded = encoded.get<Snapshot>();
	CHECK(decoded.tables[0].id == 0 && decoded.tables[3].id == 3);
    CHECK(decoded.members.front().fighter==12);
    auto oldMember=encoded["members"][0];oldMember.erase("fighter");
    CHECK(oldMember.get<Member>().fighter==-1);
    auto badMember=encoded["members"][0];badMember["fighter"]=44;
    bool rejectedFighter=false;try{badMember.get<Member>();}catch(...){rejectedFighter=true;}
    CHECK(rejectedFighter);

	// Kicked connection identity cannot rejoin under another display name.
	Action kick = TableAction(authority, host, 1, ActionKind::Kick);
	kick.revision = authority.SnapshotView().revision;
	kick.target = guest;
	CHECK(authority.Apply(host, kick).accepted);
	const auto rejected = authority.Join("Guest renamed", Peer(1));
	CHECK(!rejected.accepted && rejected.reason == RejectReason::MemberKicked);

	TestUnlimitedRematch();
	TestMatchFinishedAndSeatLifecycle();
	TestTerminalLifecycleGate();
	TestQueueWatchAndReplay();
	TestSnapshotBound();
	TestDepartureRecoveryAndSnapshotValidation();
    TestAuthorityRecovery();
    TestCommittedDepartureCheckpoint();
	TestRelativeTimerRecovery();
	TestTerminalReceiptSurvivesCompaction();

	if (failures) return 1;
	std::puts("RoomAuthority test passed");
	return 0;
}
