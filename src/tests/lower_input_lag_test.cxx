// Lower input lag (issue #77) in a room: each fighter's choice at Ready, gone
// with every readiness reset; and the match's flag, on only when both chose
// it, as every member's native projection carries it.
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
// A custom room of `members` admitted members on connections 1 to `members`.
struct Room {
	MockTransport* transport = new MockTransport();
	SessionServer server{"lower-lag", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport)};
	std::uint64_t actionId = 0;
	explicit Room(int members) {
		// Both fighters readying starts the match, which needs each member's endpoint.
		std::array<std::uint8_t, 16> authorization = {};
		authorization[0] = 77;
		server.EnableMatchAuthorization(authorization, [](session::Connection connection) {
			std::string identity(64, '0');
			identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
			return identity;
		});
		server.EnableCustomRooms("Lower lag", 8, 77);
		for (session::Connection connection = 1; connection <= static_cast<session::Connection>(members); ++connection) {
			protocol::SessionJoinRequest join;
			join.username = "Member-" + std::to_string(connection); join.sidecarHash = "build"; join.port = 30000;
			join.customRooms = true; join.roomProtocol = room::ProtocolVersion;
			protocol::SessionHelloMsg hello; hello.admission = json(join);
			Send(connection, json(hello));
		}
		server.AdvanceCustomRoom(1000);
	}
	void Send(session::Connection connection, const json& message) {
		transport->Push(connection, message);
		CHECK(server.Step() == 0);
	}
	void Act(session::Connection connection, room::ActionKind kind, bool lowerLag = false) {
		const auto snapshot = *server.RoomSnapshot();
		room::Action action;
		action.kind = kind; action.roomEpoch = snapshot.roomEpoch; action.revision = snapshot.revision;
		action.tableRevision = snapshot.tables[0].revision; action.actionId = ++actionId; action.lowerLag = lowerLag;
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
	// The match's flag in the last native projection sent to `connection`.
	bool Projected(session::Connection connection) const {
		for (auto sent = transport->outgoing.rbegin(); sent != transport->outgoing.rend(); ++sent)
			if (sent->first == connection && sent->second.value("type", std::string()) == "data_update")
				return sent->second.at("matchData").get<protocol::MatchData>().lowerInputLag;
		CHECK(false);
		return false;
	}
};

// Both fighters on: on for them and for the member queued behind them.
// One on: off for all. Unready gives the seat's choice back, and the next
// Ready takes a fresh one.
void TestMatchFlag() {
	Room both(3);
	both.Seat();
	both.Act(1, room::ActionKind::Ready, true);
	CHECK(both.Table().lowerLag[0] && !both.Table().lowerLag[1] && !room::MatchLowerLag(both.Table()));
	both.Act(2, room::ActionKind::Ready, true);
	CHECK(both.Table().ready[1] && room::MatchLowerLag(both.Table()));
	for (session::Connection connection = 1; connection <= 3; ++connection) CHECK(both.Projected(connection));

	Room one(3);
	one.Seat();
	one.Act(1, room::ActionKind::Ready, true);
	one.Act(1, room::ActionKind::Unready);
	CHECK(!one.Table().ready[0] && !one.Table().lowerLag[0]);
	one.Act(1, room::ActionKind::Ready, true);
	one.Act(2, room::ActionKind::Ready, false);
	CHECK(one.Table().ready[0] && one.Table().ready[1] && one.Table().lowerLag[0] && !room::MatchLowerLag(one.Table()));
	for (session::Connection connection = 1; connection <= 3; ++connection) CHECK(!one.Projected(connection));
}

// A room's own record of the seats, with a host and two fighters at table 0.
struct Authority {
	room::RoomAuthority authority{"Lower lag", 8, 77};
	room::MemberId p1 = 0, p2 = 0;
	std::uint64_t actionId = 0;
	room::MemberId Join(const std::string& name, const std::string& user, bool host = false) {
		const auto result = authority.Join(name, room::ConnectionRef{"room", user}, host);
		CHECK(result.accepted);
		for (const auto& member : result.snapshot.members) if (member.connection.user == user) return member.id;
		return 0;
	}
	room::Action Make(room::MemberId, room::ActionKind kind) {
		const auto& snapshot = authority.SnapshotView();
		room::Action action;
		action.kind = kind; action.roomEpoch = snapshot.roomEpoch; action.revision = snapshot.revision;
		action.tableRevision = snapshot.tables[0].revision; action.actionId = ++actionId;
		return action;
	}
	bool Act(room::MemberId member, room::ActionKind kind) { return authority.Apply(member, Make(member, kind)).accepted; }
	bool Ready(room::MemberId member, bool lowerLag) {
		auto action = Make(member, room::ActionKind::Ready);
		action.lowerLag = lowerLag;
		return authority.Apply(member, action).accepted;
	}
	// Both fighters seated and readied with it on.
	void Seat() {
		authority.AdvanceTime(1000);
		Join("Host", "0", true);
		p1 = Join("P1", "1"); p2 = Join("P2", "2");
		CHECK(Act(p1, room::ActionKind::Queue) && Act(p2, room::ActionKind::Queue));
	}
	const room::Table& Table() const { return authority.SnapshotView().tables[0]; }
	bool Cleared() const { return !Table().lowerLag[0] && !Table().lowerLag[1] && !json(Table()).contains("lower_lag"); }
};

// Every readiness reset gives the seats' choices back with the Ready: a match
// ending, a fighter leaving or unqueueing, and a tournament start called off.
// The live match keeps them until it ends.
void TestReadinessResets() {
	{
		Authority room;
		room.Seat();
		CHECK(room.Ready(room.p1, true) && room.Ready(room.p2, true) && room::MatchLowerLag(room.Table()));
		CHECK(room.authority.BeginMatch(0, room.p1, room.p2).accepted && room::MatchLowerLag(room.Table()));
		CHECK(room.authority.EndMatch(0, room.Table().matchGeneration, room::MatchResult::P1Win).accepted);
		CHECK(room.Cleared());
	}
	{
		Authority room;
		room.Seat();
		CHECK(room.Ready(room.p1, true) && room.Table().lowerLag[0]);
		CHECK(room.Act(room.p1, room::ActionKind::Leave) && room.Cleared());
	}
	{
		Authority room;
		room.Seat();
		CHECK(room.Ready(room.p1, true) && room.Act(room.p2, room::ActionKind::Unqueue));
		CHECK(room.Table().p1 == room.p1 && room.Cleared());
	}
	{
		// A tournament table: both ready, one permit arrives, and its window
		// runs out before the other's; the start is called off.
		const std::string a(64, 'a'), b(64, 'b');
		Authority room;
		room.authority.AdvanceTime(1000);
		room.p1 = room.Join("A", a, true);
		room::TournamentBinding binding;
		binding.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
		binding.assignmentGeneration = 1; binding.bindingRevision = 1; binding.gamesToWin = 2;
		binding.fighters[0] = {a, "emb1_aaaa"}; binding.fighters[1] = {b, "emb1_bbbb"};
		CHECK(room.authority.BindTournament(binding).accepted);
		room.p2 = room.Join("B", b);
		CHECK(room.Ready(room.p1, true) && room.Ready(room.p2, true) && room.Table().permitGeneration);
		room.authority.AdvanceTime(11000);
		auto permit = room.Make(room.p1, room::ActionKind::PermitReady);
		permit.matchGeneration = room.Table().permitGeneration;
		permit.text = "per_one";
		permit.startWindowMs = 30000;
		CHECK(room.authority.Apply(room.p1, permit).accepted && room::MatchLowerLag(room.Table()));
		room.authority.AdvanceTime(1000 + 30000 - room::PermitStartMarginMs);
		CHECK(!room.Table().ready[0] && !room.Table().ready[1] && room.Table().permitGeneration == 0);
		CHECK(room.Cleared());
	}
}

// On the wire each is written only when on and reads as off when absent, so
// state and messages from builds before it read as before.
void TestWire() {
	room::Action ready;
	ready.kind = room::ActionKind::Ready;
	CHECK(!json(ready).contains("lower_lag") && !json(ready).get<room::Action>().lowerLag);
	ready.lowerLag = true;
	const json readyWire = ready;
	CHECK(readyWire.at("lower_lag") == true && readyWire.get<room::Action>().lowerLag);
	json wrongType = readyWire;
	wrongType["lower_lag"] = 1;
	bool refused = false;
	try { (void)wrongType.get<room::Action>(); } catch (const std::exception&) { refused = true; }
	CHECK(refused);

	room::Table table;
	CHECK(!json(table).contains("lower_lag"));
	table.lowerLag[1] = true;
	const json tableWire = table;
	CHECK(tableWire.at("lower_lag") == json::array({false, true}));
	const auto tableBack = tableWire.get<room::Table>();
	CHECK(!tableBack.lowerLag[0] && tableBack.lowerLag[1]);
	json shortList = tableWire;
	shortList["lower_lag"] = json::array({true});
	refused = false;
	try { (void)shortList.get<room::Table>(); } catch (const std::exception&) { refused = true; }
	CHECK(refused);
	json older = tableWire;
	older.erase("lower_lag");
	const auto olderBack = older.get<room::Table>();
	CHECK(!olderBack.lowerLag[0] && !olderBack.lowerLag[1]);

	protocol::MatchData match;
	match.lowerInputLag = true;
	CHECK(json(match).get<protocol::MatchData>().lowerInputLag);
	json olderMatch = match;
	olderMatch.erase("lowerInputLag");
	CHECK(!olderMatch.get<protocol::MatchData>().lowerInputLag);
	match.Clear();
	CHECK(!match.lowerInputLag);
}
}

int main() {
	TestMatchFlag();
	TestReadinessResets();
	TestWire();
	std::puts("LowerInputLag test passed");
	return 0;
}
