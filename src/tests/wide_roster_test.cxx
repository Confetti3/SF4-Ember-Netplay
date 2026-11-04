// The shared code under another game's catalog: 45 fighters, fewer costumes,
// colours and Ultras than SF4's, and 40 stages numbered 0 to 39. Fighter 44
// and stage 35, which SF4's catalog refuses, must pass the room model, its
// JSON, the session protocol and the session server; the next ones past the
// catalog's limits must not. This executable links this catalog instead of
// SF4's (cmake/sf4e_test_targets.cmake), as another game's binaries would.

#include "session_server_transport_support.hxx"
#include "room_authority_support.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/GameCatalog.hxx"
#include "../netplay/PlayerPreferences.hxx"

namespace {
constexpr int Fighters = 45, Costumes = 12, Colors = 30, Ultras = 1, Stages = 40;

bool WidePick(const sf4e::selection::Pick& pick, bool) {
	return pick.fighter >= 0 && pick.fighter < Fighters && pick.costume >= 0 && pick.costume < Costumes &&
		pick.color >= 0 && pick.color < Colors && pick.ultra >= 0 && pick.ultra < Ultras;
}
bool WideStage(std::int64_t id) { return id >= 0 && id < Stages; }

constexpr ember::GameCatalog Wide = {Fighters, Costumes, Colors, Ultras, &WidePick, &WideStage, {0, 11, 22, 44}};
}

namespace ember {
const GameCatalog& Catalog() { return Wide; }
}

static void TestRoomModel() {
	RoomAuthority authority("Wide roster", 8, 1);
	const auto host = Join(authority, 0, true);
	MemberProfile wide;
	wide.mainFighter = 44;
	CHECK(authority.Join("Last fighter", Peer(1), false, wide).accepted);
	wide.mainFighter = 45;
	CHECK(!authority.Join("Past the roster", Peer(2), false, wide).accepted);

	Chara pick;
	pick.fighter = 44; pick.costume = 11; pick.color = 29; pick.ultra = 0;
	CHECK(authority.SetMemberChara(host, pick));
	const auto shown = [&] { return authority.SnapshotView().members.front(); };
	CHECK(shown().fighter == 44 && shown().costume == 11 && shown().color == 29 && shown().ultra == 0);
	// Past this catalog's limits the look reads as not shared; SF4's would show them.
	pick.costume = 12; pick.color = 30; pick.ultra = 1;
	CHECK(authority.SetMemberChara(host, pick));
	CHECK(shown().fighter == 44 && shown().costume == -1 && shown().color == -1 && shown().ultra == -1);
	pick.fighter = 45;
	CHECK(!authority.SetMemberChara(host, pick) && shown().fighter == 44);

	nlohmann::json member = shown();
	member["main_fighter"] = 44;
	member["costume"] = 11; member["color"] = 29; member["ultra"] = 0;
	const auto read = member.get<Member>();
	CHECK(read.fighter == 44 && read.mainFighter == 44 && read.costume == 11 && read.color == 29);
	for (const char* key : {"fighter", "main_fighter"}) {
		auto past = member;
		past[key] = 45;
		bool rejected = false;
		try { past.get<Member>(); } catch (const std::exception&) { rejected = true; }
		CHECK(rejected);
	}
	for (const char* key : {"costume", "color", "ultra"}) {
		auto past = member;
		past[key] = std::string(key) == "costume" ? Costumes : std::string(key) == "color" ? Colors : Ultras;
		bool rejected = false;
		try { past.get<Member>(); } catch (const std::exception&) { rejected = true; }
		CHECK(rejected);
	}
}

static void TestProtocol() {
	protocol::LobbyData lobby;
	protocol::MatchData match;
	match.chara[0].charaID = 44;
	match.chara[1].charaID = 43;
	match.stageID = 35;
	CHECK(protocol::FindMatchProblem(lobby, match) == protocol::MatchProblem::None);
	match.stageID = Stages;
	CHECK(protocol::FindMatchProblem(lobby, match) == protocol::MatchProblem::Stage);
	match.stageID = 35;
	match.chara[1].charaID = 45;
	CHECK(protocol::FindMatchProblem(lobby, match) == protocol::MatchProblem::Fighter);

	protocol::PreBattleSetStage set;
	set.stageID = 35;
	json stage = set;
	CHECK(stage.get<protocol::PreBattleSetStage>().stageID == 35);
	stage["stageID"] = Stages;
	bool rejected = false;
	try { stage.get<protocol::PreBattleSetStage>(); } catch (const json::exception&) { rejected = true; }
	CHECK(rejected);

	netplay::PlayerPreferences preferences;
	preferences.displayName = "Wide";
	preferences.mainFighter = 44;
	CHECK(preferences.Valid());
	preferences.mainFighter = 45;
	CHECK(!preferences.Valid());
}

static void TestSessionServer() {
	auto* transport = new MockTransport();
	SessionServer server("custom-room", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> room = {};
	room[0] = 1;
	server.EnableMatchAuthorization(room, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Wide roster", 16, 9);
	CHECK(server.Listen(0) == 0);
	const auto step = [&] { CHECK(server.Step() == 0); };
	const auto join = [&](session::Connection connection, int mainFighter) {
		transport->Push(connection, json(protocol::SessionHelloMsg()));
		step();
		protocol::SessionJoinRequest request;
		request.username = "Member" + std::to_string(connection);
		request.sidecarHash = "build";
		request.port = 30000;
		request.customRooms = true;
		request.roomProtocol = room::ProtocolVersion;
		json requestJson = request;
		requestJson["mainFighter"] = mainFighter;
		transport->Push(connection, requestJson);
		step();
		return server.roomMembers.count(connection) == 1;
	};
	CHECK(join(1, 44));
	CHECK(!join(2, 45));
	const auto fighterOf = [&](session::Connection connection) {
		for (const auto& m : server.RoomSnapshot()->members) if (m.id == server.roomMembers.at(connection)) return m.fighter;
		return -2;
	};
	CHECK(server.RoomSnapshot()->members.front().mainFighter == 44);
	// Only a seated player shows a fighter.
	room::Action queue;
	const auto* snapshot = server.RoomSnapshot();
	queue.kind = room::ActionKind::Queue;
	queue.roomEpoch = snapshot->roomEpoch;
	queue.revision = snapshot->revision;
	queue.table = 0;
	queue.tableRevision = snapshot->tables[0].revision;
	queue.actionId = 1;
	protocol::RoomActionMessage message;
	message.action = queue;
	transport->Push(1, json(message));
	step();
	protocol::PreBattleSetChara shown;
	shown.chara = protocol::MatchData{}.chara[0];
	shown.chara.charaID = 44;
	transport->Push(1, json(shown));
	step();
	CHECK(fighterOf(1) == 44);
	shown.chara.charaID = 45;
	transport->Push(1, json(shown));
	step();
	CHECK(fighterOf(1) == 44);
}

int main() {
	TestRoomModel();
	TestProtocol();
	TestSessionServer();
	return TEST_EXIT_CODE();
}
