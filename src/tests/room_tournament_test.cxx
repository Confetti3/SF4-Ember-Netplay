// A room bound to a tournament match: fighters-only seating at table 0, the
// permit each game waits for, and the binding surviving a checkpoint.
#include "room_authority_support.hxx"

#include <memory>
#include <string>

static const std::string EndpointA(64, 'a');
static const std::string EndpointB(64, 'b');
static const std::string EndpointC(64, 'c');

static MemberId JoinAs(RoomAuthority& authority, const std::string& name, const std::string& endpoint, bool host = false) {
	const auto result = authority.Join(name, ConnectionRef{"room", endpoint}, host);
	if (!result.accepted) return 0;
	for (const auto& member : result.snapshot.members)
		if (member.connection.user == endpoint) return member.id;
	return 0;
}

static TournamentBinding Binding(std::uint64_t revision = 1, const std::string& second = EndpointB) {
	TournamentBinding binding;
	binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
	binding.assignmentGeneration = 1;
	binding.bindingRevision = revision;
	binding.gamesToWin = 2;
	binding.fighters[0] = {EndpointA, "emb1_aaaa"};
	binding.fighters[1] = {second, "emb1_bbbb"};
	return binding;
}

static bool HasEvent(const Result& result, Event::Kind kind) {
	return std::any_of(result.events.begin(), result.events.end(), [kind](const Event& event) { return event.kind == kind; });
}

static Action Permit(const RoomAuthority& authority, MemberId member, const std::string& permit,
	std::uint64_t window = PermitStartMs) {
	Action action = TableAction(authority, member, 0, ActionKind::PermitReady);
	action.matchGeneration = authority.SnapshotView().tables[0].permitGeneration;
	action.text = permit;
	action.startWindowMs = window;
	return action;
}

static void ReadyBoth(RoomAuthority& authority) {
	const auto table = authority.SnapshotView().tables[0];
	for (const auto member : {table.p1, table.p2})
		CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Ready)).accepted);
}

// Binding seats the two fighters by slot, sends anyone else away, and keeps
// strangers and seat changes out.
static void TestFightersOnly() {
	RoomAuthority authority("Match", 8, 1);
	const MemberId a = JoinAs(authority, "A", EndpointA, true);
	const MemberId stranger = JoinAs(authority, "C", EndpointC);
	CHECK(a && stranger);
	auto bound = authority.BindTournament(Binding());
	CHECK(bound.accepted && HasEvent(bound, Event::Kind::MemberRemoved));
	CHECK(!FindMember(authority.SnapshotView(), stranger));
	CHECK(authority.SnapshotView().tables[0].p1 == a && authority.SnapshotView().tables[0].rules.format == SetFormat::Ft2);
	const MemberId b = JoinAs(authority, "B", EndpointB);
	CHECK(b && authority.SnapshotView().tables[0].p2 == b);
	CHECK(authority.SnapshotView().tables[0].phase == TablePhase::Waiting);
	CHECK(!JoinAs(authority, "C", EndpointC));
	// Seats and rules at the bound table come from the binding alone.
	for (const auto kind : {ActionKind::Unqueue, ActionKind::Watch, ActionKind::SetRules})
		CHECK(!authority.Apply(b, TableAction(authority, b, 0, kind)).accepted);
	Action kick = TableAction(authority, a, 0, ActionKind::Kick);
	kick.target = b;
	CHECK(!authority.Apply(a, kick).accepted);
	// An older binding, or one for another match, is refused; the same one again changes nothing.
	CHECK(authority.BindTournament(Binding()).accepted);
	auto other = Binding(2);
	other.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a99";
	CHECK(!authority.BindTournament(other).accepted);
	auto invalid = Binding(2);
	invalid.fighters[1].endpoint = EndpointA;
	CHECK(!authority.BindTournament(invalid).accepted);
	// A fighter's new endpoint (a restarted helper) takes the slot when it joins.
	CHECK(authority.BindTournament(Binding(2, EndpointC)).accepted);
	CHECK(!FindMember(authority.SnapshotView(), b));
	const MemberId again = JoinAs(authority, "B", EndpointC);
	CHECK(again && authority.SnapshotView().tables[0].p2 == again);
	CHECK(!authority.BindTournament(Binding(1)).accepted);
	// A fighter who leaves and comes back takes its slot again.
	CHECK(authority.Leave(again).accepted);
	CHECK(authority.SnapshotView().tables[0].p2 == 0);
	const MemberId back = JoinAs(authority, "B", EndpointC);
	CHECK(back && authority.SnapshotView().tables[0].p2 == back);
}

// Ready reserves a generation and waits for both fighters to hold the same
// permit for it; the game then plays that generation, and a finished set
// keeps both fighters in their slots.
static void TestPermitGate() {
	RoomAuthority authority("Match", 8, 1);
	authority.AdvanceTime(1000);
	const MemberId a = JoinAs(authority, "A", EndpointA, true);
	CHECK(authority.BindTournament(Binding()).accepted);
	const MemberId b = JoinAs(authority, "B", EndpointB);
	for (int game = 0; game < 2; ++game) {
		const auto table = authority.SnapshotView().tables[0];
		CHECK(authority.Apply(table.p1, TableAction(authority, table.p1, 0, ActionKind::Ready)).accepted);
		const auto last = authority.Apply(table.p2, TableAction(authority, table.p2, 0, ActionKind::Ready));
		CHECK(last.accepted && !HasEvent(last, Event::Kind::MatchReady));
		const auto reserved = authority.SnapshotView().tables[0].permitGeneration;
		CHECK(reserved != 0 && PermitPending(authority.SnapshotView().tables[0]));
		CHECK(ReadyCancellable(authority.SnapshotView().tables[0], 0));
		CHECK(!authority.BeginMatch(0, a, b).accepted);
		CHECK(authority.Apply(a, Permit(authority, a, "per_one")).accepted);
		CHECK(!authority.Apply(b, Permit(authority, b, "not-a-permit")).accepted);
		const auto differing = authority.Apply(b, Permit(authority, b, "per_two"));
		CHECK(differing.accepted && !HasEvent(differing, Event::Kind::MatchReady));
		const auto agreed = authority.Apply(b, Permit(authority, b, "per_one"));
		CHECK(agreed.accepted && HasEvent(agreed, Event::Kind::MatchReady));
		CHECK(authority.BeginMatch(0, a, b).accepted);
		CHECK(authority.SnapshotView().tables[0].matchGeneration == reserved);
		CHECK(authority.SnapshotView().tables[0].permitGeneration == 0);
		CHECK(authority.EndMatch(0, reserved, MatchResult::P1Win).accepted);
		for (const auto member : authority.TerminalMembers(0, reserved)) {
			Action acknowledgment = TableAction(authority, member, 0, ActionKind::AcknowledgeTerminal);
			acknowledgment.matchGeneration = reserved;
			CHECK(authority.Apply(member, acknowledgment).accepted);
		}
	}
	const auto& table = authority.SnapshotView().tables[0];
	CHECK(table.lastSet.score[0] == 2 && table.lastSet.winnerSeat == 0);
	CHECK(table.p1 == a && table.p2 == b && table.queue.empty());
}

// A permit that never comes calls the start off, and taking Ready back does
// too; the reserved generation is never offered again.
static void TestPermitHoldEnds() {
	RoomAuthority authority("Match", 8, 1);
	authority.AdvanceTime(1000);
	const MemberId a = JoinAs(authority, "A", EndpointA, true);
	CHECK(authority.BindTournament(Binding()).accepted);
	const MemberId b = JoinAs(authority, "B", EndpointB);
	ReadyBoth(authority);
	const auto first = authority.SnapshotView().tables[0].permitGeneration;
	CHECK(authority.Apply(a, Permit(authority, a, "per_one")).accepted);
	CHECK(!authority.HasDueTimerTransition(1000 + PermitHoldMs - 1));
	CHECK(authority.HasDueTimerTransition(1000 + PermitHoldMs));
	{
		// The other permit at the last moment of the hold still starts the
		// game, inside the permit's start window; once the hold ends it cannot.
		RoomAuthority late("Match", 8, 1);
		CHECK(late.RestoreCheckpoint(authority.Checkpoint()));
		late.AdvanceTime(1000 + PermitHoldMs - 1);
		const auto inTime = late.Apply(b, Permit(late, b, "per_one"));
		CHECK(inTime.accepted && HasEvent(inTime, Event::Kind::MatchReady));
	}
	authority.AdvanceTime(1000 + PermitHoldMs);
	{
		const auto& table = authority.SnapshotView().tables[0];
		CHECK(table.phase == TablePhase::Waiting && !table.ready[0] && !table.ready[1] && table.permitGeneration == 0);
	}
	ReadyBoth(authority);
	const auto second = authority.SnapshotView().tables[0].permitGeneration;
	CHECK(second > first);
	CHECK(authority.Apply(b, TableAction(authority, b, 0, ActionKind::Unready)).accepted);
	CHECK(authority.SnapshotView().tables[0].permitGeneration == 0);
	// A permit for the abandoned generation no longer counts.
	Action stale = TableAction(authority, a, 0, ActionKind::PermitReady);
	stale.matchGeneration = second;
	stale.text = "per_one";
	CHECK(!authority.Apply(a, stale).accepted);
}

// The binding and a waiting permit move with the room to a new owner.
static void TestCheckpoint() {
	RoomAuthority authority("Match", 8, 1);
	authority.AdvanceTime(1000);
	const MemberId a = JoinAs(authority, "A", EndpointA, true);
	CHECK(authority.BindTournament(Binding()).accepted);
	JoinAs(authority, "B", EndpointB);
	ReadyBoth(authority);
	CHECK(authority.Apply(a, Permit(authority, a, "per_one")).accepted);
	const auto reserved = authority.SnapshotView().tables[0].permitGeneration;
	const auto checkpoint = authority.Checkpoint();
	RoomAuthority restored("Other", 8, 1);
	CHECK(restored.RestoreCheckpoint(checkpoint));
	CHECK(restored.SnapshotView().tournament.Active());
	CHECK(restored.SnapshotView().tables[0].permitGeneration == reserved);
	CHECK(restored.SnapshotView().tables[0].permits[0] == "per_one");
	CHECK(restored.SnapshotView().tables[0].permitWindows[0] == PermitStartMs);
	CHECK(!JoinAs(restored, "C", EndpointC));
	// The wire form carries the binding to every member.
	Snapshot copy;
	nlohmann::json(authority.SnapshotView()).get_to(copy);
	CHECK(copy.tournament.matchId == Binding().matchId && copy.tournament.fighters[1].endpoint == EndpointB);
	CHECK(copy.tables[0].permitWindows[0] == PermitStartMs && copy.tables[0].permitWindows[1] == 0);
	// A window needs its permit.
	auto windowless = checkpoint;
	windowless["snapshot"]["tables"][0]["permit_windows"] = {PermitStartMs, PermitStartMs};
	CHECK(!restored.RestoreCheckpoint(windowless));
	// A permit hold without a binding is not a room state.
	auto forged = checkpoint;
	forged["snapshot"]["tournament"] = nlohmann::json::object();
	CHECK(!restored.RestoreCheckpoint(forged));
}

// A bound room whose fighters readied at 1000 and whose first fighter gave its
// permit at `permitAt`. Returns the reserved generation.
static std::uint64_t HoldOnePermit(RoomAuthority& authority, MemberId& a, MemberId& b, std::uint64_t permitAt,
	std::uint64_t window = PermitStartMs) {
	authority.AdvanceTime(1000);
	a = JoinAs(authority, "A", EndpointA, true);
	CHECK(authority.BindTournament(Binding()).accepted);
	b = JoinAs(authority, "B", EndpointB);
	ReadyBoth(authority);
	authority.AdvanceTime(permitAt);
	CHECK(authority.Apply(a, Permit(authority, a, "per_one", window)).accepted);
	return authority.SnapshotView().tables[0].permitGeneration;
}

// What a recovering owner holds: the room paused, its timers as ages, as a
// session's recovery checkpoint carries it.
static nlohmann::json PausedCheckpoint(const RoomAuthority& authority) {
	RoomAuthority paused = authority;
	paused.PauseForRecovery();
	return paused.Checkpoint();
}

// The permit's own window ends a start, whatever the hold allows: the game
// must begin before the shorter window of the two seats, less the margin,
// runs out from the reservation. A start past it is called off, and the
// fighters ready again for a new generation and a new permit.
static void TestPermitWindowGate() {
	RoomAuthority authority("Match", 8, 1);
	MemberId a = 0, b = 0;
	const auto reserved = HoldOnePermit(authority, a, b, 11000, 30000);
	const auto due = 1000 + 30000 - PermitStartMarginMs;
	// A permit with no window, or a window too long to be the bridge's, is refused.
	CHECK(!authority.Apply(b, Permit(authority, b, "per_one", 0)).accepted);
	CHECK(!authority.Apply(b, Permit(authority, b, "per_one", MaximumPermitWindowMs + 1)).accepted);
	CHECK(!authority.HasDueTimerTransition(due - 1));
	CHECK(authority.HasDueTimerTransition(due));
	{
		// The other permit just inside the window starts the game.
		RoomAuthority inTime("Match", 8, 1);
		CHECK(inTime.RestoreCheckpoint(authority.Checkpoint()));
		inTime.AdvanceTime(due - 1);
		const auto agreed = inTime.Apply(b, Permit(inTime, b, "per_one"));
		CHECK(agreed.accepted && HasEvent(agreed, Event::Kind::MatchReady));
		CHECK(inTime.BeginMatch(0, a, b).accepted && inTime.SnapshotView().tables[0].matchGeneration == reserved);
	}
	{
		// Arriving after it, even with a longer window of its own, it starts
		// nothing: the shorter window holds.
		RoomAuthority late("Match", 8, 1);
		CHECK(late.RestoreCheckpoint(authority.Checkpoint()));
		late.AgePermitHolds(due);
		const auto agreed = late.Apply(b, Permit(late, b, "per_one"));
		CHECK(agreed.accepted && !HasEvent(agreed, Event::Kind::MatchReady));
		CHECK(!late.BeginMatch(0, a, b).accepted);
		CHECK(late.HasDueTimerTransition(due));
	}
	authority.AdvanceTime(due);
	{
		const auto& table = authority.SnapshotView().tables[0];
		CHECK(table.phase == TablePhase::Waiting && !table.ready[0] && !table.ready[1]);
		CHECK(table.permitGeneration == 0 && table.permitWindows[0] == 0);
	}
	ReadyBoth(authority);
	CHECK(authority.SnapshotView().tables[0].permitGeneration > reserved);
}

// A permit held while the room's coordination is lost. Its window keeps
// running on a paused replica whether coordination is healthy or not, so a new
// owner after a long outage calls the start off and a short one still starts.
static void TestPermitWindowAcrossRecovery() {
	for (const std::uint64_t outage : {std::uint64_t(100000), std::uint64_t(20000)}) {
		RoomAuthority leader("Match", 8, 1);
		MemberId a = 0, b = 0;
		// The first permit 30 s into the reservation.
		const auto reserved = HoldOnePermit(leader, a, b, 31000);
		RoomAuthority owner("Other", 8, 1);
		CHECK(owner.RestoreCheckpoint(PausedCheckpoint(leader)));
		CHECK(owner.PermitAges().tables[0].generation == reserved && owner.PermitAges().tables[0].ageMs == 30000);
		// Healthy for 5 s: the room's own timers and the permit age once each.
		owner.AgePermitHolds(500);
		owner.AdvancePausedTimers(5000);
		owner.AgePermitHolds(5500);
		CHECK(owner.PermitAges().tables[0].ageMs == 35000);
		// Then the outage: no healthy time, but the permit's window runs on.
		owner.AgePermitHolds(5500 + outage);
		CHECK(owner.PermitAges().tables[0].ageMs == 35000 + outage);
		// The new owner's clock is its own, here lower than the old one's.
		owner.ResumeRecovery(7000);
		const bool expired = 35000 + outage + PermitStartMarginMs >= PermitStartMs;
		CHECK(expired == (outage == 100000));
		CHECK(owner.HasDueTimerTransition(7000) == expired);
		const auto agreed = owner.Apply(b, Permit(owner, b, "per_one"));
		CHECK(agreed.accepted && HasEvent(agreed, Event::Kind::MatchReady) == !expired);
		CHECK(owner.BeginMatch(0, a, b).accepted == !expired);
		if (!expired) {
			CHECK(owner.SnapshotView().tables[0].matchGeneration == reserved);
			continue;
		}
		owner.AdvanceTime(7000);
		const auto& table = owner.SnapshotView().tables[0];
		CHECK(table.phase == TablePhase::Waiting && table.permitGeneration == 0 && !table.ready[0] && !table.ready[1]);
		ReadyBoth(owner);
		CHECK(owner.SnapshotView().tables[0].permitGeneration > reserved);
	}
}

// The table's permit age as of its last count, as a checkpoint carries it.
static std::uint64_t PermitAge(const RoomAuthority& room) {
	const auto& timer = room.PermitAges().tables[0];
	return (std::max)(timer.ageMs, timer.heldMs);
}

// A replica restoring a commit keeps the age its own clock gave the same
// reservation: an older commit takes nothing back, a newer one is not counted
// twice, another reservation's age is never carried, and restoring again and
// again, aged between or not, loses nothing. A checkpoint carries the merged
// age as of the last count.
static void TestRestoreKeepsPermitAge() {
	RoomAuthority leader("Match", 8, 1);
	MemberId a = 0, b = 0;
	const auto reserved = HoldOnePermit(leader, a, b, 31000);
	// Committed 30 s into the reservation.
	const auto stale = PausedCheckpoint(leader);
	const auto restore = [](const nlohmann::json& checkpoint) {
		auto room = std::make_unique<RoomAuthority>("Other", 8, 1);
		CHECK(room->RestoreCheckpoint(checkpoint));
		return room;
	};
	const auto exported = [](const RoomAuthority& room) { return room.Checkpoint()["permit_age"][0].get<std::uint64_t>(); };
	// This replica's own count: the commit anchored at 1, 130 s at 100 s.
	auto replica = restore(stale);
	replica->AgePermitHolds(1);
	replica->AgePermitHolds(100001);
	const auto aged = replica->PermitAges();
	CHECK(aged.clockMs == 100001 && aged.tables[0].generation == reserved && aged.tables[0].ageMs == 130000);
	{
		// An older commit takes nothing back, before or after aging.
		auto restored = restore(stale);
		restored->KeepPermitAges(aged);
		CHECK(PermitAge(*restored) == 130000 && exported(*restored) == 130000);
		restored->AgePermitHolds(110001);
		CHECK(PermitAge(*restored) == 140000);
	}
	{
		// A newer commit's age already counts the time since this replica's
		// last count: 30 s kept at 100 s, 100 s committed, aged at 170 s is
		// still 100 s. Past that both counts agree.
		auto newer = restore(stale);
		newer->AgePermitHolds(1);
		newer->AgePermitHolds(70001);
		const auto fresh = newer->Checkpoint();
		auto kept = restore(stale);
		kept->AgePermitHolds(100000);
		auto merged = restore(fresh);
		merged->KeepPermitAges(kept->PermitAges());
		CHECK(PermitAge(*merged) == 100000 && exported(*merged) == 100000);
		merged->AgePermitHolds(170000);
		CHECK(PermitAge(*merged) == 100000);
		merged->AgePermitHolds(180000);
		CHECK(PermitAge(*merged) == 110000);
		// The same commit right after this replica's count, as a member
		// catching up receives them, counts on from its own first aging.
		auto caughtUp = restore(fresh);
		caughtUp->KeepPermitAges(kept->PermitAges());
		caughtUp->AgePermitHolds(100100);
		CHECK(PermitAge(*caughtUp) == 100000);
		caughtUp->AgePermitHolds(150000);
		CHECK(PermitAge(*caughtUp) == 149900);
		// Where this replica's own count is the older one it takes over once
		// it passes the commit's: 90 s kept at 60 s overtakes 100 s at 70 s.
		auto behind = restore(stale);
		behind->AgePermitHolds(1);
		behind->AgePermitHolds(60001);
		auto overtaken = restore(fresh);
		overtaken->KeepPermitAges(behind->PermitAges());
		overtaken->AgePermitHolds(110001);
		CHECK(PermitAge(*overtaken) == 140000);
	}
	{
		// A held age past the window stops the start before anything ages it:
		// 116 s committed, this replica's 30 s counted at 100 s.
		auto newer = restore(stale);
		newer->AgePermitHolds(1);
		newer->AgePermitHolds(86001);
		auto kept = restore(stale);
		kept->AgePermitHolds(100000);
		auto held = restore(newer->Checkpoint());
		held->KeepPermitAges(kept->PermitAges());
		const auto agreed = held->Apply(b, Permit(*held, b, "per_one"));
		CHECK(agreed.accepted && !HasEvent(agreed, Event::Kind::MatchReady));
		CHECK(!held->BeginMatch(0, a, b).accepted && held->HasDueTimerTransition(100000));
		held->AdvanceTime(100000);
		CHECK(held->SnapshotView().tables[0].permitGeneration == 0);
	}
	// A held age joins the count when aged at the count's own time, with
	// nothing added, and an earlier time leaves it there: 100 s committed over
	// this replica's 30 s at 100 s, aged or resumed at 100 s, is 110 s at
	// 110 s, and the missing permit's hold ends the start at 120 s.
	for (const bool resume : {false, true}) {
		auto newer = restore(stale);
		newer->AgePermitHolds(1);
		newer->AgePermitHolds(70001);
		auto kept = restore(stale);
		kept->AgePermitHolds(100000);
		auto sameTime = restore(newer->Checkpoint());
		sameTime->KeepPermitAges(kept->PermitAges());
		if (resume) sameTime->ResumeRecovery(100000);
		else sameTime->AgePermitHolds(100000);
		sameTime->AgePermitHolds(100000);
		sameTime->AgePermitHolds(90000);
		const auto& timer = sameTime->PermitAges().tables[0];
		CHECK(timer.ageMs == 100000 && timer.heldMs == 0 && timer.sampleMs == 100000);
		sameTime->AgePermitHolds(110000);
		CHECK(PermitAge(*sameTime) == 110000);
		sameTime->AdvanceTime(120000);
		CHECK(sameTime->SnapshotView().tables[0].permitGeneration == 0);
	}
	{
		// Another reservation's age is neither carried nor charged to this one.
		auto other = restore(stale);
		auto elsewhere = aged;
		elsewhere.tables[0].generation = reserved + 1;
		other->KeepPermitAges(elsewhere);
		other->AgePermitHolds(200001);
		CHECK(PermitAge(*other) == 30000);
	}
	// A retried recovery restores the same commit tick after tick, aging
	// between restores or not.
	for (const bool between : {true, false}) {
		auto current = restore(stale);
		current->AgePermitHolds(1);
		for (std::uint64_t nowMs = 20001; nowMs <= 100001; nowMs += 20000) {
			const auto before = current->PermitAges();
			current = restore(stale);
			current->KeepPermitAges(before);
			if (between) current->AgePermitHolds(nowMs);
		}
		CHECK(exported(*current) == (between ? 130000u : 30000u));
		current->AgePermitHolds(100001);
		CHECK(PermitAge(*current) == 130000 && exported(*current) == 130000);
		// The next owner restores the merged age.
		CHECK(PermitAge(*restore(current->Checkpoint())) == 130000);
	}
	// A room that pauses itself ages its permit on from where it stood, even
	// when nothing ages it before it resumes: reserved at 1 s, 30 s old at
	// 31 s, the window is past at 161 s.
	for (int way = 0; way < 3; ++way) {
		RoomAuthority live("Match", 8, 1);
		MemberId p = 0, q = 0;
		const auto held = HoldOnePermit(live, p, q, 31000);
		live.PauseForRecovery();
		if (way == 0) {
			live.AgePermitHolds(161000);
			CHECK(PermitAge(live) == 160000);
			live.ResumeRecovery(161000);
		} else if (way == 1) {
			live.ResumeRecovery(161000);
			CHECK(PermitAge(live) == 160000);
		}
		live.AdvanceTime(161001);
		CHECK(live.SnapshotView().tables[0].permitGeneration != held);
	}
	// Resuming without aging first, directly or through AdvanceTime, keeps
	// the merged age: 130 s at 100 s is 140 s at 110 s, past the window, so
	// the reservation is called off instead of starting late.
	for (const bool advance : {false, true}) {
		auto resumed = restore(stale);
		resumed->KeepPermitAges(aged);
		if (advance) resumed->AdvanceTime(110001);
		else {
			resumed->ResumeRecovery(110001);
			CHECK(PermitAge(*resumed) == 140000);
			resumed->AdvanceTime(110002);
		}
		CHECK(resumed->SnapshotView().tables[0].permitGeneration != reserved);
	}
}

// Permit timers run on the owner's clock as given. A room resumed on a clock
// below its restored ages keeps its own timers ahead of that clock for a
// while, yet a permit's age grows with every millisecond the owner sees, and
// a reservation made meanwhile counts from the owner's time as well. A local
// time of zero is a time like any other.
static void TestPermitClockIsTheOwners() {
	RoomAuthority leader("Match", 8, 1);
	leader.AdvanceTime(1000);
	const MemberId a = JoinAs(leader, "A", EndpointA, true);
	CHECK(leader.BindTournament(Binding()).accepted);
	const MemberId b = JoinAs(leader, "B", EndpointB);
	Action chat = TableAction(leader, a, 0, ActionKind::Chat);
	chat.text = "gl";
	CHECK(leader.Apply(a, chat).accepted);
	ReadyBoth(leader);
	const auto reserved = leader.SnapshotView().tables[0].permitGeneration;
	leader.AdvanceTime(31000);
	CHECK(leader.Apply(a, Permit(leader, a, "per_one")).accepted);
	// The chat's 30 s age puts the resumed room's own clock at 30 s.
	const auto checkpoint = PausedCheckpoint(leader);
	{
		RoomAuthority resumed("Other", 8, 1);
		CHECK(resumed.RestoreCheckpoint(checkpoint));
		resumed.ResumeRecovery(1000);
		resumed.AdvanceTime(20000);
		CHECK(PermitAge(resumed) == 49000);
		resumed.AdvanceTime(100000);
		CHECK(resumed.SnapshotView().tables[0].permitGeneration == 0);
	}
	{
		// A live checkpoint carries the permit clock, not the room's time
		// ahead of it: restored, the age counts on from the owner's 1 s, and a
		// permit given at 116 s starts nothing.
		RoomAuthority resumed("Other", 8, 1);
		CHECK(resumed.RestoreCheckpoint(checkpoint));
		resumed.ResumeRecovery(1000);
		RoomAuthority moved("Other", 8, 1);
		CHECK(moved.RestoreCheckpoint(resumed.Checkpoint()));
		moved.AdvanceTime(20000);
		CHECK(PermitAge(moved) == 49000);
		moved.AgePermitHolds(87000);
		const auto agreed = moved.Apply(b, Permit(moved, b, "per_one"));
		CHECK(agreed.accepted && !HasEvent(agreed, Event::Kind::MatchReady));
		CHECK(!moved.BeginMatch(0, a, b).accepted);
		moved.AdvanceTime(87000);
		CHECK(moved.SnapshotView().tables[0].permitGeneration == 0);
	}
	{
		// Paused again before that later tick, it still counts the time.
		RoomAuthority resumed("Other", 8, 1);
		CHECK(resumed.RestoreCheckpoint(checkpoint));
		resumed.ResumeRecovery(1000);
		resumed.PauseForRecovery();
		resumed.AgePermitHolds(100000);
		CHECK(PermitAge(resumed) == 129000);
	}
	{
		// A new reservation at the owner's 2 s is 18 s old at its 20 s.
		RoomAuthority resumed("Other", 8, 1);
		CHECK(resumed.RestoreCheckpoint(checkpoint));
		resumed.ResumeRecovery(1000);
		resumed.AdvanceTime(2000);
		CHECK(resumed.Apply(b, TableAction(resumed, b, 0, ActionKind::Unready)).accepted);
		const auto table = resumed.SnapshotView().tables[0];
		for (std::size_t seat = 0; seat < 2; ++seat) {
			const auto member = seat ? table.p2 : table.p1;
			if (!table.ready[seat]) CHECK(resumed.Apply(member, TableAction(resumed, member, 0, ActionKind::Ready)).accepted);
		}
		CHECK(resumed.SnapshotView().tables[0].permitGeneration > reserved);
		resumed.AdvanceTime(20000);
		CHECK(PermitAge(resumed) == 18000);
	}
	{
		// A restored age anchored at zero grows from zero.
		RoomAuthority restored("Other", 8, 1);
		CHECK(restored.RestoreCheckpoint(checkpoint));
		restored.AgePermitHolds(0);
		restored.AgePermitHolds(100000);
		CHECK(PermitAge(restored) == 130000);
	}
	{
		// So does a reservation made at zero and paused there.
		RoomAuthority zero("Match", 8, 1);
		JoinAs(zero, "A", EndpointA, true);
		CHECK(zero.BindTournament(Binding()).accepted);
		JoinAs(zero, "B", EndpointB);
		ReadyBoth(zero);
		CHECK(zero.SnapshotView().tables[0].permitGeneration != 0);
		zero.PauseForRecovery();
		zero.AgePermitHolds(100000);
		CHECK(PermitAge(zero) == 100000);
	}
}

// A begun bound game keeps its permit timer, with the shorter window, until
// it natively starts or ends, and a checkpoint carries both to the next
// owner. A replica that still held the permit when the commit beginning the
// game arrives keeps its own count and takes the window from the commit.
static void TestNativeStartWindow() {
	RoomAuthority authority("Match", 8, 1);
	MemberId a = 0, b = 0;
	const auto reserved = HoldOnePermit(authority, a, b, 31000, 90000);
	CHECK(authority.Apply(b, Permit(authority, b, "per_one")).accepted);
	const auto holding = authority.PermitAges();
	CHECK(holding.tables[0].windowMs == 0);
	CHECK(authority.BeginMatch(0, a, b).accepted);
	CHECK(!authority.NativeStartExpired(0, reserved));
	const auto checkpoint = PausedCheckpoint(authority);
	CHECK(checkpoint.contains("start_window") && checkpoint.at("start_window").at(0) == 90000 &&
		checkpoint.at("permit_age").at(0) == 30000);
	const auto age = [](RoomAuthority& owner, std::uint64_t nowMs) { owner.AgePermitHolds(nowMs); };
	for (const bool replica : {false, true}) {
		RoomAuthority owner("Other", 8, 1);
		CHECK(owner.RestoreCheckpoint(checkpoint));
		if (replica) owner.KeepPermitAges(holding);
		else age(owner, 31000);
		age(owner, 90999);
		CHECK(PermitAge(owner) == 89999 && !owner.NativeStartExpired(0, reserved));
		age(owner, 91000);
		CHECK(owner.NativeStartExpired(0, reserved) && !owner.NativeStartExpired(0, reserved + 1));
		// The native start retires the timer.
		owner.NativeStarted(0, reserved);
		CHECK(!owner.NativeStartExpired(0, reserved) && owner.PermitAges().tables[0].generation == 0);
		CHECK(!owner.Checkpoint().contains("start_window"));
	}
	// So does the game's end.
	CHECK(authority.EndMatch(0, reserved, MatchResult::Cancel).accepted);
	CHECK(authority.PermitAges().tables[0].generation == 0 && !authority.Checkpoint().contains("start_window"));
	// A window belongs to a begun game at the bound table only.
	RoomAuthority other("Other", 8, 1);
	auto forged = checkpoint;
	forged["start_window"][1] = std::uint64_t(1000);
	CHECK(!other.RestoreCheckpoint(forged));
	forged = checkpoint;
	forged["start_window"][0] = MaximumPermitWindowMs + 1;
	CHECK(!other.RestoreCheckpoint(forged));
}

// Casual rooms are unchanged: Ready starts the game with no permit.
static void TestCasualRoomsNeedNoPermit() {
	RoomAuthority authority("Casual", 8, 1);
	const MemberId host = Join(authority, 0, true);
	const MemberId other = Join(authority, 1);
	for (const auto member : {host, other})
		CHECK(authority.Apply(member, TableAction(authority, member, 0, ActionKind::Queue)).accepted);
	ReadyBoth(authority);
	CHECK(authority.SnapshotView().tables[0].permitGeneration == 0);
	CHECK(authority.BeginMatch(0, host, other).accepted);
}

int main() {
	TestFightersOnly();
	TestPermitGate();
	TestPermitHoldEnds();
	TestCheckpoint();
	TestPermitWindowGate();
	TestPermitWindowAcrossRecovery();
	TestRestoreKeepsPermitAge();
	TestPermitClockIsTheOwners();
	TestNativeStartWindow();
	TestCasualRoomsNeedNoPermit();
	if (failures) std::printf("%d failure(s)\n", failures);
	else std::printf("room tournament tests passed\n");
	return failures ? 1 : 0;
}
