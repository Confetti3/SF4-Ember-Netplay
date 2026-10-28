#include "session_server_transport_support.hxx"

static void TestMaximumRoomResultBurst() {
	auto* transport = new MockTransport();
	SessionServer server("maximum-result-burst", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> authorizationRoom = {};
	authorizationRoom[0] = 61;
	server.EnableMatchAuthorization(authorizationRoom, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Maximum result burst", room::MaximumMembers, 71);
	constexpr std::uint64_t term = 5;
	std::uint64_t request = 1, revision = 0;
	server.SetAuthority(term, revision, true);
	std::set<std::pair<session::Connection, std::uint64_t>> replies;
	std::size_t acceptedReplies = 0;
	std::size_t latestProposalBytes = 0, latestProposalEffects = 0;
	auto commitCandidate = [&](bool collectReplies) {
		CHECK(server.HasRecoveryCandidate());
		CHECK(!server.RecoveryCandidateOverflowed());
		CHECK(server.ProposeCheckpoint(request, term, revision, server.RecoveryCheckpoint()));
		const auto proposal = server.PendingProposal();
		CHECK(proposal != nullptr);
		latestProposalBytes = json(*proposal).dump().size();
		latestProposalEffects = proposal->effects.size();
		CHECK(session::EffectJournalBytes(proposal->effects) <= session::MaxEffectJournalBytes);
		CHECK(latestProposalBytes <= 1024 * 1024);
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		for (const auto& outgoing : transport->outgoing) {
			CHECK(outgoing.second.dump().size() <= 64 * 1024);
			if (collectReplies && outgoing.second.value("type", std::string()) == "room_result") {
				replies.emplace(outgoing.first, outgoing.second.value("actionId", std::uint64_t(0)));
				if (outgoing.second.at("result").at("accepted").get<bool>()) ++acceptedReplies;
			}
		}
		transport->outgoing.clear();
		++request; ++revision;
	};
	for (session::Connection connection = 1; connection <= room::MaximumMembers; ++connection) {
		protocol::SessionJoinRequest join;
		join.username = "Burst-" + std::to_string(connection);
		join.sidecarHash = "build";
		join.port = static_cast<std::uint16_t>(32000 + connection);
		join.customRooms = true;
		join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello;
		hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		commitCandidate(false);
	}
	CHECK(server.RoomSnapshot()->members.size() == room::MaximumMembers);

	// Fill the retained public projection to its validated chat bound. Quotes
	// and backslashes exercise JSON expansion; the first ordinary message also
	// proves that player text containing "capability" remains replayable.
	for (std::uint64_t actionId = 1; actionId <= room::MaximumChatMessages; ++actionId) {
		server.AdvanceCustomRoom(actionId * 1000);
		CHECK(!server.HasRecoveryCandidate());
		const auto snapshot = *server.RoomSnapshot();
		room::Action action;
		action.kind = room::ActionKind::Chat;
		action.roomEpoch = snapshot.roomEpoch;
		action.revision = snapshot.revision;
		action.tableRevision = snapshot.tables[0].revision;
		action.actionId = actionId;
		if (actionId == 1) action.text = std::string(room::MaximumChatBytes - 10, 'x') + "capability";
		else action.text = std::string(room::MaximumChatBytes, actionId & 1 ? '\\' : '"');
		protocol::RoomActionMessage message;
		message.action = action;
		transport->Push(1, json(message));
		CHECK(server.Step() == 0);
		commitCandidate(false);
	}
	CHECK(server.RoomSnapshot()->chat.size() == room::MaximumChatMessages);
	CHECK(std::none_of(server.CommittedEffectHistory().begin(), server.CommittedEffectHistory().end(),
		[](const session::EffectEnvelope& effect) { return effect.type == "room_snapshot" && effect.privatePayload; }));

	// The first queued intent mutates the full room. Fifteen further members
	// submit explicit stale-room actions at the same time. Recovery polls one
	// intent per candidate, so every large result is committed and delivered
	// without overflowing or losing the remaining bounded transport queue.
	server.AdvanceCustomRoom((room::MaximumChatMessages + 1) * 1000);
	CHECK(!server.HasRecoveryCandidate());
	const auto beforeBurst = *server.RoomSnapshot();
	for (session::Connection connection = 1; connection <= room::MaximumMembers; ++connection) {
		room::Action action;
		action.kind = room::ActionKind::Chat;
		action.roomEpoch = connection == 1 ? beforeBurst.roomEpoch : beforeBurst.roomEpoch + 1;
		action.revision = beforeBurst.revision;
		action.tableRevision = beforeBurst.tables[0].revision;
		action.actionId = connection == 1 ? room::MaximumChatMessages + 1 : 1;
		action.text = std::string(room::MaximumChatBytes, connection & 1 ? '\\' : '"');
		protocol::RoomActionMessage message;
		message.action = action;
		transport->Push(connection, json(message));
	}
	for (std::size_t remaining = room::MaximumMembers; remaining > 0; --remaining) {
		CHECK(transport->incoming.size() == remaining);
		CHECK(server.Step() == 0);
		CHECK(transport->incoming.size() == remaining - 1);
		commitCandidate(true);
		if (remaining == room::MaximumMembers) {
			CHECK(latestProposalEffects == room::MaximumMembers * 2 + 1);
			std::cout << "Maximum retained-room single-intent proposal bytes=" << latestProposalBytes
				<< " effects=" << latestProposalEffects << '\n';
		}
	}
	CHECK(replies.size() == room::MaximumMembers);
	CHECK(acceptedReplies == 1);
	for (session::Connection connection = 1; connection <= room::MaximumMembers; ++connection)
		CHECK(replies.count({connection, connection == 1 ? room::MaximumChatMessages + 1 : 1}) == 1);
}

// A recipient that missed its MatchEnded (lost while reconnecting) can never
// acknowledge the receipt, and the table stays fenced. The server must be
// able to replay exactly the receipts a member still owes: once, to the
// member's connection, with the terminal-replay flag, plus the native
// game_end the fighter may still be waiting for.
static void TestTerminalReceiptReplay() {
	auto* transport = new MockTransport();
	SessionServer server("terminal-replay", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> authorizationRoom = {};
	authorizationRoom[0] = 74;
	server.EnableMatchAuthorization(authorizationRoom, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Terminal replay", 4, 80);
	CHECK(server.Listen(0) == 0);
	for (session::Connection connection = 1; connection <= 3; ++connection) {
		protocol::SessionJoinRequest join;
		join.username = "Replay-" + std::to_string(connection);
		join.sidecarHash = "build";
		join.port = static_cast<std::uint16_t>(34000 + connection);
		join.customRooms = true;
		join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello;
		hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		CHECK(server.roomMembers.count(connection) == 1);
	}
	const auto admitted = *server.RoomSnapshot();
	room::RoomAuthority model("Terminal replay", 4, admitted.roomEpoch);
	for (const auto& member : admitted.members) {
		CHECK(model.Join(member.name, member.connection, member.id == admitted.host, {member.mainFighter, member.link}).accepted);
		model.SetMemberIncarnation(member.id, member.incarnation);
	}
	const auto applyTable = [&](room::MemberId member, room::ActionKind kind, std::uint64_t actionId) {
		room::Action action;
		action.kind = kind;
		action.roomEpoch = model.SnapshotView().roomEpoch;
		action.revision = model.SnapshotView().revision;
		action.tableRevision = model.SnapshotView().tables[0].revision;
		action.actionId = actionId;
		action.table = 0;
		return model.Apply(member, action);
	};
	CHECK(applyTable(admitted.members[0].id, room::ActionKind::Queue, 1).accepted);
	CHECK(applyTable(admitted.members[1].id, room::ActionKind::Queue, 2).accepted);
	CHECK(applyTable(admitted.members[2].id, room::ActionKind::Watch, 3).accepted);
	const auto seated = model.SnapshotView().tables[0];
	CHECK(applyTable(seated.p1, room::ActionKind::Ready, 4).accepted);
	CHECK(applyTable(seated.p2, room::ActionKind::Ready, 5).accepted);
	CHECK(model.BeginMatch(0, seated.p1, seated.p2).accepted);
	const auto generation = model.SnapshotView().tables[0].matchGeneration;
	CHECK(model.EndMatch(0, generation, room::MatchResult::P2Win).accepted);
	// The spectator already acknowledged; the two fighters did not.
	{
		room::Action ack;
		ack.kind = room::ActionKind::AcknowledgeTerminal;
		ack.roomEpoch = model.SnapshotView().roomEpoch;
		ack.table = 0; ack.matchGeneration = generation; ack.actionId = 6;
		CHECK(model.Apply(admitted.members[2].id, ack).accepted);
	}
	auto checkpoint = server.Checkpoint();
	checkpoint["room"] = model.Checkpoint();
	CHECK(server.RestoreCheckpoint(checkpoint));

	const auto typeNamed = [](const json& message, const char* type) {
		return message.contains("type") && message.at("type").is_string() && message.at("type").get<std::string>() == type;
	};
	const auto replayedEvents = [&](session::Connection connection) {
		std::size_t events = 0;
		for (const auto& sent : transport->outgoing) {
			if (sent.first != connection) continue;
			if (!typeNamed(sent.second, "room_event")) continue;
			const auto event = sent.second.at("event");
			CHECK(event.at("kind").get<int>() == static_cast<int>(room::Event::Kind::MatchEnded));
			CHECK(event.at("match_generation").get<std::uint64_t>() == generation);
			CHECK(event.at("result").get<int>() == static_cast<int>(room::MatchResult::P2Win));
			CHECK(event.at("terminal_replay").get<bool>());
			++events;
		}
		return events;
	};
	const auto nativeEnds = [&](session::Connection connection) {
		std::size_t count = 0;
		for (const auto& sent : transport->outgoing)
			if (sent.first == connection && typeNamed(sent.second, "game_end") &&
				sent.second.at("generation").get<std::uint64_t>() == generation) ++count;
		return count;
	};
	// A fighter that owes the receipt gets exactly one replay and one game_end.
	transport->outgoing.clear();
	CHECK(server.ReplayPendingTerminalEvents(1, server.roomMembers.at(1)) == 1);
	CHECK(replayedEvents(1) == 1);
	// game_end is an after-data message: it leaves with the next step.
	CHECK(server.Step() == 0);
	CHECK(replayedEvents(1) == 1);
	CHECK(nativeEnds(1) == 1);
	CHECK(replayedEvents(2) == 0);
	// The spectator that already acknowledged owes nothing.
	transport->outgoing.clear();
	CHECK(server.ReplayPendingTerminalEvents(3, server.roomMembers.at(3)) == 0);
	CHECK(transport->outgoing.empty());
	// Unknown member or connection: nothing is sent.
	CHECK(server.ReplayPendingTerminalEvents(0, server.roomMembers.at(2)) == 0);
	CHECK(server.ReplayPendingTerminalEvents(2, 0) == 0);
	CHECK(transport->outgoing.empty());
	// An acknowledgement for a generation the ledger does not hold is
	// answered with the receipts the member actually owes.
	{
		room::Action wrong;
		wrong.kind = room::ActionKind::AcknowledgeTerminal;
		wrong.roomEpoch = server.RoomSnapshot()->roomEpoch;
		wrong.table = 0; wrong.matchGeneration = generation + 40; wrong.actionId = 7;
		protocol::RoomActionMessage message;
		message.action = wrong;
		transport->outgoing.clear();
		transport->Push(2, json(message));
		CHECK(server.Step() == 0);
		bool rejected = false;
		for (const auto& sent : transport->outgoing)
			if (sent.first == 2 && typeNamed(sent.second, "room_result"))
				rejected = !sent.second.at("result").at("accepted").get<bool>();
		CHECK(rejected);
		CHECK(replayedEvents(2) == 1);
		CHECK(nativeEnds(2) == 1);
	}
}

static void TestTerminalAcknowledgmentBatch() {
	auto* transport = new MockTransport();
	SessionServer server("terminal-ack-batch", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(transport));
	std::array<std::uint8_t, 16> authorizationRoom = {};
	authorizationRoom[0] = 73;
	server.EnableMatchAuthorization(authorizationRoom, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	server.EnableCustomRooms("Terminal ACK batch", room::MaximumMembers, 79);
	std::uint64_t request = 1, revision = 0;
	constexpr std::uint64_t term = 7;
	server.SetAuthority(term, revision, true);
	auto commitCandidate = [&]() {
		CHECK(server.HasRecoveryCandidate());
		CHECK(!server.RecoveryCandidateOverflowed());
		CHECK(server.ProposeCheckpoint(request, term, revision, server.RecoveryCheckpoint()));
		const auto proposal = server.PendingProposal();
		CHECK(proposal != nullptr);
		CHECK(json(*proposal).dump().size() <= 1024 * 1024);
		CHECK(session::EffectJournalBytes(proposal->effects) <= session::MaxEffectJournalBytes);
		CHECK(transport->outgoing.empty());
		CHECK(server.ApplyCommit(request, term, revision + 1, proposal->checkpoint, proposal->effectsDigest));
		++request; ++revision;
		return proposal;
	};
	for (session::Connection connection = 1; connection <= room::MaximumMembers; ++connection) {
		protocol::SessionJoinRequest join;
		join.username = "ACK-" + std::to_string(connection);
		join.sidecarHash = "build";
		join.port = static_cast<std::uint16_t>(33000 + connection);
		join.customRooms = true;
		join.roomProtocol = room::ProtocolVersion;
		protocol::SessionHelloMsg hello;
		hello.admission = json(join);
		transport->Push(connection, json(hello));
		CHECK(server.Step() == 0);
		commitCandidate();
		transport->outgoing.clear();
	}

	// Build a validated maximum room projection with one terminal receipt for
	// every authenticated member. Injecting that portable RoomAuthority state
	// avoids conflating this transport batching test with native preparation.
	const auto admitted = *server.RoomSnapshot();
	room::RoomAuthority model("Terminal ACK batch", room::MaximumMembers, admitted.roomEpoch);
	for (const auto& member : admitted.members) {
		CHECK(model.Join(member.name, member.connection, member.id == admitted.host, {member.mainFighter, member.link}).accepted);
		CHECK(model.SnapshotView().members.back().id == member.id);
		model.SetMemberIncarnation(member.id, member.incarnation);
	}
	for (std::uint64_t index = 1; index <= room::MaximumChatMessages; ++index) {
		model.AdvanceTime(index * 1000);
		room::Action chat;
		chat.kind = room::ActionKind::Chat;
		chat.roomEpoch = model.SnapshotView().roomEpoch;
		chat.revision = model.SnapshotView().revision;
		chat.actionId = index;
		chat.text = std::string(room::MaximumChatBytes, index & 1 ? '\\' : '"');
		CHECK(model.Apply(admitted.host, chat).accepted);
	}
	const auto applyTable = [&](room::MemberId member, room::ActionKind kind, std::uint64_t actionId) {
		room::Action action;
		action.kind = kind;
		action.roomEpoch = model.SnapshotView().roomEpoch;
		action.revision = model.SnapshotView().revision;
		action.tableRevision = model.SnapshotView().tables[0].revision;
		action.actionId = actionId;
		action.table = 0;
		return model.Apply(member, action);
	};
	CHECK(applyTable(admitted.members[0].id, room::ActionKind::Queue, 1001).accepted);
	CHECK(applyTable(admitted.members[1].id, room::ActionKind::Queue, 1002).accepted);
	for (std::size_t index = 2; index < admitted.members.size(); ++index)
		CHECK(applyTable(admitted.members[index].id, room::ActionKind::Watch, 1001 + index).accepted);
	const auto seated = model.SnapshotView().tables[0];
	CHECK(applyTable(seated.p1, room::ActionKind::Ready, 2001).accepted);
	CHECK(applyTable(seated.p2, room::ActionKind::Ready, 2002).accepted);
	CHECK(model.BeginMatch(0, seated.p1, seated.p2).accepted);
	const auto generation = model.SnapshotView().tables[0].matchGeneration;
	CHECK(model.EndMatch(0, generation, room::MatchResult::P1Win).accepted);
	CHECK(model.TerminalMembers(0, generation).size() == room::MaximumMembers);
	auto checkpoint = server.Checkpoint();
	checkpoint["room"] = model.Checkpoint();
	CHECK(server.RestoreCheckpoint(checkpoint));
	CHECK(server.RoomSnapshot()->chat.size() == room::MaximumChatMessages);
	server.AdvanceCustomRoom((room::MaximumChatMessages + 1) * 1000);
	CHECK(!server.HasRecoveryCandidate());

	// Matching fighter reports are durable evidence, but each report need not
	// pay for a separate full-state Raft commit. Consume the ordered result
	// prefix together and leave the following ordinary action at the queue head.
	const auto terminalSnapshot = *server.RoomSnapshot();
	for (std::size_t index = 0; index < 2; ++index) {
		room::Action report;
		report.kind = room::ActionKind::RecordResult;
		report.roomEpoch = terminalSnapshot.roomEpoch;
		report.actionId = 8000 + index;
		report.table = 0;
		report.matchGeneration = generation;
		report.result = room::MatchResult::P1Win;
		protocol::RoomActionMessage message;
		message.action = report;
		transport->Push(index + 1, json(message));
	}
	room::Action reportBoundary;
	reportBoundary.kind = room::ActionKind::SetCapacity;
	reportBoundary.roomEpoch = terminalSnapshot.roomEpoch;
	reportBoundary.revision = terminalSnapshot.revision;
	reportBoundary.actionId = 8999;
	reportBoundary.capacity = room::MaximumMembers;
	protocol::RoomActionMessage reportBoundaryMessage;
	reportBoundaryMessage.action = reportBoundary;
	transport->Push(1, json(reportBoundaryMessage));
	CHECK(server.Step() == 0);
	CHECK(transport->incoming.size() == 1);
	const auto reportProposal = commitCandidate();
	CHECK(std::count_if(reportProposal->effects.begin(), reportProposal->effects.end(), [](const auto& effect) {
		return effect.type == "room_result";
	}) == 2);
	transport->outgoing.clear();
	CHECK(server.Step() == 0);
	CHECK(transport->incoming.empty());
	commitCandidate();
	transport->outgoing.clear();

	std::vector<json> acknowledgments;
	for (session::Connection connection = 1; connection <= room::MaximumMembers; ++connection) {
		const auto snapshot = *server.RoomSnapshot();
		room::Action action;
		action.kind = room::ActionKind::AcknowledgeTerminal;
		action.roomEpoch = snapshot.roomEpoch;
		action.revision = snapshot.revision;
		action.tableRevision = snapshot.tables[0].revision;
		action.actionId = 9000 + connection;
		action.table = 0;
		action.matchGeneration = generation;
		protocol::RoomActionMessage message;
		message.action = action;
		acknowledgments.push_back(json(message));
		transport->Push(connection, acknowledgments.back());
	}
	const auto beforeBatch = *server.RoomSnapshot();
	room::Action followingChat;
	followingChat.kind = room::ActionKind::Chat;
	followingChat.roomEpoch = beforeBatch.roomEpoch;
	followingChat.revision = beforeBatch.revision;
	followingChat.actionId = 10000;
	followingChat.text = std::string(room::MaximumChatBytes, '\\');
	protocol::RoomActionMessage followingMessage;
	followingMessage.action = followingChat;
	transport->Push(1, json(followingMessage));
	CHECK(transport->incoming.size() == room::MaximumMembers + 1);
	CHECK(server.Step() == 0);
	// A recovery batch is an ordered terminal-ACK prefix. The following ordinary
	// action remains transport-owned until the ACK candidate commits.
	CHECK(transport->incoming.size() == 1);
	CHECK(server.HasRecoveryCandidate());
	CHECK(!server.RecoveryCandidateOverflowed());
	CHECK(server.ProposeCheckpoint(request, term, revision, server.RecoveryCheckpoint()));
	const auto acknowledgmentProposal = server.PendingProposal();
	CHECK(acknowledgmentProposal != nullptr);
	CHECK(json(*acknowledgmentProposal).dump().size() <= 1024 * 1024);
	CHECK(session::EffectJournalBytes(acknowledgmentProposal->effects) <= session::MaxEffectJournalBytes);
	CHECK(std::count_if(acknowledgmentProposal->effects.begin(), acknowledgmentProposal->effects.end(), [](const auto& effect) {
		return effect.type == "room_result" && effect.publicPayload.is_null() && !effect.privatePayload;
	}) == room::MaximumMembers);
	CHECK(std::count_if(acknowledgmentProposal->effects.begin(), acknowledgmentProposal->effects.end(), [](const auto& effect) {
		return effect.type == "room_snapshot";
	}) == room::MaximumMembers);
	CHECK(std::count_if(acknowledgmentProposal->effects.begin(), acknowledgmentProposal->effects.end(), [](const auto& effect) {
		return effect.type == "data_update";
	}) == room::MaximumMembers);
	CHECK(transport->outgoing.empty());
	CHECK(server.ApplyCommit(request, term, revision + 1, acknowledgmentProposal->checkpoint,
		acknowledgmentProposal->effectsDigest));
	++request; ++revision;
	CHECK(std::count_if(transport->outgoing.begin(), transport->outgoing.end(), [](const auto& message) {
		return message.second.value("type", std::string()) == "room_result" &&
			message.second.at("result").at("accepted").get<bool>();
	}) == room::MaximumMembers);
	CHECK(!server.RoomSnapshot()->terminalPending[0]);
	transport->outgoing.clear();

	// The non-ACK behind the prefix is processed by the next one-intent
	// candidate and retains the ordinary public reply/replay behavior.
	CHECK(server.Step() == 0);
	CHECK(transport->incoming.empty());
	const auto chatProposal = commitCandidate();
	CHECK(std::any_of(chatProposal->effects.begin(), chatProposal->effects.end(), [](const auto& effect) {
		return effect.type == "room_result" && !effect.publicPayload.is_null();
	}));
	CHECK(std::any_of(transport->outgoing.begin(), transport->outgoing.end(), [](const auto& message) {
		return message.second.value("type", std::string()) == "room_result" &&
			message.second.value("actionId", std::uint64_t(0)) == 10000 &&
			message.second.at("result").at("accepted").get<bool>();
	}));
	transport->outgoing.clear();

	// Syntactically valid but unauthorized generations receive the same bounded,
	// commit-gated digest envelopes; authority checks still reject every one.
	for (session::Connection connection = 1; connection <= room::MaximumMembers; ++connection) {
		auto invalid = acknowledgments[connection - 1];
		invalid["action"]["action_id"] = 12000 + connection;
		invalid["action"]["match_generation"] = generation + 100;
		transport->Push(connection, invalid);
	}
	CHECK(server.Step() == 0);
	CHECK(transport->incoming.empty());
	CHECK(server.ProposeCheckpoint(request, term, revision, server.RecoveryCheckpoint()));
	const auto rejectedProposal = server.PendingProposal();
	CHECK(rejectedProposal != nullptr && rejectedProposal->effects.size() == room::MaximumMembers);
	CHECK(json(*rejectedProposal).dump().size() <= 1024 * 1024);
	CHECK(std::all_of(rejectedProposal->effects.begin(), rejectedProposal->effects.end(), [](const auto& effect) {
		return effect.type == "room_result" && effect.publicPayload.is_null() && !effect.privatePayload;
	}));
	CHECK(transport->outgoing.empty());
	CHECK(server.ApplyCommit(request, term, revision + 1, rejectedProposal->checkpoint, rejectedProposal->effectsDigest));
	++request; ++revision;
	CHECK(std::count_if(transport->outgoing.begin(), transport->outgoing.end(), [](const auto& message) {
		return message.second.value("type", std::string()) == "room_result" &&
			!message.second.at("result").at("accepted").get<bool>() &&
			message.second.at("result").at("reason").get<int>() == static_cast<int>(room::RejectReason::WrongGeneration);
	}) == room::MaximumMembers);

	// Simulate successor import after the ACK state commits but before any local
	// reply is delivered. Digest-only ACK replies are recovered by the client's
	// stable exact retry against the imported receipt/tombstone.
	auto imported = acknowledgmentProposal->checkpoint;
	auto history = imported.value("effect_journal", std::vector<session::EffectEnvelope>{});
	history.insert(history.end(), acknowledgmentProposal->effects.begin(), acknowledgmentProposal->effects.end());
	session::CompactEffectJournal(history);
	imported["effect_journal"] = history;
	imported["authority"] = {{"term", term}, {"revision", acknowledgmentProposal->baseRevision + 1}, {"writable", false}};
	auto* replicaTransport = new MockTransport();
	SessionServer replica("terminal-ack-batch", "build", true, 3, {0, 99},
		std::unique_ptr<session::ServerTransport>(replicaTransport));
	replica.EnableMatchAuthorization(authorizationRoom, [](session::Connection connection) {
		std::string identity(64, '0');
		identity[63] = "0123456789abcdef"[static_cast<std::size_t>(connection) & 15];
		return identity;
	});
	CHECK(replica.RestoreRecoveryCheckpoint(imported));
	{
		// The recovery bridge imports the leader's untouched checkpoint with the
		// compacted journal and authority passed beside it. That must produce
		// the same replica as splicing both into a JSON copy first.
		SessionServer direct("terminal-ack-batch", "build", true, 3, {0, 99},
			std::unique_ptr<session::ServerTransport>(new MockTransport()));
		direct.EnableMatchAuthorization(authorizationRoom, [](session::Connection) { return std::string(64, '0'); });
		CHECK(direct.RestoreRecoveryCheckpoint(acknowledgmentProposal->checkpoint, history,
			session::AuthorityStamp{term, acknowledgmentProposal->baseRevision + 1, false}));
		CHECK(direct.RecoveryCheckpoint() == replica.RecoveryCheckpoint());
		CHECK(direct.CommittedEffectHistory() == history);
	}
	std::vector<SessionServer::StableRebind> bindings;
	for (const auto& row : imported.at("members")) {
		const auto data = row.at("data").get<protocol::MemberData>();
		bindings.emplace_back(row.at("member").get<room::MemberId>(), data.roomMember,
			data.connId, row.at("incarnation").get<std::uint64_t>());
	}
	CHECK(replica.RebindMembers(bindings));
	replica.SetAuthority(term, acknowledgmentProposal->baseRevision + 1, true);
	replicaTransport->Push(1, acknowledgments[0]);
	CHECK(replica.Step() == 0);
	CHECK(replica.HasRecoveryCandidate() && !replica.RecoveryCandidateOverflowed());
	CHECK(replica.ProposeCheckpoint(1, term, acknowledgmentProposal->baseRevision + 1, replica.RecoveryCheckpoint()));
	const auto retryProposal = replica.PendingProposal();
	CHECK(retryProposal != nullptr && retryProposal->effects.size() == 1);
	CHECK(retryProposal->effects[0].type == "room_result" && retryProposal->effects[0].publicPayload.is_null());
	CHECK(replicaTransport->outgoing.empty());
	CHECK(replica.ApplyCommit(1, term, acknowledgmentProposal->baseRevision + 2,
		retryProposal->checkpoint, retryProposal->effectsDigest));
	CHECK(replicaTransport->outgoing.size() == 1);
	CHECK(replicaTransport->outgoing[0].second.at("result").at("accepted").get<bool>());
}

int main() {
	TestMaximumRoomResultBurst();
	TestTerminalAcknowledgmentBatch();
	TestTerminalReceiptReplay();
	std::cout << "Session server mock transport terminal tests passed\n";
}
