// SessionServer: one handler per inbound message type, called from Step().

#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <exception>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "sf4e__SessionProtocol.hxx"
#include "sf4e__SessionServer.hxx"
#include "RoomMessageQueue.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"

using nlohmann::json;

namespace SessionProtocol = sf4e::SessionProtocol;
namespace session = sf4e::session;
namespace room = sf4e::room;
namespace diag = sf4e::diag;
using Dimps::Math::FixedPoint;
using sf4e::SessionServer;

void SessionServer::HandleSessionHello(session::Connection conn, const json& msg, const session::Message& incoming) {
	SessionProtocol::SessionHelloMsg hello;
	try { msg.get_to(hello); }
	catch (const std::exception&) { spdlog::warn("Server: malformed session hello"); return; }
	SessionProtocol::SessionHelloResp cidMsg;
	cidMsg.cid.host = _identity;
	cidMsg.cid.user = std::to_string(conn);
	// Native room CIDs must survive transport-handle churn.  The
	// helper-backed authorization resolver already has the fresh
	// authenticated endpoint for this connection, while the numeric
	// handle is only a local socket slot and can be reused after a
	// takeover.
	if (_roomAuthority && _matchAuthorizationConfigured && _matchAuthorizationIdentity) {
		const auto stable = _matchAuthorizationIdentity(conn);
		if (!stable.empty()) cidMsg.cid.user = stable;
	}
	cidMap[conn] = cidMsg.cid;
	SessionProtocol::JoinResult admissionResult = SessionProtocol::JOIN_OK;
	bool admitted = false;
	if (!hello.admission.is_null() && !hello.admission.empty()) {
		try {
			SessionProtocol::SessionJoinRequest admission = hello.admission.get<SessionProtocol::SessionJoinRequest>();
			if ((_roomAuthority && (!admission.customRooms || admission.roomProtocol != room::ProtocolVersion)) ||
				(!_roomAuthority && admission.customRooms)) admissionResult = SessionProtocol::JR_REQUEST_INVALID;
			else admissionResult = RegisterToWait(conn, admission.port, admission.sidecarHash, admission.username,
				incoming.peerAddress, cidMsg.cid, admission.mainFighter);
			admitted = admissionResult == SessionProtocol::JOIN_OK;
			if (admitted) EnableChatDelta(conn, admission.roomChatDelta);
		} catch (const std::exception&) { admissionResult = SessionProtocol::JR_REQUEST_INVALID; }
	}
	if (admitted && _roomAuthority) {
		cidMsg.roomMember = roomMembers[conn];
		cidMsg.authenticatedEndpoint = roomPeerIdentities[cidMsg.roomMember];
		cidMsg.incarnation = roomIncarnations[cidMsg.roomMember];
		// The bootstrap hello is the complete custom-room admission.
		// Journal its initial room projection in this same candidate so
		// the client never needs the legacy join_req round trip.
		_dataDirty = true;
	}
	// A bare hello and an admission rejection are transport handshakes,
	// not room effects. An admitted hello is journaled with the room
	// member so it cannot expose a provisional roster before commit.
	if (admitted) Respond(conn, json(cidMsg));
	else if (!_transport || !_transport->Send(conn, json(cidMsg).dump())) _transportFailed = true;
	if (!admitted && !hello.admission.is_null() && !hello.admission.empty()) {
		SessionProtocol::SessionJoinReject reject; reject.result = admissionResult;
		if (!_transport || !_transport->Send(conn, json(reject).dump())) _transportFailed = true;
	}
}

void SessionServer::HandleRoomAction(session::Connection conn, const json& msg, std::vector<room::Event>& deferredRoomEvents) {
	SessionProtocol::RoomActionMessage actionMessage;
	try { msg.get_to(actionMessage); }
	catch (const std::exception&) {
		SessionProtocol::RoomResultMessage invalid;
		invalid.result.accepted = false;
		invalid.result.reason = room::RejectReason::StaleRoom;
		Respond(conn, json(invalid));
		return;
	}
	const auto roomMember = roomMembers.find(conn);
	if (roomMember == roomMembers.end()) return;
	const auto priorSnapshot = _roomAuthority->SnapshotCopy();
	if (actionMessage.action.kind == room::ActionKind::Leave) CaptureFrozenMember(roomMember->second, priorSnapshot);
	else if (actionMessage.action.kind == room::ActionKind::Kick) CaptureFrozenMember(actionMessage.action.target, priorSnapshot);
	auto result = _roomAuthority->Apply(roomMember->second, actionMessage.action);
	std::vector<room::Event> events = result.events;
	const bool duplicateTerminalAcknowledgment = result.accepted &&
		actionMessage.action.kind == room::ActionKind::AcknowledgeTerminal &&
		result.snapshot.revision == priorSnapshot.revision && events.empty();
	if (result.accepted && (actionMessage.action.kind == room::ActionKind::Leave ||
		actionMessage.action.kind == room::ActionKind::Unwatch ||
		actionMessage.action.kind == room::ActionKind::Watch || actionMessage.action.kind == room::ActionKind::Kick)) {
		const room::MemberId departedMember = actionMessage.action.kind == room::ActionKind::Kick
			? actionMessage.action.target : roomMember->second;
		const auto connectionFor = [&](room::MemberId id) -> session::Connection {
			for (const auto& mapping : roomMembers) if (mapping.second == id) return mapping.first;
			return 0;
		};
		const auto oldWasInTable = [&](const room::Table& table) {
			return table.p1 == departedMember || table.p2 == departedMember ||
				std::find(table.spectators.begin(), table.spectators.end(), departedMember) != table.spectators.end() ||
				std::find(table.watchingNext.begin(), table.watchingNext.end(), departedMember) != table.watchingNext.end();
		};
		for (std::uint8_t tableId = 0; tableId < room::TableCount; ++tableId) {
			const auto& oldTable = priorSnapshot.tables[tableId];
			if (!oldWasInTable(oldTable) || (actionMessage.action.kind == room::ActionKind::Watch && tableId == actionMessage.action.table)) continue;
			auto* authority = RoomMatchAuthority(tableId);
			if (!authority || authority->GetPhase() == session::MatchAuthority::Phase::Idle) continue;
			const auto departing = connectionFor(departedMember);
			if (!departing || !authority->MemberDeparted(departing, MatchSender())) { _transportFailed = true; continue; }
			// A pre-start link or a fighter departure aborts the room
			// generation so the next grant is built from a fresh roster.
			const bool fighter = oldTable.p1 == departedMember || oldTable.p2 == departedMember;
			if (fighter || authority->GetPhase() == session::MatchAuthority::Phase::Idle) {
				const auto current = _roomAuthority->SnapshotView().tables[tableId];
				if (current.phase == room::TablePhase::Playing || current.phase == room::TablePhase::Paused) {
					auto aborted = _roomAuthority->EndMatch(tableId, current.matchGeneration, room::MatchResult::Abort);
					if (aborted.accepted) events.insert(events.end(), aborted.events.begin(), aborted.events.end());
				}
			}
		}
	}
	for (const auto& event : result.events) {
		if (event.kind == room::Event::Kind::MatchReady) {
			const auto& table = _roomAuthority->SnapshotView().tables[event.table];
			auto started = _roomAuthority->BeginMatch(event.table, table.p1, table.p2);
			if (started.accepted) {
				events.insert(events.end(), started.events.begin(), started.events.end());
				for (const auto& startedEvent : started.events) {
					if (startedEvent.kind == room::Event::Kind::MatchStarted &&
						!BeginAuthorizedTable(startedEvent.table, startedEvent.matchGeneration)) {
						_transportFailed = true;
					}
				}
			}
		}
		else if (event.kind == room::Event::Kind::MatchEnded) {
			auto* authority = RoomMatchAuthority(event.table);
			bool nativeEndSent = false;
			if (authority && authority->Generation() == event.matchGeneration &&
				authority->GetPhase() != session::MatchAuthority::Phase::Idle) {
				nativeEndSent = true;
				if (!authority->End(MatchSender())) _transportFailed = true;
			}
			// A terminal receipt can outlive MatchAuthority and the generic
			// effect journal. Replay the exact fighter generation directly
			// from the durable room ledger when native teardown was already
			// retired, so Ending cannot wait forever for an evicted game_end.
			if (event.terminalReplay && !nativeEndSent && _roomAuthority) {
				bool sent = true;
				for (const auto member : _roomAuthority->TerminalMembers(event.table, event.matchGeneration)) {
					for (const auto& mapping : roomMembers) {
						if (mapping.second != member) continue;
						sent = MatchSender()(mapping.first, json{{"type", "game_end"}, {"generation", event.matchGeneration}}) && sent;
						break;
					}
				}
				if (!sent) _transportFailed = true;
			}
		}
		else if (event.kind == room::Event::Kind::RoomClosed) {
			for (auto& authority : _roomMatchAuthorities) {
				if (authority && !authority->End(MatchSender())) _transportFailed = true;
			}
		}
		if (event.kind == room::Event::Kind::MemberRemoved && event.member != roomMember->second) {
			if (actionMessage.action.kind == room::ActionKind::Kick) {
				auto identity = roomPeerIdentities.find(event.member);
				if (identity != roomPeerIdentities.end()) roomBannedIdentities.insert(identity->second);
			}
			for (auto kicked = roomMembers.begin(); kicked != roomMembers.end(); ++kicked) {
				if (kicked->second == event.member) {
					const auto kickedConnection = kicked->first;
					if (actionMessage.action.kind == room::ActionKind::Kick) {
						SessionProtocol::RoomResultMessage kickedResponse;
						kickedResponse.result.accepted = false;
						kickedResponse.result.reason = room::RejectReason::MemberKicked;
						kickedResponse.result.snapshot = _roomAuthority->SnapshotCopy();
						kickedResponse.result.snapshot.localMember = 0;
						Respond(kickedConnection, json(kickedResponse));
					}
					roomSelectedTables.erase(kicked->first);
					roomPeerIdentities.erase(event.member);
					roomMembers.erase(kicked);
					cidMap.erase(kickedConnection);
					clients.erase(std::remove_if(clients.begin(), clients.end(), [&](const SessionMember& client) {
						return client.conn == kickedConnection;
					}), clients.end());
					for (auto& loaded : _roomBattleLoaded) loaded.erase(kickedConnection);
					for (auto& punch : _roomPunchReady) punch.erase(kickedConnection);
					break;
				}
			}
		}
	}
	SessionProtocol::RoomResultMessage response;
	response.result = result;
	response.actionId = actionMessage.action.actionId;
	response.result.snapshot = _roomAuthority->SnapshotFor(roomMember->second);
	if (result.accepted) {
		for (const auto& local : response.result.snapshot.members) {
			if (local.id == roomMember->second && local.table >= 0 && local.table < static_cast<std::int8_t>(room::TableCount)) {
				roomSelectedTables[conn] = static_cast<std::uint8_t>(local.table);
			}
		}
	}
	// ACK clients retain this exact action identity until an accepted
	// response. Commit the full local reply and its stable digest, while
	// leaving the optional successor replay copy out of the bounded journal;
	// a successor answers the stable retry from the receipt/tombstone.
	Respond(conn, json(response), actionMessage.action.kind != room::ActionKind::AcknowledgeTerminal);
	// An acknowledgement for a generation the ledger does not hold
	// means the client is acting on a stale MatchEnded. Send it the
	// receipts it actually owes so it can acknowledge those instead.
	if (!result.accepted && actionMessage.action.kind == room::ActionKind::AcknowledgeTerminal &&
		result.reason == room::RejectReason::WrongGeneration)
		ReplayPendingTerminalEvents(conn, roomMember->second);
	if (result.accepted && actionMessage.action.kind == room::ActionKind::Leave) {
		const auto leavingConnection = conn;
		const auto leavingMember = roomMember->second;
		roomPeerIdentities.erase(leavingMember);
		roomSelectedTables.erase(conn);
		roomMembers.erase(roomMember);
		cidMap.erase(leavingConnection);
		clients.erase(std::remove_if(clients.begin(), clients.end(), [&](const SessionMember& client) {
			return client.conn == leavingConnection;
		}), clients.end());
		for (auto& loaded : _roomBattleLoaded) loaded.erase(leavingConnection);
		for (auto& punch : _roomPunchReady) punch.erase(leavingConnection);
	}
	if (result.accepted) {
		if (actionMessage.action.table < room::TableCount) {
			const auto& before = priorSnapshot.tables[actionMessage.action.table];
			const auto& after = result.snapshot.tables[actionMessage.action.table];
			if (before.p1 != after.p1 || before.p2 != after.p2) _roomMatchData[actionMessage.action.table].Clear();
		}
		// An exact receipt/tombstone retry still commits its room_result so a
		// lost reply can complete, but it did not change public room state.
		// Leave a pre-existing dirty flag untouched and avoid another full
		// snapshot/projection burst for every member.
		if (!duplicateTerminalAcknowledgment) _dataDirty = true;
		// Coalesce all room actions observed in this poll into one
		// snapshot/projection burst. Each action still receives its own
		// result, but 16-member rooms must not enqueue one full snapshot
		// per participant per action.
		deferredRoomEvents.insert(deferredRoomEvents.end(), events.begin(), events.end());
	}
}

void SessionServer::HandleMatchAcknowledgement(session::Connection conn, const json& msg) {
	try {
		if (_roomAuthority) {
			const auto generation = msg.value("generation", std::uint64_t(0));
			const auto table = RoomTableForGeneration(generation);
			auto* authority = RoomMatchAuthority(table);
			if (authority && !authority->Acknowledge(conn, msg, MatchSender())) _transportFailed = true;
		} else if (_matchAuthority && !_matchAuthority->Acknowledge(conn, msg, MatchSender())) {
			_transportFailed = true;
		}
	} catch (const json::exception&) { /* Malformed acknowledgment has no authority. */ }
}

void SessionServer::HandleForward(session::Connection conn, const json& msg, const SessionProtocol::ConnectionID& cid) {
	SessionProtocol::ForwardMessage fwdMsg;
	try {
		msg.get_to(fwdMsg);
	}
	catch (const json::exception&) {
		spdlog::debug("Server: could not deserialize forwarding message");
		return;
	}

	// If this is a connection ID managed by this server, we can apply
	// additional security- messages with this source address should
	// only be coming from the connection that the address is assigned
	// to.
	if (!(fwdMsg.src == cid)) {
		spdlog::debug("Server: dropping fraudulent forwarding source");
		return;
	}
	const auto sourceTable = _roomAuthority ? RoomTableFor(conn) : static_cast<std::uint8_t>(room::TableCount);
	if (_roomAuthority && (!RoomMatchAuthority(sourceTable) || RoomMatchAuthority(sourceTable)->GetPhase() == session::MatchAuthority::Phase::Idle ||
		!IsRoomTableParticipant(conn, sourceTable))) return;

	if (fwdMsg.dest.host != _identity) {
		spdlog::info("Server: cannot forward to nonlocal identity {}@{}, clustering not yet implemented", fwdMsg.dest.user, fwdMsg.dest.host);
		return;
	}

	// Check that the destination is in fact the lobby
	auto clientIter = clients.begin();
	for (; clientIter != clients.end(); clientIter++) {
		if (clientIter->data.connId == fwdMsg.dest) {
			break;
		}
	}
	if (clientIter != clients.end() && (!_roomAuthority || IsRoomTableParticipant(clientIter->conn, sourceTable))) {
				Respond(clientIter->conn, msg);
	}
	else {
		spdlog::debug("Server: Could not forward to {}@{}: not in known lobby", fwdMsg.dest.user, fwdMsg.dest.host);
	}
}

void SessionServer::HandleJoinRequest(session::Connection conn, const json& msg, const session::Message& incoming, SessionProtocol::ConnectionID cid) {
	SessionProtocol::SessionJoinRequest request;
	const auto main=msg.find("mainFighter");
	const bool invalidMain=main!=msg.end()&&(!main->is_number_integer()||
		(main->is_number_unsigned()?main->get<std::uint64_t>()>43:
		main->get<std::int64_t>() < -1 || main->get<std::int64_t>() > 43));
	if(invalidMain) {
		SessionProtocol::SessionJoinReject reject;reject.result=SessionProtocol::JR_REQUEST_INVALID;
		Respond(conn,json(reject));return;
	}
	try {
		msg.get_to(request);
	}
	catch (const json::exception&) {
		spdlog::info("Server: could not deserialize join request");
		SessionProtocol::SessionJoinReject reject;
		reject.result = SessionProtocol::JR_REQUEST_INVALID;
		json rejectMsg = reject;
		Respond(conn, rejectMsg);
		return;
	}
	if ((_roomAuthority && (!request.customRooms || request.roomProtocol != room::ProtocolVersion)) ||
		(!_roomAuthority && request.customRooms)) {
		SessionProtocol::SessionJoinReject reject;
		reject.result = SessionProtocol::JR_REQUEST_INVALID;
		Respond(conn, json(reject));
		return;
	}

	SessionProtocol::JoinResult joinResult = RegisterToWait(conn, request.port, request.sidecarHash, request.username, incoming.peerAddress, cid, request.mainFighter);
	if (joinResult != SessionProtocol::JOIN_OK) {
		spdlog::info("Server: rejecting registration for reason {}", (int)joinResult);
		SessionProtocol::SessionJoinReject reject;
		reject.result = joinResult;
		json rejectMsg = reject;
		Respond(conn, rejectMsg);
		return;
	}
	EnableChatDelta(conn, request.roomChatDelta);

	_dataDirty = true;
}

void SessionServer::HandleSetChara(session::Connection conn, const json& msg) {
	const std::uint8_t tableId = RoomTableFor(conn);
    if(_roomAuthority&&tableId>=room::TableCount)return;
	if ((!_roomAuthority && _matchAuthority && _matchAuthority->GetPhase() != session::MatchAuthority::Phase::Idle) ||
		(_roomAuthority && RoomMatchAuthority(tableId) && RoomMatchAuthority(tableId)->GetPhase() != session::MatchAuthority::Phase::Idle)) return;
	const auto table = _roomAuthority ? _roomAuthority->SnapshotView().tables[tableId] : room::Table{};
	int side = _roomAuthority ? RoomSideFor(conn, tableId) : -1;
	if (!_roomAuthority) for (int i = 0; i < 2; i++) {
		if (clients.size() > i && clients.at(i).conn == conn) { side = i; break; }
	}
	if (side == -1) {
		spdlog::info("Server: sender {} tried to set chara, but is not playing", conn);
		return;
	}

	SessionProtocol::PreBattleSetChara request;
	try {
		msg.get_to(request);
	}
	catch (const json::exception&) {
		spdlog::info("Server: could not deserialize SetConditionsRequest");
		return;
	}
	auto& matchData = _roomAuthority ? _roomMatchData[tableId] : _matchData;
	if ((_roomAuthority ? table.ready[side] : matchData.readyMessageNum[side] != -1) ||
		!selection::Valid(selection::FromNative(request.chara), _roomAuthority ? table.rules.editionSelect : _lobbyData.editionSelect)) {
		spdlog::info("Server: rejected unavailable or locked character selection from {}", conn);
		return;
	}
	matchData.chara[side] = request.chara;
    if(_roomAuthority&&_roomAuthority->SetMemberFighter(side==0?table.p1:table.p2,request.chara.charaID))BroadcastRoomState({});
	_dataDirty = true;
}

void SessionServer::HandleSetEnv(session::Connection conn, const json& msg) {
	const std::uint8_t tableId = RoomTableFor(conn);
	int side = _roomAuthority ? RoomSideFor(conn, tableId) : -1;
	if (!_roomAuthority) for (int i = 0; i < 2; i++) {
		if (clients.size() > i && clients.at(i).conn == conn) { side = i; break; }
	}
	if (side != 0) {
		spdlog::info("Server: sender {} tried to set env, but is not P1", conn);
		return;
	}
	SessionProtocol::PreBattleSetEnv request;
	try {
		msg.get_to(request);
	}
	catch (const json::exception&) {
		spdlog::info("Server: could not deserialize SetConditionsRequest");
		return;
	}
	if (_roomAuthority && (_roomAuthority->SnapshotView().tables[tableId].ready[0] ||
		_roomAuthority->SnapshotView().tables[tableId].phase != room::TablePhase::Waiting)) return;
	(_roomAuthority ? _roomMatchData[tableId] : _matchData).rngSeed = request.rngSeed;
	_dataDirty = true;
}

void SessionServer::HandleSetStage(session::Connection conn, const json& msg) {
	const std::uint8_t tableId = RoomTableFor(conn);
	if ((!_roomAuthority && _matchAuthority && _matchAuthority->GetPhase() != session::MatchAuthority::Phase::Idle) ||
		(_roomAuthority && RoomMatchAuthority(tableId) && RoomMatchAuthority(tableId)->GetPhase() != session::MatchAuthority::Phase::Idle)) return;
	// P1 submits the stage before its Ready message. P2 may already be ready.
	if ((_roomAuthority && _roomAuthority->SnapshotView().tables[tableId].ready[0]) ||
		(!_roomAuthority && _matchData.readyMessageNum[0] != -1)) return;
	int side = -1;
	side = _roomAuthority ? RoomSideFor(conn, tableId) : -1;
	if (!_roomAuthority) for (int i = 0; i < 2; i++) {
		if (clients.size() > i && clients.at(i).conn == conn) { side = i; break; }
	}
	if (side != 0) {
		spdlog::info("Server: sender {} tried to set stage, but is not P1", conn);
		return;
	}
	SessionProtocol::PreBattleSetStage request;
	try {
		msg.get_to(request);
	}
	catch (const json::exception&) {
		spdlog::info("Server: could not deserialize SetConditionsRequest");
		return;
	}

	(_roomAuthority ? _roomMatchData[tableId] : _matchData).stageID = request.stageID;
	_dataDirty = true;
}

void SessionServer::HandleLobbySetSettings(session::Connection conn, const json& msg) {
	if (_roomAuthority) return;
	int side = -1;
	for (int i = 0; i < 2; i++) {
		if (clients.size() > i && clients.at(i).conn == conn) {
			side = i;
			break;
		}
	}
	if (side != 0) {
		spdlog::info("Server: sender {} tried to set lobby settings, but is not P1", conn);
		return;
	}
	SessionProtocol::LobbySetSettings request;
	try {
		if (_matchAuthority) {
			const auto& rounds = msg.at("roundCount");
			const auto& time = msg.at("roundTime");
			// Validate before get_to narrows JSON numbers into game fields.
			if (!rounds.is_number_integer() || rounds < 1 || rounds > 99 ||
				!msg.at("editionSelect").is_boolean() || !msg.at("trainingMode").is_boolean() ||
				!time.at("integral").is_number_integer() || time.at("integral") < 30 || time.at("integral") > 9999 ||
				!time.at("fractional").is_number_integer() || time.at("fractional") != 0) return;
		}
		msg.get_to(request);
	}
	catch (const json::exception&) {
		spdlog::info("Server: could not deserialize LobbySetSettings");
		return;
	}

	// Once readiness begins, changing settings would invalidate the
	// conditions the other player has accepted. Keep legacy comparison
	// behavior while enforcing this gate on authorized Iroh rooms.
	if (_matchAuthority) {
		netplay::LobbySettings settings;
		settings.editionSelect = request.editionSelect;
		settings.roundCount = request.roundCount;
		settings.roundTime = request.roundTime.integral;
		if (!settings.Valid() || request.roundTime.fractional != 0 || request.trainingMode ||
			_matchAuthority->GetPhase() != session::MatchAuthority::Phase::Idle ||
			_matchData.readyMessageNum[0] != -1 || _matchData.readyMessageNum[1] != -1) return;
	}
	_lobbyData.editionSelect = request.editionSelect;
	_lobbyData.roundCount = request.roundCount;
	_lobbyData.roundTime = request.roundTime;
	_lobbyData.trainingMode = request.trainingMode;
	spdlog::info(
		"Server: lobby settings set by host: edition={} rounds={} time={} training={}",
		request.editionSelect,
		request.roundCount,
		request.roundTime.integral,
		request.trainingMode
	);
	_dataDirty = true;
}

void SessionServer::HandleBattleLoaded(session::Connection conn, bool& bSendBattleSynced) {
	if (_roomAuthority) {
		const auto tableId = RoomTableFor(conn);
		if (!RoomMatchAuthority(tableId) || RoomMatchAuthority(tableId)->GetPhase() == session::MatchAuthority::Phase::Idle ||
			!IsRoomTableParticipant(conn, tableId)) return;
		_roomBattleLoaded[tableId].insert(conn);
		const auto& table = _roomAuthority->SnapshotView().tables[tableId];
		std::set<session::Connection> expected;
		const auto add = [&](room::MemberId id) {
			for (const auto& member : roomMembers) if (member.second == id) { expected.insert(member.first); break; }
		};
		add(table.p1); add(table.p2); for (const auto spectator : table.spectators) add(spectator);
		if (!expected.empty() && std::all_of(expected.begin(), expected.end(), [&](session::Connection peer) {
			return _roomBattleLoaded[tableId].count(peer) != 0;
		})) {
			SendRoomTable(tableId, json(SessionProtocol::BattleSynced()));
		}
	} else {
		bSendBattleSynced = true;
		for (int i = 0; i < clients.size(); i++) {
			if (clients.at(i).conn == conn) clients.at(i).data.flags |= SessionProtocol::MF_BATTLE_LOADED;
			bSendBattleSynced = bSendBattleSynced && (clients.at(i).data.flags & SessionProtocol::MF_BATTLE_LOADED);
		}
		_dataDirty = true;
	}
}

void SessionServer::HandleLobbyReady(session::Connection conn, const json& msg, const session::Message& incoming, bool& bSendLobbyAllReady) {
	if (_roomAuthority) return;
	if (_matchAuthority && _matchAuthority->GetPhase() != session::MatchAuthority::Phase::Idle) return;
	int side = -1;
	for (int i = 0; i < 2; i++) {
		if (clients.size() > i && clients.at(i).conn == conn) {
			side = i;
			break;
		}
	}
	if (side == -1) {
		spdlog::info("Server: sender {} tried to ready, but is not playing", conn);
		return;
	}

	SessionProtocol::LobbyReady request;
	try {
		msg.get_to(request);
	}
	catch (const json::exception&) {
		spdlog::info("Server: could not deserialize ReportResultsRequest");
		return;
	}
	if (!selection::Valid(selection::FromNative(_matchData.chara[side]), _lobbyData.editionSelect)) return;
	_matchData.readyMessageNum[side] = incoming.messageId;
	bSendLobbyAllReady = bSendLobbyAllReady || _matchData.IsAllReady();
	_dataDirty = true;
}

void SessionServer::HandleLobbyReportResults(session::Connection conn, const json& msg) {
	if (_roomAuthority) return;
	if (_matchAuthority && (_matchAuthority->GetPhase() != session::MatchAuthority::Phase::Started ||
		!msg.contains("generation") || !msg["generation"].is_number_unsigned() ||
		msg["generation"].get<std::uint64_t>() != _matchAuthority->Generation())) return;
	SessionProtocol::LobbyReportResults request;
	try {
		msg.get_to(request);
	}
	catch (const json::exception&) {
		spdlog::info("Server: could not deserialize ReportResultsRequest");
		return;
	}

	// Results can rotate only a player that actually exists, and
	// only either active player may report them.
	if (clients.size() < 2 || request.loserSide < 0 || request.loserSide > 1 ||
		(clients[0].conn != conn && clients[1].conn != conn)) return;
	HandleResults(request.loserSide);
	_dataDirty = true;
}

void SessionServer::HandleLobbyReset(session::Connection conn, const json& msg) {
	if (_roomAuthority) return;
	if (_matchAuthority && (!msg.contains("generation") || !msg["generation"].is_number_unsigned() ||
		msg["generation"].get<std::uint64_t>() != _matchAuthority->Generation())) return;
	int side = -1;
	for (int i = 0; i < 2; i++) {
		if (clients.size() > i && clients.at(i).conn == conn) {
			side = i;
			break;
		}
	}
	if (side == -1) {
		spdlog::info("Server: sender {} tried to reset lobby, but is not playing", conn);
		return;
	}
	ResetLobbyForRematch();
}

void SessionServer::HandlePunchReady(session::Connection conn) {
	if (_roomAuthority) {
		const auto tableId = RoomTableFor(conn);
		if (!RoomMatchAuthority(tableId) || RoomMatchAuthority(tableId)->GetPhase() == session::MatchAuthority::Phase::Idle ||
			!IsRoomTableParticipant(conn, tableId)) return;
		_roomPunchReady[tableId].insert(conn);
		const auto& table = _roomAuthority->SnapshotView().tables[tableId];
		std::set<session::Connection> expected;
		const auto add = [&](room::MemberId id) { for (const auto& pair : roomMembers) if (pair.second == id) { expected.insert(pair.first); break; } };
		add(table.p1); add(table.p2); for (const auto spectator : table.spectators) add(spectator);
		if (!expected.empty() && std::all_of(expected.begin(), expected.end(), [&](session::Connection peer) { return _roomPunchReady[tableId].count(peer) != 0; })) {
			SendRoomTable(tableId, json(SessionProtocol::PunchGo()));
			_roomPunchReady[tableId].clear();
		}
		return;
	}
	int side = -1;
	for (int i = 0; i < 2 && i < (int)clients.size(); i++) {
		if (clients.at(i).conn == conn) {
			side = i;
			break;
		}
	}
	if (side == -1) {
		spdlog::info("Server: sender {} sent punch_ready but is not playing", conn);
		return;
	}
	_punchReady[side] = true;
	if (_punchReady[0] && _punchReady[1]) {
		SessionProtocol::PunchGo go;
		json goMsg = go;
		BroadcastMessage(goMsg);
		_punchReady[0] = false;
		_punchReady[1] = false;
	}
}

void SessionServer::HandleGgpoFrame(session::Connection conn, const json& msg, const SessionProtocol::ConnectionID& cid) {
	SessionProtocol::BattleGgpoFrame frame;
	try {
		msg.get_to(frame);
	}
	catch (const json::exception&) {
		spdlog::debug("Server: could not deserialize GGPO frame");
		return;
	}

	if (!(frame.src == cid)) {
		spdlog::debug("Server: dropping fraudulent GGPO frame");
		return;
	}
	const auto sourceTable = _roomAuthority ? RoomTableFor(conn) : static_cast<std::uint8_t>(room::TableCount);
	if (_roomAuthority && (!RoomMatchAuthority(sourceTable) || RoomMatchAuthority(sourceTable)->GetPhase() == session::MatchAuthority::Phase::Idle ||
		!IsRoomTableParticipant(conn, sourceTable))) return;

	for (auto clientIter = clients.begin(); clientIter != clients.end(); clientIter++) {
		if (clientIter->data.connId == frame.dest && (!_roomAuthority || IsRoomTableParticipant(clientIter->conn, sourceTable))) {
			Respond(clientIter->conn, msg);
			break;
		}
	}
}
