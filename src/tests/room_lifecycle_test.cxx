#include "room_authority_support.hxx"

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

static void TestQueueTakesAskedSeatAndEndsWatching() {
	RoomAuthority authority("Seats", 8, 5);
	Join(authority, 0, true);
	const auto first = Join(authority, 1), second = Join(authority, 2), watcher = Join(authority, 3);
	const auto queue = [&](MemberId member, std::uint8_t table, int seat) {
		auto action = TableAction(authority, member, table, ActionKind::Queue);
		action.seat = static_cast<std::int8_t>(seat);
		return authority.Apply(member, action).accepted;
	};
	CHECK(queue(first, 0, 1));
	CHECK(authority.SnapshotView().tables[0].p2 == first && authority.SnapshotView().tables[0].p1 == 0);
	// The asked-for seat is taken: refused, never swapped for the other one.
	CHECK(!queue(second, 0, 1));
	CHECK(authority.SnapshotView().tables[0].p1 == 0 && authority.SnapshotView().tables[0].queue.empty());
	CHECK(queue(second, 0, 0));
	CHECK(authority.SnapshotView().tables[0].p1 == second);
	// A watcher sits down straight from watching, and stops watching.
	CHECK(authority.Apply(watcher, TableAction(authority, watcher, 0, ActionKind::Watch)).accepted);
	CHECK(queue(watcher, 1, 0));
	const auto& tables = authority.SnapshotView().tables;
	CHECK(tables[1].p1 == watcher && tables[0].spectators.empty() && tables[0].watchingNext.empty());
}

// A host, fighters a and b at table 0 with the next player q queued behind them,
// and w watching. A game is under way: q is listed as a spectator of it, and w
// asked to watch the next one.
struct LiveTable {
	RoomAuthority authority{"Live table", 8, 5};
	MemberId host = 0, a = 0, b = 0, q = 0, w = 0;
	std::uint64_t generation = 0;
	LiveTable() {
		host = Join(authority, 0, true); a = Join(authority, 1); b = Join(authority, 2);
		q = Join(authority, 3); w = Join(authority, 4);
		for (const auto member : {a, b, q}) CHECK(Do(member, ActionKind::Queue).accepted);
		for (const auto member : {a, b}) CHECK(Do(member, ActionKind::Ready).accepted);
		CHECK(authority.BeginMatch(0, a, b).accepted);
		generation = table().matchGeneration;
		CHECK(Do(w, ActionKind::Watch).accepted);
	}
	Result Do(MemberId member, ActionKind kind) { return authority.Apply(member, TableAction(authority, member, 0, kind)); }
	const Table& table() const { return authority.SnapshotView().tables[0]; }
	const Member& member(MemberId id) const {
		const auto& members = authority.SnapshotView().members;
		return *std::find_if(members.begin(), members.end(), [id](const Member& value) { return value.id == id; });
	}
	bool Has(const std::vector<MemberId>& list, MemberId id) const { return std::find(list.begin(), list.end(), id) != list.end(); }
	void Acknowledge(MemberId id, std::uint64_t forGeneration) {
		Action ack = TableAction(authority, id, 0, ActionKind::AcknowledgeTerminal);
		ack.matchGeneration = forGeneration;
		CHECK(authority.Apply(id, ack).accepted);
	}
};

// A fighter who leaves, or is kicked, mid-game ends that game for everyone: the
// watcher who asked for the next game joins it, the survivor's delay unlocks
// and the old spectators stop reading as watching.
static void TestFighterDepartureClosesTheGame() {
	for (int how = 0; how < 3; ++how) {
		LiveTable live;
		CHECK(live.table().watchingNext.size() == 1 && live.member(live.w).status == MemberStatus::WatchingNext);
		CHECK(live.member(live.b).delayLocked);
		if (how == 2) {
			// A disputed result pauses the table; leaving it closes the game too.
			const std::pair<MemberId, MatchResult> reports[] = {{live.a, MatchResult::P1Win}, {live.b, MatchResult::P2Win}};
			for (const auto& entry : reports) {
				Action report = TableAction(live.authority, entry.first, 0, ActionKind::RecordResult);
				report.matchGeneration = live.generation; report.result = entry.second;
				CHECK(live.authority.Apply(entry.first, report).accepted);
			}
			CHECK(live.table().phase == TablePhase::Paused);
		}
		Result departure;
		if (how == 1) {
			Action kick = TableAction(live.authority, live.host, 0, ActionKind::Kick);
			kick.target = live.a;
			departure = live.authority.Apply(live.host, kick);
		} else departure = live.authority.Leave(live.a);
		CHECK(departure.accepted);
		CHECK(std::any_of(departure.events.begin(), departure.events.end(), [](const Event& event) { return event.kind == Event::Kind::MatchEnded; }));
		const auto& table = live.table();
		CHECK(table.phase == TablePhase::Waiting && table.p1 == live.q && table.p2 == live.b);
		CHECK(table.watchingNext.empty() && live.Has(table.spectators, live.w));
		CHECK(live.authority.MatchRoster(0).empty());
		CHECK(live.member(live.b).status == MemberStatus::Seated && !live.member(live.b).delayLocked);
		CHECK(live.member(live.w).status == MemberStatus::WatchingNext);
		// The watcher is granted the game that replaces it.
		live.Acknowledge(live.b, live.generation); live.Acknowledge(live.q, live.generation);
		for (const auto member : {live.q, live.b}) CHECK(live.Do(member, ActionKind::Ready).accepted);
		CHECK(live.authority.BeginMatch(0, live.q, live.b).accepted);
		CHECK(live.Has(live.authority.MatchRoster(0), live.w) && live.member(live.w).status == MemberStatus::Watching);
	}
}

// The delay lock goes with the ready flag on every path that clears it.
static void TestReadinessClearsTheDelayLock() {
	RoomAuthority authority("Delay lock", 8, 6);
	const MemberId host = Join(authority, 0, true), p1 = Join(authority, 1), p2 = Join(authority, 2);
	const auto locked = [&](MemberId id) {
		for (const auto& member : authority.SnapshotView().members) if (member.id == id) return member.delayLocked;
		return false;
	};
	const auto Do = [&](MemberId member, ActionKind kind) { return authority.Apply(member, TableAction(authority, member, 0, kind)); };
	for (const auto member : {p1, p2}) CHECK(Do(member, ActionKind::Queue).accepted);
	// The opponent stands up.
	CHECK(Do(p1, ActionKind::Ready).accepted && locked(p1));
	CHECK(Do(p2, ActionKind::Unqueue).accepted);
	CHECK(!locked(p1) && !locked(p2));
	// The host changes the rules.
	CHECK(Do(p2, ActionKind::Queue).accepted);
	CHECK(Do(p1, ActionKind::Ready).accepted && locked(p1));
	Action rules = TableAction(authority, host, 0, ActionKind::SetRules);
	rules.rules = authority.SnapshotView().tables[0].rules;
	rules.rules.roundCount = rules.rules.roundCount == 3 ? 5 : 3;
	CHECK(authority.Apply(host, rules).accepted);
	CHECK(!locked(p1) && !authority.SnapshotView().tables[0].ready[0]);
	// The opponent leaves after both readied. That ends no game.
	for (const auto member : {p1, p2}) CHECK(Do(member, ActionKind::Ready).accepted);
	CHECK(authority.SnapshotView().tables[0].phase == TablePhase::Ready);
	const auto leaving = authority.Leave(p2);
	CHECK(leaving.accepted);
	CHECK(std::none_of(leaving.events.begin(), leaving.events.end(), [](const Event& event) { return event.kind == Event::Kind::MatchEnded; }));
	CHECK(!locked(p1));
	for (const auto& member : authority.SnapshotView().members) if (member.id == p1) CHECK(member.status == MemberStatus::Seated);
}

// A queued member watches the game it waits out without having chosen to. Once
// it leaves the queue that place ends with the game it is watching, or at once
// when that game is over.
static void TestLeavingQueueEndsTheQueuedWatch() {
	{
		LiveTable live;
		CHECK(live.Has(live.table().spectators, live.q) && live.member(live.q).status == MemberStatus::Queued);
		CHECK(live.authority.EndMatch(0, live.generation, MatchResult::P1Win).accepted);
		for (const auto member : {live.a, live.b, live.q}) live.Acknowledge(member, live.generation);
		// Between games the grant is spent: leaving the queue leaves the table.
		CHECK(live.Do(live.q, ActionKind::Unqueue).accepted);
		const auto& table = live.table();
		CHECK(!live.Has(table.queue, live.q) && !live.Has(table.spectators, live.q) && !live.Has(table.watchingNext, live.q));
		CHECK(live.member(live.q).status == MemberStatus::Idle && live.member(live.q).table == -1);
		CHECK(!WatchesByChoice(table, live.q));
		for (const auto member : {live.a, live.b}) CHECK(live.Do(member, ActionKind::Ready).accepted);
		CHECK(live.authority.BeginMatch(0, live.a, live.b).accepted);
		// (w asked to watch the next game, and does.)
		CHECK((live.authority.MatchRoster(0) == std::vector<MemberId>{live.a, live.b, live.w}));
	}
	{
		LiveTable live;
		// During the game it keeps the stream it is watching, but not by choice.
		CHECK(live.Do(live.q, ActionKind::Unqueue).accepted);
		CHECK(live.Has(live.table().spectators, live.q) && live.Has(live.table().endingWatchers, live.q));
		CHECK(live.Has(live.authority.MatchRoster(0), live.q) && live.member(live.q).status == MemberStatus::Watching);
		CHECK(!WatchesByChoice(live.table(), live.q));
		// The marker is part of the room state a follower restores.
		RoomAuthority replica("Replica");
		CHECK(replica.RestoreCheckpoint(nlohmann::json::parse(live.authority.Checkpoint().dump())));
		CHECK(replica.Checkpoint() == live.authority.Checkpoint());
		CHECK(live.Has(replica.SnapshotView().tables[0].endingWatchers, live.q));
		auto broken = live.authority.Checkpoint();
		broken["snapshot"]["tables"][0]["spectators"] = nlohmann::json::array();
		RoomAuthority rejecting("Rejecting");
		CHECK(!rejecting.RestoreCheckpoint(broken));
		// The game ends and the place goes with it.
		CHECK(live.authority.EndMatch(0, live.generation, MatchResult::P2Win).accepted);
		const auto& table = live.table();
		CHECK(!live.Has(table.spectators, live.q) && table.endingWatchers.empty());
		CHECK(live.member(live.q).status == MemberStatus::Idle);
		for (const auto member : {live.a, live.b, live.q}) live.Acknowledge(member, live.generation);
		for (const auto member : {live.a, live.b}) CHECK(live.Do(member, ActionKind::Ready).accepted);
		CHECK(live.authority.BeginMatch(0, live.a, live.b).accepted);
		CHECK(!live.Has(live.authority.MatchRoster(0), live.q));
	}
	{
		LiveTable live;
		// Watching on purpose makes the place its own.
		CHECK(live.Do(live.q, ActionKind::Unqueue).accepted);
		CHECK(live.Do(live.q, ActionKind::Watch).accepted);
		CHECK(live.table().endingWatchers.empty() && WatchesByChoice(live.table(), live.q));
		CHECK(live.authority.EndMatch(0, live.generation, MatchResult::P1Win).accepted);
		CHECK(live.Has(live.table().spectators, live.q) && live.member(live.q).status == MemberStatus::WatchingNext);
	}
	{
		// A fighter leaving mid-game skips EndMatch; the place still ends.
		LiveTable live;
		const MemberId r = Join(live.authority, 5);
		CHECK(live.Do(r, ActionKind::Queue).accepted);
		CHECK(live.authority.Leave(live.b).accepted);
		CHECK(live.table().p2 == live.q && live.Has(live.table().queue, r));
		CHECK(live.Do(r, ActionKind::Unqueue).accepted);
		CHECK(!live.Has(live.table().spectators, r) && live.member(r).status == MemberStatus::Idle);
	}
	{
		// A leaving watcher takes its marker along.
		LiveTable live;
		CHECK(live.Do(live.q, ActionKind::Unqueue).accepted);
		CHECK(live.authority.Leave(live.q).accepted);
		CHECK(live.table().endingWatchers.empty());
	}
}

// A spectator's HUD keeps the score the watched game started with, however far
// the room has moved on; a fighter's follows the table.
static void TestHudScoreFollowsTheWatchedGame() {
	LiveTable live;
	CHECK(live.authority.EndMatch(0, live.generation, MatchResult::P1Win).accepted);
	for (const auto member : {live.a, live.b, live.q}) live.Acknowledge(member, live.generation);
	for (const auto member : {live.a, live.b}) CHECK(live.Do(member, ActionKind::Ready).accepted);
	CHECK(live.authority.BeginMatch(0, live.a, live.b).accepted);
	const std::uint32_t start[2] = { live.table().score[0], live.table().score[1] };
	CHECK(start[0] == 1 && start[1] == 0);
	std::uint32_t shown[2] = { 9, 9 };
	const auto fromTable = [&](bool spectating) {
		const std::uint32_t current[2] = { live.table().score[0], live.table().score[1] };
		return sf4e::HudScore(spectating, true, start, true, current, shown);
	};
	CHECK(fromTable(true) && shown[0] == 1 && shown[1] == 0);
	// The room counts the game the spectator is still watching.
	CHECK(live.authority.EndMatch(0, live.table().matchGeneration, MatchResult::P1Win).accepted);
	CHECK(fromTable(true) && shown[0] == 1 && shown[1] == 0);
	CHECK(fromTable(false) && shown[0] == 2 && shown[1] == 0);
	// The loser leaves and the table's score resets.
	CHECK(live.authority.Leave(live.b).accepted);
	CHECK(live.table().score[0] == 0 && live.table().score[1] == 0);
	CHECK(fromTable(true) && shown[0] == 1 && shown[1] == 0);
	CHECK(fromTable(false) && shown[0] == 0 && shown[1] == 0);
	// Nothing known, nothing shown.
	const std::uint32_t none[2] = { 0, 0 };
	CHECK(!sf4e::HudScore(true, false, none, true, start, shown));
	CHECK(!sf4e::HudScore(false, true, start, false, none, shown));
}

int main() {
	TestQueueTakesAskedSeatAndEndsWatching();
	TestQueueWatchAndReplay();
	TestFighterDepartureClosesTheGame();
	TestReadinessClearsTheDelayLock();
	TestLeavingQueueEndsTheQueuedWatch();
	TestHudScoreFollowsTheWatchedGame();

	if (failures) return 1;
	std::puts("RoomLifecycle test passed");
	return 0;
}
