// A room's Max ping rule (ROOM_PING_RULE.md): chosen when the room is created,
// changed by its host (SetMaxPing), it keeps a member whose measured ping to
// someone the seat could pair them with is over the limit from that seat,
// queue place or cabinet. A ping that was not measured never counts, nobody
// seated or queued is moved, and it travels on the wire only when on.
#include "room_authority_support.hxx"

#include <exception>

namespace {
Charter Limited(std::uint16_t maxPingMs, RoomType type = RoomType::Classic) {
	Charter charter;
	charter.type = type;
	charter.tableCount = DefaultTableCount(type);
	charter.maxPingMs = maxPingMs;
	return charter;
}

// TableAction with an action id of its own: a refused action leaves the
// revision where it was, and the same id again would read as its repeat.
Action Act(const RoomAuthority& authority, MemberId member, std::uint8_t table, ActionKind kind) {
	static ActionId next = 1;
	auto action = TableAction(authority, member, table, kind);
	action.actionId = next++;
	return action;
}

// A Queue at `table` (and `seat`, -1 for the first open one or the queue)
// carrying `figures`, as the board sends it.
Action Sit(const RoomAuthority& authority, MemberId member, std::uint8_t table, int seat, std::vector<PingFigure> figures = {}) {
	auto action = Act(authority, member, table, ActionKind::Queue);
	action.seat = static_cast<std::int8_t>(seat);
	action.pings = std::move(figures);
	return action;
}

Result SetMaxPing(RoomAuthority& authority, MemberId member, std::uint16_t ms) {
	auto action = Act(authority, member, 0, ActionKind::SetMaxPing);
	action.maxPingMs = ms;
	return authority.Apply(member, action);
}

template <typename T> bool Throws(const nlohmann::json& json) {
	try { (void)json.get<T>(); } catch (const std::exception&) { return true; }
	return false;
}
}

static void TestPureRule() {
	// The choices: Off and five limits; anything else is no choice.
	for (const int ms : {0, 80, 100, 120, 150, 200}) EXPECT(MaxPingValid(ms));
	for (const int ms : {-1, 1, 90, 250, 999}) EXPECT(!MaxPingValid(ms));
	// Stepping holds at either end, or goes round from the last to Off.
	EXPECT(StepMaxPing(0, 1, false) == 80 && StepMaxPing(0, -1, false) == 0 && StepMaxPing(200, 1, false) == 200);
	EXPECT(StepMaxPing(200, 1, true) == 0 && StepMaxPing(0, -1, true) == 200 && StepMaxPing(100, 1, true) == 120);
	EXPECT(StepMaxPing(90, 1, false) == 80);

	// Classic: the table's fighters and its queue, never the member itself.
	Snapshot classic;
	classic.tables[0].p1 = 1; classic.tables[0].p2 = 2; classic.tables[0].queue = {3, 4};
	EXPECT((PingOpponents(classic, 9, 0) == std::vector<MemberId>{1, 2, 3, 4}));
	EXPECT((PingOpponents(classic, 3, 0) == std::vector<MemberId>{1, 2, 4}));
	EXPECT(PingOpponents(classic, 9, 1).empty());
	// A table out of play has nobody.
	EXPECT(PingOpponents(classic, 9, TableCount).empty());
	// Cabinets: everyone at one, whichever cabinet is named; one already at a
	// cabinet who moves is compared with nobody.
	Snapshot arcade;
	arcade.type = RoomType::Arcade; arcade.tableCount = static_cast<std::uint8_t>(TableCount);
	arcade.cabinets[0] = 1; arcade.cabinets[5] = 2;
	EXPECT((PingOpponents(arcade, 9, 3) == std::vector<MemberId>{1, 2}));
	EXPECT(PingOpponents(arcade, 1, 3).empty());

	// The verdict: off says nothing; a figure over the limit names its member;
	// one at the limit is within it; a member with no figure never counts.
	const std::vector<MemberId> opponents{1, 2};
	const std::vector<PingFigure> figures{{1, 100}, {2, 140}};
	EXPECT(!PingOverLimit(classic, opponents, figures).Over());
	classic.maxPingMs = 100;
	const auto over = PingOverLimit(classic, opponents, figures);
	EXPECT(over.Over() && over.opponent == 2 && over.ms == 140);
	classic.maxPingMs = 150;
	EXPECT(!PingOverLimit(classic, opponents, figures).Over());
	classic.maxPingMs = 80;
	EXPECT(!PingOverLimit(classic, opponents, {}).Over());
	// A figure for someone the seat does not pair them with is not asked.
	EXPECT(!PingOverLimit(classic, {3}, figures).Over());
}

static void TestClassicSeats() {
	{
		// Off by default: a figure over any limit sits as anyone does.
		RoomAuthority room("Off", 16, 61);
		const auto host = Join(room, 0, true), a = Join(room, 1), b = Join(room, 2);
		EXPECT(room.SnapshotView().maxPingMs == 0);
		EXPECT(room.Apply(a, Sit(room, a, 0, 0)).accepted);
		EXPECT(room.Apply(b, Sit(room, b, 0, 1, {{a, 900}})).accepted && room.SnapshotView().tables[0].p2 == b);
		(void)host;
	}
	{
		// Created with 100 ms: the first player sits at the empty table with
		// nothing to measure; the second is checked against them.
		RoomAuthority room("Limited", 16, 62, Rules(), Limited(100));
		const auto host = Join(room, 0, true), a = Join(room, 1), far = Join(room, 2), near = Join(room, 3), unmeasured = Join(room, 4);
		EXPECT(room.SnapshotView().maxPingMs == 100);
		EXPECT(room.Apply(a, Sit(room, a, 0, -1)).accepted && room.SnapshotView().tables[0].p1 == a);
		const auto revision = room.SnapshotView().revision;
		EXPECT(room.Apply(far, Sit(room, far, 0, 1, {{a, 140}})).reason == RejectReason::PingOverLimit);
		EXPECT(room.Apply(far, Sit(room, far, 0, -1, {{a, 140}})).reason == RejectReason::PingOverLimit);
		// The refusal changes nothing.
		EXPECT(room.SnapshotView().revision == revision && StatusOf(room, far) == MemberStatus::Idle && room.SnapshotView().tables[0].p2 == 0);
		// They still watch and chat.
		EXPECT(room.Apply(far, Act(room, far, 0, ActionKind::Watch)).accepted && StatusOf(room, far) == MemberStatus::WatchingNext);
		auto chat = Act(room, far, 0, ActionKind::Chat);
		chat.text = "gg";
		EXPECT(room.Apply(far, chat).accepted);
		// Another table with nobody at it seats them.
		EXPECT(room.Apply(far, Sit(room, far, 1, 0, {{a, 140}})).accepted && room.SnapshotView().tables[1].p1 == far);
		// At the limit is within it.
		EXPECT(room.Apply(near, Sit(room, near, 0, 1, {{a, 100}})).accepted && room.SnapshotView().tables[0].p2 == near);
		// Behind a full table the queue counts as much as the seats; a figure
		// for someone else in the room is not asked.
		EXPECT(room.Apply(unmeasured, Sit(room, unmeasured, 0, -1, {{a, 40}, {far, 400}})).accepted);
		EXPECT(StatusOf(room, unmeasured) == MemberStatus::Queued);
		const auto late = Join(room, 5);
		EXPECT(room.Apply(late, Sit(room, late, 0, -1, {{a, 40}, {near, 60}, {unmeasured, 180}})).reason == RejectReason::PingOverLimit);
		// A ping that was not measured, to a player in a game for one, never counts.
		EXPECT(room.Apply(late, Sit(room, late, 0, -1, {{a, 40}})).accepted && StatusOf(room, late) == MemberStatus::Queued);
		(void)host;
	}
}

static void TestHostSetsTheRule() {
	RoomAuthority room("Host", 16, 63);
	const auto host = Join(room, 0, true), other = Join(room, 1);
	EXPECT(SetMaxPing(room, other, 100).reason == RejectReason::NotHost && room.SnapshotView().maxPingMs == 0);
	EXPECT(SetMaxPing(room, host, 90).reason == RejectReason::InvalidRules && room.SnapshotView().maxPingMs == 0);
	auto stale = Act(room, host, 0, ActionKind::SetMaxPing);
	stale.maxPingMs = 100; stale.revision -= 1;
	EXPECT(room.Apply(host, stale).reason == RejectReason::StaleRoom && room.SnapshotView().maxPingMs == 0);
	const auto before = room.SnapshotView().revision;
	EXPECT(SetMaxPing(room, host, 120).accepted && room.SnapshotView().maxPingMs == 120 && room.SnapshotView().revision > before);
	// The same value again changes nothing.
	const auto on = room.SnapshotView().revision;
	EXPECT(SetMaxPing(room, host, 120).accepted && room.SnapshotView().revision == on);
	EXPECT(SetMaxPing(room, host, 0).accepted && room.SnapshotView().maxPingMs == 0);
}

static void TestNobodyMoves() {
	// Turned on with a pair seated and one queued, it moves nobody: the queued
	// player sits when the set rotates, whatever their ping.
	Rules firstToOne;
	firstToOne.format = SetFormat::Ft1;
	firstToOne.rotation = RotationMode::WinnerStays;
	RoomAuthority room("Mid-set", 16, 64, firstToOne);
	const auto host = Join(room, 0, true), a = Join(room, 1), b = Join(room, 2), c = Join(room, 3);
	EXPECT(room.Apply(a, Sit(room, a, 0, 0)).accepted && room.Apply(b, Sit(room, b, 0, 1, {{a, 300}})).accepted);
	EXPECT(room.Apply(c, Sit(room, c, 0, -1, {{a, 300}, {b, 300}})).accepted);
	EXPECT(SetMaxPing(room, host, 80).accepted);
	for (const auto fighter : {a, b}) EXPECT(room.Apply(fighter, Act(room, fighter, 0, ActionKind::Ready)).accepted);
	EXPECT(room.BeginMatch(0, a, b).accepted);
	EXPECT(room.EndMatch(0, room.SnapshotView().tables[0].matchGeneration, MatchResult::P1Win).accepted);
	const auto& table = room.SnapshotView().tables[0];
	EXPECT(table.p1 == a && table.p2 == c);
}

static void TestCabinets() {
	RoomAuthority room("Arcade", 16, 65, Rules(), Limited(100, RoomType::Arcade));
	const auto host = Join(room, 0, true), a = Join(room, 1), far = Join(room, 2), near = Join(room, 3);
	EXPECT(room.Apply(a, Sit(room, a, 0, 0)).accepted && CabinetOf(room.SnapshotView(), a) == 0);
	// Anyone at a cabinet can be the next opponent, whichever cabinet is named.
	EXPECT(room.Apply(far, Sit(room, far, 4, 1, {{a, 150}})).reason == RejectReason::PingOverLimit && CabinetOf(room.SnapshotView(), far) < 0);
	EXPECT(room.Apply(near, Sit(room, near, 4, 1, {{a, 50}})).accepted && CabinetOf(room.SnapshotView(), near) == 4 + TableCount);
	// Moving between cabinets is not sitting down: the host lowers the limit
	// and the one already sitting still moves.
	EXPECT(SetMaxPing(room, host, 80).accepted);
	EXPECT(room.Apply(near, Sit(room, near, 6, 0)).accepted && CabinetOf(room.SnapshotView(), near) == 6);
}

static void TestWire() {
	RoomAuthority room("Wire", 16, 66);
	const auto host = Join(room, 0, true), a = Join(room, 1);
	// A room without the rule has no max_ping_ms, so it reads as before.
	const nlohmann::json plain = room.SnapshotFor(host);
	EXPECT(!plain.contains("max_ping_ms") && plain.get<Snapshot>().maxPingMs == 0);
	EXPECT(SetMaxPing(room, host, 150).accepted);
	const nlohmann::json limited = room.SnapshotFor(host);
	EXPECT(limited.at("max_ping_ms") == 150 && limited.get<Snapshot>().maxPingMs == 150);
	// The chat-free snapshot and a checkpoint carry it too.
	EXPECT(room.SnapshotFor(host, false).maxPingMs == 150);
	RoomAuthority restored("Restored");
	EXPECT(restored.RestoreCheckpoint(nlohmann::json::parse(room.Checkpoint().dump())) && restored.SnapshotView().maxPingMs == 150);
	// A state saved before the rule leaves it out: off. A limit no host can
	// choose, or not a number, does not read.
	auto older = limited;
	older.erase("max_ping_ms");
	EXPECT(older.get<Snapshot>().maxPingMs == 0);
	for (const nlohmann::json bad : {nlohmann::json(90), nlohmann::json("100"), nlohmann::json(-80)}) {
		auto broken = limited;
		broken["max_ping_ms"] = bad;
		EXPECT(Throws<Snapshot>(broken));
	}
	// The room that replaces this one is created with it.
	EXPECT(CharterOf(room.SnapshotView()).maxPingMs == 150);
	// A charter with a limit no host can choose starts the room off.
	RoomAuthority odd("Odd", 16, 67, Rules(), Limited(90));
	EXPECT(odd.SnapshotView().maxPingMs == 0);

	// SetMaxPing: max_ping_ms only when set; an older authority knows no kind past its last.
	auto set = Act(room, host, 0, ActionKind::SetMaxPing);
	set.maxPingMs = 80;
	const nlohmann::json sent = set;
	EXPECT(sent.at("max_ping_ms") == 80 && sent.get<Action>().maxPingMs == 80 && sent.get<Action>().kind == ActionKind::SetMaxPing);
	set.maxPingMs = 0;
	const nlohmann::json off = set;
	EXPECT(!off.contains("max_ping_ms") && off.get<Action>().maxPingMs == 0);
	EXPECT(static_cast<int>(ActionKind::SetMaxPing) <= static_cast<int>(LastActionKind));
	auto unknown = sent;
	unknown["kind"] = static_cast<int>(LastActionKind) + 1;
	EXPECT(Throws<Action>(unknown));

	// A seat's figures: only when there are some, and read back in order.
	const auto bare = Sit(room, a, 0, 0);
	EXPECT(!nlohmann::json(bare).contains("pings") && nlohmann::json(bare).get<Action>().pings.empty());
	const auto measured = Sit(room, a, 0, 0, {{host, 42}, {7, 999}});
	const nlohmann::json carried = measured;
	EXPECT(carried.at("pings").size() == 2 && carried.get<Action>().pings == measured.pings);
	// A figure of no member, of none or past the helper's range, a member
	// twice, or more figures than members, does not read.
	const nlohmann::json badFigures[] = {
		nlohmann::json::array({{{"member", MemberId{0}}, {"ms", 40}}}),
		nlohmann::json::array({{{"member", MemberId{3}}, {"ms", 0}}}),
		nlohmann::json::array({{{"member", MemberId{3}}, {"ms", 1000}}}),
		nlohmann::json::array({{{"member", MemberId{3}}, {"ms", 40}}, {{"member", MemberId{3}}, {"ms", 50}}}),
		nlohmann::json::object(),
	};
	for (const auto& figures : badFigures) {
		auto broken = carried;
		broken["pings"] = figures;
		EXPECT(Throws<Action>(broken));
	}
	auto many = nlohmann::json::array();
	for (MemberId member = 1; member <= MaximumMembers; ++member) many.push_back({{"member", member}, {"ms", 40}});
	auto crowded = carried;
	crowded["pings"] = many;
	EXPECT(Throws<Action>(crowded));

	// The refusal reaches the member as itself; a reason past the last is refused whole.
	EXPECT(SetMaxPing(room, host, 80).accepted);
	EXPECT(room.Apply(a, Sit(room, a, 0, 0)).accepted);
	const auto b = Join(room, 2);
	const nlohmann::json refused = room.Apply(b, Sit(room, b, 0, 1, {{a, 81}}));
	EXPECT(refused.at("reason") == static_cast<int>(RejectReason::PingOverLimit) && refused.get<Result>().reason == RejectReason::PingOverLimit);
	EXPECT(static_cast<int>(RejectReason::PingOverLimit) <= static_cast<int>(LastRejectReason));
	auto later = refused;
	later["reason"] = static_cast<int>(LastRejectReason) + 1;
	EXPECT(Throws<Result>(later));
}

int main() {
	TestPureRule();
	TestClassicSeats();
	TestHostSetsTheRule();
	TestNobodyMoves();
	TestCabinets();
	TestWire();
	if (TEST_FAILURES()) { std::printf("%d failures\n", TEST_FAILURES()); return 1; }
	std::printf("Room max ping rules passed.\n");
	return 0;
}
