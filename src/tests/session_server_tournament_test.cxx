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

// A permit's window keeps running while this server is not writable, whether
// its coordination is healthy or not. After a long outage the owner calls the
// start off; after a short one the start still waits for the other permit.
static void TestPermitWindowThroughAnOutage() {
	for (const std::uint64_t outage : {std::uint64_t(100000), std::uint64_t(20000)}) {
		auto* transport = new MockTransport();
		SessionServer server("tournament", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport));
		std::array<std::uint8_t, 16> roomId = {};
		roomId[0] = 78;
		server.EnableMatchAuthorization(roomId, [](session::Connection connection) { return Endpoint(connection); });
		server.EnableCustomRooms("Match", 16, 51);
		std::uint64_t request = 1, revision = 0, actionId = 0;
		constexpr std::uint64_t term = 3;
		server.SetAuthority(term, revision, true);
		const auto commit = [&]() {
			CHECK(server.HasRecoveryCandidate());
			CHECK(server.ProposeCheckpoint(request, term, revision, nullptr));
			const auto proposal = server.PendingProposal();
			CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
			++request; ++revision;
		};
		for (const session::Connection connection : {session::Connection(1), session::Connection(2)}) {
			protocol::SessionJoinRequest join;
			join.username = "Fighter-" + std::to_string(connection); join.sidecarHash = "build"; join.port = 30000;
			join.customRooms = true; join.roomProtocol = room::ProtocolVersion;
			protocol::SessionHelloMsg hello; hello.admission = json(join);
			transport->Push(connection, json(hello));
			CHECK(server.Step() == 0);
			commit();
		}
		room::TournamentBinding binding;
		binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
		binding.assignmentGeneration = 1;
		binding.bindingRevision = 1;
		binding.gamesToWin = 3;
		binding.fighters[0] = {Endpoint(1), "emb1_one"};
		binding.fighters[1] = {Endpoint(2), "emb1_two"};
		CHECK(server.BindTournament(binding));
		commit();
		const auto send = [&](session::Connection by, room::ActionKind kind) {
			const auto& snapshot = server._roomAuthority->SnapshotView();
			room::Action action;
			action.kind = kind; action.roomEpoch = snapshot.roomEpoch; action.revision = snapshot.revision;
			action.table = room::TournamentTable; action.tableRevision = snapshot.tables[room::TournamentTable].revision;
			action.actionId = ++actionId;
			action.matchGeneration = snapshot.tables[room::TournamentTable].permitGeneration;
			action.text = "per_one";
			action.startWindowMs = room::PermitStartMs;
			protocol::RoomActionMessage message; message.action = action;
			transport->Push(by, json(message));
			CHECK(server.Step() == 0);
			commit();
		};
		// Both ready at 1000; the first permit 30 s later.
		server.AdvanceCustomRoom(1000);
		send(1, room::ActionKind::Ready);
		send(2, room::ActionKind::Ready);
		const auto reserved = server._roomAuthority->SnapshotView().tables[room::TournamentTable].permitGeneration;
		CHECK(reserved != 0);
		server.AdvanceCustomRoom(31000);
		send(1, room::ActionKind::PermitReady);
		// Coordination is lost, and the outage passes with no healthy time.
		server.SetAuthority(term, revision, false, false);
		server.AdvanceCustomRoom(31500);
		server.AdvanceCustomRoom(31500 + outage);
		CHECK(!server.HasRecoveryCandidate());
		// Writable again: the room's timer work runs on the next tick.
		server.SetAuthority(term, revision, true);
		server.AdvanceCustomRoom(32000 + outage);
		const auto& table = server._roomAuthority->SnapshotView().tables[room::TournamentTable];
		if (outage == 100000) {
			CHECK(server.HasRecoveryCandidate());
			CHECK(table.phase == room::TablePhase::Waiting && table.permitGeneration == 0);
			commit();
		} else {
			CHECK(!server.HasRecoveryCandidate());
			CHECK(table.phase == room::TablePhase::Ready && table.permitGeneration == reserved && table.permits[0] == "per_one");
		}
	}
}

int main() {
	TestBindingThroughTheServer();
	TestPermitWindowThroughAnOutage();
	std::printf("session server tournament tests passed\n");
	return 0;
}
