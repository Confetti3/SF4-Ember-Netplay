// The session server's start gate for a stage a fighter's copy lacks (SFV
// Ember's EMBER_LIBRARY.md section 38): StartReadyTables takes P1's Ready back
// instead of beginning a game on a stage either fighter lacks, and the table
// hears StageNotShared; a stage both have begins, and a watcher that lacks it
// sits the game out. An SF4 room, where nobody lacks a stage, starts as it
// always did, with or without a stage sent.
#include "../session/sf4e__SessionServer.hxx"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "test_support.hxx"
#include "server_transport_support.hxx"

using namespace sf4e;
namespace protocol = sf4e::SessionProtocol;
using nlohmann::json;

namespace {
// A custom room whose members, on connections 1 and up, lack the stages
// given for each.
struct Room {
	MockTransport* transport = new MockTransport();
	SessionServer server{"missing-stages", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport)};
	std::uint64_t actionId = 0;
	explicit Room(const std::vector<std::vector<std::int64_t>>& missing) {
		// Both fighters readying starts the match, which needs each member's endpoint.
		std::array<std::uint8_t, 16> authorization = {};
		authorization[0] = 38;
		server.EnableMatchAuthorization(authorization, [](session::Connection connection) {
			std::string identity(64, '0');
			identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
			return identity;
		});
		server.EnableCustomRooms("Missing stages", 8, 38);
		for (std::size_t i = 0; i < missing.size(); ++i) {
			const auto connection = static_cast<session::Connection>(i + 1);
			protocol::SessionJoinRequest join;
			join.username = "Member-" + std::to_string(connection); join.sidecarHash = "build"; join.port = 30000;
			join.customRooms = true; join.roomProtocol = room::ProtocolVersion;
			join.missingStages = missing[i];
			protocol::SessionHelloMsg hello; hello.admission = json(join);
			Send(connection, json(hello));
		}
		server.AdvanceCustomRoom(1000);
	}
	void Send(session::Connection connection, const json& message) {
		transport->Push(connection, message);
		CHECK(server.Step() == 0);
	}
	void Act(session::Connection connection, room::ActionKind kind) {
		const auto snapshot = *server.RoomSnapshot();
		room::Action action;
		action.kind = kind; action.roomEpoch = snapshot.roomEpoch; action.revision = snapshot.revision;
		action.tableRevision = snapshot.tables[0].revision; action.actionId = ++actionId;
		protocol::RoomActionMessage message; message.action = action;
		Send(connection, json(message));
	}
	void Stage(std::int64_t stage) {
		protocol::PreBattleSetStage message; message.stageID = static_cast<std::int32_t>(stage);
		Send(1, json(message));
	}
	room::MemberId Member(session::Connection connection) const { return server.roomMembers.at(connection); }
	const room::Table& Table() const { return server.RoomSnapshot()->tables[0]; }
	// The room events of `kind` sent to `connection`.
	std::vector<room::Event> Events(session::Connection connection, room::Event::Kind kind) const {
		std::vector<room::Event> found;
		for (const auto& sent : transport->outgoing)
			if (sent.first == connection && sent.second.value("type", std::string()) == "room_event" &&
				sent.second.at("event").at("kind") == static_cast<int>(kind)) found.push_back(sent.second.at("event").get<room::Event>());
		return found;
	}
};

// Stages of SF4's catalog, which the library's tests are built with.
std::int64_t StageAt(std::size_t index) { return selection::StageList()[index].id; }

void TestOpponentLacksTheStage() {
	const auto theirs = StageAt(1), watchers = StageAt(2);
	// P2 lacks `theirs`; the third member watches and lacks `watchers`.
	Room room({{}, {theirs}, {watchers}});
	room.Act(1, room::ActionKind::Queue);
	room.Act(2, room::ActionKind::Queue);
	room.Act(3, room::ActionKind::Watch);
	room.Stage(theirs);
	room.Act(2, room::ActionKind::Ready);
	room.Act(1, room::ActionKind::Ready);
	// No game: P1's Ready is taken back, P2's stays, and the table says why.
	CHECK(!room.Table().ready[0] && room.Table().ready[1]);
	CHECK(room.Table().phase == room::TablePhase::Waiting && room.Table().matchGeneration == 0);
	for (session::Connection connection = 1; connection <= 3; ++connection) {
		CHECK(room.Events(connection, room::Event::Kind::MatchStarted).empty());
		const auto refused = room.Events(connection, room::Event::Kind::StageNotShared);
		CHECK(refused.size() == 1 && refused[0].member == room.Member(1) && refused[0].matchGeneration == 0);
	}
	// P1 chooses a stage both fighters have: the game begins, without the
	// watcher, who lacks this one.
	room.transport->outgoing.clear();
	room.Stage(watchers);
	room.Act(1, room::ActionKind::Ready);
	CHECK(room.Table().phase == room::TablePhase::Playing && room.Table().matchGeneration != 0);
	CHECK(room.Events(1, room::Event::Kind::MatchStarted).size() == 1);
	const auto sitting = room.Events(3, room::Event::Kind::StageNotShared);
	CHECK(sitting.size() == 1 && sitting[0].member == room.Member(3) && sitting[0].matchGeneration == room.Table().matchGeneration);
	const auto roster = room.server._roomAuthority->MatchRoster(0);
	CHECK(roster.size() == 2 && std::find(roster.begin(), roster.end(), room.Member(3)) == roster.end());
}

// A watcher that has the stage watches; nobody hears StageNotShared.
void TestEveryoneHasTheStage() {
	Room room({{StageAt(3)}, {StageAt(4)}, {StageAt(5)}});
	room.Act(1, room::ActionKind::Queue);
	room.Act(2, room::ActionKind::Queue);
	room.Act(3, room::ActionKind::Watch);
	room.Stage(StageAt(0));
	room.Act(1, room::ActionKind::Ready);
	room.Act(2, room::ActionKind::Ready);
	CHECK(room.Table().phase == room::TablePhase::Playing);
	for (session::Connection connection = 1; connection <= 3; ++connection)
		CHECK(room.Events(connection, room::Event::Kind::StageNotShared).empty());
	const auto roster = room.server._roomAuthority->MatchRoster(0);
	CHECK(std::find(roster.begin(), roster.end(), room.Member(3)) != roster.end());
}

// SF4: nobody lacks a stage, so a Ready is never taken back, with a stage
// sent or none at all (the table's stored stage is then -1).
void TestSf4RoomStarts() {
	for (const bool sent : {true, false}) {
		Room room({{}, {}});
		room.Act(1, room::ActionKind::Queue);
		room.Act(2, room::ActionKind::Queue);
		if (sent) room.Stage(StageAt(1));
		room.Act(1, room::ActionKind::Ready);
		room.Act(2, room::ActionKind::Ready);
		CHECK(room.Table().phase == room::TablePhase::Playing);
		CHECK(room.Events(1, room::Event::Kind::StageNotShared).empty());
		// The join requests carried no list, and the members have none.
		for (const auto& member : room.server.RoomSnapshot()->members) CHECK(member.missingStages.empty());
	}
}
} // namespace

int main() {
	TestOpponentLacksTheStage();
	TestEveryoneHasTheStage();
	TestSf4RoomStarts();
	std::puts("SessionServerMissingStages test passed");
	return 0;
}
