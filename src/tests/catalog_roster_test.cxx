// The shared code under game catalogs other than SF4's. Under a wide one (45
// fighters, fewer costumes, colours and Ultras than SF4, and 40 stages
// numbered 0 to 39), fighter 44 and stage 35, which SF4's catalog refuses,
// pass the room model, its JSON, the room host's status, the session protocol
// and the session server, and the values past each limit do not. Under a
// narrow one (40 fighters), an older peer's fighter 43, which SF4 accepts, is
// refused. This executable defines Catalog() itself instead of linking
// sf4e_catalog (cmake/sf4e_test_targets.cmake), as another game's binaries
// would, and switches between the two.

#include "session_server_transport_support.hxx"
#include "room_authority_support.hxx"
#include "../../server/roomhost/RoomHostStatus.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/GameCatalog.hxx"
#include "../netplay/PlayerPreferences.hxx"

namespace {
constexpr int Stages = 40;

// Any fighter of the catalog in a look within its limits.
bool CatalogPick(const sf4e::selection::Pick& pick, bool) {
	const ember::GameCatalog& catalog = ember::Catalog();
	return pick.fighter >= 0 && pick.fighter < catalog.fighterCount && pick.costume >= 0 && pick.costume < catalog.costumeLimit &&
		pick.color >= 0 && pick.color < catalog.colorLimit && pick.ultra >= 0 && pick.ultra < catalog.ultraChoices;
}
bool CatalogStage(std::int64_t id) { return id >= 0 && id < Stages; }

constexpr ember::GameCatalog Wide = {45, 12, 30, 1, &CatalogPick, &CatalogStage, {0, 11, 22, 44}};
constexpr ember::GameCatalog Narrow = {40, 12, 30, 1, &CatalogPick, &CatalogStage, {0, 11, 22, 33}};
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
	void Show(session::Connection connection, int fighter) {
		protocol::PreBattleSetChara shown;
		shown.chara = protocol::MatchData{}.chara[0];
		shown.chara.charaID = static_cast<std::uint8_t>(fighter);
		transport->Push(connection, json(shown));
		Step();
	}
	int FighterOf(session::Connection connection) const {
		for (const auto& m : server.RoomSnapshot()->members) if (m.id == server.roomMembers.at(connection)) return m.fighter;
		return -2;
	}
};

static void TestWideRoomModel() {
	RoomAuthority authority("Wide roster", 8, 1);
	const auto host = Join(authority, 0, true);
	MemberProfile profile;
	profile.mainFighter = 44;
	CHECK(authority.Join("Last fighter", Peer(1), false, profile).accepted);
	profile.mainFighter = 45;
	CHECK(!authority.Join("Past the roster", Peer(2), false, profile).accepted);
	const auto details = roomhost::DetailsOf(authority.SnapshotView());
	CHECK(std::find(details.fighters.begin(), details.fighters.end(), 44) != details.fighters.end());

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
		CHECK(Refused<Member>(past));
	}
	const std::pair<const char*, int> limits[] = {{"costume", 12}, {"color", 30}, {"ultra", 1}};
	for (const auto& limit : limits) {
		auto past = member;
		past[limit.first] = limit.second;
		CHECK(Refused<Member>(past));
	}

	// A winner who keeps fighter 44 at a table.
	nlohmann::json table = authority.SnapshotView().tables[0];
	table["keeper"] = host;
	table["kept_fighter"] = 44;
	CHECK(table.get<Table>().keptFighter == 44);
	table["kept_fighter"] = 45;
	CHECK(Refused<Table>(table));
}

static void TestWideProtocol() {
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
	CHECK(Refused<protocol::PreBattleSetStage>(stage));

	netplay::PlayerPreferences preferences;
	preferences.displayName = "Wide";
	preferences.mainFighter = 44;
	CHECK(preferences.Valid());
	preferences.mainFighter = 45;
	CHECK(!preferences.Valid());
}

static void TestWideSessionServer() {
	ServerRoom room;
	// Signed and unsigned JSON integers take different branches of the check.
	CHECK(room.Join(1, json(44)));
	CHECK(room.Join(2, json(44u)));
	CHECK(!room.Join(3, json(45)));
	CHECK(!room.Join(4, json(45u)));
	CHECK(room.server.RoomSnapshot()->members.front().mainFighter == 44);
	room.Seat(1);
	room.Show(1, 44);
	CHECK(room.FighterOf(1) == 44);
	room.Show(1, 45);
	CHECK(room.FighterOf(1) == 44);
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
	CHECK(room.FighterOf(1) == -1);
	room.Show(1, 39);
	CHECK(room.FighterOf(1) == 39);
	current = Wide;
}

int main() {
	TestWideRoomModel();
	TestWideProtocol();
	TestWideSessionServer();
	TestNarrowRoster();
	return TEST_EXIT_CODE();
}
