// The ping to each room member on the native side (MEMBER_LATENCY.md 5, C++ 9
// and 10): the board subscription and the member_latency events it takes,
// driven through an IrohRoom on a scripted helper, and the board pulse and
// the rule for when the subscription is wanted.
#include "../session/IrohRoom.hxx"
#include "../session/MemberLatency.hxx"
#include "../netplay/LatencyBoard.hxx"
#include <array>
#include <iostream>
#include <string>
#include <vector>

#include "test_support.hxx"
#include "fake_helper_pipe.hxx"

using namespace sf4e;
using nlohmann::json;

namespace {
const std::array<std::uint8_t, 16> kRoom{{7}}, kOtherRoom{{8}};

json Rows() {
	return json::array({{{"member", 2}, {"incarnation", 22}, {"state", "value"}, {"rtt_ms", 62}, {"route", "relay"}},
		{{"member", 3}, {"incarnation", 33}, {"state", "measuring"}},
		{{"member", 4}, {"incarnation", 44}, {"state", "unavailable"}, {"reason", "in_match"}}});
}

json Table(std::uint64_t epoch, const std::array<std::uint8_t, 16>& room, std::uint64_t view, json rows = Rows()) {
	return {{"type", "member_latency"}, {"epoch", epoch}, {"room", room}, {"view", view}, {"members", std::move(rows)}};
}

void TestSubscription() {
	session::LatencySubscription subscription;
	CHECK(!subscription.Read(Table(1, kRoom, 1), 1, kRoom));
	CHECK(subscription.Due(1, kRoom, 1000) == 1);
	// Not queued: no view is active, and the next start takes a new number.
	CHECK(!subscription.Read(Table(1, kRoom, 1), 1, kRoom) && subscription.Active() == 0);
	CHECK(subscription.Due(1, kRoom, 1000) == 2);
	subscription.Sent(2, 1, kRoom, 1000);
	CHECK(subscription.Active() == 2 && subscription.Due(1, kRoom, 2000) == 0);
	CHECK(subscription.Due(1, kRoom, 1000 + session::LatencySubscription::RenewMs) == 2);
	CHECK(subscription.Read(Table(1, kRoom, 2), 1, kRoom));
	const auto& table = subscription.Table();
	CHECK(table.size() == 3);
	CHECK(table.at(2).state == session::MemberLatency::State::Value && table.at(2).rttMs == 62 && table.at(2).relay && table.at(2).incarnation == 22);
	CHECK(table.at(3).state == session::MemberLatency::State::Measuring);
	CHECK(table.at(4).state == session::MemberLatency::State::Unavailable && table.at(4).reason == "in_match");
	const auto kept = table;
	// A stale view, epoch or room, a malformed table and one too long are dropped.
	CHECK(!subscription.Read(Table(1, kRoom, 1, json::array()), 1, kRoom));
	CHECK(!subscription.Read(Table(2, kRoom, 2, json::array()), 1, kRoom));
	CHECK(!subscription.Read(Table(1, kOtherRoom, 2, json::array()), 1, kRoom));
	CHECK(!subscription.Read(Table(1, kRoom, 2, json::array({{{"member", 2}, {"incarnation", 1}, {"state", "value"}, {"rtt_ms", 0}, {"route", "direct"}}})), 1, kRoom));
	CHECK(!subscription.Read(Table(1, kRoom, 2, json::array({{{"member", 2}, {"incarnation", 1}, {"state", "fast"}}})), 1, kRoom));
	// A value without its estimate or route and an unavailable row without its reason are malformed.
	CHECK(!subscription.Read(Table(1, kRoom, 2, json::array({{{"member", 2}, {"incarnation", 1}, {"state", "value"}}})), 1, kRoom));
	CHECK(!subscription.Read(Table(1, kRoom, 2, json::array({{{"member", 2}, {"incarnation", 1}, {"state", "value"}, {"rtt_ms", 5}}})), 1, kRoom));
	CHECK(!subscription.Read(Table(1, kRoom, 2, json::array({{{"member", 2}, {"incarnation", 1}, {"state", "value"}, {"rtt_ms", 5}, {"route", "lan"}}})), 1, kRoom));
	CHECK(!subscription.Read(Table(1, kRoom, 2, json::array({{{"member", 2}, {"incarnation", 1}, {"state", "unavailable"}}})), 1, kRoom));
	json twice = json::array({{{"member", 2}, {"incarnation", 1}, {"state", "measuring"}}, {{"member", 2}, {"incarnation", 1}, {"state", "measuring"}}});
	CHECK(!subscription.Read(Table(1, kRoom, 2, twice), 1, kRoom));
	json many = json::array();
	for (int member = 1; member <= static_cast<int>(room::MaxMembers); ++member)
		many.push_back({{"member", member}, {"incarnation", member}, {"state", "measuring"}});
	CHECK(!subscription.Read(Table(1, kRoom, 2, many), 1, kRoom));
	CHECK(subscription.Table() == kept);
	// Stopping drops the table and names the view to close.
	CHECK(subscription.Stop() == 2 && subscription.Table().empty() && subscription.Active() == 0);
	CHECK(subscription.Stop() == 0);
	CHECK(!subscription.Read(Table(1, kRoom, 2), 1, kRoom));
	// A new epoch or room ends the active view before the next start.
	CHECK(subscription.Due(1, kRoom, 5000) == 3);
	subscription.Sent(3, 1, kRoom, 5000);
	CHECK(subscription.Due(2, kRoom, 5001) == 4 && subscription.Active() == 0);
	subscription.Sent(4, 2, kRoom, 5001);
	CHECK(subscription.Due(2, kOtherRoom, 5002) == 5 && subscription.Active() == 0);
}

// The latency_watch commands the room sent: `count` of them, waiting a moment
// for the helper client's writer, or every one sent within that moment when
// `count` is 0.
std::vector<json> Watches(FakeHelperPipe& pipe, std::size_t count = 1) {
	std::vector<json> found;
	for (int wait = 0; wait < (count ? 2000 : 100) && (!count || found.size() < count); ++wait) {
		pipe.Drain();
		for (const auto& command : pipe.received)
			if (command.is_object() && command.value("type", std::string()) == "latency_watch") found.push_back(command);
		pipe.received.clear();
		if (!count || found.size() < count) Sleep(1);
	}
	return found;
}

// The join the room sends its helper says it watches a tournament match only
// when it does: a plain join is the message every helper reads.
void TestJoinWire() {
	for (const bool observer : {false, true}) {
		FakeHelperPipe pipe;
		platform::HelperClient helper;
		pipe.Connect(helper);
		session::IrohRoom room(helper);
		CHECK(room.Join("invitation", "build", observer));
		json join;
		for (int wait = 0; wait < 2000 && join.is_null(); ++wait) {
			pipe.Drain();
			for (const auto& command : pipe.received)
				if (command.is_object() && command.value("type", std::string()) == "join") join = command;
			if (join.is_null()) Sleep(1);
		}
		CHECK(join.is_object() && join.at("invitation") == "invitation" && join.at("build") == "build");
		CHECK(join.contains("observer") == observer && (!observer || join.at("observer") == true));
	}
}

void TestIrohRoom() {
	FakeHelperPipe pipe;
	platform::HelperClient helper;
	pipe.Connect(helper);
	session::IrohRoom room(helper);
	const auto settle = [&](const auto& done) {
		for (int tick = 0; tick < 5000 && !done(); ++tick) { room.Poll(); pipe.Drain(); Sleep(1); }
		CHECK(done());
	};
	// Before the room is open nothing is sent.
	room.WatchLatency(true);
	CHECK(room.Join("invitation", "build"));
	const auto epoch = room.Epoch();
	room.WatchLatency(true);
	CHECK(Watches(pipe, 0).empty());
	const std::string leader(64, 'a');
	pipe.Send({{"type", "status"}, {"request_id", 1}, {"endpoint", std::string(64, 'b')}, {"fixed_port", 0}});
	pipe.Send({{"type", "coordination_state"}, {"epoch", epoch}, {"room", kRoom}, {"term", 1}, {"revision", 0},
		{"incarnation", 11}, {"leader", leader}, {"writable", true}, {"leader_local", false}});
	pipe.Send({{"type", "control_rebound"}, {"epoch", epoch}, {"room", kRoom}, {"leader", leader},
		{"member_incarnations", json::object()}, {"members", json::array()}});
	settle([&] { return room.GetState() == session::IrohRoom::State::Ready; });

	room.WatchLatency(true);
	room.WatchLatency(true);
	auto watches = Watches(pipe);
	CHECK(watches.size() == 1 && Watches(pipe, 0).empty());
	const auto view = watches[0].at("view").get<std::uint64_t>();
	CHECK(view && watches[0].at("on") == true && watches[0].at("epoch") == epoch && watches[0].at("room") == json(kRoom));

	pipe.Send(Table(epoch, kRoom, view));
	settle([&] { return room.MemberLatencies().size() == 3; });
	// Stale tables leave it as it is.
	pipe.Send(Table(epoch, kRoom, view + 1, json::array()));
	pipe.Send(Table(epoch + 1, kRoom, view, json::array()));
	pipe.Send(Table(epoch, kOtherRoom, view, json::array()));
	pipe.Send(Table(epoch, kRoom, view, json::array({Rows()[0]})));
	settle([&] { return room.MemberLatencies().size() == 1; });
	CHECK(room.MemberLatencies().at(2).rttMs == 62);

	// A table queued before the board closed is dropped after it.
	pipe.Send(Table(epoch, kRoom, view));
	room.WatchLatency(false);
	CHECK(room.MemberLatencies().empty());
	for (int tick = 0; tick < 50; ++tick) { room.Poll(); pipe.Drain(); Sleep(1); }
	CHECK(room.MemberLatencies().empty());
	watches = Watches(pipe);
	CHECK(watches.size() == 1 && watches[0].at("view") == view && watches[0].at("on") == false);
	room.WatchLatency(false);
	CHECK(Watches(pipe, 0).empty());

	// The next board takes a new view; the old one's tables stay dropped.
	room.WatchLatency(true);
	watches = Watches(pipe);
	CHECK(watches.size() == 1 && watches[0].at("view").get<std::uint64_t>() > view);
	const auto next = watches[0].at("view").get<std::uint64_t>();
	pipe.Send(Table(epoch, kRoom, view));
	pipe.Send(Table(epoch, kRoom, next, json::array({Rows()[1]})));
	settle([&] { return room.MemberLatencies().size() == 1; });
	CHECK(room.MemberLatencies().count(3) == 1);
}

void TestPulse() {
	netplay::LatencyBoardPulse pulse;
	CHECK(!pulse.Shown(42, 1000));
	pulse.Note(42, 1000);
	CHECK(pulse.Shown(42, 1000) && pulse.Shown(42, 1000 + netplay::LatencyBoardPulse::ExpiryMs - 1));
	CHECK(!pulse.Shown(42, 1000 + netplay::LatencyBoardPulse::ExpiryMs));
	// Another room's board, or none, is not this room's.
	CHECK(!pulse.Shown(43, 1001) && !pulse.Shown(0, 1001));
	pulse.Note(43, 3000);
	CHECK(!pulse.Shown(42, 3000) && pulse.Shown(43, 3000));
}

void TestWanted() {
	room::Snapshot room;
	room.roomEpoch = 5; room.localMember = 1;
	room::Member me; me.id = 1; room.members.push_back(me);
	using netplay::RoomState; using netplay::MatchState;
	CHECK(netplay::LatencyWanted(true, RoomState::Joined, MatchState::None, room, false));
	CHECK(netplay::LatencyWanted(true, RoomState::Joined, MatchState::PostMatch, room, false));
	CHECK(!netplay::LatencyWanted(false, RoomState::Joined, MatchState::None, room, false));
	CHECK(!netplay::LatencyWanted(true, RoomState::Opening, MatchState::None, room, false));
	CHECK(!netplay::LatencyWanted(true, RoomState::Joined, MatchState::Preparing, room, false));
	CHECK(!netplay::LatencyWanted(true, RoomState::Joined, MatchState::Playing, room, false));
	CHECK(!netplay::LatencyWanted(true, RoomState::Joined, MatchState::None, room, true));
	room.members[0].table = 1; room.members[0].seat = 0;
	for (const auto phase : {room::TablePhase::Idle, room::TablePhase::Waiting}) {
		room.tables[1].phase = phase;
		CHECK(netplay::LatencyWanted(true, RoomState::Joined, MatchState::None, room, false));
	}
	for (const auto phase : {room::TablePhase::Ready, room::TablePhase::Playing, room::TablePhase::Paused}) {
		room.tables[1].phase = phase;
		CHECK(!netplay::LatencyWanted(true, RoomState::Joined, MatchState::None, room, false));
	}
}
}

namespace {
// What a seat request carries for Max ping (ROOM_PING_RULE.md): a figure for
// each other member measured for their current incarnation, through a relay
// or not, or else the last one kept for that incarnation; one still being
// measured, in a game, with no row or measured for an earlier incarnation
// gives none without one kept, and only the first is still measuring.
void TestSeatPings() {
	room::Snapshot room;
	for (const room::MemberId id : {1, 2, 3, 4, 5, 6}) {
		room::Member member;
		member.id = id; member.incarnation = id * 11;
		room.members.push_back(member);
	}
	room.localMember = 1;
	session::MemberLatencyTable table;
	const auto entry = [](std::uint64_t incarnation, session::MemberLatency::State state, unsigned rtt) {
		session::MemberLatency latency;
		latency.incarnation = incarnation; latency.state = state; latency.rttMs = rtt; latency.relay = true;
		return latency;
	};
	table[1] = entry(11, session::MemberLatency::State::Value, 5);
	table[2] = entry(22, session::MemberLatency::State::Value, 62);
	table[3] = entry(33, session::MemberLatency::State::Measuring, 0);
	table[4] = entry(44, session::MemberLatency::State::Unavailable, 0);
	table[6] = entry(65, session::MemberLatency::State::Value, 40);
	const auto measuring = [&](const session::KnownPings& known, const std::vector<room::MemberId>& opponents) {
		return session::PendingPing(table, known, room, opponents, false) == session::PingPending::Measuring;
	};
	const auto figures = session::MeasuredPings(table, {}, room);
	CHECK(figures.size() == 1 && figures[0].member == 2 && figures[0].ms == 62);
	CHECK(measuring({}, {2, 3}) && !measuring({}, {2, 4, 5, 6}));
	// Someone who left the room is measured by nobody.
	CHECK(!measuring({}, {9}) && !measuring({}, {}));
	CHECK(session::CurrentLatency(table, room.members[1]) && !session::CurrentLatency(table, room.members[5]));
	// A figure kept for the member's incarnation stands in for the one the
	// helper has none of now; the live value wins, and one kept for an
	// earlier incarnation or for the local member is not carried.
	const session::KnownPings known{{1, {11, 7}}, {2, {22, 90}}, {3, {33, 120}}, {4, {44, 150}}, {6, {65, 30}}};
	const auto kept = session::MeasuredPings(table, known, room);
	CHECK((kept == std::vector<room::PingFigure>{{2, 62}, {3, 120}, {4, 150}}));
	// A member with a kept figure is weighed with it, not waited for.
	CHECK(!measuring(known, {2, 3}));
}

// Watching, this PC's helper measures nobody: what the board does about an
// opponent it has no figure for. One in a game cannot be measured by anyone
// and holds nothing; one out of a game waits for this PC to leave its game,
// unless the helper is measuring it or cannot reach it.
void TestPendingWhileWatching() {
	room::Snapshot room;
	for (const room::MemberId id : {1, 2, 3, 4, 5}) {
		room::Member member;
		member.id = id; member.incarnation = id * 11;
		room.members.push_back(member);
	}
	room.localMember = 1;
	// Table 1 plays 3 against 4, watched by 1; 2 waits at table 0, 5 is idle.
	room.tables[1].phase = room::TablePhase::Playing; room.tables[1].p1 = 3; room.tables[1].p2 = 4; room.tables[1].spectators = {1};
	room.tables[0].phase = room::TablePhase::Waiting; room.tables[0].p1 = 2;
	CHECK(room::InGame(room, 1) && room::InGame(room, 3) && room::InGame(room, 4));
	CHECK(!room::InGame(room, 2) && !room::InGame(room, 5) && !room::InGame(room, 0));
	// A queued member listed to watch the game it waits out is in it too; a
	// table between games holds nobody in one.
	room.tables[1].queue = {5}; room.tables[1].spectators = {1, 5};
	CHECK(room::InGame(room, 5));
	room.tables[1].phase = room::TablePhase::Waiting;
	CHECK(!room::InGame(room, 1) && !room::InGame(room, 3));
	room.tables[1].phase = room::TablePhase::Playing;
	room.tables[1].queue.clear(); room.tables[1].spectators = {1};

	session::MemberLatencyTable table;
	const auto unavailable = [](std::uint64_t incarnation, const char* reason) {
		session::MemberLatency latency;
		latency.incarnation = incarnation; latency.state = session::MemberLatency::State::Unavailable; latency.reason = reason;
		return latency;
	};
	const auto pending = [&](const session::KnownPings& known, const std::vector<room::MemberId>& opponents, bool inGame) {
		return session::PendingPing(table, known, room, opponents, inGame);
	};
	// No table at all (the board closed while watching), then every member
	// room_unavailable, as the helper names them while this PC is in a game.
	for (int rows = 0; rows < 2; ++rows) {
		if (rows) for (const room::MemberId id : {2, 3, 4, 5}) table[id] = unavailable(id * 11, "room_unavailable");
		CHECK(pending({}, {2}, true) == session::PingPending::AfterGame);
		CHECK(pending({}, {3, 4}, true) == session::PingPending::None);
		CHECK(pending({{2, {22, 40}}}, {2, 3, 4}, true) == session::PingPending::None);
		// One kept for an earlier incarnation of them is no figure.
		CHECK(pending({{2, {21, 40}}}, {2}, true) == session::PingPending::AfterGame);
		// Out of a game, nothing waits: a board without figures yet never blocks.
		CHECK(pending({}, {2}, false) == session::PingPending::None);
	}
	// A member the helper cannot reach, or in a game by its roster, holds nothing.
	table[2] = unavailable(22, "unreachable");
	CHECK(pending({}, {2}, true) == session::PingPending::None);
	table[2] = unavailable(22, "in_match");
	CHECK(pending({}, {2}, true) == session::PingPending::None);
}

// Watch, then Queue in a Max ping room (ROOM_PING_RULE.md): the figure
// measured before the watched game began is still carried once the helper
// measures nobody, so the room refuses the seat it is over the limit for, and
// takes it within the limit. Without any figure the board waits instead.
void TestWatchThenQueue() {
	room::Charter charter;
	charter.maxPingMs = 100;
	room::RoomAuthority authority("Watched", 16, 70, room::Rules(), charter);
	const auto join = [&](int index, bool host) {
		const auto result = authority.Join("Player" + std::to_string(index), room::ConnectionRef{"host", std::to_string(index)}, host);
		CHECK(result.accepted);
		return result.snapshot.members.back().id;
	};
	room::ActionId nextAction = 1;
	const auto act = [&](room::MemberId member, std::uint8_t table, room::ActionKind kind, int seat = -1, std::vector<room::PingFigure> pings = {}) {
		room::Action action;
		action.kind = kind;
		action.roomEpoch = authority.SnapshotView().roomEpoch;
		action.revision = authority.SnapshotView().revision;
		action.table = table;
		action.tableRevision = authority.SnapshotView().tables[table].revision;
		action.actionId = nextAction++;
		action.seat = static_cast<std::int8_t>(seat);
		action.pings = std::move(pings);
		return authority.Apply(member, action);
	};
	join(0, true);
	const auto distant = join(1, false), p1 = join(2, false), p2 = join(3, false), me = join(4, false);
	CHECK(act(distant, 0, room::ActionKind::Queue, 0).accepted);
	for (const auto fighter : {p1, p2}) CHECK(act(fighter, 1, room::ActionKind::Queue).accepted);

	// On the board before watching, the helper measured `distant` at 140 ms.
	const auto incarnationOf = [&](room::MemberId member) { return room::FindMember(authority.SnapshotView(), member)->incarnation; };
	const auto row = [&](room::MemberId member, json state) {
		state["member"] = member; state["incarnation"] = incarnationOf(member);
		return state;
	};
	session::LatencySubscription subscription;
	const auto open = [&](std::uint64_t epoch) {
		const auto view = subscription.Due(epoch, kRoom, 1000);
		subscription.Sent(view, epoch, kRoom, 1000);
		return view;
	};
	auto view = open(1);
	CHECK(subscription.Read(Table(1, kRoom, view, json::array({row(distant, {{"state", "value"}, {"rtt_ms", 140}, {"route", "relay"}}),
		row(p1, {{"state", "measuring"}}), row(p2, {{"state", "measuring"}})})), 1, kRoom));

	// Watching: the game begins, the board closes, and the board opened over
	// the game finds every member room_unavailable.
	CHECK(act(me, 1, room::ActionKind::Watch).accepted);
	for (const auto fighter : {p1, p2}) CHECK(act(fighter, 1, room::ActionKind::Ready).accepted);
	CHECK(authority.BeginMatch(1, p1, p2).accepted);
	subscription.Stop();
	CHECK(subscription.Table().empty() && subscription.Known().at(distant).ms == 140);
	view = open(1);
	const json unavailable{{"state", "unavailable"}, {"reason", "room_unavailable"}};
	CHECK(subscription.Read(Table(1, kRoom, view, json::array({row(distant, unavailable), row(p1, unavailable), row(p2, unavailable)})), 1, kRoom));
	const auto watched = authority.SnapshotFor(me);
	CHECK(room::InGame(watched, me) && !room::InGame(watched, distant));

	// Queue at distant's table: the kept figure is carried, and the board and the
	// room both refuse it; within the limit the room seats them.
	const auto opponents = room::PingOpponents(watched, me, 0);
	const auto pings = session::MeasuredPings(subscription.Table(), subscription.Known(), watched);
	CHECK((pings == std::vector<room::PingFigure>{{distant, 140}}));
	CHECK(room::PingOverLimit(watched, opponents, pings).Over());
	CHECK(session::PendingPing(subscription.Table(), subscription.Known(), watched, opponents, true) == session::PingPending::None);
	CHECK(act(me, 0, room::ActionKind::Queue, 1, pings).reason == room::RejectReason::PingOverLimit);

	// A room epoch of its own forgets every figure: with none for distant, the
	// board waits until this PC leaves the game rather than offer the seat.
	subscription.Reset();
	view = open(2);
	CHECK(subscription.Known().empty());
	CHECK(subscription.Read(Table(2, kRoom, view, json::array({row(distant, unavailable), row(p1, unavailable), row(p2, unavailable)})), 2, kRoom));
	CHECK(session::MeasuredPings(subscription.Table(), subscription.Known(), watched).empty());
	CHECK(session::PendingPing(subscription.Table(), subscription.Known(), watched, opponents, true) == session::PingPending::AfterGame);
	// The fighters at the watched table are in a game: nobody can measure them.
	CHECK(session::PendingPing(subscription.Table(), subscription.Known(), watched, room::PingOpponents(watched, me, 1), true) ==
		session::PingPending::None);

	// A table that no longer lists a member, or lists another incarnation of
	// them, drops their kept figure.
	CHECK(subscription.Read(Table(2, kRoom, view, json::array({row(distant, {{"state", "value"}, {"rtt_ms", 60}, {"route", "direct"}})})), 2, kRoom));
	CHECK(subscription.Known().at(distant).ms == 60);
	auto other = row(distant, unavailable);
	other["incarnation"] = incarnationOf(distant) + 1;
	CHECK(subscription.Read(Table(2, kRoom, view, json::array({other})), 2, kRoom) && subscription.Known().empty());
	CHECK(subscription.Read(Table(2, kRoom, view, json::array({row(distant, {{"state", "value"}, {"rtt_ms", 60}, {"route", "direct"}})})), 2, kRoom));
	CHECK(subscription.Read(Table(2, kRoom, view, json::array()), 2, kRoom) && subscription.Known().empty());

	// Within the limit, the watcher's Queue sits them.
	CHECK(act(me, 0, room::ActionKind::Queue, 1, {{distant, 60}}).accepted && authority.SnapshotView().tables[0].p2 == me);
}
}

int main() {
	TestSeatPings();
	TestPendingWhileWatching();
	TestWatchThenQueue();
	TestSubscription();
	TestJoinWire();
	TestIrohRoom();
	TestPulse();
	TestWanted();
	std::cout << "Member latency tests passed\n";
	return 0;
}
