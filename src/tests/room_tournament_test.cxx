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
		late.PauseForRecovery();
		late.AgePermitHolds(1);
		late.AgePermitHolds(1 + due - 11000);
		late.ResumeRecovery(due);
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
		CHECK(owner.PermitAges().tables[0].first == reserved && owner.PermitAges().tables[0].second == 30000);
		// Healthy for 5 s: the room's own timers and the permit age once each.
		owner.AgePermitHolds(500);
		owner.AdvancePausedTimers(5000);
		owner.AgePermitHolds(5500);
		CHECK(owner.PermitAges().tables[0].second == 35000);
		// Then the outage: no healthy time, but the permit's window runs on.
		owner.AgePermitHolds(5500 + outage);
		CHECK(owner.PermitAges().tables[0].second == 35000 + outage);
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

// A replica restoring a commit made before an outage keeps the age its own
// clock gave the same reservation, brought to the time it next ages; a newer
// commit's age is not counted twice, and another reservation's is not carried.
static void TestRestoreKeepsPermitAge() {
	RoomAuthority leader("Match", 8, 1);
	MemberId a = 0, b = 0;
	const auto reserved = HoldOnePermit(leader, a, b, 31000);
	const auto stale = PausedCheckpoint(leader);
	RoomAuthority replica("Other", 8, 1);
	CHECK(replica.RestoreCheckpoint(stale));
	replica.AgePermitHolds(1);
	replica.AgePermitHolds(100001);
	const auto aged = replica.PermitAges();
	CHECK(aged.clockMs == 100001 && aged.tables[0].first == reserved && aged.tables[0].second == 130000);
	RoomAuthority restored("Other", 8, 1);
	CHECK(restored.RestoreCheckpoint(stale));
	restored.KeepPermitAges(aged);
	CHECK(restored.PermitAges().tables[0].second == 130000);
	restored.AgePermitHolds(110001);
	CHECK(restored.PermitAges().tables[0].second == 140000);
	// A newer commit's age already covers the time since the kept one was
	// taken: 30 s kept at 100 s, 100 s committed, aged at 170 s is 100 s.
	RoomAuthority newer("Other", 8, 1);
	CHECK(newer.RestoreCheckpoint(stale));
	newer.AgePermitHolds(1);
	newer.AgePermitHolds(70001);
	const auto fresh = newer.Checkpoint();
	RoomAuthority kept("Other", 8, 1);
	CHECK(kept.RestoreCheckpoint(stale));
	kept.AgePermitHolds(100000);
	RoomAuthority merged("Other", 8, 1);
	CHECK(merged.RestoreCheckpoint(fresh));
	merged.KeepPermitAges(kept.PermitAges());
	merged.AgePermitHolds(170000);
	CHECK(merged.PermitAges().tables[0].second == 100000);
	// Another reservation's age is neither carried nor charged to this one.
	RoomAuthority other("Other", 8, 1);
	CHECK(other.RestoreCheckpoint(stale));
	auto elsewhere = aged;
	elsewhere.tables[0].first = reserved + 1;
	other.KeepPermitAges(elsewhere);
	other.AgePermitHolds(200001);
	CHECK(other.PermitAges().tables[0].second == 30000);
	// A retried recovery restores the same commit tick after tick. Each
	// restore ages on from the last instead of starting the clock again.
	auto current = std::make_unique<RoomAuthority>("Other", 8, 1);
	CHECK(current->RestoreCheckpoint(stale));
	current->AgePermitHolds(1);
	for (std::uint64_t nowMs = 20001; nowMs <= 100001; nowMs += 20000) {
		const auto before = current->PermitAges();
		current = std::make_unique<RoomAuthority>("Other", 8, 1);
		CHECK(current->RestoreCheckpoint(stale));
		current->KeepPermitAges(before);
		current->AgePermitHolds(nowMs);
	}
	CHECK(current->PermitAges().tables[0].second == 130000);
	// Resuming without aging first, directly or through AdvanceTime, takes
	// the kept age along: 130 s at 100 s is 140 s at 110 s, past the window,
	// so the reservation is called off instead of starting late.
	for (const bool advance : {false, true}) {
		RoomAuthority resumed("Other", 8, 1);
		CHECK(resumed.RestoreCheckpoint(stale));
		resumed.KeepPermitAges(aged);
		if (advance) resumed.AdvanceTime(110001);
		else {
			resumed.ResumeRecovery(110001);
			CHECK(resumed.PermitAges().tables[0].second == 140000);
			resumed.AdvanceTime(110002);
		}
		CHECK(resumed.SnapshotView().tables[0].permitGeneration != reserved);
	}
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
	TestCasualRoomsNeedNoPermit();
	if (failures) std::printf("%d failure(s)\n", failures);
	else std::printf("room tournament tests passed\n");
	return failures ? 1 : 0;
}
