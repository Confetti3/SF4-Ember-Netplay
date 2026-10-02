// The session server binding the room it leads to a tournament match: the
// change goes through a recovery checkpoint like any other, strangers are sent
// away, and only the bound endpoints can join afterwards.
#include "../session/sf4e__SessionServer.hxx"

#include <array>
#include <string>

#include "server_transport_support.hxx"

using namespace sf4e;
namespace protocol = sf4e::SessionProtocol;
using nlohmann::json;

static std::string Endpoint(session::Connection connection) {
	return std::string(64, "0abcdef"[static_cast<std::size_t>(connection) % 7]);
}

static void TestBindingThroughTheServer() {
	auto* transport = new MockTransport();
	SessionServer server("tournament", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> roomId = {};
	roomId[0] = 77;
	server.EnableMatchAuthorization(roomId, [](session::Connection connection) { return Endpoint(connection); });
	server.EnableCustomRooms("Match", 16, 51);
	std::uint64_t request = 1, revision = 0;
	constexpr std::uint64_t term = 3;
	server.SetAuthority(term, revision, true);
	const auto commit = [&]() {
		CHECK(server.HasRecoveryCandidate());
		CHECK(server.ProposeCheckpoint(request, term, revision, nullptr));
		const auto proposal = server.PendingProposal();
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		++request; ++revision;
	};
	const auto admit = [&](session::Connection connection) {
		protocol::SessionJoinRequest join;
		join.username = "Fighter-" + std::to_string(connection); join.sidecarHash = "build"; join.port = 30000;
		join.customRooms = true; join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello; hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
	};
	admit(1); commit();
	admit(2); commit();
	admit(3); commit();
	CHECK(server.RoomSnapshot()->members.size() == 3);

	room::TournamentBinding binding;
	binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
	binding.assignmentGeneration = 1;
	binding.bindingRevision = 1;
	binding.gamesToWin = 3;
	binding.fighters[0] = {Endpoint(1), "emb1_one"};
	binding.fighters[1] = {Endpoint(2), "emb1_two"};
	transport->outgoing.clear();
	// A quorum round in progress makes the server try again later.
	admit(4);
	CHECK(!server.BindTournament(binding));
	commit();
	transport->outgoing.clear();
	CHECK(server.BindTournament(binding));
	commit();
	const auto& bound = *server.RoomSnapshot();
	CHECK(bound.tournament.Active() && bound.tournament.gamesToWin == 3);
	CHECK(bound.members.size() == 2);
	CHECK(bound.tables[room::TournamentTable].p1 && bound.tables[room::TournamentTable].p2);
	CHECK(bound.tables[room::TournamentTable].rules.format == room::SetFormat::Ft3);
	// The strangers were told they were removed, and everyone saw the room.
	bool removed = false, broadcast = false;
	for (const auto& sent : transport->outgoing) {
		if ((sent.first == 3 || sent.first == 4) && sent.second.value("type", std::string()) == "room_result" &&
			sent.second.at("result").value("reason", 0) == static_cast<int>(room::RejectReason::MemberKicked)) removed = true;
		if (sent.first == 2 && sent.second.value("type", std::string()) == "room_snapshot") broadcast = true;
	}
	CHECK(removed && broadcast);
	// A stranger cannot join a bound room.
	transport->outgoing.clear();
	admit(5);
	CHECK(server.RoomSnapshot()->members.size() == 2);
	// An older binding is refused.
	auto older = binding;
	older.bindingRevision = 0;
	CHECK(!server.BindTournament(older));
}

int main() {
	TestBindingThroughTheServer();
	std::printf("session server tournament tests passed\n");
	return 0;
}
