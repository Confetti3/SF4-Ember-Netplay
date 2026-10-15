// A Practice room's tables, as the room host projects them to each member's
// game: every game is a Practice match at the longest round time, whatever
// time the table's rules hold. Any other room projects the rules as they are.
#include "../session/sf4e__SessionServer.hxx"
#include <string>

#include "test_support.hxx"
#include "server_transport_support.hxx"

using namespace sf4e;
namespace protocol = sf4e::SessionProtocol;
using nlohmann::json;

namespace {
// A custom room with two admitted members on connections 1 and 2.
struct Room {
	MockTransport* transport = new MockTransport();
	SessionServer server{"practice", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport)};
	explicit Room(bool practice) {
		room::Rules rules;
		rules.roundTime = 60;
		server.EnableCustomRooms("Practice projection", 8, 31, rules, practice);
		for (session::Connection connection = 1; connection <= 2; ++connection) {
			protocol::SessionJoinRequest join;
			join.username = "Member-" + std::to_string(connection); join.sidecarHash = "build"; join.port = 30000;
			join.customRooms = true; join.roomProtocol = room::ProtocolVersion;
			protocol::SessionHelloMsg hello; hello.admission = json(join);
			transport->Push(connection, json(hello));
			CHECK(server.Step() == 0);
		}
	}
	// The last native projection sent to `connection`.
	protocol::LobbyData Projected(session::Connection connection) const {
		for (auto sent = transport->outgoing.rbegin(); sent != transport->outgoing.rend(); ++sent)
			if (sent->first == connection && sent->second.value("type", std::string()) == "data_update")
				return sent->second.at("lobbyData").get<protocol::LobbyData>();
		CHECK(false);
		return {};
	}
};
}

int main() {
	Room practice(true);
	CHECK(practice.server.RoomSnapshot()->practice && practice.server.RoomSnapshot()->tables[0].rules.roundTime == 60);
	for (session::Connection connection = 1; connection <= 2; ++connection) {
		const auto lobby = practice.Projected(connection);
		CHECK(lobby.trainingMode && lobby.roundTime.integral == room::PracticeRoundTime && lobby.roundTime.fractional == 0);
	}
	Room plain(false);
	CHECK(!plain.server.RoomSnapshot()->practice);
	for (session::Connection connection = 1; connection <= 2; ++connection) {
		const auto lobby = plain.Projected(connection);
		CHECK(!lobby.trainingMode && lobby.roundTime.integral == 60);
	}
	std::puts("Practice room projection passed.");
	return 0;
}
