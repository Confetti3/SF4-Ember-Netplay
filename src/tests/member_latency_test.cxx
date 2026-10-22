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

int main() {
	TestSubscription();
	TestJoinWire();
	TestIrohRoom();
	TestPulse();
	TestWanted();
	std::cout << "Member latency tests passed\n";
	return 0;
}
