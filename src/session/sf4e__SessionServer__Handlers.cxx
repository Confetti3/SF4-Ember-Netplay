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
#include "../common/JsonDump.hxx"
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
	if (_matchAuthorizationConfigured && _matchAuthorizationIdentity) {
		const auto stable = _matchAuthorizationIdentity(conn);
		if (!stable.empty()) cidMsg.cid.user = stable;
	}
	cidMap[conn] = cidMsg.cid;
	SessionProtocol::JoinResult admissionResult = SessionProtocol::JOIN_OK;
	bool admitted = false;
	if (!hello.admission.is_null() && !hello.admission.empty()) {
		try {
			SessionProtocol::SessionJoinRequest admission = hello.admission.get<SessionProtocol::SessionJoinRequest>();
			if (!admission.customRooms || admission.roomProtocol != room::ProtocolVersion) admissionResult = SessionProtocol::JR_REQUEST_INVALID;
			else admissionResult = RegisterToWait(conn, cidMsg.cid, admission.Admission(incoming.peerAddress));
			admitted = admissionResult == SessionProtocol::JOIN_OK;
			if (admitted) EnableChatDelta(conn, admission.roomChatDelta);
		} catch (const std::exception&) { admissionResult = SessionProtocol::JR_REQUEST_INVALID; }
	}
	if (admitted) {
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
	else if (!_transport || !_transport->Send(conn, DumpJson(json(cidMsg)))) _transportFailed = true;
	if (!admitted && !hello.admission.is_null() && !hello.admission.empty()) {
		SessionProtocol::SessionJoinReject reject; reject.result = admissionResult;
		if (!_transport || !_transport->Send(conn, DumpJson(json(reject)))) _transportFailed = true;
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
	// A table's keeper readies only over the fighter and edition they won with,
	// as the match data the game starts from holds them: a pick the host
	// refused (HandleSetChara) or a pairing's cleared data would start them
	// as someone else.
	if (actionMessage.action.kind == room::ActionKind::Ready && actionMessage.action.table < room::TableCount) {
		const auto& chara = _roomMatchData[actionMessage.action.table].chara;
		const int side = RoomSideFor(conn, actionMessage.action.table);
		if (side >= 0 && !_roomAuthority->FighterAllowed(roomMember->second, chara[side].charaID, selection::FromNative(chara[side]).edition)) {
			SessionProtocol::RoomResultMessage refused;
			refused.result.accepted = false;
			refused.result.reason = room::RejectReason::Unauthorized;
			refused.actionId = actionMessage.action.actionId;
			refused.result.snapshot = _roomAuthority->SnapshotFor(roomMember->second);
			Respond(conn, json(refused));
			return;
		}
	}
	auto result = _roomAuthority->Apply(roomMember->second, actionMessage.action);
	std::vector<room::Event> events = result.events;
	const bool duplicateTerminalAcknowledgment = result.accepted &&
		actionMessage.action.kind == room::ActionKind::AcknowledgeTerminal &&
		result.snapshot.revision == priorSnapshot.revision && events.empty();
	if (result.accepted && (actionMessage.action.kind == room::ActionKind::Leave ||
		actionMessage.action.kind == room::ActionKind::Unwatch ||
		actionMessage.action.kind == room::ActionKind::Watch || actionMessage.action.kind == room::ActionKind::Kick ||
		actionMessage.action.kind == room::ActionKind::Queue)) {
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
	const auto started = StartReadyTables(result.events);
	events.insert(events.end(), started.begin(), started.end());
	for (const auto& event : result.events) {
		if (event.kind == room::Event::Kind::MatchEnded) {
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
			if (event.terminalReplay && !nativeEndSent) {
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
				// A server-owned room bans the account inside the authority.
				auto identity = roomPeerIdentities.find(event.member);
				if (identity != roomPeerIdentities.end() && !priorSnapshot.serverOwned) roomBannedIdentities.insert(identity->second);
				for (const auto& kicked : roomMembers) {
					if (kicked.second != event.member) continue;
					SessionProtocol::RoomResultMessage kickedResponse;
					kickedResponse.result.accepted = false;
					kickedResponse.result.reason = room::RejectReason::MemberKicked;
					kickedResponse.result.snapshot = _roomAuthority->SnapshotCopy();
					kickedResponse.result.snapshot.localMember = 0;
					Respond(kicked.first, json(kickedResponse));
					break;
				}
			}
			ForgetRoomMember(event.member);
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
		_serverOwnedLeftEmpty = _roomAuthority->ServerOwned() && _roomAuthority->SnapshotView().members.empty();
		roomPeerIdentities.erase(leavingMember);
		roomIncarnations.erase(leavingMember);
		roomMemberRecords.erase(leavingMember);
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
		ForgetReseatedMatchData(SeatedPairsOf(priorSnapshot));
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

void SessionServer::HandleMatchAcknowledgement(session::Connection conn, const json& msg,
	std::vector<room::Event>& events) {
	try {
		const auto generation = msg.value("generation", std::uint64_t(0));
		const auto table = RoomTableForGeneration(generation);
		auto* authority = RoomMatchAuthority(table);
		if (!authority) return;
		// A bound game whose permit window ran out during its preparation
		// is called off at the next step toward its start, so game_start
		// never goes out for it. A bound room seats its two fighters only,
		// so no departure can start the game instead. A game_start proposed
		// inside the window can still be committed after a same-term outage
		// and start then: that delay is bounded only by the commit latency
		// of its proposal. If no report arrives by start_by plus SILENT_SECS
		// (30 minutes), the bridge's maintenance moves the attempt to
		// review; reports after that are kept as evidence rather than
		// reconciled automatically.
		if (authority->Expects(conn, msg) && _roomAuthority->NativeStartExpired(table, generation)) {
			if (!authority->End(MatchSender())) _transportFailed = true;
			const auto ended = _roomAuthority->EndMatch(table, generation, room::MatchResult::Cancel);
			if (!ended.accepted) { _transportFailed = true; return; }
			_roomBattleLoaded[table].clear();
			_roomPunchReady[table].clear();
			_roomMatchData[table].Clear();
			_dataDirty = true;
			events.insert(events.end(), ended.events.begin(), ended.events.end());
			return;
		}
		if (!authority->Acknowledge(conn, msg, MatchSender())) _transportFailed = true;
		if (authority->GetPhase() == session::MatchAuthority::Phase::Started)
			_roomAuthority->NativeStarted(table, generation);
	} catch (const json::exception&) { /* Malformed acknowledgment has no authority. */ }
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
	if (!request.customRooms || request.roomProtocol != room::ProtocolVersion) {
		SessionProtocol::SessionJoinReject reject;
		reject.result = SessionProtocol::JR_REQUEST_INVALID;
		Respond(conn, json(reject));
		return;
	}

	SessionProtocol::JoinResult joinResult = RegisterToWait(conn, cid, request.Admission(incoming.peerAddress));
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
	if (tableId >= room::TableCount) return;
	if (RoomMatchAuthority(tableId) && RoomMatchAuthority(tableId)->GetPhase() != session::MatchAuthority::Phase::Idle) return;
	const auto table = _roomAuthority->SnapshotView().tables[tableId];
	const int side = RoomSideFor(conn, tableId);
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
	auto& matchData = _roomMatchData[tableId];
	const room::MemberId sender = side == 0 ? table.p1 : table.p2;
	const auto pick = selection::FromNative(request.chara);
	if (!_roomAuthority->FighterAllowed(sender, pick.fighter, pick.edition)) {
		spdlog::info("Server: table {} seat {} keeps the fighter it won with; refused fighter {}", tableId, side, request.chara.charaID);
		return;
	}
	if (table.ready[side] || !selection::Valid(pick, table.rules.editionSelect)) {
		spdlog::info("Server: rejected unavailable or locked character selection from {}", conn);
		return;
	}
	matchData.chara[side] = request.chara;
	bool withdrew = false;
	if (_roomAuthority->SetMemberChara(sender, {pick.fighter, pick.edition, pick.costume, pick.color, pick.ultra}, &withdrew)) {
		if (withdrew) spdlog::info("Server: table {} seat {} changed fighter to {}; the other seat's Ready was taken back", tableId, side, request.chara.charaID);
		BroadcastRoomState({});
	}
	_dataDirty = true;
}

void SessionServer::HandleSetEnv(session::Connection conn, const json& msg) {
	const std::uint8_t tableId = RoomTableFor(conn);
	if (RoomSideFor(conn, tableId) != 0) {
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
	const auto& table = _roomAuthority->SnapshotView().tables[tableId];
	if (table.ready[0] || table.phase != room::TablePhase::Waiting) return;
	_roomMatchData[tableId].rngSeed = request.rngSeed;
	_dataDirty = true;
}

void SessionServer::HandleSetStage(session::Connection conn, const json& msg) {
	const std::uint8_t tableId = RoomTableFor(conn);
	if (RoomMatchAuthority(tableId) && RoomMatchAuthority(tableId)->GetPhase() != session::MatchAuthority::Phase::Idle) return;
	// P1 submits the stage before its Ready message. P2 may already be ready.
	if (_roomAuthority->SnapshotView().tables[tableId].ready[0]) return;
	if (RoomSideFor(conn, tableId) != 0) {
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

	_roomMatchData[tableId].stageID = request.stageID;
	_dataDirty = true;
}
