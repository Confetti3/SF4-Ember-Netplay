#include "session_server_transport_support.hxx"

// Hello and join, wrong build, duplicate name, settings, stage and character validation, ready ordering,
// overflow, forwarding source spoofing, the authorized start with its grants and acknowledgments, and a
// failing transport.
static void TestLobbyAndSelection() {
	auto* transport = new MockTransport();
	SessionServer server("room", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	CHECK(server.Listen(0) == 0);
	auto step = [&]() { CHECK(server.Step() == 0); };
	auto hello = [&](session::Connection connection) {
		transport->Push(connection, json(protocol::SessionHelloMsg()));
		step();
		CHECK(server.cidMap.count(connection) == 1);
	};
	auto join = [&](session::Connection connection, const char* name, const char* hash = "build") {
		protocol::SessionJoinRequest request;
		request.username = name;
		request.sidecarHash = hash;
		request.port = 30000;
		transport->outgoing.clear();
		transport->Push(connection, json(request));
		step();
	};
	hello(10000000001ULL); // The core must not truncate to a GNS handle.
	hello(2);
	join(10000000001ULL, "Host");
	join(2, "Guest", "wrong-build");
	CHECK(server.ConnectedClientCount() == 1);
	CHECK(transport->Contains("join_rej"));
	join(2, "Host");
	CHECK(server.ConnectedClientCount() == 1);
	join(2, "Guest");
	CHECK(server.ConnectedClientCount() == 2);
	join(2, "Duplicate");
	CHECK(server.ConnectedClientCount() == 2);
	CHECK(transport->Contains("join_rej"));

	protocol::LobbySetSettings settings;
	settings.roundCount = 5;
	transport->Push(2, json(settings)); step();
	CHECK(server._lobbyData.roundCount == 3);
	// The trainingMode an older build still sends is not read: a lobby's games
	// are never Practice matches; only a Practice room's are.
	json older = settings;
	older["trainingMode"] = true;
	transport->Push(10000000001ULL, older); step();
	CHECK(server._lobbyData.roundCount == 5 && !server._lobbyData.trainingMode);
	protocol::PreBattleSetStage stage;
	for (const auto& option : selection::StageList()) {
		stage.stageID = option.id;
		transport->Push(10000000001ULL, json(stage)); step();
		CHECK(server._matchData.stageID == option.id);
	}
	stage.stageID = 8;
	transport->Push(2, json(stage)); step();
	CHECK(server._matchData.stageID == 29); // Only the authenticated host chooses.
	for (json value : {json(-1), json(22), json(23), json(30), json(4294967296ULL), json(UINT64_MAX),
		json(1.5), json(1.0), json(true), json("1"), json(nullptr), json::array()}) {
		json request = stage; request["stageID"] = value;
		transport->Push(10000000001ULL, request); step();
		CHECK(server._matchData.stageID == 29);
		CHECK(selection::PreferenceStage(request, "stageID", 0) == 0);
	}
	json missingStage = stage; missingStage.erase("stageID");
	transport->Push(10000000001ULL, missingStage); step();
	CHECK(server._matchData.stageID == 29);

	// A selection belongs to the authenticated player. Reject malformed bytes
	// before narrowing and reject unsupported combinations before storing them.
	protocol::PreBattleSetChara selection;
	selection.chara = server._matchData.chara[1];
	selection.chara.charaID = 1;
	selection.chara.costume = 1;
	selection.chara.color = 2;
	transport->Push(2, json(selection)); step();
	const json guestPick = server._matchData.chara[1];
	const json hostPick = server._matchData.chara[0];
	CHECK(guestPick == json(selection.chara) && hostPick != guestPick);
	// Costume indices past the game's own (7-98) are custom slots, valid with up to ten colors: each PC shows its
	// own install of that slot or the original costume.
	json custom = selection; custom["chara"]["costume"] = 7; custom["chara"]["color"] = 9;
	transport->Push(2, custom); step();
	CHECK(server._matchData.chara[1].costume == 7 && server._matchData.chara[1].color == 9);
	transport->Push(2, json(selection)); step();
	CHECK(json(server._matchData.chara[1]) == guestPick);
	// Colors 30-99 (indices 29-98) of any costume are custom colors, valid the same way.
	json extraColor = selection; extraColor["chara"]["color"] = 29;
	transport->Push(2, extraColor); step();
	CHECK(server._matchData.chara[1].costume == 1 && server._matchData.chara[1].color == 29);
	transport->Push(2, json(selection)); step();
	CHECK(json(server._matchData.chara[1]) == guestPick);
	json customSlotColor = custom; customSlotColor["chara"]["color"] = 40;
	transport->Push(2, customSlotColor); step();
	CHECK(server._matchData.chara[1].costume == 7 && server._matchData.chara[1].color == 40);
	transport->Push(2, json(selection)); step();
	CHECK(json(server._matchData.chara[1]) == guestPick);
	const std::pair<const char*, int> invalidOptions[] = {
		{"charaID", 44}, {"costume", 99}, {"color", 22}, {"color", 99}, {"unc_edition", 0},
		{"ultraCombo", 3}, {"personalAction", 10}, {"winQuote", 11}, {"handicap", 5}
	};
	for (const auto& option : invalidOptions) {
		json request = selection; request["chara"][option.first] = option.second;
		transport->Push(2, request); step();
		CHECK(json(server._matchData.chara[1]) == guestPick);
	}
	json customColor = custom; customColor["chara"]["color"] = 10;
	transport->Push(2, customColor); step();
	CHECK(json(server._matchData.chara[1]) == guestPick);
	for (const char* field : {"charaID", "costume", "color", "unc_edition", "ultraCombo",
		"personalAction", "winQuote", "handicap", "_unused"}) {
		for (json value : {json(-1), json(256), json(1.5), json(true), json("1"), json(nullptr)}) {
			json request = selection; request["chara"][field] = value;
			transport->Push(2, request); step();
			CHECK(json(server._matchData.chara[1]) == guestPick);
		}
	}
	json unsupported = selection;
	unsupported["chara"]["unc_edition"] = 13;
	unsupported["chara"]["ultraCombo"] = 1; // SFIV has one Ultra.
	transport->Push(2, unsupported); step();
	CHECK(json(server._matchData.chara[1]) == guestPick);
	unsupported["chara"]["charaID"] = 43; // Decapre has no SFIV edition.
	unsupported["chara"]["ultraCombo"] = 0;
	transport->Push(2, unsupported); step();
	CHECK(json(server._matchData.chara[1]) == guestPick);
	transport->Push(2, json(protocol::LobbyReady()), 90); step();
	// P2 may ready first; P1 still sends its stage immediately before Ready.
	transport->Push(10000000001ULL, json(stage)); step();
	CHECK(server._matchData.stageID == 8);
	selection.chara.color = 3;
	transport->Push(2, json(selection)); step();
	CHECK(json(server._matchData.chara[1]) == guestPick);
	CHECK(json(server._matchData.chara[0]) == hostPick);
	transport->Push(10000000001ULL, json(protocol::LobbyReset())); step();
	transport->Push(10000000001ULL, json(protocol::LobbyReady()), 91); step();
	const auto lockedStage = server._matchData.stageID;
	stage.stageID = 25;
	transport->Push(10000000001ULL, json(stage)); step();
	CHECK(server._matchData.stageID == lockedStage);
	transport->Push(10000000001ULL, json(protocol::LobbyReset())); step();

	for (std::int64_t generation = 1; generation <= 50; ++generation) {
		transport->outgoing.clear();
		transport->Push(10000000001ULL, json(protocol::LobbyReady()), generation * 100);
		transport->Push(2, json(protocol::LobbyReady()), generation * 100 + 1);
		step();
		CHECK(server._matchData.readyMessageNum[0] == generation * 100);
		CHECK(server._matchData.readyMessageNum[1] == generation * 100 + 1);
		CHECK(transport->Contains("lobby_allready"));
		transport->Push(10000000001ULL, json(protocol::LobbyReset())); step();
		CHECK(!server._matchData.IsAllReady());
	}

	hello(3); join(3, "Spectator");
	hello(4); join(4, "Spectator2");
	hello(5); join(5, "Overflow");
	CHECK(server.ConnectedClientCount() == 4);
	CHECK(transport->Contains("join_rej"));
	transport->outgoing.clear();
	transport->Push(3, json(protocol::LobbyReady()), 9000); step();
	CHECK(!server._matchData.IsAllReady());
	protocol::LobbyReportResults results;
	results.loserSide = -1;
	transport->Push(2, json(results)); step();
	results.loserSide = 0;
	transport->Push(3, json(results)); step();
	CHECK(server.clients[0].conn == 10000000001ULL);

	// A forwarding source must match the authenticated connection even when
	// it claims a different host. The old path logged some forgeries then sent.
	protocol::ForwardMessage forward;
	forward.src = {"other-room", "2"};
	forward.dest = server.cidMap.at(10000000001ULL);
	transport->outgoing.clear();
	transport->Push(2, json(forward)); step();
	CHECK(transport->outgoing.empty());
	forward.src = server.cidMap.at(2);
	transport->Push(2, json(forward)); step();
	CHECK(transport->outgoing.size() == 1);
	CHECK(transport->outgoing[0].first == 10000000001ULL);

	transport->disconnected.push_back(2); step();
	CHECK(server.ConnectedClientCount() == 3 && server.cidMap.count(2) == 0);
	hello(2); join(2, "Returned");
	CHECK(server.ConnectedClientCount() == 4);
	std::array<std::uint8_t, 16> room = {}; room[0] = 1;
	server.EnableMatchAuthorization(room, [](session::Connection connection) {
		return std::string(64, connection == 10000000001ULL ? '1' : static_cast<char>('0' + connection));
	});
	settings.editionSelect = true;
	settings.roundCount = 7;
	settings.roundTime = {0, 99};
	transport->Push(10000000001ULL, json(settings)); step();
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect);
	settings.roundCount = 2; settings.editionSelect = false;
	transport->Push(10000000001ULL, json(settings)); step();
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect);
	settings.roundCount = 5; settings.roundTime = {1, 99};
	transport->Push(10000000001ULL, json(settings)); step();
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect);
	settings.roundTime = {0, 99};
	transport->Push(3, json(settings)); step(); // Current P2 cannot edit.
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect);
	json narrowed = settings;
	narrowed["roundTime"]["fractional"] = 65536;
	transport->Push(10000000001ULL, narrowed); step();
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect);
	narrowed = settings; narrowed["roundCount"] = 5.75;
	transport->Push(10000000001ULL, narrowed); step();
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect);
	transport->Push(10000000001ULL, json(protocol::LobbyReady()), 10000); step();
	transport->Push(10000000001ULL, json(settings)); step();
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect); // One ready player freezes settings.
	transport->outgoing.clear();
	transport->Push(3, json(protocol::LobbyReady()), 10001); step();
	CHECK(transport->Contains("game_prepare") && !transport->Contains("lobby_allready"));
	std::map<session::Connection, json> grants;
	for (const auto& sent : transport->outgoing) if (sent.second["type"] == "game_prepare") grants.emplace(sent.first, sent.second);
	CHECK(grants.size() == 4 && grants[10000000001ULL]["links"].size() == 3);
	transport->Push(10000000001ULL, json(settings)); step();
	CHECK(server._lobbyData.roundCount == 7 && server._lobbyData.editionSelect); // Connecting is also immutable.
	CHECK(grants[3]["links"].size() == 1 && grants[4]["links"].size() == 1);
	CHECK(grants[10000000001ULL]["links"][0]["capability"] == grants[3]["links"][0]["capability"]);
	CHECK(grants[3]["links"][0]["capability"] != grants[4]["links"][0]["capability"]);
	auto ack = [&](session::Connection connection, const char* type, std::uint64_t generation) {
		transport->Push(connection, json{{"type", type}, {"generation", generation}}); step();
	};
	transport->outgoing.clear();
	ack(5, "game_prepared", 1); // A rejected/non-member connection has no vote.
	ack(10000000001ULL, "game_prepared", 0);
	ack(10000000001ULL, "game_prepared", 1);
	ack(10000000001ULL, "game_prepared", 1); // Duplicates cannot count twice.
	ack(3, "game_prepared", 1); ack(4, "game_prepared", 1);
	CHECK(!transport->Contains("game_connect"));
	ack(2, "game_prepared", 1); CHECK(transport->Contains("game_connect"));
	transport->outgoing.clear();
	ack(10000000001ULL, "game_ready", 1); ack(3, "game_ready", 1); ack(4, "game_ready", 1);
	CHECK(!transport->Contains("game_start"));
	ack(2, "game_ready", 0); CHECK(!transport->Contains("game_start"));
	ack(2, "game_ready", 1); CHECK(transport->Contains("game_start"));
	transport->outgoing.clear();
	json report = protocol::LobbyReportResults(); report["loserSide"] = 0; report["generation"] = std::uint64_t(1);
	transport->Push(4, report); step(); CHECK(!transport->Contains("game_end"));
	report["generation"] = std::uint64_t(0); transport->Push(10000000001ULL, report); step(); CHECK(!transport->Contains("game_end"));
	report["generation"] = std::uint64_t(1); transport->Push(10000000001ULL, report); step(); CHECK(transport->Contains("game_end"));
	CHECK(transport->outgoing.size() == 8);
	for (std::size_t i = 0; i < 4; ++i) CHECK(transport->outgoing[i].second["type"] == "data_update");
	for (std::size_t i = 4; i < 8; ++i) CHECK(transport->outgoing[i].second["type"] == "game_end");
	CHECK(server.clients[0].conn == 3 && server.clients[1].conn == 4);
	transport->outgoing.clear();
	transport->Push(3, json(protocol::LobbyReady()), 11000); transport->Push(4, json(protocol::LobbyReady()), 11001); step();
	CHECK(transport->Contains("game_prepare"));
	transport->outgoing.clear();
	ack(3, "game_prepared", 1); ack(4, "game_prepared", 1); ack(2, "game_prepared", 1); ack(10000000001ULL, "game_prepared", 1);
	CHECK(!transport->Contains("game_connect"));
	transport->disconnected.push_back(4); step(); CHECK(transport->Contains("game_end"));
	transport->writable = false;
	server.ResetLobbyForRematch();
	CHECK(server.Step() == -1); // Bounded transport failure reaches the owner.
	server.Close();
	CHECK(transport->closed && server.cidMap.empty() && server.clients.empty());
}

int main() {
	TestLobbyAndSelection();
	std::cout << "Session server mock transport lobby tests passed\n";
}
