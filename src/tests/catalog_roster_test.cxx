// The shared code under game catalogs other than SF4's (fake_catalog.hxx).
// Under a wide one (60 fighters, 100 colours, fewer costumes and Ultras than
// SF4, and 40 stages numbered 0 to 39), fighter 59, colour 99 and stage 35,
// which SF4's catalog refuses, pass the room model, its JSON, the room host's
// status, the session protocol and the session server, and the values past
// each limit do not. Under a narrow one (40 fighters), an older peer's fighter
// 43, which SF4 accepts, is refused. This executable defines Catalog() itself
// instead of linking sf4e_catalog (cmake/sf4e_test_targets.cmake), as another
// game's binaries would, and switches between the two.

#include "fake_catalog.hxx"
#include "session_server_transport_support.hxx"
#include "room_authority_support.hxx"
#include "../../server/roomhost/RoomHostStatus.hxx"
#include "../common/GameCatalog.hxx"
#include "../common/SelectionPick.hxx"
#include "../netplay/PlayerPreferences.hxx"

namespace {
constexpr int Stages = ember::test::CatalogStages;
constexpr int LastFighter = ember::test::CatalogFighters - 1;
constexpr int LastCostume = ember::test::CatalogCostumes - 1;
constexpr int LastColor = ember::test::CatalogColors - 1;

constexpr ember::GameCatalog Wide = ember::test::TestCatalog(ember::test::CatalogFighters);
constexpr ember::GameCatalog Narrow = ember::test::TestCatalog(40);
static_assert(ember::Usable(Wide) && ember::Usable(Narrow), "The test catalogs must be usable");
ember::GameCatalog current = Wide;
}

namespace ember {
const GameCatalog& Catalog() { return current; }
}

template <class T> static bool Refused(const nlohmann::json& value) {
	try { value.get<T>(); } catch (const std::exception&) { return true; }
	return false;
}

// A custom room whose members join with a main fighter and show the fighter
// they pick once seated.
struct ServerRoom {
	MockTransport* transport = new MockTransport();
	SessionServer server{"custom-room", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport)};
	ServerRoom() {
		std::array<std::uint8_t, 16> room = {};
		room[0] = 1;
		server.EnableMatchAuthorization(room, [](session::Connection connection) {
			std::string identity(64, '0');
			identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
			return identity;
		});
		server.EnableCustomRooms("Catalog roster", 16, 9);
		CHECK(server.Listen(0) == 0);
	}
	void Step() { CHECK(server.Step() == 0); }
	bool Join(session::Connection connection, const json& mainFighter) {
		transport->Push(connection, json(protocol::SessionHelloMsg()));
		Step();
		protocol::SessionJoinRequest request;
		request.username = "Member" + std::to_string(connection);
		request.sidecarHash = "build";
		request.port = 30000;
		request.customRooms = true;
		request.roomProtocol = room::ProtocolVersion;
		json requestJson = request;
		requestJson["mainFighter"] = mainFighter;
		transport->Push(connection, requestJson);
		Step();
		return server.roomMembers.count(connection) == 1;
	}
	// Only a seated player shows a fighter.
	void Seat(session::Connection connection) {
		const auto* snapshot = server.RoomSnapshot();
		room::Action queue;
		queue.kind = room::ActionKind::Queue;
		queue.roomEpoch = snapshot->roomEpoch;
		queue.revision = snapshot->revision;
		queue.table = 0;
		queue.tableRevision = snapshot->tables[0].revision;
		queue.actionId = connection;
		protocol::RoomActionMessage message;
		message.action = queue;
		transport->Push(connection, json(message));
		Step();
	}
	// The pick the seat shows, as the game's Fighter Select sends it.
	void Show(session::Connection connection, int fighter, int color = 0) {
		protocol::PreBattleSetChara shown;
		shown.chara = protocol::MatchData{}.chara[0];
		shown.chara.charaID = static_cast<std::uint8_t>(fighter);
		shown.chara.color = static_cast<std::uint8_t>(color);
		transport->Push(connection, json(shown));
		Step();
	}
	const Member& Shown(session::Connection connection) const {
		const auto& members = server.RoomSnapshot()->members;
		return *std::find_if(members.begin(), members.end(),
			[&](const Member& m) { return m.id == server.roomMembers.at(connection); });
	}
};

static void TestWideRoomModel() {
	RoomAuthority authority("Wide roster", 8, 1);
	const auto host = Join(authority, 0, true);
	MemberProfile profile;
	profile.mainFighter = LastFighter;
	CHECK(authority.Join("Last fighter", Peer(1), false, profile).accepted);
	profile.mainFighter = LastFighter + 1;
	CHECK(!authority.Join("Past the roster", Peer(2), false, profile).accepted);
	const auto details = roomhost::DetailsOf(authority.SnapshotView());
	CHECK(std::find(details.fighters.begin(), details.fighters.end(), LastFighter) != details.fighters.end());

	Chara pick;
	pick.fighter = LastFighter; pick.costume = LastCostume; pick.color = LastColor; pick.ultra = 0;
	CHECK(authority.SetMemberChara(host, pick));
	const auto shown = [&] { return authority.SnapshotView().members.front(); };
	CHECK(shown().fighter == LastFighter && shown().costume == LastCostume && shown().color == LastColor && shown().ultra == 0);
	// Past this catalog's limits the look reads as not shared.
	pick.costume = LastCostume + 1; pick.color = LastColor + 1; pick.ultra = 1;
	CHECK(authority.SetMemberChara(host, pick));
	CHECK(shown().fighter == LastFighter && shown().costume == -1 && shown().color == -1 && shown().ultra == -1);
	pick.fighter = LastFighter + 1;
	CHECK(!authority.SetMemberChara(host, pick) && shown().fighter == LastFighter);

	nlohmann::json member = shown();
	member["main_fighter"] = LastFighter;
	member["costume"] = LastCostume; member["color"] = LastColor; member["ultra"] = 0;
	const auto read = member.get<Member>();
	CHECK(read.fighter == LastFighter && read.mainFighter == LastFighter && read.costume == LastCostume && read.color == LastColor);
	for (const char* key : {"fighter", "main_fighter"}) {
		auto past = member;
		past[key] = LastFighter + 1;
		CHECK(Refused<Member>(past));
	}
	const std::pair<const char*, int> limits[] = {{"costume", LastCostume + 1}, {"color", LastColor + 1}, {"ultra", 1}};
	for (const auto& limit : limits) {
		auto past = member;
		past[limit.first] = limit.second;
		CHECK(Refused<Member>(past));
	}

	// A winner who keeps the last fighter at a table.
	nlohmann::json table = authority.SnapshotView().tables[0];
	table["keeper"] = host;
	table["kept_fighter"] = LastFighter;
	CHECK(table.get<Table>().keptFighter == LastFighter);
	table["kept_fighter"] = LastFighter + 1;
	CHECK(Refused<Table>(table));
}

static void TestWideProtocol() {
	protocol::LobbyData lobby;
	protocol::MatchData match;
	match.chara[0].charaID = LastFighter;
	match.chara[0].color = LastColor;
	match.chara[1].charaID = 43;
	match.stageID = 35;
	CHECK(protocol::FindMatchProblem(lobby, match) == protocol::MatchProblem::None);
	match.stageID = Stages;
	CHECK(protocol::FindMatchProblem(lobby, match) == protocol::MatchProblem::Stage);
	match.stageID = 35;
	match.chara[0].color = LastColor + 1;
	CHECK(protocol::FindMatchProblem(lobby, match) == protocol::MatchProblem::Fighter);
	match.chara[0].color = LastColor;
	match.chara[1].charaID = LastFighter + 1;
	CHECK(protocol::FindMatchProblem(lobby, match) == protocol::MatchProblem::Fighter);

	protocol::PreBattleSetStage set;
	set.stageID = 35;
	json stage = set;
	CHECK(stage.get<protocol::PreBattleSetStage>().stageID == 35);
	stage["stageID"] = Stages;
	CHECK(Refused<protocol::PreBattleSetStage>(stage));

	netplay::PlayerPreferences preferences;
	preferences.displayName = "Wide";
	preferences.mainFighter = LastFighter;
	CHECK(preferences.Valid());
	preferences.mainFighter = LastFighter + 1;
	CHECK(!preferences.Valid());
}

static void TestWideSessionServer() {
	ServerRoom room;
	// Signed and unsigned JSON integers take different branches of the check.
	CHECK(room.Join(1, json(LastFighter)));
	CHECK(room.Join(2, json(static_cast<unsigned>(LastFighter))));
	CHECK(!room.Join(3, json(LastFighter + 1)));
	CHECK(!room.Join(4, json(static_cast<unsigned>(LastFighter + 1))));
	CHECK(room.server.RoomSnapshot()->members.front().mainFighter == LastFighter);
	room.Seat(1);
	// The session server takes the pick (the game's pick rule), and the room shows it.
	room.Show(1, LastFighter, LastColor);
	CHECK(room.Shown(1).fighter == LastFighter && room.Shown(1).color == LastColor);
	room.Show(1, LastFighter + 1);
	CHECK(room.Shown(1).fighter == LastFighter && room.Shown(1).color == LastColor);
	room.Show(1, 0, LastColor + 1);
	CHECK(room.Shown(1).fighter == LastFighter && room.Shown(1).color == LastColor);
}


// A peer on SF4's roster sends fighter 43 to a game with 40 fighters.
static void TestNarrowRoster() {
	current = Narrow;
	RoomAuthority authority("Narrow roster", 8, 2);
	const auto host = Join(authority, 0, true);
	MemberProfile profile;
	profile.mainFighter = 43;
	CHECK(!authority.Join("Older peer", Peer(1), false, profile).accepted);
	Chara pick;
	pick.fighter = 43;
	CHECK(!authority.SetMemberChara(host, pick));
	pick.fighter = 39;
	CHECK(authority.SetMemberChara(host, pick));
	nlohmann::json member = authority.SnapshotView().members.front();
	member["fighter"] = 43;
	CHECK(Refused<Member>(member));

	protocol::LobbyData lobby;
	protocol::MatchData match;
	match.chara[0].charaID = 43;
	match.stageID = 1;
	CHECK(protocol::FindMatchProblem(lobby, match) == protocol::MatchProblem::Fighter);

	// Connection 1 hosts the room, so it joins first.
	ServerRoom room;
	CHECK(room.Join(1, json(39)));
	CHECK(!room.Join(2, json(43)) && !room.Join(3, json(43u)));
	room.Seat(1);
	room.Show(1, 43);
	CHECK(room.Shown(1).fighter == -1);
	room.Show(1, 39);
	CHECK(room.Shown(1).fighter == 39);
	current = Wide;
}

int main() {
	TestWideRoomModel();
	TestWideProtocol();
	TestWideSessionServer();
	TestNarrowRoster();
	return TEST_EXIT_CODE();
}
