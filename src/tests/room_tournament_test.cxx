// A room bound to a tournament match: fighters-only seating at table 0, the
// permit each game waits for, and the binding surviving a checkpoint.
#include "room_authority_support.hxx"

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

static Action Permit(const RoomAuthority& authority, MemberId member, const std::string& permit) {
	Action action = TableAction(authority, member, 0, ActionKind::PermitReady);
	action.matchGeneration = authority.SnapshotView().tables[0].permitGeneration;
	action.text = permit;
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
	CHECK(!JoinAs(restored, "C", EndpointC));
	// The wire form carries the binding to every member.
	Snapshot copy;
	nlohmann::json(authority.SnapshotView()).get_to(copy);
	CHECK(copy.tournament.matchId == Binding().matchId && copy.tournament.fighters[1].endpoint == EndpointB);
	// A permit hold without a binding is not a room state.
	auto forged = checkpoint;
	forged["snapshot"]["tournament"] = nlohmann::json::object();
	CHECK(!restored.RestoreCheckpoint(forged));
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
	TestCasualRoomsNeedNoPermit();
	if (failures) std::printf("%d failure(s)\n", failures);
	else std::printf("room tournament tests passed\n");
	return failures ? 1 : 0;
}
