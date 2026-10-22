#include "room_authority_support.hxx"

using nlohmann::json;

static MemberId Add(RoomAuthority& authority, int index, bool capable) {
	MemberProfile profile;
	profile.continuousCapable = capable;
	const auto joined = authority.Join("Player" + std::to_string(index), Peer(index), index == 0, profile);
	CHECK(joined.accepted);
	return joined.snapshot.members.back().id;
}
static void Act(RoomAuthority& authority, MemberId member, ActionKind kind) {
	CHECK(authority.Apply(member, TableAction(authority, member, 0, kind)).accepted);
}
static void Ready(RoomAuthority& authority, MemberId member, bool capable = true) {
	auto ready = TableAction(authority, member, 0, ActionKind::Ready);
	ready.continuous = capable;
	CHECK(authority.Apply(member, json(ready).get<Action>()).accepted);
}
static void OptIn(RoomAuthority& authority, MemberId host) {
	auto rules = TableAction(authority, host, 0, ActionKind::SetRules);
	rules.rules.continuousRematch = true;
	CHECK(authority.Apply(host, json(rules).get<Action>()).accepted);
}

static void RosterCapability() {
	// Noncapable fighters, explicit watchers and promoted queue watchers all
	// select the ordinary path. An idle member elsewhere does not affect it.
	for (int unavailable : {-1, 0, 1, 2, 3, 4}) {
		for (bool optIn : {false, true}) {
			RoomAuthority authority("capability");
			const auto a = Add(authority, 0, unavailable != 0);
			const auto b = Add(authority, 1, unavailable != 1);
			const auto watcher = Add(authority, 2, unavailable != 2);
			const auto queued = Add(authority, 3, unavailable != 3);
			Add(authority, 4, unavailable != 4);
			CHECK(!authority.SnapshotView().tables[0].rules.continuousRematch);
			if (optIn) OptIn(authority, a);
			Act(authority, a, ActionKind::Queue);
			Act(authority, b, ActionKind::Queue);
			Ready(authority, a);
			Ready(authority, b);
			// Joining the watch/queue after Ready must still take part in the
			// capability decision at the native grant boundary.
			Act(authority, watcher, ActionKind::Watch);
			Act(authority, queued, ActionKind::Queue);
			const auto saved = authority.Checkpoint();
			CHECK(authority.RestoreCheckpoint(json::parse(saved.dump())));
			CHECK(authority.BeginMatch(0, a, b).accepted);
			const auto& table = authority.SnapshotView().tables[0];
			const bool expected = optIn && (unavailable == -1 || unavailable == 4);
			CHECK(table.continuous.enabled == expected);
			const auto roster = authority.MatchRoster(0);
			CHECK(roster.size() == 4);
			CHECK(std::find(roster.begin(), roster.end(), watcher) != roster.end());
			CHECK(std::find(roster.begin(), roster.end(), queued) != roster.end());
			CHECK(json(table).get<Table>().continuous.enabled == expected);
			// A later watcher has no native grant and cannot switch a running
			// generation out of the negotiated mode.
			const auto late = Add(authority, 5, false);
			Act(authority, late, ActionKind::Watch);
			CHECK(authority.SnapshotView().tables[0].continuous.enabled == expected);
			CHECK(authority.MatchRoster(0) == roster);
		}
	}
	RoomAuthority unavailableAtReady("ready");
	const auto a = Add(unavailableAtReady, 0, true), b = Add(unavailableAtReady, 1, true);
	OptIn(unavailableAtReady, a);
	Act(unavailableAtReady, a, ActionKind::Queue); Act(unavailableAtReady, b, ActionKind::Queue);
	Ready(unavailableAtReady, a); Ready(unavailableAtReady, b, false);
	CHECK(unavailableAtReady.BeginMatch(0, a, b).accepted);
	CHECK(!unavailableAtReady.SnapshotView().tables[0].continuous.enabled);
}

static void WireAndRoomKinds() {
	CHECK(LastActionKind == ActionKind::FinishContinuousMatch);
	for (auto kind : {ActionKind::RecordContinuousResult, ActionKind::FinishContinuousMatch}) {
		Action action; action.kind = kind; action.continuousGame = 9;
		CHECK(json(action).get<Action>().kind == kind);
	}
	Rules rules;
	CHECK(!rules.continuousRematch && !json(rules).contains("continuous_rematch"));
	CHECK(!json(rules).get<Rules>().continuousRematch);
	Snapshot snapshot;
	CHECK(ContinuousAllowed(snapshot));
	snapshot.practice = true; CHECK(!ContinuousAllowed(snapshot));
	snapshot.practice = false; snapshot.serverOwned = true; CHECK(!ContinuousAllowed(snapshot));
	snapshot.serverOwned = false; snapshot.tournament.matchId = "bound"; CHECK(!ContinuousAllowed(snapshot));
	RoomAuthority practice("practice", 8, 2, Rules{}, true);
	const auto host = Add(practice, 0, true);
	auto edit = TableAction(practice, host, 0, ActionKind::SetRules);
	edit.rules.continuousRematch = true;
	CHECK(practice.Apply(host, edit).reason == RejectReason::InvalidRules);
	RoomAuthority ordinary("ordinary");
	const auto first = Add(ordinary, 0, true), other = Add(ordinary, 1, true);
	edit = TableAction(ordinary, other, 0, ActionKind::SetRules);
	edit.rules.continuousRematch = true;
	CHECK(ordinary.Apply(other, edit).reason == RejectReason::NotHost);
	const auto member = json(*FindMember(ordinary.SnapshotView(), first));
	CHECK(member.get<Member>().continuousCapable);
	auto legacy = member; legacy.erase("continuous_capable");
	CHECK(!legacy.get<Member>().continuousCapable);
}

static void RetiringWatcherDoesNotHoldNextGame() {
	RoomAuthority authority("retiring");
	const auto a = Add(authority, 0, true), b = Add(authority, 1, true), watcher = Add(authority, 2, false);
	OptIn(authority, a);
	Act(authority, a, ActionKind::Queue); Act(authority, b, ActionKind::Queue);
	Act(authority, watcher, ActionKind::Watch);
	Ready(authority, a); Ready(authority, b);
	CHECK(authority.BeginMatch(0, a, b).accepted);
	CHECK(!authority.SnapshotView().tables[0].continuous.enabled);
	const auto generation = authority.SnapshotView().tables[0].matchGeneration;
	CHECK(authority.EndMatch(0, generation, MatchResult::Draw).accepted);
	for (const auto member : {a, b}) {
		auto ack = TableAction(authority, member, 0, ActionKind::AcknowledgeTerminal);
		ack.matchGeneration = generation;
		CHECK(authority.Apply(member, ack).accepted);
	}
	// This watcher is still retiring the old transport and receives no new
	// grant. Its capability cannot disable the fighters' next generation.
	Ready(authority, a); Ready(authority, b);
	CHECK(authority.BeginMatch(0, a, b).accepted);
	CHECK(authority.SnapshotView().tables[0].continuous.enabled);
	CHECK(authority.MatchRoster(0) == std::vector<MemberId>({a, b}));
}

static void OrdinaryQuorumStillWaitsForReports() {
	for (bool disputed : {false, true}) {
		RoomAuthority authority("ordinary");
		const auto a = Add(authority, 0, true), b = Add(authority, 1, true);
		Act(authority, a, ActionKind::Queue); Act(authority, b, ActionKind::Queue);
		Ready(authority, a); Ready(authority, b);
		CHECK(authority.BeginMatch(0, a, b).accepted);
		const auto generation = authority.SnapshotView().tables[0].matchGeneration;
		auto finish = TableAction(authority, a, 0, ActionKind::MatchFinished);
		finish.matchGeneration = generation;
		CHECK(authority.Apply(a, finish).accepted);
		CHECK(authority.SnapshotView().tables[0].resultPending);
		auto report = TableAction(authority, a, 0, ActionKind::RecordResult);
		report.matchGeneration = generation; report.result = MatchResult::P2Win;
		CHECK(authority.Apply(a, report).accepted);
		report = TableAction(authority, a, 0, ActionKind::RecordResult);
		report.matchGeneration = generation; report.result = MatchResult::P2Win;
		CHECK(authority.Apply(a, report).reason == RejectReason::DuplicateResult);
		CHECK(authority.SnapshotView().tables[0].score[1] == 0);
		auto second = TableAction(authority, b, 0, ActionKind::RecordResult);
		second.matchGeneration = generation; second.result = disputed ? MatchResult::P1Win : MatchResult::P2Win;
		const auto result = authority.Apply(b, second);
		CHECK(result.accepted);
		CHECK(authority.SnapshotView().tables[0].phase == (disputed ? TablePhase::Paused : TablePhase::Waiting));
		CHECK(authority.SnapshotView().tables[0].score[1] == (disputed ? 0u : 1u));
	}
}

int main() {
	RosterCapability();
	WireAndRoomKinds();
	RetiringWatcherDoesNotHoldNextGame();
	OrdinaryQuorumStillWaitsForReports();
	std::printf("RoomContinuousCapability: %d failures\n", failures);
	return failures ? 1 : 0;
}
