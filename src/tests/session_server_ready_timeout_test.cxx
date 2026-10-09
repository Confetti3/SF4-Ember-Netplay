// The Ready timeout as the session server carries it out: the fighter it
// unseats leaves the native projection exactly as an explicit unseat does, and
// the room's announcement of it reaches every member, at any table, only once
// its transition has committed.
#include "../session/sf4e__SessionServer.hxx"
#include <algorithm>
#include <string>
#include <vector>

#include "test_support.hxx"
#include "server_transport_support.hxx"

using namespace sf4e;
namespace protocol = sf4e::SessionProtocol;
using nlohmann::json;

namespace {
// A custom room of `members` admitted members on connections 1 to `members`.
// With recovery, every step is its own committed candidate unless the test
// holds it.
struct Room {
	MockTransport* transport = new MockTransport();
	SessionServer server{"ready-timeout", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport)};
	const bool recovery;
	const std::uint64_t term = 5;
	std::uint64_t request = 1, revision = 0, actionId = 0;
	Room(int members, bool recovery) : recovery(recovery) {
		server.EnableCustomRooms("Ready timeout", 8, 71);
		if (recovery) server.SetAuthority(term, revision, true);
		for (session::Connection connection = 1; connection <= static_cast<session::Connection>(members); ++connection) {
			protocol::SessionJoinRequest join;
			join.username = "Member-" + std::to_string(connection); join.sidecarHash = "build"; join.port = 30000;
			join.customRooms = true; join.roomProtocol = room::ProtocolVersion;
			protocol::SessionHelloMsg hello; hello.admission = json(join);
			Send(connection, json(hello));
		}
	}
	void Commit() {
		CHECK(server.HasRecoveryCandidate());
		CHECK(server.ProposeCheckpoint(request, term, revision, nullptr));
		const auto proposal = server.PendingProposal();
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		++request; ++revision;
	}
	void Send(session::Connection connection, const json& message) {
		transport->Push(connection, message);
		CHECK(server.Step() == 0);
		if (recovery && server.HasRecoveryCandidate()) Commit();
	}
	void Act(session::Connection connection, room::ActionKind kind, std::uint8_t table = 0) {
		const auto snapshot = *server.RoomSnapshot();
		room::Action action;
		action.kind = kind; action.roomEpoch = snapshot.roomEpoch; action.revision = snapshot.revision;
		action.table = table; action.tableRevision = snapshot.tables[table].revision; action.actionId = ++actionId;
		protocol::RoomActionMessage message; message.action = action;
		Send(connection, json(message));
	}
	room::MemberId Member(session::Connection connection) const { return server.roomMembers.at(connection); }
	// The last native projection sent to `connection`.
	json Projection(session::Connection connection) const {
		for (auto sent = transport->outgoing.rbegin(); sent != transport->outgoing.rend(); ++sent)
			if (sent->first == connection && sent->second.value("type", std::string()) == "data_update") return sent->second;
		return nullptr;
	}
	// The Ready timeout events sent to `connection`.
	std::vector<json> Timeouts(session::Connection connection) const {
		std::vector<json> found;
		for (const auto& sent : transport->outgoing)
			if (sent.first == connection && sent.second.value("type", std::string()) == "room_event" &&
				sent.second.at("event").at("kind") == static_cast<int>(room::Event::Kind::ReadyTimeout)) found.push_back(sent.second);
		return found;
	}
};

// Three members at table 0, the third queued behind the two fighters, who
// have chosen their fighters, P1's stage and seed. P1 is ready.
void SeatWithConditions(Room& room) {
	for (session::Connection connection = 1; connection <= 3; ++connection) room.Act(connection, room::ActionKind::Queue);
	for (session::Connection connection = 1; connection <= 2; ++connection) {
		protocol::PreBattleSetChara pick; pick.chara = protocol::MatchData{}.chara[0];
		pick.chara.charaID = connection == 1 ? 5 : 6;
		room.Send(connection, json(pick));
	}
	protocol::PreBattleSetStage stage; stage.stageID = selection::StageList().back().id;
	room.Send(1, json(stage));
	protocol::PreBattleSetEnv env; env.rngSeed = 1234;
	room.Send(1, json(env));
	room.Act(1, room::ActionKind::Ready);
	CHECK(room.server._roomMatchData[0].chara[1].charaID == 6 && room.server._roomMatchData[0].rngSeed == 1234);
}

// P2 leaving the seat by hand and P2 timed out leave the same table and the
// same native projection: the queued member moves up, and the previous pair's
// fighters, stage and seed are gone.
void TestTimeoutProjectsLikeAnExplicitUnseat() {
	Room explicitUnseat(3, false), timedOut(3, false);
	for (auto* room : {&explicitUnseat, &timedOut}) {
		room->server.AdvanceCustomRoom(1000);
		SeatWithConditions(*room);
		room->transport->outgoing.clear();
	}
	explicitUnseat.Act(2, room::ActionKind::Unqueue);
	timedOut.server.AdvanceCustomRoom(1000 + room::ReadyTimeoutMs);
	for (auto* room : {&explicitUnseat, &timedOut}) {
		const auto& table = room->server.RoomSnapshot()->tables[0];
		CHECK(table.p1 == room->Member(1) && table.p2 == room->Member(3) && table.queue.empty());
		CHECK(!table.ready[0] && !table.ready[1]);
	}
	CHECK(timedOut.Timeouts(1).size() == 1 && explicitUnseat.Timeouts(1).empty());
	for (session::Connection connection = 1; connection <= 3; ++connection) {
		const auto expected = explicitUnseat.Projection(connection), actual = timedOut.Projection(connection);
		CHECK(!expected.is_null() && expected == actual);
	}
	const auto projected = timedOut.Projection(1);
	const auto& members = projected.at("lobbyData").at("members");
	CHECK(members.size() == 2 && members.at(0).at("name") == "Member-1" && members.at(1).at("name") == "Member-3");
	CHECK(projected.at("matchData") == json(protocol::MatchData{}));
}

// The announcement is room-wide: a member at another table hears it too. It is
// journaled with the transition, sent only when that commits, and kept for a
// successor's replay. A discarded transition sends nothing and keeps the
// deadline's age from before it, so it fires again a tick later.
void TestTimeoutReachesEveryMemberOnCommit() {
	Room room(4, true);
	room.server.AdvanceCustomRoom(1000);
	room.Act(1, room::ActionKind::Queue); room.Act(2, room::ActionKind::Queue);
	room.Act(3, room::ActionKind::Queue, 1);
	room.Act(1, room::ActionKind::Ready);
	const auto timedOut = room.Member(2);
	room.transport->outgoing.clear();

	room.server.AdvanceCustomRoom(999 + room::ReadyTimeoutMs);
	CHECK(!room.server.HasRecoveryCandidate());
	room.server.AdvanceCustomRoom(1000 + room::ReadyTimeoutMs);
	CHECK(room.server.HasRecoveryCandidate() && room.transport->outgoing.empty());
	CHECK(room.server.ProposeCheckpoint(room.request, room.term, room.revision, nullptr));
	room.server.DiscardProposal();
	CHECK(room.transport->outgoing.empty() && room.server.RoomSnapshot()->tables[0].p2 == timedOut);

	room.server.AdvanceCustomRoom(2000 + room::ReadyTimeoutMs);
	CHECK(!room.server.HasRecoveryCandidate() && room.server.RoomSnapshot()->tables[0].p2 == timedOut);
	room.server.AdvanceCustomRoom(2001 + room::ReadyTimeoutMs);
	CHECK(room.server.HasRecoveryCandidate() && room.transport->outgoing.empty());
	room.Commit();
	CHECK(room.server.RoomSnapshot()->tables[0].p2 == 0);
	for (session::Connection connection = 1; connection <= 4; ++connection) {
		const auto timeouts = room.Timeouts(connection);
		CHECK(timeouts.size() == 1);
		if (timeouts.size() != 1) continue;
		CHECK(timeouts[0].at("event").at("member") == timedOut && timeouts[0].at("event").at("table") == 0);
		CHECK(timeouts[0].contains("_commit"));
		const auto recipient = room.Member(connection);
		CHECK(std::count_if(room.server.CommittedEffectHistory().begin(), room.server.CommittedEffectHistory().end(),
			[&](const session::EffectEnvelope& effect) {
				return effect.recipient == recipient && effect.type == "room_event" && !effect.privatePayload &&
					effect.publicPayload.value("event", json::object()).value("kind", -1) == static_cast<int>(room::Event::Kind::ReadyTimeout);
			}) == 1);
	}
}
}

int main() {
	TestTimeoutProjectsLikeAnExplicitUnseat();
	TestTimeoutReachesEveryMemberOnCommit();
	std::puts("SessionServerReadyTimeout test passed");
	return 0;
}
