#pragma once
#include "../session/sf4e__SessionServer.hxx"
#include "../session/RoomRecoveryRuntime.hxx"
#include "../session/RoomMessageTag.hxx"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>

#include "test_support.hxx"
#include "server_transport_support.hxx"
#include "fake_helper_pipe.hxx"

using namespace sf4e;
namespace protocol = sf4e::SessionProtocol;
using nlohmann::json;

inline void CheckIdleRecoveryWork(SessionServer& server, const char* phase) {
	server.AdvanceCustomRoom(0);
	CHECK(server.Step()==0 && !server.HasRecoveryCandidate());
	const auto before=server.RecoveryCheckpoint();
	const auto builds=server.RecoveryCheckpointBuilds();
	const auto started=std::chrono::steady_clock::now();
	for(int tick=0;tick<240;++tick) {
		server.AdvanceCustomRoom(0);
		CHECK(server.Step()==0 && !server.HasRecoveryCandidate());
	}
	const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
	const auto built=server.RecoveryCheckpointBuilds()-builds;
	std::cout << "Idle recovery " << phase << ": ticks=240 checkpoint_builds=" << built
		<< " total_ms=" << elapsed << " mean_ms=" << elapsed/240 << std::endl;
	CHECK(server.RecoveryCheckpoint()==before);
	CHECK(built==0);
}

// Two fighters and a spectator at table 0 of a room whose replicas import
// every recovery checkpoint. Connections 16 apart share an authenticated
// endpoint, as one player rejoining does; each has its own incarnation.
enum class SetupStage { Preparing, P1Prepared, FightersPrepared, AllPrepared, Started };

struct ParticipantRoom {
	MockTransport* transport = new MockTransport();
	SessionServer server{"custom-room", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport)};
	std::array<std::uint8_t, 16> room{{1}};
	std::uint64_t actionId = 0, generation = 0;
	static std::string IdentityOf(session::Connection connection) {
		std::string identity(64, '0'); identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15]; return identity;
	}
	static std::uint64_t IncarnationOf(session::Connection connection) { return connection; }

	explicit ParticipantRoom(SetupStage stage) {
		server.EnableMatchAuthorization(room, IdentityOf, IncarnationOf);
		server.EnableCustomRooms("Departed participant", 16, 9);
		CHECK(server.Listen(0) == 0);
		for (session::Connection connection = 1; connection <= 4; ++connection) Join(connection);
		Act(2, room::ActionKind::Queue); Act(3, room::ActionKind::Queue); Act(4, room::ActionKind::Watch);
		Act(2, room::ActionKind::Ready); Act(3, room::ActionKind::Ready);
		generation = server.RoomSnapshot()->tables[0].matchGeneration;
		if (stage >= SetupStage::P1Prepared) Acknowledge(2, "game_prepared", {{"spectators_optional", true}});
		if (stage >= SetupStage::FightersPrepared) Acknowledge(3, "game_prepared");
		if (stage >= SetupStage::AllPrepared) Acknowledge(4, "game_prepared");
		if (stage >= SetupStage::Started) { Acknowledge(2, "game_ready"); Acknowledge(3, "game_ready"); }
	}
	void Step() { CHECK(server.Step() == 0); }
	void Join(session::Connection connection) {
		transport->Push(connection, json(protocol::SessionHelloMsg())); Step();
		protocol::SessionJoinRequest request;
		request.username = "Member" + std::to_string(connection); request.sidecarHash = "build"; request.port = 30000;
		request.customRooms = true; request.roomProtocol = room::ProtocolVersion;
		transport->Push(connection, json(request)); Step();
		CHECK(server.roomMembers.count(connection) == 1);
	}
	void Act(session::Connection connection, room::ActionKind kind) {
		const auto* snapshot = server.RoomSnapshot();
		protocol::RoomActionMessage message;
		message.action.kind = kind; message.action.roomEpoch = snapshot->roomEpoch; message.action.revision = snapshot->revision;
		message.action.table = 0; message.action.tableRevision = snapshot->tables[0].revision; message.action.actionId = ++actionId;
		transport->Push(connection, json(message)); Step();
	}
	void Acknowledge(session::Connection connection, const char* type, json extra = json::object()) {
		extra["type"] = type; extra["generation"] = generation; transport->Push(connection, extra); Step();
	}
	void Depart(session::Connection connection, bool disconnect) {
		if (disconnect) { transport->disconnected.push_back(connection); Step(); }
		else Act(connection, room::ActionKind::Leave);
		CHECK(server.roomMembers.count(connection) == 0);
	}
	static std::size_t FrozenRows(const json& checkpoint) {
		return std::count_if(checkpoint.at("members").begin(), checkpoint.at("members").end(),
			[](const json& row) { return row.value("frozen", false); });
	}
	// A replica restores the checkpoint and rebinds every active row, as a
	// member's recovery import does before it activates the commit.
	static bool Imports(SessionServer& replica, const json& checkpoint) {
		if (!replica.RestoreRecoveryCheckpoint(checkpoint)) return false;
		std::vector<SessionServer::StableRebind> bindings;
		session::Connection handle = 200;
		for (const auto& row : checkpoint.at("members")) {
			if (row.value("frozen", false)) continue;
			bindings.emplace_back(row.at("member").get<room::MemberId>(), handle++,
				row.at("data").get<protocol::MemberData>().connId, row.at("incarnation").get<std::uint64_t>());
		}
		return replica.RebindMembers(bindings);
	}
	std::unique_ptr<SessionServer> Replica() const {
		auto replica = std::make_unique<SessionServer>("custom-room", "build", true, 3, Dimps::Math::FixedPoint{0, 99},
			std::unique_ptr<session::ServerTransport>(new MockTransport()));
		replica->EnableMatchAuthorization(room, IdentityOf, IncarnationOf);
		return replica;
	}
	bool Imports(const json& checkpoint) const { return Imports(*Replica(), checkpoint); }
};
