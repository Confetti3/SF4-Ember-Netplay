#include "session_server_transport_support.hxx"

// A room table's pre-battle selection: only its P1 sets the stage, each
// fighter sets only its own pick, malformed or unsupported picks are refused
// before they are stored, and Ready locks both. Also the join refusals (another
// build, a name already taken) and a failing transport.
static void TestTableSelection() {
	ParticipantRoom fixture(SetupStage::Seated);
	auto& server = fixture.server;
	auto* transport = fixture.transport;
	const auto step = [&] { fixture.Step(); };
	const auto refusedJoin = [&](session::Connection connection, const char* name, const char* hash) {
		transport->Push(connection, json(protocol::SessionHelloMsg())); step();
		protocol::SessionJoinRequest request;
		request.username = name; request.sidecarHash = hash; request.port = 30000;
		request.customRooms = true; request.roomProtocol = room::ProtocolVersion;
		transport->outgoing.clear();
		transport->Push(connection, json(request)); step();
		CHECK(transport->Contains("join_rej") && server.roomMembers.count(connection) == 0);
	};
	refusedJoin(5, "Guest", "wrong-build");
	refusedJoin(6, "Member2", "build");

	const auto& table = server.RoomSnapshot()->tables[0];
	const session::Connection p1 = table.p1 == server.roomMembers.at(2) ? 2 : 3;
	const session::Connection p2 = p1 == 2 ? 3 : 2;
	const auto& match = server._roomMatchData[0];

	protocol::PreBattleSetStage stage;
	for (const auto& option : selection::StageList()) {
		stage.stageID = option.id;
		transport->Push(p1, json(stage)); step();
		CHECK(match.stageID == option.id);
	}
	const auto lastStage = match.stageID;
	stage.stageID = 8;
	transport->Push(p2, json(stage)); step();
	CHECK(match.stageID == lastStage); // Only the table's P1 chooses.
	transport->Push(4, json(stage)); step();
	CHECK(match.stageID == lastStage); // A spectator does not either.
	for (json value : {json(-1), json(22), json(23), json(30), json(4294967296ULL), json(UINT64_MAX),
		json(1.5), json(1.0), json(true), json("1"), json(nullptr), json::array()}) {
		json request = stage; request["stageID"] = value;
		transport->Push(p1, request); step();
		CHECK(match.stageID == lastStage);
		CHECK(selection::PreferenceStage(request, "stageID", 0) == 0);
	}
	json missingStage = stage; missingStage.erase("stageID");
	transport->Push(p1, missingStage); step();
	CHECK(match.stageID == lastStage);

	// A selection belongs to the authenticated player. Reject malformed bytes
	// before narrowing and reject unsupported combinations before storing them.
	protocol::PreBattleSetChara selection;
	selection.chara = match.chara[1];
	selection.chara.charaID = 1;
	selection.chara.costume = 1;
	selection.chara.color = 2;
	transport->Push(p2, json(selection)); step();
	const json guestPick = match.chara[1];
	const json hostPick = match.chara[0];
	CHECK(guestPick == json(selection.chara) && hostPick != guestPick);
	// Costume indices past the game's own (7-98) are custom slots, valid with up to ten colors: each PC shows its
	// own install of that slot or the original costume.
	json custom = selection; custom["chara"]["costume"] = 7; custom["chara"]["color"] = 9;
	transport->Push(p2, custom); step();
	CHECK(match.chara[1].costume == 7 && match.chara[1].color == 9);
	transport->Push(p2, json(selection)); step();
	CHECK(json(match.chara[1]) == guestPick);
	// Colors 30-99 (indices 29-98) of any costume are custom colors, valid the same way.
	json extraColor = selection; extraColor["chara"]["color"] = 29;
	transport->Push(p2, extraColor); step();
	CHECK(match.chara[1].costume == 1 && match.chara[1].color == 29);
	transport->Push(p2, json(selection)); step();
	CHECK(json(match.chara[1]) == guestPick);
	json customSlotColor = custom; customSlotColor["chara"]["color"] = 40;
	transport->Push(p2, customSlotColor); step();
	CHECK(match.chara[1].costume == 7 && match.chara[1].color == 40);
	transport->Push(p2, json(selection)); step();
	CHECK(json(match.chara[1]) == guestPick);
	const std::pair<const char*, int> invalidOptions[] = {
		{"charaID", 44}, {"costume", 99}, {"color", 22}, {"color", 99}, {"unc_edition", 0},
		{"ultraCombo", 3}, {"personalAction", 10}, {"winQuote", 11}, {"handicap", 5}
	};
	for (const auto& option : invalidOptions) {
		json request = selection; request["chara"][option.first] = option.second;
		transport->Push(p2, request); step();
		CHECK(json(match.chara[1]) == guestPick);
	}
	json customColor = custom; customColor["chara"]["color"] = 10;
	transport->Push(p2, customColor); step();
	CHECK(json(match.chara[1]) == guestPick);
	for (const char* field : {"charaID", "costume", "color", "unc_edition", "ultraCombo",
		"personalAction", "winQuote", "handicap", "_unused"}) {
		for (json value : {json(-1), json(256), json(1.5), json(true), json("1"), json(nullptr)}) {
			json request = selection; request["chara"][field] = value;
			transport->Push(p2, request); step();
			CHECK(json(match.chara[1]) == guestPick);
		}
	}
	json unsupported = selection;
	unsupported["chara"]["unc_edition"] = 13;
	unsupported["chara"]["ultraCombo"] = 1; // SFIV has one Ultra.
	transport->Push(p2, unsupported); step();
	CHECK(json(match.chara[1]) == guestPick);
	unsupported["chara"]["charaID"] = 43; // Decapre has no SFIV edition.
	unsupported["chara"]["ultraCombo"] = 0;
	transport->Push(p2, unsupported); step();
	CHECK(json(match.chara[1]) == guestPick);

	// P2 may ready first; P1 still sends its stage immediately before Ready.
	fixture.Act(p2, room::ActionKind::Ready);
	transport->Push(p1, json(stage)); step();
	CHECK(match.stageID == 8);
	selection.chara.color = 3;
	transport->Push(p2, json(selection)); step();
	CHECK(json(match.chara[1]) == guestPick);
	CHECK(json(match.chara[0]) == hostPick);
	fixture.Act(p1, room::ActionKind::Ready);
	stage.stageID = 25;
	transport->Push(p1, json(stage)); step();
	CHECK(match.stageID == 8);

	// A failing transport reaches the owner.
	transport->writable = false;
	transport->Push(7, json(protocol::SessionHelloMsg()));
	CHECK(server.Step() == -1);
	server.Close();
	CHECK(transport->closed && server.cidMap.empty() && server.ConnectedClientCount() == 0);
}

int main() {
	TestTableSelection();
	std::cout << "Session server mock transport selection tests passed\n";
}
