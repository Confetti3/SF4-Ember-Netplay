#include "session_client_mock.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../session/IrohRoom.hxx"
#include <cstdlib>
#include <iostream>

#define CHECK(c) do { if (!(c)) { std::cerr << "Check failed at " << __LINE__ << ": " #c << '\n'; std::exit(1); } } while (false)

using namespace sf4e;
using nlohmann::json;
using sf4e::test::MockClient;
namespace protocol = sf4e::SessionProtocol;

static void TestFailedRoomCanLeaveAfterNativeTeardown() {
	platform::HelperClient helper;
	session::IrohRoom room(helper);
	room.Poll();
	CHECK(room.GetState() == session::IrohRoom::State::Failed);
	CHECK(!room.Coordination().writable && !room.Coordination().rebound);
	CHECK(!room.CloseFailedRoom(true));
	CHECK(room.GetState() == session::IrohRoom::State::Failed);
	CHECK(room.CloseFailedRoom(false));
	// No helper epoch was opened in this fixture, so retirement is immediate.
	CHECK(room.GetState() == session::IrohRoom::State::Idle);
	CHECK(!room.CloseFailedRoom(false));
}

struct Observer { int ready = 0, synced = 0, error = 0; };
int main() {
	TestFailedRoomCanLeaveAfterNativeTeardown();
	Observer observer;
	SessionClient::Callbacks callbacks = {};
	callbacks.data = &observer;
	callbacks.OnReady = [](SessionClient*, const SessionClient::Callbacks& c) { ++static_cast<Observer*>(c.data)->ready; };
	callbacks.OnBattleSynced = [](SessionClient*, const SessionClient::Callbacks& c) { ++static_cast<Observer*>(c.data)->synced; };
	callbacks.OnError = [](SessionClient::ErrorType, SessionClient*, const SessionClient::Callbacks& c) { ++static_cast<Observer*>(c.data)->error; };
	std::string name = "Player";
	SessionClient client(callbacks, "build", 30000, name);
	CHECK(client.Step() == 0);
	CHECK(client.Lobby_Ready() == session::SendResult::NotConnected);
	for (int invalid : {-1, 22, 23, 30, INT32_MAX})
		CHECK(client.PreBattle_SetStage(invalid) == session::SendResult::InvalidPayload);
	for (int cycle = 0; cycle < 20; ++cycle) {
		auto* transport = new MockClient();
		CHECK(client.Connect(std::unique_ptr<session::ClientTransport>(transport), false) == 0);
		CHECK(client.Step() == 0 && transport->sent.empty());
		transport->state = session::ConnectionState::Connected;
		CHECK(client.Step() == 0 && client.IsConnected());
		CHECK(transport->sent.size() == 1 && transport->sent[0]["type"] == "hello");
		CHECK(client.ServerAddress() == "127.0.0.1");
		protocol::SessionHelloResp hello; hello.cid = {"room", "player"};
		if (cycle == 0) transport->writable = false;
		transport->Push(json(hello)); CHECK(client.Step() == 0);
		if (cycle == 0) {
			// The CID and authenticated admission request are separate reliable
			// sends. A transient queue failure must leave the request pending.
			CHECK(transport->sent.size() == 1);
			transport->writable = true;
			CHECK(client.Step() == 0);
		}
		CHECK(transport->sent.back()["type"] == "join_req");
		CHECK(transport->sent.back()["username"] == "Player");
		CHECK(client.Lobby_Ready() == session::SendResult::Queued);
		CHECK(client._outstandingReadyRequestNumber == 702);
		protocol::SessionDataUpdate update;
		update.lobbyData = protocol::LobbyData::NULL_LOBBY;
		update.lobbyData.members.push_back({hello.cid, name, "", 30000, 0});
		update.matchData.readyMessageNum[0] = 701;
		transport->Push(json(update)); CHECK(client.Step() == 0);
		CHECK(client._outstandingReadyRequestNumber == 702);
		update.matchData.readyMessageNum[0] = 702;
		transport->Push(json(update));
		transport->Push(json(protocol::LobbyAllReady()));
		transport->Push(json(protocol::BattleSynced()));
		CHECK(client.Step() == 0);
		CHECK(client._outstandingReadyRequestNumber == -1);
		CHECK(observer.ready == cycle + 1 && observer.synced == cycle + 1);
		const auto admittedJoinRequests = std::count_if(transport->sent.begin(), transport->sent.end(), [](const json& message) {
			return message.value("type", std::string()) == "join_req";
		});
		for (int tick = 0; tick < 20; ++tick) CHECK(client.Step() == 0);
		CHECK(std::count_if(transport->sent.begin(), transport->sent.end(), [](const json& message) {
			return message.value("type", std::string()) == "join_req";
		}) == admittedJoinRequests);
		for (const auto& stage : selection::StageList()) {
			CHECK(client.PreBattle_SetStage(stage.id) == session::SendResult::Queued);
			CHECK(transport->sent.back()["stageID"] == stage.id);
		}
		const auto sentCount = transport->sent.size();
		for (int invalid : {-1, 22, 23, 30, INT32_MAX})
			CHECK(client.PreBattle_SetStage(invalid) == session::SendResult::InvalidPayload);
		CHECK(transport->sent.size() == sentCount);
		transport->state = session::ConnectionState::Failed;
		CHECK(client.Step() == -1 && !client.IsConnected());
		// A failed Step must leave the object alive for its owner to handle.
		CHECK(client._cid == hello.cid);
		client.Disconnect();
		CHECK(client._lobbyData.members.empty() && client._cid.host.empty());
	}
	CHECK(observer.error == 0);
	// A queued native grant may arrive after the previous match releases its
	// projection. Once the new grant freezes the selected table, room snapshots
	// must not rewrite the legacy roster until the coordinator releases it.
	std::string roomName = "Player";
	SessionClient roomClient(callbacks, "build", 30000, roomName);
	roomClient.RequireCustomRooms();
	roomClient.SetProfileMain(13);
	auto* roomTransport = new MockClient();
	CHECK(roomClient.Connect(std::unique_ptr<session::ClientTransport>(roomTransport), false) == 0);
	roomTransport->state = session::ConnectionState::Connected;
	CHECK(roomClient.Step() == 0);
	CHECK(roomTransport->sent.size() == 1 && roomTransport->sent.back().at("admission").value("mainFighter", -1) == 13);
	protocol::SessionHelloResp roomHello; roomHello.cid = {"room", "player"};
	roomHello.roomMember = 1; roomHello.authenticatedEndpoint = "player"; roomHello.incarnation = 11;
	roomTransport->Push(json(roomHello)); CHECK(roomClient.Step() == 0);
	// The committed hello already performed custom-room admission. It must not
	// start the legacy join_req retry loop while the projection is in flight.
	CHECK(roomTransport->sent.size() == 1);
	room::Snapshot roomSnapshot;
	roomSnapshot.roomEpoch = 1; roomSnapshot.revision = 1; roomSnapshot.name = "Room";
	roomSnapshot.capacity = 2; roomSnapshot.host = 1; roomSnapshot.localMember = 1;
	roomSnapshot.members = {
		room::Member{1, "Player", {"room", "player"}, true, room::MemberStatus::Seated, 0, 0, 1},
		room::Member{2, "Guest", {"room", "guest"}, false, room::MemberStatus::Seated, 0, 1, 2},
	};
	for (std::size_t table = 0; table < room::TableCount; ++table) roomSnapshot.tables[table].id = static_cast<std::uint8_t>(table);
	roomSnapshot.tables[0].p1 = 1; roomSnapshot.tables[0].p2 = 2;
	protocol::RoomSnapshotMessage roomMessage; roomMessage.snapshot = roomSnapshot;
	roomTransport->Push(json(roomMessage)); CHECK(roomClient.Step() == 0);
	CHECK(roomClient._lobbyData.members.size() == 2 && roomClient._lobbyData.members[1].name == "Guest");
	{
		// The client advertises roomChatDelta, and a snapshot marked
		// chat_unchanged keeps the chat it holds.
		SessionClient chatClient(callbacks, "build", 30000, roomName);
		chatClient.RequireCustomRooms();
		auto* chatTransport = new MockClient();
		CHECK(chatClient.Connect(std::unique_ptr<session::ClientTransport>(chatTransport), false) == 0);
		chatTransport->state = session::ConnectionState::Connected;
		CHECK(chatClient.Step() == 0);
		CHECK(chatTransport->sent.back().at("admission").value("roomChatDelta", false));
		chatTransport->Push(json(roomHello)); CHECK(chatClient.Step() == 0);
		auto withChat = roomSnapshot;
		room::ChatMessage line; line.sequence = 1; line.sender = withChat.members.front().id; line.text = "hi";
		withChat.chat = {line};
		protocol::RoomSnapshotMessage chatMessage; chatMessage.snapshot = withChat;
		chatTransport->Push(json(chatMessage)); CHECK(chatClient.Step() == 0);
		CHECK(chatClient.GetRoomSnapshot().chat.size() == 1);
		auto unchanged = withChat; unchanged.chat.clear(); ++unchanged.revision;
		chatMessage.snapshot = unchanged;
		json payload = chatMessage;
		payload["snapshot"].erase("chat"); payload["snapshot"]["chat_unchanged"] = true;
		chatTransport->Push(payload); CHECK(chatClient.Step() == 0);
		CHECK(chatClient.GetRoomSnapshot().revision == unchanged.revision);
		CHECK(chatClient.GetRoomSnapshot().chat.size() == 1 && chatClient.GetRoomSnapshot().chat.front().text == "hi");
	}
	// Result and native-finish reports retain one exact queued action identity.
	// A later retry cannot change the generation/outcome bytes or allocate a new
	// action ID, and exact duplicates can be coalesced without dropping either
	// lifecycle kind when they interleave.
	room::Action finished;
	finished.kind = room::ActionKind::MatchFinished;
	finished.roomEpoch = roomSnapshot.roomEpoch;
	finished.revision = roomSnapshot.revision;
	finished.table = 0;
	finished.tableRevision = roomSnapshot.tables[0].revision;
	finished.matchGeneration = 7;
	std::uint64_t finishedId = 0;
	CHECK(roomClient.SendRoomAction(finished, &finishedId) == session::SendResult::Queued && finishedId != 0);
	const auto finishedPayload = roomTransport->sent.back();
	room::Action result = finished;
	result.kind = room::ActionKind::RecordResult;
	result.result = room::MatchResult::P1Win;
	std::uint64_t resultId = 0;
	CHECK(roomClient.SendRoomAction(result, &resultId) == session::SendResult::Queued && resultId > finishedId);
	const auto resultPayload = roomTransport->sent.back();
	CHECK(roomClient.RetryMatchFinished(finished, finishedId) == session::SendResult::Queued);
	CHECK(roomTransport->sent.back() == finishedPayload);
	CHECK(roomClient.RetryRoomResult(result, resultId) == session::SendResult::Queued);
	CHECK(roomTransport->sent.back() == resultPayload);
	CHECK(roomClient.RetryRoomResult(result, 0) == session::SendResult::InvalidPayload);
	CHECK(roomClient.RetryRoomResult(result, resultId + 100) == session::SendResult::InvalidPayload);
	auto changedResult = result; changedResult.result = room::MatchResult::P2Win;
	CHECK(roomClient.RetryRoomResult(changedResult, resultId) == session::SendResult::InvalidPayload);
	auto changedGeneration = result; ++changedGeneration.matchGeneration;
	CHECK(roomClient.RetryRoomResult(changedGeneration, resultId) == session::SendResult::InvalidPayload);
	CHECK(roomClient.RetryMatchFinished(result, finishedId) == session::SendResult::InvalidPayload);
	const session::Message queuedResult{7, 0, resultPayload.dump(), ""};
	CHECK(session::SameQueuedRoomRetry(queuedResult, queuedResult));
	auto otherSender = queuedResult; otherSender.connection = 8;
	CHECK(!session::SameQueuedRoomRetry(queuedResult, otherSender));
	SessionProtocol::RoomActionMessage changedResultMessage; changedResultMessage.action = changedResult;
	changedResultMessage.action.protocolVersion = room::ProtocolVersion;
	changedResultMessage.action.actionId = resultId;
	auto changedResultWire = queuedResult; changedResultWire.payload = json(changedResultMessage).dump();
	CHECK(!session::SameQueuedRoomRetry(queuedResult, changedResultWire));
	SessionProtocol::RoomActionMessage changedGenerationMessage; changedGenerationMessage.action = changedGeneration;
	changedGenerationMessage.action.protocolVersion = room::ProtocolVersion;
	changedGenerationMessage.action.actionId = resultId;
	auto changedGenerationWire = queuedResult; changedGenerationWire.payload = json(changedGenerationMessage).dump();
	CHECK(!session::SameQueuedRoomRetry(queuedResult, changedGenerationWire));
	const session::Message queuedFinished{7, 0, finishedPayload.dump(), ""};
	CHECK(session::SameQueuedRoomRetry(queuedFinished, queuedFinished));
	room::Action chat; chat.kind = room::ActionKind::Chat; chat.roomEpoch = roomSnapshot.roomEpoch;
	chat.actionId = resultId; chat.protocolVersion = room::ProtocolVersion; chat.text = "retry";
	SessionProtocol::RoomActionMessage chatMessage; chatMessage.action = chat;
	CHECK(!session::SameQueuedRoomRetry(queuedResult, {7, 0, json(chatMessage).dump(), ""}));
	roomClient.FreezeRoomProjection();
	roomSnapshot.revision = 2; roomSnapshot.members[1].name = "Guest renamed";
	roomMessage.snapshot = roomSnapshot; roomTransport->Push(json(roomMessage)); CHECK(roomClient.Step() == 0);
	CHECK(roomClient._lobbyData.members[1].name == "Guest");
	CHECK(roomClient.ReleaseRoomProjection());
	CHECK(roomClient._lobbyData.members[1].name == "Guest renamed");
	// Retain the previous match projection while the authoritative room has
	// completed it. Runtime menu gates must not lock the next selection on the
	// frozen Ready bits used by native gameplay and draining spectators.
	roomSnapshot.tables[0].phase = room::TablePhase::Playing;
	roomSnapshot.tables[0].ready[0] = roomSnapshot.tables[0].ready[1] = true;
	++roomSnapshot.revision; roomMessage.snapshot = roomSnapshot;
	roomTransport->Push(json(roomMessage)); CHECK(roomClient.Step() == 0);
	CHECK(roomClient.LocalSelectionLocked(0));
	roomClient.FreezeRoomProjection();
	roomSnapshot.tables[0].phase = room::TablePhase::Waiting;
	roomSnapshot.tables[0].ready[0] = roomSnapshot.tables[0].ready[1] = false;
	++roomSnapshot.revision; roomMessage.snapshot = roomSnapshot;
	roomTransport->Push(json(roomMessage)); CHECK(roomClient.Step() == 0);
	CHECK(roomClient._matchData.readyMessageNum[0] != -1);
	CHECK(!roomClient.LocalSelectionLocked(0));
	CHECK(roomClient.LocalSelectionLocked(1));
	CHECK(roomClient.LocalSelectionLocked(-1));
	for (unsigned rematch = 0; rematch < 20; ++rematch) {
		for (auto phase : {room::TablePhase::Waiting, room::TablePhase::Ready, room::TablePhase::Playing, room::TablePhase::Paused}) {
			roomSnapshot.tables[0].phase = phase;
			roomSnapshot.tables[0].ready[0] = roomSnapshot.tables[0].ready[1] = true;
			++roomSnapshot.revision; roomMessage.snapshot = roomSnapshot;
			roomTransport->Push(json(roomMessage)); CHECK(roomClient.Step() == 0);
			CHECK(roomClient.LocalSelectionLocked(0));
		}
		roomSnapshot.tables[0].phase = room::TablePhase::Waiting;
		roomSnapshot.tables[0].ready[0] = roomSnapshot.tables[0].ready[1] = false;
		++roomSnapshot.revision; roomMessage.snapshot = roomSnapshot;
		roomTransport->Push(json(roomMessage)); CHECK(roomClient.Step() == 0);
		CHECK(!roomClient.LocalSelectionLocked(0));
		CHECK(roomClient._matchData.readyMessageNum[0] != -1);
	}
	CHECK(roomClient.ReleaseRoomProjection());
	// A native projection must be captured for the exact grant generation. A
	// later update for that generation may describe a subsequent room change,
	// but cannot rewrite the inputs already selected for the queued match.
	roomClient.RequireMatchAuthorization();
	roomClient.FreezeRoomProjection();
	protocol::SessionDataUpdate firstProjection;
	firstProjection.matchGeneration = 7;
	firstProjection.lobbyData = roomClient._lobbyData;
	firstProjection.matchData.stageID = 11;
	firstProjection.matchData.chara[0].ultraCombo = 1;
	roomTransport->Push(json(firstProjection));
	roomTransport->Push(json({{"type", "game_prepare"}, {"generation", 7}}));
	protocol::SessionDataUpdate laterProjection = firstProjection;
	laterProjection.matchData.stageID = 99;
	laterProjection.matchData.chara[0].ultraCombo = 2;
	roomTransport->Push(json(laterProjection));
	CHECK(roomClient.Step() == 0);
	CHECK(roomClient.ApplyPendingRoomProjection(7));
	CHECK(roomClient._matchData.stageID == 11);
	CHECK(roomClient._matchData.chara[0].ultraCombo == 1);
	CHECK(roomClient.ReleaseRoomProjection());
	// A match can promote queued members to spectators at BeginMatch. If the
	// committed prepare reaches this client before its generation-matched table
	// projection, accepting the grant must use the four-member projection rather
	// than the two seated members from the preceding Waiting snapshot.
	protocol::SessionDataUpdate queuedSpectatorProjection = firstProjection;
	queuedSpectatorProjection.matchGeneration = 8;
	queuedSpectatorProjection.lobbyData.members.resize(4);
	queuedSpectatorProjection.lobbyData.members[2].connId = {"room", "queued-one"};
	queuedSpectatorProjection.lobbyData.members[2].name = "Queued one";
	queuedSpectatorProjection.lobbyData.members[3].connId = {"room", "queued-two"};
	queuedSpectatorProjection.lobbyData.members[3].name = "Queued two";
	roomTransport->Push(json({{"type", "game_prepare"}, {"generation", 8}}));
	CHECK(roomClient.Step() == 0);
	CHECK(roomClient._lobbyData.members.size() == 2);
	// The matching projection may occupy the next transport poll. The match
	// coordinator retains the private grant while SessionClient buffers this
	// later update under the already frozen generation.
	roomTransport->Push(json(queuedSpectatorProjection));
	CHECK(roomClient.Step() == 0);
	CHECK(roomClient._lobbyData.members.size() == 2);
	CHECK(roomClient.ApplyPendingRoomProjection(8));
	CHECK(roomClient._lobbyData.members.size() == 4);
	CHECK(roomClient._lobbyData.members[2].connId == queuedSpectatorProjection.lobbyData.members[2].connId);
	CHECK(roomClient._lobbyData.members[3].connId == queuedSpectatorProjection.lobbyData.members[3].connId);
    // Exercise the shared protocol serializer so the action receipt cannot
    // silently drift to a different JSON field spelling than the server.
    protocol::RoomResultMessage actionResult;
    actionResult.actionId=42;
    actionResult.result.accepted=true;
    actionResult.result.snapshot=roomClient.GetRoomSnapshot();
    roomTransport->Push(json(actionResult));
    CHECK(roomClient.Step()==0);
    SessionClient::ActionReply actionReply;
	CHECK(roomClient.TakeActionReply(actionReply) && actionReply.actionId==42 && actionReply.accepted);
	CHECK(!roomClient.TakeActionReply(actionReply));
	// A Ready that raced the player's own previous table action carries a
	// table revision the authority already moved past. The rejection brings the
	// current snapshot; the client resends from it, silently, up to three times.
	// A fourth rejection is surfaced as a Ready reply so the press can fail now.
	{
		CHECK(roomClient.Lobby_Ready() == session::SendResult::Queued);
		auto readyPayload = roomTransport->sent.back();
		CHECK(readyPayload.at("action").at("kind").get<int>() == static_cast<int>(room::ActionKind::Ready));
		auto current = roomClient.GetRoomSnapshot();
		for (int attempt = 1; attempt <= 4; ++attempt) {
			protocol::RoomResultMessage stale;
			stale.actionId = readyPayload.at("action").at("action_id").get<std::uint64_t>();
			stale.result.accepted = false;
			stale.result.reason = room::RejectReason::StaleTable;
			++current.revision; ++current.tables[0].revision;
			stale.result.snapshot = current;
			const auto sentBefore = roomTransport->sent.size();
			roomTransport->Push(json(stale));
			CHECK(roomClient.Step() == 0);
			if (attempt <= 3) {
				CHECK(roomTransport->sent.size() == sentBefore + 1);
				readyPayload = roomTransport->sent.back();
				CHECK(readyPayload.at("action").at("kind").get<int>() == static_cast<int>(room::ActionKind::Ready));
				CHECK(readyPayload.at("action").at("table_revision").get<std::uint64_t>() == current.tables[0].revision);
				CHECK(!roomClient.TakeActionReply(actionReply));
				CHECK(roomClient.RoomError().empty());
			} else {
				CHECK(roomTransport->sent.size() == sentBefore);
				CHECK(roomClient.TakeActionReply(actionReply) && !actionReply.accepted && actionReply.kindKnown &&
					actionReply.kind == room::ActionKind::Ready && actionReply.reason == room::RejectReason::StaleTable);
				CHECK(!roomClient.RoomError().empty());
				CHECK(!roomClient.TakeActionReply(actionReply));
			}
		}
		// An accepted readiness reply re-arms the budget.
		CHECK(roomClient.Lobby_Ready() == session::SendResult::Queued);
		protocol::RoomResultMessage accepted;
		accepted.actionId = roomTransport->sent.back().at("action").at("action_id").get<std::uint64_t>();
		accepted.result.accepted = true;
		accepted.result.snapshot = current;
		roomTransport->Push(json(accepted));
		CHECK(roomClient.Step() == 0);
		CHECK(roomClient.TakeActionReply(actionReply) && actionReply.accepted && actionReply.kindKnown && actionReply.kind == room::ActionKind::Ready);
		CHECK(roomClient.Lobby_Ready() == session::SendResult::Queued);
		protocol::RoomResultMessage staleAgain;
		staleAgain.actionId = roomTransport->sent.back().at("action").at("action_id").get<std::uint64_t>();
		staleAgain.result.accepted = false;
		staleAgain.result.reason = room::RejectReason::StaleTable;
		++current.revision; ++current.tables[0].revision;
		staleAgain.result.snapshot = current;
		const auto sentBefore = roomTransport->sent.size();
		roomTransport->Push(json(staleAgain));
		CHECK(roomClient.Step() == 0);
		CHECK(roomTransport->sent.size() == sentBefore + 1);
		CHECK(!roomClient.TakeActionReply(actionReply));
		// The resend answers under the id its caller was given. Callers that
		// wait for their own id (fixtures, retry owners) must not hang.
		const auto staleAgainId = staleAgain.actionId;
		protocol::RoomResultMessage resentAccepted;
		resentAccepted.actionId = roomTransport->sent.back().at("action").at("action_id").get<std::uint64_t>();
		CHECK(resentAccepted.actionId != staleAgainId);
		resentAccepted.result.accepted = true;
		resentAccepted.result.snapshot = current;
		roomTransport->Push(json(resentAccepted));
		CHECK(roomClient.Step() == 0);
		CHECK(roomClient.TakeActionReply(actionReply) && actionReply.accepted && actionReply.actionId == staleAgainId &&
			actionReply.kindKnown && actionReply.kind == room::ActionKind::Ready);
		// It also resends the caller's own request, not the client's default Ready.
		room::Action explicitReady;
		explicitReady.kind = room::ActionKind::Ready; explicitReady.table = 0; explicitReady.inputDelay = 7;
		explicitReady.revision = current.revision; explicitReady.tableRevision = current.tables[0].revision;
		std::uint64_t callerId = 0;
		CHECK(roomClient.SendRoomAction(explicitReady, &callerId) == session::SendResult::Queued);
		protocol::RoomResultMessage staleExplicit;
		staleExplicit.actionId = callerId;
		staleExplicit.result.accepted = false;
		staleExplicit.result.reason = room::RejectReason::StaleTable;
		++current.revision; ++current.tables[0].revision;
		staleExplicit.result.snapshot = current;
		roomTransport->Push(json(staleExplicit));
		CHECK(roomClient.Step() == 0);
		CHECK(roomTransport->sent.back().at("action").at("input_delay").get<int>() == 7);
		CHECK(roomTransport->sent.back().at("action").at("table_revision").get<std::uint64_t>() == current.tables[0].revision);
		protocol::RoomResultMessage explicitAccepted;
		explicitAccepted.actionId = roomTransport->sent.back().at("action").at("action_id").get<std::uint64_t>();
		explicitAccepted.result.accepted = true;
		explicitAccepted.result.snapshot = current;
		roomTransport->Push(json(explicitAccepted));
		CHECK(roomClient.Step() == 0);
		CHECK(roomClient.TakeActionReply(actionReply) && actionReply.accepted && actionReply.actionId == callerId);
	}
	// MatchEnded is an outcome notification, not an automatic receipt release.
	// The explicit acknowledgement remains in the client queue until the
	// authenticated action reply arrives, which lets the native owner fence it
	// behind profile persistence and helper/socket retirement.
	const auto sentBeforeTerminal = roomTransport->sent.size();
	room::Event terminal{room::Event::Kind::MatchEnded, 0, 7, 0, room::MatchResult::P1Win, true};
	protocol::RoomEventMessage terminalMessage; terminalMessage.event = terminal;
	roomTransport->Push(json(terminalMessage));
	CHECK(roomClient.Step() == 0);
	CHECK(roomTransport->sent.size() == sentBeforeTerminal);
	CHECK(roomClient.AcknowledgeTerminal(0, 7) == session::SendResult::Queued);
	CHECK(roomClient.AcknowledgeTerminal(0, 7) == session::SendResult::Queued);
	CHECK(roomClient.Step() == 0);
	CHECK(roomTransport->sent.back()["type"] == "room_action");
	CHECK(roomTransport->sent.back()["action"]["kind"] == static_cast<int>(room::ActionKind::AcknowledgeTerminal));
	const auto terminalActionId = roomTransport->sent.back()["action"]["action_id"].get<std::uint64_t>();
	CHECK(roomClient.AcknowledgeTerminal(0, 7) == session::SendResult::Queued);
	protocol::RoomResultMessage terminalReply; terminalReply.actionId = terminalActionId;
	terminalReply.result.accepted = true; terminalReply.result.snapshot = roomClient.GetRoomSnapshot();
	roomTransport->Push(json(terminalReply));
	CHECK(roomClient.Step() == 0);
	CHECK(roomClient.TakeActionReply(actionReply) && actionReply.actionId == terminalActionId && actionReply.accepted);
	// A delayed accepted reply can arrive after more than 72 client ticks. The
	// terminal retry keeps one stable action identity, so latency cannot evict
	// the only authenticated proof that retires the exact receipt.
	CHECK(roomClient.AcknowledgeTerminal(0, 8) == session::SendResult::Queued);
	CHECK(roomClient.Step() == 0);
	const auto delayedAckId = roomTransport->sent.back()["action"]["action_id"].get<std::uint64_t>();
	for (int tick = 0; tick < 80; ++tick) CHECK(roomClient.Step() == 0);
	CHECK(roomTransport->sent.back()["action"]["action_id"].get<std::uint64_t>() == delayedAckId);
	protocol::RoomResultMessage delayedAckReply; delayedAckReply.actionId = delayedAckId;
	delayedAckReply.result.accepted = true; delayedAckReply.result.snapshot = roomClient.GetRoomSnapshot();
	roomTransport->Push(json(delayedAckReply)); CHECK(roomClient.Step() == 0);
	CHECK(roomClient.TakeActionReply(actionReply) && actionReply.actionId == delayedAckId && actionReply.accepted);
	const auto afterDelayedReply = roomTransport->sent.size();
	for (int tick = 0; tick < 16; ++tick) CHECK(roomClient.Step() == 0);
	CHECK(roomTransport->sent.size() == afterDelayedReply);

	// If the action reply is lost entirely, a newer authenticated projection can
	// confirm completion only after this client observed the obligation pending.
	// A preterminal false snapshot, even at a newer revision, is not proof.
	roomSnapshot = roomClient.GetRoomSnapshot();
	// A pending receipt for an older generation on the same table cannot prove
	// the newly queued generation. Its subsequent false snapshot is stale with
	// respect to the new terminal ACK even though the room revision advances.
	roomSnapshot.localTerminalPending = true;
	roomSnapshot.terminalPending[0] = true;
	roomSnapshot.localTerminalGenerations[0] = 7;
	++roomSnapshot.revision;
	roomMessage.snapshot = roomSnapshot;
	roomTransport->Push(json(roomMessage)); CHECK(roomClient.Step() == 0);
	CHECK(roomClient.AcknowledgeTerminal(0, 9) == session::SendResult::Queued);
	CHECK(roomClient.Step() == 0);
	const auto lostReplyAction = roomTransport->sent.back()["action"]["action_id"].get<std::uint64_t>();
	roomSnapshot.localTerminalPending = false;
	roomSnapshot.terminalPending[0] = false;
	roomSnapshot.localTerminalGenerations[0] = 0;
	++roomSnapshot.revision;
	roomMessage.snapshot = roomSnapshot;
	roomTransport->Push(json(roomMessage)); CHECK(roomClient.Step() == 0);
	const auto beforeRequiredRetry = roomTransport->sent.size();
	for (int tick = 0; tick < 8; ++tick) CHECK(roomClient.Step() == 0);
	CHECK(roomTransport->sent.size() > beforeRequiredRetry);
	CHECK(roomTransport->sent.back()["action"]["action_id"].get<std::uint64_t>() == lostReplyAction);
	roomSnapshot.localTerminalPending = true;
	roomSnapshot.terminalPending[0] = true;
	roomSnapshot.localTerminalGenerations[0] = 9;
	++roomSnapshot.revision;
	roomMessage.snapshot = roomSnapshot;
	roomTransport->Push(json(roomMessage)); CHECK(roomClient.Step() == 0);
	roomSnapshot.localTerminalPending = false;
	roomSnapshot.terminalPending[0] = false;
	roomSnapshot.localTerminalGenerations[0] = 0;
	++roomSnapshot.revision;
	roomMessage.snapshot = roomSnapshot;
	roomTransport->Push(json(roomMessage)); CHECK(roomClient.Step() == 0);
	const auto afterSnapshotConfirmation = roomTransport->sent.size();
	for (int tick = 0; tick < 16; ++tick) CHECK(roomClient.Step() == 0);
	CHECK(roomTransport->sent.size() == afterSnapshotConfirmation);
	roomClient.Disconnect();
	std::cout << "Client transport/readiness/lifecycle tests passed\n";
}
