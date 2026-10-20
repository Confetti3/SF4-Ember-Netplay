// A room's delay policy, as the room host projects it to each member's game.
// Shared (the default): both seats play at the higher of the two Ready delays.
// Own: each seat plays at the delay its fighter chose, and the member queued
// behind them is projected the same pair.
#include "../session/sf4e__SessionServer.hxx"
#include <array>
#include <cstdint>
#include <string>

#include "test_support.hxx"
#include "server_transport_support.hxx"

using namespace sf4e;
namespace protocol = sf4e::SessionProtocol;
using nlohmann::json;

namespace {
// A custom room of three admitted members on connections 1 to 3.
struct Room {
	MockTransport* transport = new MockTransport();
	SessionServer server{"delay-policy", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport)};
	std::uint64_t actionId = 0;
	explicit Room(room::DelayPolicy policy, bool serverOwned = false) {
		// Both fighters readying starts the match, which needs each member's endpoint.
		std::array<std::uint8_t, 16> authorization = {};
		authorization[0] = 78;
		server.EnableMatchAuthorization(authorization, [](session::Connection connection) {
			std::string identity(64, '0');
			identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
			return identity;
		});
		if (serverOwned) CHECK(server.EnableServerOwnedRooms("Delay policy", 8, 78, room::Rules(), policy));
		else server.EnableCustomRooms("Delay policy", 8, 78, room::Rules(), false, policy);
		for (session::Connection connection = 1; connection <= 3; ++connection) {
			protocol::SessionJoinRequest join;
			join.username = "Member-" + std::to_string(connection); join.sidecarHash = "build"; join.port = 30000;
			join.customRooms = true; join.roomProtocol = room::ProtocolVersion;
			protocol::SessionHelloMsg hello; hello.admission = json(join);
			if (serverOwned) server.SetConnectionAdmission(connection, {"ember-" + std::to_string(connection)});
			Send(connection, json(hello));
		}
		server.AdvanceCustomRoom(1000);
	}
	void Send(session::Connection connection, const json& message) {
		transport->Push(connection, message);
		CHECK(server.Step() == 0);
	}
	void Act(session::Connection connection, room::ActionKind kind, int delay = 2) {
		const auto snapshot = *server.RoomSnapshot();
		room::Action action;
		action.kind = kind; action.roomEpoch = snapshot.roomEpoch; action.revision = snapshot.revision;
		action.tableRevision = snapshot.tables[0].revision; action.actionId = ++actionId;
		action.inputDelay = static_cast<std::uint8_t>(delay);
		protocol::RoomActionMessage message; message.action = action;
		Send(connection, json(message));
	}
	// Three members at table 0, the third queued behind the two fighters, who
	// have chosen their fighters, P1's stage and seed.
	void Seat() {
		for (session::Connection connection = 1; connection <= 3; ++connection) Act(connection, room::ActionKind::Queue);
		for (session::Connection connection = 1; connection <= 2; ++connection) {
			protocol::PreBattleSetChara pick; pick.chara = protocol::MatchData{}.chara[0];
			pick.chara.charaID = connection == 1 ? 5 : 6;
			Send(connection, json(pick));
		}
		protocol::PreBattleSetStage stage; stage.stageID = selection::StageList().back().id;
		Send(1, json(stage));
		protocol::PreBattleSetEnv env; env.rngSeed = 1234;
		Send(1, json(env));
	}
	const room::Table& Table() const { return server.RoomSnapshot()->tables[0]; }
	// The delays in the last native projection sent to `connection`.
	std::array<std::uint8_t, 2> Projected(session::Connection connection) const {
		for (auto sent = transport->outgoing.rbegin(); sent != transport->outgoing.rend(); ++sent)
			if (sent->first == connection && sent->second.value("type", std::string()) == "data_update")
				return sent->second.at("matchData").get<protocol::MatchData>().inputDelay;
		CHECK(false);
		return {};
	}
};

void CheckProjected(const Room& room, std::uint8_t first, std::uint8_t second) {
	for (session::Connection connection = 1; connection <= 3; ++connection) {
		const auto delays = room.Projected(connection);
		CHECK(delays[0] == first && delays[1] == second);
	}
}

// Fighters who chose 1 and 4: Shared plays both at 4, Own plays each at theirs.
void TestProjectionBothWays() {
	Room shared(room::DelayPolicy::Shared);
	shared.Seat();
	shared.Act(1, room::ActionKind::Ready, 1);
	shared.Act(2, room::ActionKind::Ready, 4);
	CHECK(shared.Table().ready[0] && shared.Table().ready[1] && shared.Table().inputDelay[0] == 1 && shared.Table().inputDelay[1] == 4);
	CheckProjected(shared, 4, 4);

	Room own(room::DelayPolicy::Own);
	CHECK(own.server.RoomSnapshot()->delayPolicy == room::DelayPolicy::Own);
	own.Seat();
	own.Act(1, room::ActionKind::Ready, 1);
	own.Act(2, room::ActionKind::Ready, 4);
	CHECK(own.Table().ready[0] && own.Table().ready[1]);
	CheckProjected(own, 1, 4);

	// The lower seat is the second one, too.
	Room reversed(room::DelayPolicy::Own);
	reversed.Seat();
	reversed.Act(1, room::ActionKind::Ready, 3);
	reversed.Act(2, room::ActionKind::Ready, 0);
	CheckProjected(reversed, 3, 0);
}

// A public (server-owned) room takes the policy the same way.
void TestServerOwnedRoom() {
	Room own(room::DelayPolicy::Own, true);
	CHECK(own.server.RoomSnapshot()->serverOwned && own.server.RoomSnapshot()->delayPolicy == room::DelayPolicy::Own);
	Room shared(room::DelayPolicy::Shared, true);
	CHECK(shared.server.RoomSnapshot()->serverOwned && shared.server.RoomSnapshot()->delayPolicy == room::DelayPolicy::Shared);
	CHECK(!json(*shared.server.RoomSnapshot()).contains("delay") && json(*own.server.RoomSnapshot()).at("delay") == "own");
}
}

int main() {
	TestProjectionBothWays();
	TestServerOwnedRoom();
	std::puts("Delay policy projection passed.");
	return 0;
}
