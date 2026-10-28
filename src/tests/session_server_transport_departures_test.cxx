#include "session_server_transport_support.hxx"

static void TestCustomRoomDepartures() {
	auto* transport = new MockTransport();
	SessionServer server("custom-room", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> room = {};
	room[0] = 1;
	server.EnableMatchAuthorization(room, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Departure test", 16, 9);
	CHECK(server.Listen(0) == 0);
	auto step = [&]() { CHECK(server.Step() == 0); };
	auto hello = [&](session::Connection connection) {
		transport->Push(connection, json(protocol::SessionHelloMsg()));
		step();
		CHECK(server.cidMap.count(connection) == 1);
	};
	auto join = [&](session::Connection connection, const char* name) {
		protocol::SessionJoinRequest request;
		request.username = name;
		request.sidecarHash = "build";
		request.port = 30000;
		request.customRooms = true;
		request.roomProtocol = room::ProtocolVersion;
		transport->outgoing.clear();
		json requestJson=request;requestJson["mainFighter"]=static_cast<int>(connection)+8;
		requestJson["link"]=static_cast<int>(connection%3);
		requestJson["nat"]=static_cast<int>(connection%4);
		if(connection==2)requestJson["steamId"]="76561200000000000";
		transport->Push(connection, requestJson);
		step();
		CHECK(server.roomMembers.count(connection) == 1);
		for(const auto& member:server.RoomSnapshot()->members)if(member.id==server.roomMembers.at(connection))
			CHECK(json(member).value("main_fighter",-1)==static_cast<int>(connection)+8&&
				json(member).value("link",-1)==static_cast<int>(connection%3)&&json(member).value("nat",-1)==static_cast<int>(connection%4)&&
				// Only the member who sent a profile has one in the snapshot.
				json(member).value("steam_id",std::string())==std::string(connection==2?"76561200000000000":""));
	};
	for (session::Connection connection = 1; connection <= 6; ++connection) {
		hello(connection);
		join(connection, (std::string("Member") + std::to_string(connection)).c_str());
	}
    hello(7);
    protocol::SessionJoinRequest profile;profile.username="Profile validation";profile.sidecarHash="build";
    profile.port=30000;profile.customRooms=true;profile.roomProtocol=room::ProtocolVersion;
    for(const json invalid:{json(-2),json(44),json(1.5),json("Ryu"),json(UINT64_MAX)}){
        json bad=profile;bad["mainFighter"]=invalid;
        transport->Push(7,bad);step();CHECK(server.roomMembers.count(7)==0);
    }
    json older=profile;older.erase("mainFighter");older.erase("link");older.erase("nat");transport->Push(7,older);step();
    CHECK(server.roomMembers.count(7)==1&&server.RoomSnapshot()->members.back().mainFighter==-1&&
        server.RoomSnapshot()->members.back().link==NetworkLink::Unknown&&server.RoomSnapshot()->members.back().nat==NatClass::Unknown&&
        !server.RoomSnapshot()->members.back().steamId.Present());
    const auto profileMember=server.roomMembers.at(7);
    CHECK(server.roomIncarnations.count(profileMember)==1);
    transport->disconnected.push_back(7);step();CHECK(server.roomMembers.count(7)==0);
    CHECK(server.roomIncarnations.count(profileMember)==0);
	std::map<session::Connection, std::uint64_t> actionIds;
	auto action = [&](session::Connection connection, room::ActionKind kind, std::uint8_t table, room::MemberId target = 0) {
		room::Action value;
		const auto* snapshot = server.RoomSnapshot();
		value.kind = kind;
		value.roomEpoch = snapshot->roomEpoch;
		value.revision = snapshot->revision;
		value.table = table;
		value.tableRevision = snapshot->tables[table].revision;
		value.actionId = ++actionIds[connection];
		value.target = target;
		protocol::RoomActionMessage message;
		message.action = value;
		transport->outgoing.clear();
		transport->Push(connection, json(message));
		step();
	};
	auto hasMessage = [&](const char* type, session::Connection connection = 0) {
		return std::any_of(transport->outgoing.begin(), transport->outgoing.end(), [&](const auto& sent) {
			return sent.second.value("type", std::string()) == type && (!connection || sent.first == connection);
		});
	};

	// Start table 0 with a live spectator, then remove only that spectator.
	action(2, room::ActionKind::Queue, 0);
	action(3, room::ActionKind::Queue, 0);
	action(4, room::ActionKind::Watch, 0);
    // Existing pre-battle exchange publishes only validated fighter portraits.
    protocol::PreBattleSetChara portrait;portrait.chara=protocol::MatchData{}.chara[0];portrait.chara.charaID=2;
    transport->Push(2,json(portrait));step();
    const auto fighterOf=[&](session::Connection connection){
        for(const auto& m:server.RoomSnapshot()->members)if(m.id==server.roomMembers.at(connection))return m.fighter;
        return -2;
    };
    CHECK(fighterOf(2)==2&&fighterOf(3)==-1);
    CHECK(hasMessage("room_snapshot",6));
    CHECK(server.RoomSnapshot()->members[1].mainFighter==10);
    const auto shared=json(*server.RoomSnapshot()).get<room::Snapshot>();
    CHECK(shared.members[1].mainFighter==10&&shared.members[1].fighter==2);
    portrait.chara.charaID=44;transport->Push(2,json(portrait));step();CHECK(fighterOf(2)==2);
    portrait.chara.charaID=4;transport->Push(4,json(portrait));step();CHECK(fighterOf(4)==-1);
    // A seat that changes the fighter it showed takes back the other seat's
    // Ready; showing a first fighter does not.
    {
        const auto seatOf=[&](session::Connection connection){const auto& t=server.RoomSnapshot()->tables[0];return t.p1==server.roomMembers.at(connection)?0:1;};
        action(3, room::ActionKind::Ready, 0);
        CHECK(server.RoomSnapshot()->tables[0].ready[seatOf(3)]);
        portrait.chara.charaID=2;transport->Push(2,json(portrait));step(); // the same fighter again
        CHECK(server.RoomSnapshot()->tables[0].ready[seatOf(3)]);
        portrait.chara.charaID=5;transport->Push(2,json(portrait));step();
        CHECK(fighterOf(2)==5&&!server.RoomSnapshot()->tables[0].ready[seatOf(3)]);
        CHECK(server.RoomSnapshot()->tables[0].phase==room::TablePhase::Waiting);
        action(2, room::ActionKind::Ready, 0);
        CHECK(server.RoomSnapshot()->tables[0].ready[seatOf(2)]);
        portrait.chara.charaID=6;transport->Push(3,json(portrait));step(); // a first fighter
        CHECK(fighterOf(3)==6&&server.RoomSnapshot()->tables[0].ready[seatOf(2)]);
        action(2, room::ActionKind::Unready, 0);
        portrait.chara.charaID=2;transport->Push(2,json(portrait));step();
        CHECK(fighterOf(2)==2);
    }
	action(2, room::ActionKind::Ready, 0);
	action(3, room::ActionKind::Ready, 0);
	const auto generation0 = server.RoomSnapshot()->tables[0].matchGeneration;
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	CHECK(hasMessage("game_prepare", 2) && hasMessage("game_prepare", 3) && hasMessage("game_prepare", 4));
	auto acknowledge = [&](session::Connection connection, const char* type, std::uint64_t generation, json extra = json::object()) {
		transport->outgoing.clear();
		extra["type"] = type; extra["generation"] = generation;
		transport->Push(connection, extra);
		step();
	};
	// P1 (connection 2) accepts the optional-spectator start and reports the
	// spectator's link up, so the Started authority carries a start set that
	// the portable restore below must convert.
	acknowledge(2, "game_prepared", generation0, {{"spectators_optional", true}});
	for (session::Connection connection = 3; connection <= 4; ++connection) acknowledge(connection, "game_prepared", generation0);
	acknowledge(2, "game_ready", generation0, {{"slots", json::array({2})}});
	for (session::Connection connection = 3; connection <= 4; ++connection) acknowledge(connection, "game_ready", generation0);
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	// Verification is the two fighters' exchange: a fighter's snapshot reaches
	// everyone else at the table, and a spectator's is not relayed.
	{
		protocol::BattleSnapshot verification; verification.snapshot.frameIdx = 60;
		transport->outgoing.clear();
		transport->Push(4, json(verification)); step();
		CHECK(!hasMessage("battle_snapshot"));
		transport->Push(2, json(verification)); step();
		CHECK(hasMessage("battle_snapshot", 3) && hasMessage("battle_snapshot", 4) && !hasMessage("battle_snapshot", 2));
	}
	action(4, room::ActionKind::Unwatch, 0);
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	CHECK(hasMessage("game_peer_end", 2));
	CHECK(!hasMessage("game_end"));
	const auto spectatorMember = server.roomMembers.at(4);

	// A spectator removed after Started is absent from the public room roster,
	// but its frozen native slot must survive a portable restore and handle
	// permutation. The restored authority must remain Started so the two
	// fighters can continue without regenerating private capabilities.
	const auto portableCheckpoint = server.RecoveryCheckpoint();
	auto* portableTransport = new MockTransport();
	SessionServer portable("custom-room", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(portableTransport));
	portable.EnableMatchAuthorization(room, [](session::Connection connection) {
		std::string identity(64, '0'); identity[63] = "0123456789abcdef"[connection & 15]; return identity;
	});
	// Checkpoints no longer duplicate the proposal's pending effects, and one
	// written by an older owner that still carries them must restore.
	CHECK(!portableCheckpoint.contains("pending_effects"));
	auto legacyCheckpoint = portableCheckpoint;
	legacyCheckpoint["pending_effects"] = json::array();
	legacyCheckpoint["pending_effects_digest"] = std::string(64, '0');
	CHECK(portable.RestoreRecoveryCheckpoint(legacyCheckpoint));
	std::vector<SessionServer::StableRebind> restoredBindings;
	session::Connection reboundHandle = 200;
	session::Connection spectatorHandle = 0;
	for (const auto& row : portableCheckpoint.at("members")) {
		if (row.value("frozen", false)) continue;
		const auto data = row.at("data").get<protocol::MemberData>();
		const auto member = row.at("member").get<room::MemberId>();
		if (member == spectatorMember) spectatorHandle = reboundHandle;
		restoredBindings.emplace_back(member, reboundHandle++,
			data.connId, row.at("incarnation").get<std::uint64_t>());
	}
	CHECK(spectatorHandle && portable.RebindMembers(restoredBindings));
	const auto continued = portable.RecoveryCheckpoint();
	CHECK(continued.at("match_authorities").at(0).at("phase") == static_cast<int>(session::MatchAuthority::Phase::Started));
	CHECK(continued.at("match_authorities").at(0).at("spectators_optional") == true);
	CHECK(continued.at("match_authorities").at(0).at("start_spectators").size() == 1);
	CHECK(continued.at("match_authorities").at(0).at("participants").size() == 3);
	CHECK(portable.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	CHECK(portableTransport->outgoing.empty());
	portable.SetAuthority(1, 0, true);
	room::Action leave;
	leave.kind = room::ActionKind::Leave;
	leave.roomEpoch = portable.RoomSnapshot()->roomEpoch;
	leave.revision = portable.RoomSnapshot()->revision;
	leave.actionId = 3;
	protocol::RoomActionMessage leaveMessage;
	leaveMessage.action = leave;
	portableTransport->Push(spectatorHandle, json(leaveMessage));
	CHECK(portable.Step() == 0 && portable.HasRecoveryCandidate());
	CHECK(portable.ProposeCheckpoint(1, 1, 0, portable.RecoveryCheckpoint()));
	const auto leaveProposal = portable.PendingProposal();
	CHECK(leaveProposal != nullptr);
	CHECK(portable.ApplyCommit(1, 1, 1, leaveProposal->checkpoint, leaveProposal->effectsDigest));
	CHECK(portable.roomFrozenMembers.size() == 1);
	// The departed member's incarnation lives on only in its frozen row.
	CHECK(portable.roomIncarnations.count(spectatorMember) == 0);
	CHECK(portable.roomFrozenMembers.at(spectatorMember).incarnation != 0);
	const auto retained = portable.RecoveryCheckpoint();
	CHECK(std::count_if(retained.at("members").begin(), retained.at("members").end(),
		[](const json& member) { return member.value("frozen", false); }) == 1);
	CheckIdleRecoveryWork(portable, "retained started spectator");
	const auto stillRetained = portable.RecoveryCheckpoint();
	CHECK(std::count_if(stillRetained.at("members").begin(), stillRetained.at("members").end(),
		[](const json& member) { return member.value("frozen", false); }) == 1);

	// A recovery checkpoint includes native setup and match coordination, not
	// only the UI roster. Restoring it must not send grants or touch a socket.
	const auto checkpoint = json::parse(server.Checkpoint().dump());
	auto* recoveryTransport = new MockTransport();
	SessionServer recovery("custom-room", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(recoveryTransport));
	recovery.EnableMatchAuthorization(room, [](session::Connection connection) {
		std::string identity(64, '0'); identity[63] = "0123456789abcdef"[connection & 15]; return identity;
	});
	CHECK(recovery.RestoreCheckpoint(checkpoint));
	CHECK(recovery.Checkpoint() == checkpoint);
	CHECK(recoveryTransport->outgoing.empty() && !recoveryTransport->closed);
	CHECK(checkpoint.dump().find("capability") == std::string::npos);
	for (const char* key : {"room", "members", "cids", "room_members", "peer_identities", "match_authorities"}) {
		auto damaged = checkpoint; damaged[key] = nullptr;
		CHECK(!recovery.RestoreCheckpoint(damaged));
		// The refusal names the part it stopped at, for the field log.
		CHECK(recovery.RestoreRefusal() == key);
		CHECK(recovery.Checkpoint() == checkpoint);
	}
	// Duplicate native preparation acknowledgments after recovery cannot
	// restart a generation which has already reached Started.
	recoveryTransport->Push(2, json{{"type", "game_ready"}, {"generation", generation0}});
	CHECK(recovery.Step() == 0);
	CHECK(!recoveryTransport->Contains("game_start") && !recoveryTransport->Contains("game_prepare"));

	// A pre-start spectator disconnect ends nothing for the fighters.
	action(5, room::ActionKind::Queue, 1);
	action(6, room::ActionKind::Queue, 1);
	action(4, room::ActionKind::Watch, 1);
	action(5, room::ActionKind::Ready, 1);
	action(6, room::ActionKind::Ready, 1);
	const auto generation1 = server.RoomSnapshot()->tables[1].matchGeneration;
	CHECK(server.RoomSnapshot()->tables[1].phase == room::TablePhase::Playing);
	CHECK(hasMessage("game_prepare", 4));
	transport->outgoing.clear();
	transport->disconnected.push_back(4);
	step();
	// P1 has not taken up the generation yet, so the spectator's departure ends
	// nothing and is passed on with P1's game_prepared.
	CHECK(!hasMessage("game_end"));
	CHECK(server.RoomSnapshot()->tables[1].matchGeneration == generation1);
	CHECK(server.RoomSnapshot()->tables[1].phase == room::TablePhase::Playing);
	CHECK(server.roomMembers.count(4) == 0 && server.ConnectedClientCount() == 5);
	CHECK(server.roomIncarnations.count(spectatorMember) == 0);
	acknowledge(5, "game_prepared", generation1, {{"spectators_optional", true}});
	CHECK(hasMessage("game_peer_end", 5));
	acknowledge(6, "game_prepared", generation1);
	CHECK(hasMessage("game_connect", 5) && hasMessage("game_connect", 6) && !hasMessage("game_connect", 4));

	// Kicking sends an observable rejection to the target, removes all of its
	// authenticated state, and blocks the same endpoint from rejoining.
	hello(7);
	join(7, "Kicked");
	const auto kickedMember = server.roomMembers.at(7);
	action(1, room::ActionKind::Kick, 0, kickedMember);
	CHECK(hasMessage("room_result", 7));
	const auto kickedResult = std::find_if(transport->outgoing.begin(), transport->outgoing.end(), [](const auto& sent) {
		return sent.first == 7 && sent.second.value("type", std::string()) == "room_result";
	});
	CHECK(kickedResult != transport->outgoing.end());
	CHECK(!kickedResult->second.at("result").at("accepted").get<bool>());
	CHECK(kickedResult->second.at("result").at("reason").get<int>() == static_cast<int>(room::RejectReason::MemberKicked));
	CHECK(server.roomMembers.count(7) == 0 && server.cidMap.count(7) == 0);
	CHECK(server.roomIncarnations.count(kickedMember) == 0);
	hello(7);
	protocol::SessionJoinRequest banned;
	banned.username = "Rejoined"; banned.sidecarHash = "build"; banned.port = 30000;
	banned.customRooms = true; banned.roomProtocol = room::ProtocolVersion;
	transport->outgoing.clear(); transport->Push(7, json(banned)); step();
	CHECK(transport->Contains("join_rej"));

	// A fighter disconnect from a waiting, partially-ready table fills the
	// vacancy FIFO and clears readiness before the replacement can ready.
	hello(8); join(8, "FighterA");
	hello(9); join(9, "FighterB");
	hello(10); join(10, "Queued");
	action(8, room::ActionKind::Queue, 2);
	action(9, room::ActionKind::Queue, 2);
	action(10, room::ActionKind::Queue, 2);
	action(8, room::ActionKind::Ready, 2);
	CHECK(server.RoomSnapshot()->tables[2].ready[0]);
	transport->outgoing.clear();
	transport->disconnected.push_back(8);
	step();
	const auto& recovered = server.RoomSnapshot()->tables[2];
	CHECK(recovered.p1 == server.roomMembers.at(10) && recovered.p2 == server.roomMembers.at(9));
	CHECK(!recovered.ready[0] && !recovered.ready[1] && recovered.phase == room::TablePhase::Waiting);

	// Members that come and go leave nothing behind in the incarnation map.
	for (int cycle = 0; cycle < 12; ++cycle) {
		const session::Connection connection = 27; // an identity no live member or ban uses
		hello(connection);
		join(connection, (std::string("Cycle") + std::to_string(cycle)).c_str());
		transport->disconnected.push_back(connection);
		step();
	}
	CHECK(server.roomIncarnations.size() == server.RoomSnapshot()->members.size());
	server.Close();
	CHECK(server.roomIncarnations.empty());
}

// A server-owned room has no local host: every join needs an account the
// embedding process vouched for, the first joiner moderates, and a kick bans
// the account rather than the connection.
static void TestServerOwnedRoom() {
	auto* transport = new MockTransport();
	SessionServer server("public-room", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	CHECK(server.EnableServerOwnedRooms("Public", 8, 95));
	CHECK(!server.EnableServerOwnedRooms("Again"));
	CHECK(server.Listen(0) == 0);
	auto step = [&]() { CHECK(server.Step() == 0); };
	auto hello = [&](session::Connection connection) {
		transport->Push(connection, json(protocol::SessionHelloMsg()));
		step();
	};
	// True when the server took the join.
	auto join = [&](session::Connection connection, const char* name) {
		protocol::SessionJoinRequest request;
		request.username = name; request.sidecarHash = "build"; request.port = 30000;
		request.customRooms = true; request.roomProtocol = room::ProtocolVersion;
		transport->outgoing.clear();
		transport->Push(connection, json(request));
		step();
		return server.roomMembers.count(connection) == 1;
	};
	const auto isHost = [&](session::Connection connection) {
		return server.RoomSnapshot()->host == server.roomMembers.at(connection);
	};

	hello(1);
	CHECK(!join(1, "NoAccount") && transport->Contains("join_rej") && server.RoomSnapshot()->members.empty());
	// Connection 1 is not the hosting player here: whoever joins first moderates.
	hello(2);
	server.SetConnectionAdmission(2, {"ember-2"});
	CHECK(join(2, "First") && isHost(2) && server.RoomSnapshot()->serverOwned);
	server.SetConnectionAdmission(1, {"ember-1"});
	CHECK(join(1, "Second") && !isHost(1) && isHost(2));
	// One member per account, however it connects.
	hello(3);
	server.SetConnectionAdmission(3, {"ember-1"});
	CHECK(!join(3, "Twin") && transport->Contains("join_rej"));

	// The host kicks the second member. The ban is the account.
	const auto kickedMember = server.roomMembers.at(1);
	room::Action kick;
	const auto* snapshot = server.RoomSnapshot();
	kick.kind = room::ActionKind::Kick; kick.roomEpoch = snapshot->roomEpoch; kick.revision = snapshot->revision;
	kick.tableRevision = snapshot->tables[0].revision; kick.actionId = 1; kick.target = kickedMember;
	protocol::RoomActionMessage message; message.action = kick;
	transport->outgoing.clear();
	transport->Push(2, json(message));
	step();
	CHECK(server.roomMembers.count(1) == 0);
	CHECK(server.BannedAccounts() == std::vector<std::string>{"ember-1"} && server.roomBannedIdentities.empty());
	hello(4);
	server.SetConnectionAdmission(4, {"ember-1"});
	CHECK(!join(4, "Returned") && transport->Contains("join_rej"));
	server.SetConnectionAdmission(4, {"ember-4"});
	CHECK(join(4, "Fresh") && !isHost(4));

	// An account lives as long as its connection, and an empty room stays open.
	CHECK(server._connectionAdmissions.count(2) == 1);
	transport->disconnected.push_back(2);
	step();
	CHECK(server._connectionAdmissions.count(2) == 0 && isHost(4));
	transport->disconnected.push_back(4);
	step();
	CHECK(server.RoomSnapshot()->members.empty() && server.RoomSnapshot()->host == 0 && !server.RoomSnapshot()->closed);
	hello(5);
	server.SetConnectionAdmission(5, {"ember-5"});
	CHECK(join(5, "Next") && isHost(5));
	server.Close();
	CHECK(server._connectionAdmissions.empty());
}

// A member that leaves while its table's match authority still names it keeps
// a frozen row: a spectator that left before the start stays a departed
// participant until the generation ends. Without the row no replica could
// import the room's checkpoints, and every member's room stalled behind it.
static void TestParticipantDepartureCheckpoint(SetupStage stage, bool disconnect) {
	ParticipantRoom fixture(stage);
	auto& server = fixture.server;
	const auto spectator = server.roomMembers.at(4);
	fixture.Depart(4, disconnect);
	CHECK(server.roomFrozenMembers.count(spectator) == 1 && server.roomFrozenMembers.at(spectator).incarnation != 0);
	CHECK(fixture.Imports(server.RecoveryCheckpoint()));
	if (stage == SetupStage::Started) return;
	// A fighter leaving ends the generation before the start, and the row with it.
	fixture.Depart(3, true);
	CHECK(server.roomFrozenMembers.empty());
	CHECK(fixture.Imports(server.RecoveryCheckpoint()));
}

// The rejoined member's own game applies `commits` (revisions 1, 2, ...) the
// way production does: the scripted helper hands them over through IrohRoom's
// event handling (transfer, commit marker, decode, staging), and
// RoomRecoveryRuntime imports, rebinds and activates them against a replica
// server. Returns the revision it applied up to.
static std::uint64_t ApplyAsRejoinedMember(const ParticipantRoom& fixture, const std::vector<json>& commits,
	session::Connection self) {
	FakeHelperPipe pipe;
	platform::HelperClient helper;
	pipe.Connect(helper);
	session::IrohRoom room(helper);
	CHECK(room.Join("invitation", "build"));
	const std::array<std::uint8_t, 16> id{{9}};
	const std::string leader(64, 'a');
	std::map<std::string, std::uint64_t> incarnations;
	for (const auto& row : commits.front().at("members"))
		if (!row.value("frozen", false))
			incarnations[row.at("data").at("authenticatedEndpoint").get<std::string>()] = row.at("incarnation").get<std::uint64_t>();
	pipe.Send({{"type", "status"}, {"request_id", 1}, {"endpoint", ParticipantRoom::IdentityOf(self)}, {"fixed_port", 0}});
	pipe.Send({{"type", "coordination_state"}, {"epoch", 1}, {"room", id}, {"term", 1}, {"revision", commits.size()},
		{"incarnation", ParticipantRoom::IncarnationOf(self)}, {"leader", leader}, {"writable", true}, {"leader_local", false}});
	pipe.Send({{"type", "control_rebound"}, {"epoch", 1}, {"room", id}, {"leader", leader},
		{"member_incarnations", incarnations}, {"members", json::array()}});
	for (std::uint64_t revision = 1; revision <= commits.size(); ++revision) {
		session::SessionProposal proposal;
		proposal.request = revision; proposal.term = 1; proposal.baseRevision = revision - 1;
		proposal.checkpoint = commits[revision - 1]; proposal.effectsDigest = session::EffectsDigest({});
		const auto bytes = json(proposal).dump();
		const json transfer{{"epoch", 1}, {"room", id}, {"transfer", revision}, {"term", 1},
			{"base_revision", revision - 1}, {"revision", revision}};
		auto message = [&](const char* type) { auto value = transfer; value["type"] = type; return value; };
		auto begin = message("checkpoint_begin"); begin["length"] = bytes.size(); begin["digest"] = coordination::Sha256(bytes);
		pipe.Send(begin);
		for (std::size_t offset = 0; offset < bytes.size(); offset += coordination::CheckpointChunk) {
			auto chunk = message("checkpoint_chunk");
			chunk["offset"] = offset; chunk["data"] = coordination::Encode(bytes.substr(offset, coordination::CheckpointChunk));
			pipe.Send(chunk);
		}
		auto end = begin; end["type"] = "checkpoint_end"; pipe.Send(end);
		auto committed = begin; committed["type"] = "checkpoint_committed"; pipe.Send(committed);
	}
	const auto replica = fixture.Replica();
	session::RoomRecoveryRuntime recovery;
	for (int tick = 0; tick < 5000 && recovery.AppliedRevision() < commits.size(); ++tick) {
		CHECK(recovery.Tick(*replica, room));
		pipe.Drain();
		Sleep(1);
	}
	CHECK(recovery.Error().empty() && !room.CheckpointPending());
	return recovery.AppliedRevision();
}

// The spectator leaves before the start and rejoins, again and again, while
// that generation still names its endpoint. Each rejoin is a new member and
// incarnation. The endpoint never has more than one frozen row, and every
// commit imports in order on one replica, the generation's cleanup commit
// included. The rejoined member's own game then activates the rejoin commit
// even as a room host from before this change sent it, with the old frozen
// row beside the new active one, and applies the cleanup commit behind it.
static void TestRejoinAfterPreStartDeparture() {
	ParticipantRoom fixture(SetupStage::FightersPrepared);
	auto& server = fixture.server;
	const auto replica = fixture.Replica();
	const auto endpoint = ParticipantRoom::IdentityOf(4);
	session::Connection spectator = 4;
	json frozenRow, rejoined;
	for (int cycle = 0; cycle < 6; ++cycle) {
		fixture.Depart(spectator, cycle % 2 == 0);
		const auto departed = server.RecoveryCheckpoint();
		CHECK(server.roomFrozenMembers.size() == 1 && ParticipantRoom::FrozenRows(departed) == 1);
		CHECK(ParticipantRoom::Imports(*replica, departed));
		for (const auto& row : departed.at("members")) if (row.value("frozen", false)) frozenRow = row;
		spectator += 16;
		fixture.Join(spectator);
		// The live row restores the endpoint's participations; the frozen one goes.
		rejoined = server.RecoveryCheckpoint();
		CHECK(server.roomFrozenMembers.empty() && ParticipantRoom::FrozenRows(rejoined) == 0);
		session::RosterRow active;
		CHECK(session::ActiveRosterRow(rejoined.at("members"), endpoint, active) &&
			active.member == server.roomMembers.at(spectator) && active.incarnation == spectator);
		CHECK(ParticipantRoom::Imports(*replica, rejoined));
	}
	CHECK(server.RoomSnapshot()->tables[0].matchGeneration == fixture.generation);
	// The generation ends; its cleanup commit imports behind the rejoin.
	fixture.Depart(3, true);
	const auto cleanup = server.RecoveryCheckpoint();
	session::RosterRow active;
	CHECK(session::ActiveRosterRow(cleanup.at("members"), endpoint, active) && active.incarnation == spectator);
	CHECK(ParticipantRoom::Imports(*replica, cleanup));
	auto withFrozen = rejoined;
	withFrozen.at("members").push_back(frozenRow);
	CHECK(ParticipantRoom::FrozenRows(withFrozen) == 1);
	CHECK(ApplyAsRejoinedMember(fixture, {withFrozen, cleanup}, spectator) == 2);
}

int main() {
	for (const auto stage : {SetupStage::Preparing, SetupStage::P1Prepared, SetupStage::FightersPrepared,
		SetupStage::AllPrepared, SetupStage::Started})
		for (const bool disconnect : {true, false}) TestParticipantDepartureCheckpoint(stage, disconnect);
	TestRejoinAfterPreStartDeparture();
	TestServerOwnedRoom();
	TestCustomRoomDepartures();
	std::cout << "Session server mock transport departures tests passed\n";
}
