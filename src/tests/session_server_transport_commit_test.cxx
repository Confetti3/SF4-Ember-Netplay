#include "session_server_transport_support.hxx"

static void TestAtomicMatchRebind() {
	std::array<std::uint8_t, 16> room = {};
	room[0] = 7;
	session::MatchAuthority authority(room, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	std::vector<session::MatchAuthority::Participant> participants = {
		{1, {"relay", "member-a"}}, {2, {"relay", "member-b"}}
	};
	CHECK(authority.BeginAtGeneration(participants, 12, [](session::Connection, const json&) { return true; }));
	CHECK(authority.Acknowledge(1, json{{"type", "game_prepared"}, {"generation", 12}}, [](session::Connection, const json&) { return true; }));
	CHECK(authority.Acknowledge(2, json{{"type", "game_prepared"}, {"generation", 12}}, [](session::Connection, const json&) { return true; }));
	CHECK(authority.Acknowledge(1, json{{"type", "game_ready"}, {"generation", 12}}, [](session::Connection, const json&) { return true; }));
	CHECK(authority.Acknowledge(2, json{{"type", "game_ready"}, {"generation", 12}}, [](session::Connection, const json&) { return true; }));
	CHECK(authority.GetPhase() == session::MatchAuthority::Phase::Started);
	const auto digest = authority.CapabilityDigest();
	const auto swapped = std::vector<session::MatchAuthority::RebindEntry>{
		{{"relay", "member-a"}, 2}, {{"relay", "member-b"}, 1}};
	CHECK(authority.RebindConnections(swapped));
	CHECK(authority.CapabilityDigest() == digest);
	const auto rebound = authority.Checkpoint();
	for (const auto& row : rebound.at("participants")) {
		const auto endpoint = row.at("member").at("user").get<std::string>();
		if (endpoint == "member-a") CHECK(row.at("connection") == 2);
		if (endpoint == "member-b") CHECK(row.at("connection") == 1);
	}
	// The server passes the complete room roster to every table authority;
	// unrelated spectator endpoints must be accepted while only this authority's
	// participant subset is remapped.
	CHECK(authority.RebindConnections({{{"relay", "member-a"}, 1}, {{"relay", "member-b"}, 2}, {{"relay", "spectator"}, 3}}));
	CHECK(authority.CapabilityDigest() == digest);
	const auto beforeInvalid = authority.Checkpoint();
	CHECK(!authority.RebindConnections({{{"relay", "member-a"}, 1}, {{"relay", "member-b"}, 1}}));
	CHECK(authority.Checkpoint() == beforeInvalid);
}

// Room chat is sent to a roomChatDelta client only when its committed copy is
// stale; an older client always gets the full chat.
static void TestRoomChatDelta() {
	auto* transport = new MockTransport();
	SessionServer server("chat-delta", "build", true, 3, {0, 99}, std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> room = {};
	room[0] = 31;
	server.EnableMatchAuthorization(room, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Chat delta", 16, 48);
	std::uint64_t request = 1, revision = 0;
	constexpr std::uint64_t term = 7;
	server.SetAuthority(term, revision, true);
	const auto commit = [&]() {
		transport->outgoing.clear();
		CHECK(server.HasRecoveryCandidate());
		CHECK(server.ProposeCheckpoint(request, term, revision, nullptr));
		const auto proposal = server.PendingProposal();
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		++request; ++revision;
	};
	const auto admit = [&](session::Connection connection, bool chatDelta) {
		protocol::SessionJoinRequest join;
		join.username = "Chat-" + std::to_string(connection); join.sidecarHash = "build"; join.port = 30000;
		join.customRooms = true; join.roomProtocol = room::ProtocolVersion; join.roomChatDelta = chatDelta;
		protocol::SessionHelloMsg hello; hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		commit();
	};
	std::uint64_t actionId = 0;
	const auto act = [&](session::Connection connection, room::ActionKind kind, const std::string& text = "") {
		const auto snapshot = *server.RoomSnapshot();
		room::Action value;
		value.kind = kind; value.roomEpoch = snapshot.roomEpoch; value.revision = snapshot.revision;
		value.tableRevision = snapshot.tables[0].revision; value.actionId = ++actionId; value.table = 0; value.text = text;
		protocol::RoomActionMessage message; message.action = value;
		transport->Push(connection, json(message));
		CHECK(server.Step() == 0);
		commit();
	};
	// The broadcast snapshot each client received in the last commit.
	const auto snapshotFor = [&](session::Connection connection) {
		for (const auto& sent : transport->outgoing)
			if (sent.first == connection && sent.second.value("type", std::string()) == "room_snapshot") return sent.second.at("snapshot");
		return json();
	};
	admit(1, true);
	admit(2, false);
	act(1, room::ActionKind::Chat, "hello");
	CHECK(snapshotFor(1).at("chat").size() == 1 && snapshotFor(2).at("chat").size() == 1);
	act(1, room::ActionKind::Queue);
	CHECK(snapshotFor(1).value("chat_unchanged", false) && !snapshotFor(1).contains("chat"));
	CHECK(!snapshotFor(2).contains("chat_unchanged") && snapshotFor(2).at("chat").size() == 1);
	act(2, room::ActionKind::Chat, "again");
	CHECK(snapshotFor(1).at("chat").size() == 2);
	act(2, room::ActionKind::Queue);
	CHECK(snapshotFor(1).value("chat_unchanged", false));
	// A departure prunes the leaver's lines, which counts as a change.
	act(2, room::ActionKind::Leave);
	CHECK(snapshotFor(1).at("chat").size() == 1);
}

static void TestSameTermProposalPause() {
	auto* transport = new MockTransport();
	SessionServer server("same-term-pause", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> room = {};
	room[0] = 29;
	server.EnableMatchAuthorization(room, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Same-term pause", 16, 47);
	std::uint64_t request = 1, revision = 0;
	constexpr std::uint64_t term = 7;
	server.SetAuthority(term, revision, true);
	auto commitCandidate = [&](session::Connection bootstrapRecipient = 0) {
		CHECK(transport->outgoing.empty());
		CHECK(server.HasRecoveryCandidate());
		CHECK(server.ProposeCheckpoint(request, term, revision, server.RecoveryCheckpoint()));
		const auto proposal = server.PendingProposal();
		CHECK(proposal != nullptr);
		if (bootstrapRecipient) CHECK(std::any_of(proposal->effects.begin(), proposal->effects.end(),
			[&](const session::EffectEnvelope& effect) {
				return effect.recipient == bootstrapRecipient && effect.type == "room_snapshot";
			}));
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		transport->outgoing.clear();
		++request;
		++revision;
	};
	auto admit = [&](session::Connection connection) {
		protocol::SessionJoinRequest join;
		join.username = "Pause-" + std::to_string(connection);
		join.sidecarHash = "build";
		join.port = static_cast<std::uint16_t>(31000 + connection);
		join.customRooms = true;
		join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello;
		hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		commitCandidate(connection);
	};
	auto action = [&](session::Connection connection, room::ActionKind kind, std::uint64_t actionId) {
		const auto snapshot = *server.RoomSnapshot();
		room::Action value;
		value.kind = kind;
		value.roomEpoch = snapshot.roomEpoch;
		value.revision = snapshot.revision;
		value.tableRevision = snapshot.tables[0].revision;
		value.actionId = actionId;
		value.table = 0;
		protocol::RoomActionMessage message;
		message.action = value;
		transport->Push(connection, json(message));
		CHECK(server.Step() == 0);
	};
	for (session::Connection connection = 1; connection <= 4; ++connection) admit(connection);
	for (session::Connection connection = 1; connection <= 4; ++connection) {
		action(connection, room::ActionKind::Queue, 1);
		commitCandidate();
	}
	action(1, room::ActionKind::Ready, 2);
	commitCandidate();
	action(2, room::ActionKind::Ready, 2);
	CHECK(server.HasRecoveryCandidate());
	CHECK(server.ProposeCheckpoint(request, term, revision, server.RecoveryCheckpoint()));
	const auto proposal = server.PendingProposal();
	CHECK(proposal != nullptr);
	const auto privateGrantCount = std::count_if(proposal->effects.begin(), proposal->effects.end(), [](const auto& effect) {
		return effect.privatePayload && effect.type == "game_prepare";
	});
	CHECK(privateGrantCount == 4);

	// A same-term health pause can race the helper's durable commit watch. Keep
	// the exact local candidate and its private grants until the room is rebound;
	// importing the digest-only checkpoint would permanently lose those grants.
	server.SetAuthority(term, revision, false);
	CHECK(server.PendingProposal() != nullptr);
	CHECK(server.PendingProposal()->effects == proposal->effects);
	CHECK(!server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
	CHECK(transport->outgoing.empty());
	server.SetAuthority(term, revision, true);
	CHECK(server.PendingProposal() != nullptr);
	CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
	std::set<session::Connection> grantRecipients;
	for (const auto& sent : transport->outgoing) {
		if (sent.second.value("type", std::string()) != "game_prepare") continue;
		CHECK(sent.second.contains("_commit"));
		grantRecipients.insert(sent.first);
	}
	CHECK(grantRecipients == std::set<session::Connection>({1, 2, 3, 4}));
	const auto sentCount = transport->outgoing.size();
	CHECK(!server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
	CHECK(transport->outgoing.size() == sentCount);
}

static void TestCommittedSessionGate() {
	auto* transport = new MockTransport();
	SessionServer server("gated-room", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> room = {};
	room[0] = 17;
	server.EnableMatchAuthorization(room, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Gated room", 16, 23);
	server.SetAuthority(1, 0, true);
	std::uint64_t request = 1, revision = 0, term = 1;
	auto commitCandidate = [&](bool expectOutput, const char* expectedType = nullptr, int expectedRoomAcceptance = -1,
		int expectedRoomReason = -1, int expectedRoomwide = -1, int expectedEffectCount = -1) {
		// Every gated effect is withheld until the exact proposal is accepted.
		CHECK(transport->outgoing.empty());
		CHECK(server.HasRecoveryCandidate());
		const auto candidate = server.RecoveryCheckpoint();
		CHECK(server.ProposeCheckpoint(request, term, revision, candidate));
		const auto proposal = server.PendingProposal();
		CHECK(proposal != nullptr);
		if (expectedRoomwide >= 0) {
			const auto hasSnapshot = std::any_of(proposal->effects.begin(), proposal->effects.end(), [](const auto& effect) {
				return effect.type == "room_snapshot";
			});
			const auto hasProjection = std::any_of(proposal->effects.begin(), proposal->effects.end(), [](const auto& effect) {
				return effect.type == "data_update";
			});
			if (hasSnapshot != (expectedRoomwide != 0) || hasProjection != (expectedRoomwide != 0)) {
				std::cerr << "Committed room action effects expected_roomwide=" << expectedRoomwide << " types=";
				for (const auto& effect : proposal->effects) std::cerr << effect.type << ',';
				std::cerr << '\n';
			}
			CHECK(hasSnapshot == (expectedRoomwide != 0));
			CHECK(hasProjection == (expectedRoomwide != 0));
		}
		if (expectedEffectCount >= 0) CHECK(proposal->effects.size() == static_cast<std::size_t>(expectedEffectCount));
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		if (expectOutput) {
			CHECK(!transport->outgoing.empty());
			if (expectedType) {
				CHECK(std::any_of(transport->outgoing.begin(), transport->outgoing.end(), [&](const auto& message) {
					return message.second.value("type", std::string()) == expectedType;
				}));
			}
			if (expectedRoomAcceptance >= 0) {
				const auto response = std::find_if(transport->outgoing.begin(), transport->outgoing.end(), [](const auto& message) {
					return message.second.value("type", std::string()) == "room_result";
				});
				CHECK(response != transport->outgoing.end());
				CHECK(response->second.at("result").at("accepted").get<bool>() == (expectedRoomAcceptance != 0));
				if (expectedRoomReason >= 0)
					CHECK(response->second.at("result").at("reason").get<int>() == expectedRoomReason);
			}
		} else CHECK(transport->outgoing.empty());
		transport->outgoing.clear();
		++request;
		++revision;
	};
	auto admissionHello = [&](session::Connection connection, const char* name) {
		protocol::SessionJoinRequest join;
		join.username = name;
		join.sidecarHash = "build";
		join.port = static_cast<std::uint16_t>(30000 + connection);
		join.customRooms = true;
		join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello;
		hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		CHECK(server.cidMap.count(connection) == 1 && server.roomMembers.count(connection) == 1);
		commitCandidate(true, "hello_resp");
	};
	admissionHello(1, "Gate-one");
	admissionHello(2, "Gate-two");
	CheckIdleRecoveryWork(server,"two-member room");
	const auto action = [&](session::Connection connection, room::ActionKind kind, std::uint64_t actionId,
		std::uint8_t table = 0) {
		const auto snapshot = *server.RoomSnapshot();
		room::Action value;
		value.kind = kind;
		value.roomEpoch = snapshot.roomEpoch;
		value.revision = snapshot.revision;
		value.tableRevision = snapshot.tables[table].revision;
		value.actionId = actionId;
		value.table = table;
		protocol::RoomActionMessage message;
		message.action = value;
		transport->Push(connection, json(message));
		CHECK(server.Step() == 0);
	};
	action(1, room::ActionKind::Queue, 1);
	commitCandidate(true, "room_result");
	action(2, room::ActionKind::Queue, 1);
	commitCandidate(true, "room_result");
	action(1, room::ActionKind::Ready, 2);
	commitCandidate(true, "room_result");

	// The second Ready creates native capabilities privately.  A stale term
	// must discard that preparation before it can reach the transport.  A
	// fighter report arriving while the timer candidate is still private must
	// be appended to that same candidate; Step must not starve it behind the
	// later quorum dispute.
	action(2, room::ActionKind::Ready, 2);
	const auto candidateBeforeReport = server.RecoveryCheckpoint();
	// This is the first native generation for the table in this fixture; the
	// room checkpoint keeps the table model portable but does not expose its
	// internal generation under a stable test-only JSON shape.
	const auto candidateGeneration = std::uint64_t(1);
	transport->Push(1, json{{"type", "game_prepared"}, {"generation", candidateGeneration}});
	CHECK(server.Step() == 0);
	CHECK(server.HasRecoveryCandidate());
	CHECK(server.RecoveryCheckpoint() != candidateBeforeReport);
	CHECK(transport->outgoing.empty());
	CHECK(server.HasRecoveryCandidate());
	CHECK(server.ProposeCheckpoint(request, term, revision, server.RecoveryCheckpoint()));
	CHECK(server.PendingProposal() != nullptr);
	// The bytes retained for the helper are exactly the proposal's encoding.
	CHECK(server.PendingProposal()->encoded == json(*server.PendingProposal()).dump());
	server.SetAuthority(2, revision, false);
	CHECK(server.PendingProposal() == nullptr);
	CHECK(transport->outgoing.empty());
	CHECK(server.RoomSnapshot()->tables[0].phase != room::TablePhase::Playing);
	for (const auto& effect : server.CommittedEffectHistory()) {
		if (effect.type == "game_prepare") CHECK(effect.publicPayload.is_null());
	}

	// The restored Ready state can be promoted by the new term. First commit a
	// native preparation, then simulate its leadership loss. The replacement
	// owner must journal game_end and the room transition before a new grant.
	term = 2;
	server.SetAuthority(term, revision, true);
	action(2, room::ActionKind::Ready, 3);
	commitCandidate(true, "game_prepare");
	const auto interruptedGeneration = server.RoomSnapshot()->tables[0].matchGeneration;
	term = 3;
	server.SetAuthority(term, revision, false);
	CHECK(transport->outgoing.empty());
	server.SetAuthority(term, revision, true);
	CHECK(server.CancelInterruptedPreparations());
	CHECK(transport->outgoing.empty());
	commitCandidate(true, "game_end");
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Waiting);
	auto terminalAction = [&](session::Connection connection, std::uint64_t actionId,
		std::uint64_t generation = 0, std::uint64_t epoch = 0) {
		const auto snapshot = *server.RoomSnapshot();
		room::Action value;
		value.kind = room::ActionKind::AcknowledgeTerminal;
		value.roomEpoch = epoch ? epoch : snapshot.roomEpoch;
		value.revision = snapshot.revision;
		value.tableRevision = snapshot.tables[0].revision;
		value.actionId = actionId;
		value.table = 0;
		value.matchGeneration = generation ? generation : interruptedGeneration;
		protocol::RoomActionMessage message;
		message.action = value;
		transport->Push(connection, json(message));
		CHECK(server.Step() == 0);
	};
	// Recovery cancellation creates the same durable terminal obligation as a
	// played match. Both frozen native recipients must persist and acknowledge
	// that exact generation before either can start the next match.
	terminalAction(1, 3);
	commitCandidate(true, "room_result", 1, -1, 1);
	// The first ACK reply can disappear while unrelated allowed traffic advances
	// the member's generic action watermark. Its stable terminal identity must
	// remain idempotent without lowering that newer watermark.
	{
		const auto snapshot = *server.RoomSnapshot();
		room::Action chat;
		chat.kind = room::ActionKind::Chat;
		chat.roomEpoch = snapshot.roomEpoch;
		chat.revision = snapshot.revision;
		chat.actionId = 4;
		chat.text = "ACK watermark regression";
		protocol::RoomActionMessage message;
		message.action = chat;
		transport->Push(1, json(message));
		CHECK(server.Step() == 0);
		commitCandidate(true, "room_result", 1);
	}
	terminalAction(1, 3);
	// The exact duplicate still crosses the committed reply boundary, but the
	// already acknowledged receipt did not mutate the room and must not enqueue
	// another snapshot/projection burst for every member.
	commitCandidate(true, "room_result", 1, -1, 0, 1);
	// Even an action ID equal to the generic watermark cannot bypass the exact
	// terminal receipt checks.
	terminalAction(1, 4, interruptedGeneration + 100);
	commitCandidate(true, "room_result", 0, static_cast<int>(room::RejectReason::WrongGeneration), 0, 1);
	terminalAction(1, 4, interruptedGeneration, server.RoomSnapshot()->roomEpoch + 1);
	commitCandidate(true, "room_result", 0, static_cast<int>(room::RejectReason::StaleRoom), 0, 1);
	// Member two's ACK was allocated first but did not commit before a newer chat
	// action. The older exact ACK must still perform the receipt mutation.
	{
		const auto snapshot = *server.RoomSnapshot();
		room::Action chat;
		chat.kind = room::ActionKind::Chat;
		chat.roomEpoch = snapshot.roomEpoch;
		chat.revision = snapshot.revision;
		chat.actionId = 5;
		chat.text = "ACK committed after newer action";
		protocol::RoomActionMessage message;
		message.action = chat;
		transport->Push(2, json(message));
		CHECK(server.Step() == 0);
		commitCandidate(true, "room_result", 1);
	}
	terminalAction(2, 4);
	commitCandidate(true, "room_result", 1);

	// The next Ready pair receives a fresh generation after the committed
	// cancellation. The two no-effect acknowledgments below also commit native
	// authority progress before game_connect/game_start are released.
	action(1, room::ActionKind::Ready, 5);
	commitCandidate(true, "room_result");
	action(2, room::ActionKind::Ready, 6);
	commitCandidate(true, "game_prepare");
	auto generation = server.RoomSnapshot()->tables[0].matchGeneration;
	CHECK(generation > interruptedGeneration);
	auto acknowledge = [&](session::Connection connection, const char* type, std::uint64_t matchGeneration = 0) {
		transport->Push(connection, json{{"type", type}, {"generation", matchGeneration ? matchGeneration : generation}});
		CHECK(server.Step() == 0);
	};
	acknowledge(1, "game_prepared");
	commitCandidate(false);
	acknowledge(2, "game_prepared");
	commitCandidate(true, "game_connect");
	acknowledge(1, "game_ready");
	commitCandidate(false);
	acknowledge(2, "game_ready");
	commitCandidate(true, "game_start");
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	CheckIdleRecoveryWork(server,"playing");
	// Gameplay diagnostics used to be journaled as committed effects, so every
	// 30 frames of a fight built and proposed a full room checkpoint that
	// fenced the room until each peer imported it. They carry no room
	// mutation: forward them directly, with no candidate and no commit token.
	{
		const auto hashBuilds = server.RecoveryCheckpointBuilds();
		for (int frame = 30; frame <= 240; frame += 30) {
			protocol::BattleHashV2 hash;
			hash.frameIdx = frame; hash.fromPlayer = true;
			transport->Push(1, json(hash));
		}
		CHECK(server.Step() == 0);
		CHECK(transport->incoming.empty());
		CHECK(!server.HasRecoveryCandidate());
		CHECK(server.PendingProposal() == nullptr);
		CHECK(server.RecoveryCheckpointBuilds() == hashBuilds);
		CHECK(transport->outgoing.size() == 8);
		CHECK(std::all_of(transport->outgoing.begin(), transport->outgoing.end(), [](const auto& message) {
			return message.first == 2 && message.second.value("type", std::string()) == "battle_hash" &&
				!message.second.contains("_commit");
		}));
		transport->outgoing.clear();
	}
	// Keep another table in a healthy Started generation while table zero enters
	// result reconciliation. A pending or disputed result on one table must not
	// manufacture room-wide recovery work on every game-thread tick.
	admissionHello(3, "Gate-three");
	admissionHello(4, "Gate-four");
	action(3, room::ActionKind::Queue, 1, 1);
	commitCandidate(true, "room_result");
	action(4, room::ActionKind::Queue, 1, 1);
	commitCandidate(true, "room_result");
	action(3, room::ActionKind::Ready, 2, 1);
	commitCandidate(true, "room_result");
	action(4, room::ActionKind::Ready, 2, 1);
	commitCandidate(true, "game_prepare");
	const auto otherGeneration = server.RoomSnapshot()->tables[1].matchGeneration;
	acknowledge(3, "game_prepared", otherGeneration);
	commitCandidate(false);
	acknowledge(4, "game_prepared", otherGeneration);
	commitCandidate(true, "game_connect");
	acknowledge(3, "game_ready", otherGeneration);
	commitCandidate(false);
	acknowledge(4, "game_ready", otherGeneration);
	commitCandidate(true, "game_start");
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing &&
		server.RoomSnapshot()->tables[1].phase == room::TablePhase::Playing);
	// A same-term follower transition must freeze the relative result age even
	// when no new term is elected. The writable tick later rebases the clock
	// and preserves the five-second age rather than disputing after 40 seconds
	// of passive wall time.
	// The preceding term-loss recovery leaves the imported authority paused.
	// Rebase it before creating the result so the first five-second tick is a
	// real timer candidate rather than the initial writable resume.
	server.AdvanceCustomRoom(0);
	transport->outgoing.clear();
	const auto playing = *server.RoomSnapshot();
	room::Action finished;
	finished.kind = room::ActionKind::MatchFinished;
	finished.roomEpoch = playing.roomEpoch;
	finished.revision = playing.revision;
	finished.tableRevision = playing.tables[0].revision;
	finished.actionId = 100;
	finished.table = 0;
	finished.matchGeneration = playing.tables[0].matchGeneration;
	protocol::RoomActionMessage finishedMessage; finishedMessage.action = finished;
	transport->Push(1, json(finishedMessage));
	CHECK(server.Step() == 0);
	commitCandidate(true, "room_result");
	const auto pendingBuilds = server.RecoveryCheckpointBuilds();
	server.AdvanceCustomRoom(5000);
	CHECK(!server.HasRecoveryCandidate());
	CHECK(server.RecoveryCheckpointBuilds() == pendingBuilds);
	// Advancing the owner clock before the deadline is local bookkeeping. The
	// five-second age must still survive a follower interval without requiring
	// a private candidate or quorum proposal.
	auto timed = server.RecoveryCheckpoint();
	CHECK(timed.at("room").at("snapshot").at("tables").at(0).at("result_pending") == true);
	CHECK(timed.at("room").at("result_age").at(0) == 5000);
	server.SetAuthority(term, revision, false);
	server.AdvanceCustomRoom(45000);
	timed = server.RecoveryCheckpoint();
	CHECK(timed.at("room").at("result_age").at(0) == 5000);
	server.SetAuthority(term, revision, true);
	server.AdvanceCustomRoom(45000);
	timed = server.RecoveryCheckpoint();
	CHECK(timed.at("room").at("result_age").at(0) == 5000);
	// A Started generation survives another term transition and is not treated
	// as an interrupted preparation.
	term = 4;
	server.SetAuthority(term, revision, false);
	server.AdvanceCustomRoom(90000);
	timed = server.RecoveryCheckpoint();
	CHECK(timed.at("room").at("result_age").at(0) == 5000);
	server.SetAuthority(term, revision, true);
	CHECK(server.CancelInterruptedPreparations());
	CHECK(!server.HasRecoveryCandidate());
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Playing);
	CHECK(server.RoomSnapshot()->tables[1].phase == room::TablePhase::Playing);
	// Resume the five-second age at the new owner's clock, then cross the exact
	// thirty-second deadline once. Only that transition should create a
	// candidate, and the unrelated table must remain in its healthy fight.
	const auto resumedBuilds = server.RecoveryCheckpointBuilds();
	server.AdvanceCustomRoom(90000);
	CHECK(!server.HasRecoveryCandidate() && server.RecoveryCheckpointBuilds() == resumedBuilds);
	server.AdvanceCustomRoom(115000);
	CHECK(server.HasRecoveryCandidate());
	commitCandidate(true, "room_snapshot");
	CHECK(server.RoomSnapshot()->tables[0].phase == room::TablePhase::Paused);
	CHECK(server.RoomSnapshot()->tables[0].resultPending);
	CHECK(server.RoomSnapshot()->tables[1].phase == room::TablePhase::Playing);
	CheckIdleRecoveryWork(server, "paused result beside playing table");
}

// A member's own game behind the scripted helper, with the committed leader
// as a peer, for the client queue's effect gate: live effects arrive from the
// leader, commits arrive as transfers, and RoomRecoveryRuntime activates them
// against a replica server.
struct ParkedEffectRoom {
	ParticipantRoom fixture{SetupStage::FightersPrepared};
	static constexpr session::Connection self = 2;
	json checkpoint;
	session::RosterRow local;
	FakeHelperPipe pipe;
	platform::HelperClient helper;
	std::shared_ptr<session::IrohRoom> room;
	std::unique_ptr<session::ClientTransport> client;
	const std::array<std::uint8_t, 16> id{{9}};
	const std::string leader = std::string(64, 'a');
	std::int64_t nextMessage = 2;

	// `leaderLocal`: the helper reports this game's room as the committed
	// leader, so it may propose checkpoints of its own.
	explicit ParkedEffectRoom(bool leaderLocal = false) {
		checkpoint = fixture.server.RecoveryCheckpoint();
		CHECK(session::ActiveRosterRow(checkpoint.at("members"), ParticipantRoom::IdentityOf(self), local) && local.member);
		pipe.Connect(helper);
		room = std::make_shared<session::IrohRoom>(helper);
		CHECK(room->Join("invitation", "build"));
		client = room->Client();
		CHECK(client != nullptr);
		std::map<std::string, std::uint64_t> incarnations{{leader, 1}};
		for (const auto& row : checkpoint.at("members"))
			if (!row.value("frozen", false))
				incarnations[row.at("data").at("authenticatedEndpoint").get<std::string>()] = row.at("incarnation").get<std::uint64_t>();
		pipe.Send({{"type", "status"}, {"request_id", 1}, {"endpoint", ParticipantRoom::IdentityOf(self)}, {"fixed_port", 0}});
		pipe.Send({{"type", "coordination_state"}, {"epoch", 1}, {"room", id}, {"term", 1}, {"revision", 2},
			{"incarnation", ParticipantRoom::IncarnationOf(self)}, {"leader", leader}, {"writable", true}, {"leader_local", leaderLocal}});
		pipe.Send({{"type", "control_rebound"}, {"epoch", 1}, {"room", id}, {"leader", leader},
			{"member_incarnations", incarnations}, {"members", json::array({leader})}});
	}
	// This member's data_update committed at `revision` in term 1.
	session::EffectEnvelope Projection(std::uint64_t revision) const {
		session::EffectEnvelope effect;
		effect.sequence = revision; effect.roomEpoch = 9; effect.term = 1; effect.revision = revision;
		effect.recipient = local.member; effect.endpoint = local.endpoint; effect.type = "data_update";
		effect.publicPayload = json{{"type", "data_update"}, {"marker", revision}};
		effect.payloadDigest = session::PayloadDigest(effect.publicPayload);
		return effect;
	}
	std::string Proposal(std::uint64_t revision, const std::vector<session::EffectEnvelope>& journal,
		const std::vector<session::EffectEnvelope>& effects) const {
		session::SessionProposal proposal;
		proposal.request = revision; proposal.term = 1; proposal.baseRevision = revision - 1;
		proposal.checkpoint = checkpoint;
		proposal.checkpoint["effect_journal"] = journal;
		proposal.effects = effects;
		proposal.effectsDigest = session::EffectsDigest(proposal.effects);
		return json(proposal).dump();
	}
	// The leader's live send of one committed effect.
	void SendLive(const std::string& wire) {
		pipe.Send({{"type", "message"}, {"epoch", 1}, {"peer", leader}, {"message_id", nextMessage++}, {"payload", wire}});
	}
	void Transfer(std::uint64_t revision, const std::string& bytes) {
		const json transfer{{"epoch", 1}, {"room", id}, {"transfer", revision}, {"term", 1},
			{"base_revision", revision - 1}, {"revision", revision}};
		auto begin = transfer; begin["type"] = "checkpoint_begin";
		begin["length"] = bytes.size(); begin["digest"] = coordination::Sha256(bytes);
		pipe.Send(begin);
		for (std::size_t offset = 0; offset < bytes.size(); offset += coordination::CheckpointChunk) {
			auto chunk = transfer; chunk["type"] = "checkpoint_chunk";
			chunk["offset"] = offset; chunk["data"] = coordination::Encode(bytes.substr(offset, coordination::CheckpointChunk));
			pipe.Send(chunk);
		}
		auto end = begin; end["type"] = "checkpoint_end"; pipe.Send(end);
		auto committed = begin; committed["type"] = "checkpoint_committed"; pipe.Send(committed);
	}
	// Polls the client long enough for every helper event sent so far to be
	// handled (the room handles at most 8 per poll).
	void Settle(std::vector<session::Message>& received, int polls = 50) {
		for (int tick = 0; tick < polls; ++tick) {
			CHECK(client->Poll(received, 8));
			pipe.Drain();
			Sleep(1);
		}
	}
};

// This member's live data_update of commit k arrives before commit k is
// staged and parks at the head of the client queue. Commits k and k+1 are then
// both staged and one RoomRecoveryRuntime::Tick activates them. k+1 supersedes
// that projection; its compacted journal keeps k's envelope digest-only, so the
// parked effect is still authenticated and delivered rather than dropped.
static void TestParkedProjectionAcrossStagedCommits() {
	ParkedEffectRoom member;
	const auto first = member.Projection(1), second = member.Projection(2);
	const std::vector<std::string> proposals{member.Proposal(1, {}, {first}), member.Proposal(2, {first}, {second})};
	{
		const auto decoded = session::CheckpointDecodeWorker::Decode(1, proposals[1]);
		CHECK(decoded.ok && decoded.journal.size() == 2);
		CHECK(decoded.journal[0] == first && decoded.journal[0].publicPayload.is_null());
		CHECK(decoded.journal[1] == second && !decoded.journal[1].publicPayload.is_null());
	}
	member.SendLive(session::CommittedEffectWire(first, first.publicPayload.dump()));
	std::vector<session::Message> received;
	member.Settle(received);
	// Parked: its commit is not activated yet.
	CHECK(received.empty());
	const auto parked = member.room->RecoveryState();
	CHECK(parked.clientQueueMessages == 1 && parked.clientHeadCommitted && parked.clientHeadRevision == 1);

	for (std::uint64_t revision = 1; revision <= 2; ++revision) member.Transfer(revision, proposals[revision - 1]);
	// Both commits staged before the runtime runs.
	for (int tick = 0; tick < 5000 && member.room->CommittedCheckpointsBehindHead() < 1; ++tick) {
		member.room->Poll();
		member.pipe.Drain();
		Sleep(1);
	}
	CHECK(member.room->CommittedCheckpointsBehindHead() == 1);
	const auto replica = member.fixture.Replica();
	session::RoomRecoveryRuntime recovery;
	CHECK(recovery.Tick(*replica, *member.room));
	CHECK(recovery.AppliedRevision() == 2 && recovery.Error().empty() && !member.room->CheckpointPending());

	// The parked effect of commit k is delivered first, then commit k+1's replay.
	CHECK(member.client->Poll(received, 8));
	CHECK(received.size() == 2);
	if (received.size() == 2) {
		const auto delivered = json::parse(received[0].payload), replayed = json::parse(received[1].payload);
		CHECK(delivered.at("marker") == 1 && !delivered.contains("_commit"));
		CHECK(replayed.at("marker") == 2 && !replayed.contains("_commit"));
	}
}

// A committed effect whose commit is still pending parks at the client queue
// head, so its digest is checked on first sight: a corrupt payload, or a forged
// token claiming a future term or revision, is dropped at once instead of
// parking the queue. The valid effect behind them parks, and stays parked
// across polls, until its commit activates.
static void TestCorruptTokenDroppedOnFirstSight() {
	ParkedEffectRoom member;
	const auto first = member.Projection(1);
	auto corrupt = first.publicPayload; corrupt["marker"] = 99;
	member.SendLive(session::CommittedEffectWire(first, corrupt.dump()));
	auto future = first; future.term = 5; future.revision = 7; future.sequence = 70;
	member.SendLive(session::CommittedEffectWire(future, corrupt.dump()));
	member.SendLive(session::CommittedEffectWire(first, first.publicPayload.dump()));
	std::vector<session::Message> received;
	member.Settle(received);
	CHECK(received.empty());
	const auto parked = member.room->RecoveryState();
	CHECK(parked.clientQueueMessages == 1 && parked.clientHeadCommitted &&
		parked.clientHeadRevision == 1 && parked.clientHeadSequence == 1 && parked.clientHeadTerm == 1);
	// A pending transfer of its commit changes nothing until it activates.
	const auto proposal = member.Proposal(1, {}, {first});
	member.pipe.Send({{"type", "checkpoint_begin"}, {"epoch", 1}, {"room", member.id}, {"transfer", 1}, {"term", 1},
		{"base_revision", 0}, {"revision", 1}, {"length", proposal.size()}, {"digest", coordination::Sha256(proposal)}});
	member.Settle(received);
	CHECK(received.empty() && member.room->RecoveryState().clientQueueMessages == 1);
}

// The leader's own commit marker. When the helper commits the record this game
// proposed, it sends checkpoint_committed_local instead of the bytes, and the
// game answers checkpoint_ack: the full length when it adopts the copy it still
// holds, 0 when it cannot, after which the helper sends the whole transfer.
namespace local_commit {
void PollRoom(ParkedEffectRoom& member, int polls = 5) {
	for (int tick = 0; tick < polls; ++tick) { member.room->Poll(); member.pipe.Drain(); Sleep(1); }
}
// Leader of term 1 at revision 2: this game proposes revision 3 as transfer 3,
// and the scripted helper credits every chunk until the game sends its end, as
// the helper does before it commits the record.
coordination::TransferIdentity Propose(ParkedEffectRoom& member, const std::string& bytes) {
	for (int tick = 0; tick < 5000 && member.room->GetState() != session::IrohRoom::State::Ready; ++tick) PollRoom(member, 1);
	CHECK(member.room->GetState() == session::IrohRoom::State::Ready && member.room->Coordination().leaderLocal);
	CHECK(member.room->ProposeCheckpointBytes(3, 1, 2, bytes));
	coordination::TransferIdentity identity;
	identity.room = member.id; identity.epoch = 1; identity.transfer = 3; identity.term = 1;
	identity.baseRevision = 2; identity.revision = 3; identity.length = bytes.size(); identity.digest = coordination::Sha256(bytes);
	for (int round = 0; round < 500 && !member.room->RecoveryState().proposalEnded; ++round) {
		member.pipe.Send({{"type", "checkpoint_ack"}, {"epoch", 1}, {"room", member.id}, {"transfer", identity.transfer},
			{"offset", member.room->RecoveryState().proposalSent}});
		PollRoom(member, 2);
	}
	const auto state = member.room->RecoveryState();
	CHECK(state.proposalEnded && state.proposalAcknowledged == bytes.size() && member.room->ProposalInFlight());
	return identity;
}
// The checkpoint_ack offsets the game sent for `transfer` since `received`
// was last cleared. Raw-body chunk frames parse as discarded values and are skipped.
std::vector<std::size_t> Acks(ParkedEffectRoom& member, std::uint64_t transfer) {
	std::vector<std::size_t> offsets;
	for (const auto& command : member.pipe.received) {
		if (!command.is_object() || command.value("type", std::string()) != "checkpoint_ack" ||
			command.at("transfer") != transfer) continue;
		CHECK(command.at("epoch") == 1 && command.at("room") == member.id);
		offsets.push_back(command.at("offset").get<std::size_t>());
	}
	return offsets;
}
// Sends the marker for `identity` and returns the one answer the game gave it.
std::size_t MarkerAnswer(ParkedEffectRoom& member, const coordination::TransferIdentity& identity) {
	auto marker = identity.Envelope("checkpoint_committed_local");
	marker["length"] = identity.length; marker["digest"] = identity.digest;
	member.pipe.Send(marker);
	member.pipe.received.clear();
	PollRoom(member);
	const auto answers = Acks(member, identity.transfer);
	CHECK(answers.size() == 1);
	return answers.empty() ? (std::numeric_limits<std::size_t>::max)() : answers.front();
}
// Waits for `count` staged commits and returns the head.
session::IrohRoom::CommittedCheckpoint Staged(ParkedEffectRoom& member, std::size_t count) {
	session::IrohRoom::CommittedCheckpoint head;
	for (int tick = 0; tick < 5000 && member.room->CommittedCheckpointsBehindHead() + 1 < count; ++tick) PollRoom(member, 1);
	for (int tick = 0; tick < 5000 && !member.room->TakeCommittedCheckpoint(head); ++tick) PollRoom(member, 1);
	CHECK(member.room->CommittedCheckpointsBehindHead() + 1 == count && head.proposal != nullptr);
	return head;
}
bool Same(const coordination::TransferIdentity& left, const coordination::TransferIdentity& right) {
	return left.room == right.room && left.epoch == right.epoch && left.transfer == right.transfer &&
		left.term == right.term && left.baseRevision == right.baseRevision && left.revision == right.revision &&
		left.length == right.length && left.digest == right.digest;
}
// The helper's full transfer of `bytes` as transfer 3, after an answer of 0,
// is received, acknowledged to its full length and staged like any commit.
void ExpectFullTransferStaged(ParkedEffectRoom& member, const std::string& bytes, std::size_t staged) {
	// A checkpoint-scope helper error counts as one; the transfer adds none.
	const auto errors = member.room->RecoveryState().checkpointTransferErrors;
	member.pipe.received.clear();
	member.Transfer(3, bytes);
	auto head = Staged(member, staged);
	if (staged == 2) {
		CHECK(head.identity.transfer == 2 && member.room->DiscardCommittedCheckpoint(head.identity));
		head = Staged(member, 1);
	}
	CHECK(head.identity.transfer == 3 && head.identity.revision == 3 && head.identity.length == bytes.size() &&
		head.identity.digest == coordination::Sha256(bytes) && head.proposal->request == 3);
	const auto acks = Acks(member, 3);
	CHECK(!acks.empty() && acks.back() == bytes.size());
	CHECK(!member.room->ProposalInFlight() && member.room->RecoveryState().checkpointTransferErrors == errors);
}
}

// Success: the marker for the exact proposal is answered with the full length,
// and the retained bytes are staged as the commit without a transfer.
static void TestLocalCommitAdoptsOwnProposal() {
	using namespace local_commit;
	ParkedEffectRoom member(true);
	const auto bytes = member.Proposal(3, {}, {member.Projection(3)});
	const auto identity = Propose(member, bytes);
	CHECK(MarkerAnswer(member, identity) == bytes.size());
	const auto head = Staged(member, 1);
	CHECK(Same(head.identity, identity) && head.proposal->request == 3 && head.proposal->effects.size() == 1);
	const auto state = member.room->RecoveryState();
	CHECK(state.proposalStatus == "committed" && !member.room->ProposalInFlight() && state.checkpointTransferErrors == 0);
}

// Each way the retained proposal can be missing or wrong is answered with 0,
// and the full transfer that follows is accepted.
static void TestLocalCommitFallsBackToFullTransfer() {
	using namespace local_commit;
	// A term change cleared the retained copy before the marker arrived.
	{
		ParkedEffectRoom member(true);
		const auto bytes = member.Proposal(3, {}, {member.Projection(3)});
		const auto identity = Propose(member, bytes);
		member.pipe.Send({{"type", "coordination_state"}, {"epoch", 1}, {"room", member.id}, {"term", 2}, {"revision", 3},
			{"incarnation", ParticipantRoom::IncarnationOf(member.self)}, {"leader", member.leader}, {"writable", true},
			{"leader_local", true}});
		PollRoom(member);
		CHECK(!member.room->ProposalInFlight() && member.room->RecoveryState().proposalStatus == "term_changed");
		CHECK(MarkerAnswer(member, identity) == 0);
		CHECK(!member.room->CheckpointPending());
		ExpectFullTransferStaged(member, bytes, 1);
	}
	// A checkpoint-scope helper error cleared it.
	{
		ParkedEffectRoom member(true);
		const auto bytes = member.Proposal(3, {}, {member.Projection(3)});
		const auto identity = Propose(member, bytes);
		member.pipe.Send({{"type", "error"}, {"epoch", 1}, {"code", "checkpoint_send_timeout"}});
		PollRoom(member);
		CHECK(!member.room->ProposalInFlight() && member.room->RecoveryState().proposalStatus == "checkpoint_send_timeout");
		CHECK(member.room->GetState() == session::IrohRoom::State::Ready);
		CHECK(MarkerAnswer(member, identity) == 0);
		CHECK(!member.room->CheckpointPending());
		ExpectFullTransferStaged(member, bytes, 1);
	}
	// The receiver holds another transfer, here revision 2 still arriving.
	// The answer is 0 and nothing is adopted; once that transfer completes, the
	// full transfer of the own proposal is staged behind it.
	{
		ParkedEffectRoom member(true);
		const auto bytes = member.Proposal(3, {}, {member.Projection(3)});
		const auto previous = member.Proposal(2, {}, {member.Projection(2)});
		const auto identity = Propose(member, bytes);
		member.pipe.Send({{"type", "checkpoint_begin"}, {"epoch", 1}, {"room", member.id}, {"transfer", 2}, {"term", 1},
			{"base_revision", 1}, {"revision", 2}, {"length", previous.size()}, {"digest", coordination::Sha256(previous)}});
		PollRoom(member);
		CHECK(member.room->RecoveryState().checkpointActive && member.room->RecoveryState().checkpointTransfer == 2);
		CHECK(MarkerAnswer(member, identity) == 0);
		const auto busy = member.room->RecoveryState();
		CHECK(busy.checkpointTransfer == 2 && !busy.checkpointComplete && member.room->ProposalInFlight());
		member.Transfer(2, previous);
		ExpectFullTransferStaged(member, bytes, 2);
		CHECK(member.room->RecoveryState().proposalStatus == "committed");
	}
	// The committed record is not the one proposed here: same transfer, other
	// bytes. The digest differs, so the answer is 0 and the committed bytes
	// arrive in full.
	{
		ParkedEffectRoom member(true);
		const auto bytes = member.Proposal(3, {}, {member.Projection(3)});
		const auto committed = member.Proposal(3, {}, {});
		CHECK(committed != bytes);
		auto identity = Propose(member, bytes);
		identity.length = committed.size(); identity.digest = coordination::Sha256(committed);
		CHECK(MarkerAnswer(member, identity) == 0);
		CHECK(!member.room->CheckpointPending() && !member.room->RecoveryState().checkpointActive);
		ExpectFullTransferStaged(member, committed, 1);
		CHECK(Staged(member, 1).proposal->effects.empty());
	}
}

// The tag a room edge fills from its one parse says what each reader that
// used to parse the payload concluded, for every shape of payload.
static void TestRoomMessageTags() {
	const auto action = [](room::ActionKind kind, std::uint32_t version) {
		protocol::RoomActionMessage message;
		message.action.kind = kind; message.action.protocolVersion = version; message.action.actionId = 7;
		return json(message);
	};
	auto malformedAction = action(room::ActionKind::RecordResult, room::ProtocolVersion);
	malformedAction["action"] = "not an action";
	std::vector<std::string> payloads = {
		action(room::ActionKind::RecordResult, room::ProtocolVersion).dump(),
		action(room::ActionKind::AcknowledgeTerminal, room::ProtocolVersion).dump(),
		action(room::ActionKind::MatchFinished, room::ProtocolVersion).dump(),
		action(room::ActionKind::RecordResult, room::ProtocolVersion - 1).dump(),
		action(room::ActionKind::Ready, room::ProtocolVersion).dump(),
		malformedAction.dump(),
		json{{"type", "battle_hash"}, {"frameIdx", 3}}.dump(),
		json{{"type", "battle_snapshot"}}.dump(),
		json{{"type", 5}}.dump(),
		json{{"type", "hello_resp"}, {"_commit", json::object()}}.dump(),
		json{{"type", "room_event"}, {"_terminal_replay", json::object()}}.dump(),
		json{{"kind", "no type"}}.dump(),
		"[1,2]", "\"room_action\"", "not json", ""};
	for (const auto& payload : payloads) {
		const session::Message plain{4, 9, payload, ""};
		session::Message tagged = plain;
		tagged.tag = session::ClassifyRoomMessage(payload);
		CHECK(tagged.tag.classified);
		// The verification and retry readers agree with their own parse.
		CHECK(session::IsRoomVerificationMessage(tagged) == session::IsRoomVerificationMessage(plain));
		CHECK(session::SameQueuedRoomRetry(plain, tagged) == session::SameQueuedRoomRetry(plain, plain));
		const auto decoded = json::parse(payload, nullptr, false);
		const bool object = !decoded.is_discarded() && decoded.is_object();
		std::string type;
		if (object && decoded.contains("type") && decoded.at("type").is_string()) type = decoded.at("type").get<std::string>();
		CHECK(tagged.tag.type == type);
		CHECK(tagged.tag.committed == (object && decoded.contains("_commit")));
		CHECK(tagged.tag.terminalReplay == (object && decoded.contains("_terminal_replay")));
		bool roomAction = false;
		std::uint32_t version = 0;
		room::ActionKind kind = room::ActionKind::Join;
		if (object) {
			try {
				const auto parsed = decoded.get<protocol::RoomActionMessage>();
				roomAction = parsed.type == protocol::MT_ROOM_ACTION;
				version = parsed.action.protocolVersion; kind = parsed.action.kind;
			} catch (const std::exception&) {}
		}
		CHECK(tagged.tag.roomAction == roomAction);
		if (roomAction) CHECK(tagged.tag.protocolVersion == version && tagged.tag.actionKind == static_cast<std::uint8_t>(kind));
		// Classifying the parsed object is the same as classifying its text.
		if (!decoded.is_discarded()) {
			const auto fromObject = session::ClassifyRoomMessage(decoded);
			CHECK(fromObject.type == tagged.tag.type && fromObject.committed == tagged.tag.committed &&
				fromObject.terminalReplay == tagged.tag.terminalReplay && fromObject.verification == tagged.tag.verification &&
				fromObject.roomAction == tagged.tag.roomAction && fromObject.actionKind == tagged.tag.actionKind &&
				fromObject.protocolVersion == tagged.tag.protocolVersion);
		}
	}
}

// The admission fence opens when the client says it is about to send its
// hello, not by recognising the hello inside Send. Only then does a bare
// hello_resp from the leader reach the client, with its parsed body attached
// at drain; join_rej closes the fence again.
static void TestAdmissionFence() {
	ParkedEffectRoom member;
	std::vector<session::Message> received;
	member.Settle(received);
	const auto hello = json{{"type", "hello_resp"}, {"cid", {{"host", "relay"}, {"user", "early"}}}};
	member.SendLive(hello.dump());
	member.Settle(received);
	CHECK(received.empty());
	member.client->BeginAdmission();
	member.SendLive(hello.dump());
	member.Settle(received);
	CHECK(received.size() == 1);
	if (received.size() == 1) {
		CHECK(json::parse(received[0].payload) == hello);
		CHECK(received[0].body && *received[0].body == hello);
	}
	received.clear();
	member.SendLive(json{{"type", "join_rej"}}.dump());
	member.Settle(received);
	CHECK(received.size() == 1 && json::parse(received[0].payload).at("type") == "join_rej");
	received.clear();
	member.SendLive(hello.dump());
	member.Settle(received);
	CHECK(received.empty());
}

int main() {
	TestParkedProjectionAcrossStagedCommits();
	TestCorruptTokenDroppedOnFirstSight();
	TestLocalCommitAdoptsOwnProposal();
	TestLocalCommitFallsBackToFullTransfer();
	TestRoomMessageTags();
	TestAdmissionFence();
	TestAtomicMatchRebind();
	TestRoomChatDelta();
	TestSameTermProposalPause();
	TestCommittedSessionGate();
	std::cout << "Session server mock transport commit tests passed\n";
}
