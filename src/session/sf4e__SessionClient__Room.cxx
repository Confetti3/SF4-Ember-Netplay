// SessionClient: custom-room membership, table projection, room actions and their replies.

#include <stdlib.h>
#include <algorithm>
#include <string>
#include <utility>
#include <limits>

#include <windows.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <ggponet.h>

#include "../Dimps/Dimps.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/ConfirmedCheckpoint.hxx"
#include "../common/sf4e__StateHash.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"

#include "../sf4e/sf4e__Game__Battle__System.hxx"

#include "sf4e__SessionClient.hxx"
#include "sf4e__SessionProtocol.hxx"
#include "../sf4e/sf4e__NetplayFacade.hxx"

using nlohmann::json;

namespace SessionProtocol = sf4e::SessionProtocol;
namespace session = sf4e::session;
using rSystem = Dimps::Game::Battle::System;
using fSystem = sf4e::Game::Battle::System;
using sf4e::SessionClient;
using sf4e::SessionProtocol::LobbyReady;

// Catalog id of the player-facing text for a refused room action. The runtime
// resolves it with loc::T where RoomError() reaches the status line; this
// library does not link the catalogs. A DuplicateResult is the normal reply to
// a retried report and is not an error.
static const char* RoomRejectText(sf4e::room::RejectReason reason) {
	using sf4e::room::RejectReason;
	switch (reason) {
	case RejectReason::Closed: return "room.reject.closed";
	case RejectReason::RoomFull: return "room.reject.full";
	case RejectReason::AdmissionLocked: return "room.reject.admission_locked";
	case RejectReason::NameTaken: return "room.reject.name_taken";
	case RejectReason::StaleRoom: return "room.reject.stale_room";
	case RejectReason::StaleTable: return "room.reject.stale_table";
	case RejectReason::WrongPhase: return "room.reject.wrong_phase";
	case RejectReason::WrongGeneration: return "room.reject.wrong_generation";
	case RejectReason::Unauthorized: return "room.reject.unauthorized";
	case RejectReason::NotSeated: return "room.reject.not_seated";
	case RejectReason::AlreadySeated: return "room.reject.already_seated";
	case RejectReason::AlreadyQueued: return "room.reject.already_queued";
	case RejectReason::NotQueued: return "room.reject.not_queued";
	case RejectReason::NotWatching: return "room.reject.not_watching";
	case RejectReason::InvalidRules: return "room.reject.invalid_rules";
	case RejectReason::InvalidCapacity: return "room.reject.invalid_capacity";
	case RejectReason::InvalidChat: return "room.reject.invalid_chat";
	case RejectReason::MemberKicked: return "runtime.removed_from_room";
	case RejectReason::TerminalLedgerFull: return "room.reject.ledger_full";
	case RejectReason::DuplicateResult: return "";
	default: return "room.reject.other";
	}
}

void SessionClient::TrySendPendingJoinRequest() {
	if (!_joinRequestPending || !_transport || !_connected || _stepCounter < _joinRequestNextStep) return;
	SessionProtocol::SessionJoinRequest request;
	request.sidecarHash = _sidecarHash;
	request.username = _name;
	request.port = _ggpoPort;
	request.customRooms = _customRoomsRequired;
	request.roomProtocol = _customRoomsRequired ? room::ProtocolVersion : 0;
	request.mainFighter = _mainFighter;
	request.roomChatDelta = _customRoomsRequired;
	json payload = request;
	const auto sent = Send(payload, nullptr);
	if (sent == session::SendResult::Queued) {
		// Local queue acceptance is not authenticated admission.  Keep the
		// request pending until a committed custom-room snapshot or a legacy data
		// update proves this exact CID was admitted. A control send can fail after
		// this enqueue during leader recovery, and the retry is idempotent at the
		// server.
		_joinRequestNextStep = _stepCounter + 8;
		return;
	}
	// The CID response and the authenticated join request are separate
	// reliable messages. A temporary degraded/rebinding transport must not
	// consume the one-shot projection admission request.
	_joinRequestNextStep = _stepCounter + ((sent == session::SendResult::NotConnected || sent == session::SendResult::QueueFull) ? 1 : 4);
	if (sent != session::SendResult::NotConnected && sent != session::SendResult::QueueFull)
		_roomError = "join_request_send_failed";
}

void SessionClient::ReconcileTerminalAcks() {
	for (auto pending = _pendingTerminalAcks.begin(); pending != _pendingTerminalAcks.end();) {
		if (pending->roomEpoch != _roomSnapshot.roomEpoch) {
			pending = _pendingTerminalAcks.erase(pending);
			continue;
		}
		const auto local = std::find_if(_roomSnapshot.members.begin(), _roomSnapshot.members.end(),
			[&](const room::Member& member) { return member.id == _roomSnapshot.localMember; });
		if (_roomSnapshot.localMember != pending->member || local == _roomSnapshot.members.end() ||
			local->incarnation != pending->incarnation) {
			pending = _pendingTerminalAcks.erase(pending);
			continue;
		}
		if (_roomSnapshot.localTerminalPending && pending->table < room::TableCount &&
			_roomSnapshot.localTerminalGenerations[pending->table] == pending->generation) {
			pending->observedPendingRevision = (std::max)(pending->observedPendingRevision, _roomSnapshot.revision);
			++pending;
			continue;
		}
		// Aggregate false is authoritative only after a same-room projection
		// first proved this exact recipient/generation was pending. This rejects
		// delayed preterminal snapshots and receipts from an older generation.
		if (pending->observedPendingRevision && !_roomSnapshot.localTerminalPending &&
			_roomSnapshot.revision > pending->observedPendingRevision) {
			pending = _pendingTerminalAcks.erase(pending);
			continue;
		}
		++pending;
	}
}

bool SessionClient::TakeRoomEvent(room::Event& event) {
	if (_roomEvents.empty()) return false;
	event = _roomEvents.front();
	_roomEvents.pop_front();
	return true;
}

bool SessionClient::LocalSelectionLocked(int slot) const {
	if (_roomSnapshot.roomEpoch) {
		const auto member = std::find_if(_roomSnapshot.members.begin(), _roomSnapshot.members.end(),
			[&](const room::Member& value) { return value.id == _roomSnapshot.localMember; });
		if (_roomSnapshot.closed || member == _roomSnapshot.members.end() || member->seat != slot ||
			slot < 0 || slot >= 2 || member->table < 0 || member->table >= room::TableCount) return true;
		const auto& table = _roomSnapshot.tables[member->table];
		if ((slot == 0 ? table.p1 : table.p2) != member->id) return true;
		return table.phase != room::TablePhase::Waiting || table.ready[slot];
	}
	return slot < 0 || slot >= 2 || _matchData.readyMessageNum[slot] != -1;
}

bool SessionClient::ReleaseRoomProjection() {
	if (!_projectionFrozen) return true;
	_projectionFrozen = false;
	ProjectSelectedRoomTable();
	return true;
}

bool SessionClient::ApplyPendingRoomProjection(std::uint64_t generation) {
	if (!_projectionFrozen || !generation) return false;
	const auto* projection = _queuedGrantProjection && _queuedGrantGeneration == generation
		? &*_queuedGrantProjection : _pendingRoomProjection && _pendingRoomProjection->matchGeneration == generation
		? &*_pendingRoomProjection : nullptr;
	if (!projection) return _appliedRoomProjectionGeneration == generation;
	// This committed projection is the native roster for the exact grant
	// generation. It may add queued members which BeginMatch promoted to frozen
	// spectators after the preceding Waiting snapshot, so apply it atomically
	// with the native settings before the grant validates the roster.
	_lobbyData = projection->lobbyData;
	_matchData = projection->matchData;
	_appliedRoomProjectionGeneration = generation;
	if (_queuedGrantProjection && _queuedGrantGeneration == generation) {
		_queuedGrantProjection.reset(); _queuedGrantGeneration = 0;
	}
	if (_pendingRoomProjection && _pendingRoomProjection->matchGeneration == generation) _pendingRoomProjection.reset();
	return true;
}

void SessionClient::ProjectSelectedRoomTable() {
	if (!_customRoomsSeen || _projectionFrozen || _selectedRoomTable >= room::TableCount) return;
	for (const auto& member : _roomSnapshot.members) {
		if (member.id == _roomSnapshot.localMember && member.table >= 0 && member.table < static_cast<std::int8_t>(room::TableCount)) {
			_selectedRoomTable = static_cast<std::uint8_t>(member.table);
			break;
		}
	}
	const auto& table = _roomSnapshot.tables[_selectedRoomTable];
	_lobbyData.members.clear();
	_lobbyData.editionSelect = table.rules.editionSelect;
	_lobbyData.roundCount = table.rules.roundCount;
	_lobbyData.roundTime.integral = table.rules.roundTime;
	_lobbyData.roundTime.fractional = 0;
	_lobbyData.trainingMode = false;
	for (const auto id : {table.p1, table.p2}) {
		if (!id) {
			_lobbyData.members.push_back(SessionProtocol::MemberData{});
			continue;
		}
		for (const auto& member : _roomSnapshot.members) if (member.id == id) {
			SessionProtocol::MemberData data;
			data.connId.host = member.connection.host;
			data.connId.user = member.connection.user;
			data.name = member.name;
			data.port = 0; data.ip.clear(); data.flags = 0;
			_lobbyData.members.push_back(data);
			break;
		}
	}
	for (const auto id : table.spectators) {
		for (const auto& member : _roomSnapshot.members) if (member.id == id) {
			SessionProtocol::MemberData data;
			data.connId.host = member.connection.host;
			data.connId.user = member.connection.user;
			data.name = member.name;
			data.port = 0; data.ip.clear(); data.flags = 0;
			_lobbyData.members.push_back(data);
			break;
		}
	}
	_matchData.ClearReady();
	_matchData.readyMessageNum[0] = table.ready[0] ? 0 : -1;
	_matchData.readyMessageNum[1] = table.ready[1] ? 0 : -1;
}

session::SendResult SessionClient::SendRoomAction(room::Action action, std::uint64_t* actionId) {
	if (!_transport || !_customRoomsSeen || _nextRoomActionId == (std::numeric_limits<std::uint64_t>::max)()) return session::SendResult::NotConnected;
	action.protocolVersion = room::ProtocolVersion;
	action.actionId = _nextRoomActionId++;
	if (!action.roomEpoch) action.roomEpoch = _roomSnapshot.roomEpoch;
	SessionProtocol::RoomActionMessage message;
	message.action = action;
	json payload = message;
    const auto sent=Send(payload,nullptr);
	if (sent==session::SendResult::Queued) {
		RememberSentRoomAction(action);
		if (auto* retry=RetryRecord(action.kind)) {
			retry->actionId=action.actionId;
			retry->payload=payload.dump();
		}
		if (actionId) *actionId=action.actionId;
	}
    return sent;
}

void SessionClient::RememberSentRoomAction(const room::Action& action) {
	for (const auto& sent : _sentRoomActions)
		if (sent.actionId == action.actionId) return;
	if (_sentRoomActions.size() >= 32) _sentRoomActions.pop_front();
	_sentRoomActions.push_back({action.actionId, action.kind, action.table, action.matchGeneration,
		action.inputDelay, action.actionId});
}

sf4e::room::Action SessionClient::TableAction(room::ActionKind kind, std::uint8_t table, std::uint8_t inputDelay) const {
	room::Action action;
	action.kind = kind; action.table = table; action.inputDelay = inputDelay;
	action.roomEpoch = _roomSnapshot.roomEpoch;
	action.revision = _roomSnapshot.revision;
	action.tableRevision = _roomSnapshot.tables[table].revision;
	return action;
}

void SessionClient::LogRejectedRoomAction(std::uint64_t actionId, room::RejectReason reason) {
	if (actionId == _lastRejectedActionId && reason == _lastRejectedReason) return;
	_lastRejectedActionId = actionId;
	_lastRejectedReason = reason;
	const auto found = std::find_if(_sentRoomActions.begin(), _sentRoomActions.end(),
		[&](const SentRoomAction& sent) { return sent.actionId == actionId; });
	// A duplicate result report is the expected reply to a retried report.
	const auto level = reason == room::RejectReason::DuplicateResult ? spdlog::level::info : spdlog::level::warn;
	if (found == _sentRoomActions.end()) {
		spdlog::log(level, "Room action rejected action={} kind=unknown reason={}", actionId, static_cast<int>(reason));
		return;
	}
	spdlog::log(level, "Room action rejected action={} kind={} table={} generation={} reason={}",
		actionId, static_cast<int>(found->kind), found->table, found->generation, static_cast<int>(reason));
}

session::SendResult SessionClient::RetryRoomResult(room::Action action, std::uint64_t actionId) {
	return RetryRoomLifecycleAction(std::move(action),actionId,room::ActionKind::RecordResult);
}

session::SendResult SessionClient::RetryMatchFinished(room::Action action, std::uint64_t actionId) {
	return RetryRoomLifecycleAction(std::move(action),actionId,room::ActionKind::MatchFinished);
}

SessionClient::RetainedRoomRetry* SessionClient::RetryRecord(room::ActionKind kind) {
	if(kind==room::ActionKind::RecordResult) return &_resultRetry;
	if(kind==room::ActionKind::MatchFinished) return &_finishRetry;
	return nullptr;
}

session::SendResult SessionClient::RetryRoomLifecycleAction(room::Action action,
	std::uint64_t actionId, room::ActionKind expectedKind) {
	if (!_transport || !_customRoomsSeen) return session::SendResult::NotConnected;
	if (action.kind != expectedKind || !actionId || actionId >= _nextRoomActionId)
		return session::SendResult::InvalidPayload;
	action.protocolVersion = room::ProtocolVersion;
	action.actionId = actionId;
	SessionProtocol::RoomActionMessage message;
	message.action = action;
	json payload = message;
	const auto* retry=RetryRecord(expectedKind);
	if(!retry || retry->actionId!=actionId || retry->payload!=payload.dump())
		return session::SendResult::InvalidPayload;
	return Send(payload, nullptr);
}

session::SendResult SessionClient::AcknowledgeTerminal(std::uint8_t table, std::uint64_t generation) {
	if (!_transport || !_customRoomsSeen || !generation || table >= room::TableCount || !_roomSnapshot.roomEpoch)
		return session::SendResult::NotConnected;
	for (const auto& pending : _pendingTerminalAcks)
		if (pending.table == table && pending.generation == generation && pending.roomEpoch == _roomSnapshot.roomEpoch)
			return session::SendResult::Queued;
	if (_pendingTerminalAcks.size() >= 32) return session::SendResult::QueueFull;
	const auto local = std::find_if(_roomSnapshot.members.begin(), _roomSnapshot.members.end(),
		[&](const room::Member& member) { return member.id == _roomSnapshot.localMember; });
	if (!_roomSnapshot.localMember || local == _roomSnapshot.members.end() || !local->incarnation)
		return session::SendResult::NotConnected;
	const auto observed = _roomSnapshot.localTerminalPending &&
		_roomSnapshot.localTerminalGenerations[table] == generation
		? _roomSnapshot.revision : 0;
	_pendingTerminalAcks.push_back({table, generation, _roomSnapshot.roomEpoch, _stepCounter + 1,
		0, _roomSnapshot.localMember, local->incarnation, observed});
	return session::SendResult::Queued;
}

	bool SessionClient::TakeActionReply(ActionReply& reply) {
	if (_actionReplies.empty()) return false;
	reply=_actionReplies.front(); _actionReplies.pop_front();
	if(reply.accepted || reply.reason==room::RejectReason::DuplicateResult) {
		for(auto* retry:{&_resultRetry,&_finishRetry})
			if(retry->actionId==reply.actionId) *retry={};
	}
	for (auto pending = _pendingTerminalAcks.begin(); pending != _pendingTerminalAcks.end();) {
		if (pending->actionId != reply.actionId) { ++pending; continue; }
		if (reply.accepted) pending = _pendingTerminalAcks.erase(pending);
		else if (reply.reason == room::RejectReason::WrongGeneration && ++pending->rejections >= 3) {
			// The authority no longer knows this generation (a stale
			// MatchEnded from a table that had already moved on). Retrying
			// forever only re-raised the same error every four steps.
			spdlog::warn("Client: dropping terminal acknowledgement table={} generation={} after {} WrongGeneration replies",
				pending->table, pending->generation, pending->rejections);
			pending = _pendingTerminalAcks.erase(pending);
		}
		else {
			pending->nextStep = _stepCounter + 4;
			++pending;
		}
		break;
	}
	return true;
}

SessionClient::~SessionClient()
{
	Disconnect();
}

bool SessionClient::HandleRoomSnapshot(json& msg) {
	SessionProtocol::RoomSnapshotMessage snapshot;
	try { msg.get_to(snapshot); }
	catch (const std::exception&) { _roomError = "invalid_room_snapshot"; return false; }
	// The owner leaves out chat this client already holds (it advertised
	// roomChatDelta). Keep the current room's chat; a different room has
	// none to keep.
	if (snapshot.chatUnchanged && _roomSnapshot.roomEpoch && snapshot.snapshot.roomEpoch == _roomSnapshot.roomEpoch)
		snapshot.snapshot.chat = _roomSnapshot.chat;
	if (snapshot.snapshot.protocolVersion != room::ProtocolVersion ||
		(snapshot.snapshot.localMember == 0 && _customRoomsRequired)) { _roomError = "incompatible_room_protocol"; return false; }
    if(_roomSnapshot.roomEpoch && (snapshot.snapshot.roomEpoch!=_roomSnapshot.roomEpoch ||
        snapshot.snapshot.revision<_roomSnapshot.revision)) return true;
	_roomSnapshot = std::move(snapshot.snapshot);
	_customRoomsSeen = true;
	if (_roomSnapshot.localMember != 0) _joinRequestPending = false;
	ReconcileTerminalAcks();
	_roomError.clear();
	ProjectSelectedRoomTable();
	if (_callbacks.OnRoomSnapshot) _callbacks.OnRoomSnapshot(this, _roomSnapshot, _callbacks);
	return true;
}

bool SessionClient::HandleRoomResult(json& msg) {
	SessionProtocol::RoomResultMessage result;
	try { msg.get_to(result); }
	catch (const std::exception&) { _roomError = "invalid_room_result"; return true; }
	const auto sent = std::find_if(_sentRoomActions.begin(), _sentRoomActions.end(),
		[&](const SentRoomAction& entry) { return entry.actionId == result.actionId; });
	// A resent Ready/Unready answers under the id its caller was given.
	const auto replyId = sent != _sentRoomActions.end() ? sent->callerId : result.actionId;
	const bool readiness = sent != _sentRoomActions.end() &&
		(sent->kind == room::ActionKind::Ready || sent->kind == room::ActionKind::Unready);
	if (result.result.snapshot.roomEpoch != 0 && (!_roomSnapshot.roomEpoch ||
        (result.result.snapshot.roomEpoch==_roomSnapshot.roomEpoch && result.result.snapshot.revision>=_roomSnapshot.revision))) {
		_roomSnapshot = std::move(result.result.snapshot);
		_customRoomsSeen = true;
		if (_roomSnapshot.localMember != 0) _joinRequestPending = false;
		ReconcileTerminalAcks();
		ProjectSelectedRoomTable();
	}
	// Ready pressed right after the player's own Unready (or vice versa)
	// carries a table revision the authority has already moved past. The
	// rejection brings the current snapshot, so resend from it instead of
	// leaving the press parked until its timeout.
	if (readiness && result.result.accepted) _staleTableRetries = 0;
	if (readiness && !result.result.accepted && result.result.reason == room::RejectReason::StaleTable &&
		_staleTableRetries < 3) {
		++_staleTableRetries;
		// Resend the same request (kind, table, input delay) from the
		// fresher snapshot. Copy first: sending may evict `sent`.
		const auto kind = sent->kind;
		const auto callerId = sent->callerId;
		const auto resent = SendRoomAction(TableAction(kind, sent->table, sent->inputDelay));
		if (resent == session::SendResult::Queued) {
			// SendRoomAction just remembered the resend as the newest entry.
			_sentRoomActions.back().callerId = callerId;
			spdlog::info("Client: {} raced the table revision; resent (attempt {})",
				kind == room::ActionKind::Ready ? "Ready" : "Unready", _staleTableRetries);
			if (_callbacks.OnRoomSnapshot) _callbacks.OnRoomSnapshot(this, _roomSnapshot, _callbacks);
			return true;
		}
	}
    if (replyId) {
        if (_actionReplies.size()>=32) _actionReplies.pop_front();
		ActionReply reply;
		reply.actionId = replyId; reply.accepted = result.result.accepted; reply.reason = result.result.reason;
		if (sent != _sentRoomActions.end()) { reply.kind = sent->kind; reply.kindKnown = true; }
        _actionReplies.push_back(reply);
        if (!result.result.accepted) LogRejectedRoomAction(replyId, result.result.reason);
    }
	if (!result.result.accepted) {
		// A finish or result report for a game the authority already
		// closed (the opponent's report landed first) is routine
		// bookkeeping, not something the player needs to read.
		const bool staleReport = sent != _sentRoomActions.end() &&
			(sent->kind == room::ActionKind::MatchFinished || sent->kind == room::ActionKind::RecordResult) &&
			(result.result.reason == room::RejectReason::WrongGeneration || result.result.reason == room::RejectReason::DuplicateResult);
		const char* text = RoomRejectText(result.result.reason);
		if (text[0] && !staleReport) _roomError = text;
	}
	else { _roomError.clear(); }
	if (_callbacks.OnRoomSnapshot) _callbacks.OnRoomSnapshot(this, _roomSnapshot, _callbacks);
	return true;
}

bool SessionClient::HandleRoomEvent(json& msg) {
	SessionProtocol::RoomEventMessage event;
	try { msg.get_to(event); }
	catch (const std::exception&) { _roomError = "invalid_room_event"; return true; }
	const bool lifecycle = event.event.kind == room::Event::Kind::MatchReady ||
		event.event.kind == room::Event::Kind::MatchStarted ||
		event.event.kind == room::Event::Kind::MatchEnded ||
		event.event.kind == room::Event::Kind::RoomClosed ||
		event.event.kind == room::Event::Kind::ResultDisputed;
	if (_roomEvents.size() >= 64) {
		if (!lifecycle) return true; // the accompanying snapshot is authoritative
		auto discard = std::find_if(_roomEvents.begin(), _roomEvents.end(), [&](const room::Event& queued) {
			return queued.kind != room::Event::Kind::MatchReady && queued.kind != room::Event::Kind::MatchStarted &&
				queued.kind != room::Event::Kind::MatchEnded && queued.kind != room::Event::Kind::RoomClosed &&
				queued.kind != room::Event::Kind::ResultDisputed;
		});
		if (discard == _roomEvents.end()) { _roomError = "room_event_overflow"; return false; }
		_roomEvents.erase(discard);
	}
	_roomEvents.push_back(event.event);
	// MatchEnded is only an outcome notification. The application queues
	// AcknowledgeTerminal after profile persistence and native/helper
	// retirement; spectators and fighters therefore cannot release a
	// durable receipt merely by receiving this event.
	if (_callbacks.OnRoomEvent) _callbacks.OnRoomEvent(this, event.event, _callbacks);
	return true;
}
