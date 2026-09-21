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

const int sf4e::SESSION_CLIENT_MAX_MESSAGES_PER_POLL = 20;
bool SessionClient::bVerboseLogging = false;

// Bound for buffered remote v2 hashes (matches the checkpoint ring span).
static const size_t MAX_PENDING_REMOTE_HASHES = 64;

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

// The legacy snapshot comparison is a whole-struct memcmp, so a mismatch used
// to be reported as "Desync detected!" and nothing else. That is not enough to
// find a determinism bug afterwards: name the exact frame and the exact fields
// that diverged, with both values, so a player's log identifies the subsystem.
// StateSnapshot is 4-byte-aligned throughout with no padding, so every byte the
// memcmp sees is covered by the fields below.
static void ReportSnapshotDivergence(
	const char* stage,
	const sf4e::SessionProtocol::StateSnapshot& local,
	const sf4e::SessionProtocol::StateSnapshot& remote
) {
	spdlog::error("Desync v1 ({}): mismatch @ local frame {} remote frame {}",
		stage, local.frameIdx, remote.frameIdx);
	if (local.frameIdx != remote.frameIdx) {
		spdlog::error("Desync v1 ({}): frameIdx differs; snapshots are not for the same frame", stage);
	}
	const auto fixed = [](const Dimps::Math::FixedPoint& value) {
		return static_cast<int>(value.integral) + static_cast<double>(value.fractional) / 65536.0;
	};
	for (int i = 0; i < 2; i++) {
		const auto& a = local.chara[i];
		const auto& b = remote.chara[i];
		if (a.status != b.status) spdlog::error("Desync v1 ({}): chara{} status {} != {}", stage, i, a.status, b.status);
		if (a.side != b.side) spdlog::error("Desync v1 ({}): chara{} side {} != {}", stage, i, a.side, b.side);
		for (int axis = 0; axis < 4; axis++) {
			if (memcmp(&a.rootPos[axis], &b.rootPos[axis], sizeof(float)) != 0) {
				spdlog::error("Desync v1 ({}): chara{} rootPos[{}] {:a} != {:a}",
					stage, i, axis, a.rootPos[axis], b.rootPos[axis]);
			}
		}
		const struct { const char* name; const Dimps::Math::FixedPoint& mine; const Dimps::Math::FixedPoint& theirs; } fields[] = {
			{"vit", a.vit, b.vit}, {"vitmax", a.vitmax, b.vitmax},
			{"revenge", a.revenge, b.revenge}, {"revengemax", a.revengemax, b.revengemax},
			{"recoverable", a.recoverable, b.recoverable}, {"recoverablemax", a.recoverablemax, b.recoverablemax},
			{"super", a.super, b.super}, {"supermax", a.supermax, b.supermax},
			{"sctimeamt", a.sctimeamt, b.sctimeamt}, {"sctimemax", a.sctimemax, b.sctimemax},
			{"uctime", a.uctime, b.uctime}, {"uctimemax", a.uctimemax, b.uctimemax},
			{"damage", a.damage, b.damage}, {"combodamage", a.combodamage, b.combodamage},
		};
		for (const auto& field : fields) {
			if (field.mine.integral != field.theirs.integral || field.mine.fractional != field.theirs.fractional) {
				spdlog::error("Desync v1 ({}): chara{} {} {:.4f} != {:.4f}",
					stage, i, field.name, fixed(field.mine), fixed(field.theirs));
			}
		}
	}
}

// Strict debug mode: terminate the match on an authoritative v2 mismatch.
// Default (unset) logs and reports only — v2 must not end release matches
// while it is being validated; the legacy snapshot system retains its
// existing termination behavior.
static bool StrictDesyncEnabled() {
	static int s_cached = -1;
	if (s_cached < 0) {
		const char* env = getenv("SF4E_STRICT_DESYNC");
		s_cached = (env && env[0] == '1') ? 1 : 0;
	}
	return s_cached == 1;
}

// Diagnostic report for one v2 mismatch: exact frame, per-subsystem
// classification, and nearby checkpoint hashes. Never touches the legacy
// snapshot state. Termination only in strict mode, only between players.
static void ReportHashMismatch(
	SessionClient* client,
	const fSystem::HashCheckpoint& local,
	const sf4e::SessionProtocol::BattleHashV2& remote
) {
	if (local.hashes.overall == remote.overall) {
		return;
	}
	spdlog::error(
		"Desync v2: mismatch @ frame {} overall {:016x} != remote {:016x} (fromPlayer={}) fp={}",
		local.frameIdx, local.hashes.overall, remote.overall, remote.fromPlayer, sf4e::statehash::FpEnvironment()
	);
	spdlog::error(
		"Desync v2: subsystems flow:{} chara0:{} chara1:{}",
		local.hashes.flow == remote.flow ? "match" : "MISMATCH",
		local.hashes.chara[0] == remote.chara0 ? "match" : "MISMATCH",
		local.hashes.chara[1] == remote.chara1 ? "match" : "MISMATCH"
	);
	for (int i = 0; i < fSystem::NUM_HASH_CHECKPOINTS; i++) {
		const fSystem::HashCheckpoint& cp = fSystem::hashCheckpoints[i];
		if (!cp.valid || cp.frameIdx == local.frameIdx) {
			continue;
		}
		int distance = cp.frameIdx - local.frameIdx;
		if (distance >= -90 && distance <= 90) {
			spdlog::error(
				"Desync v2: nearby frame {} overall {:016x} flow {:016x} c0 {:016x} c1 {:016x}",
				cp.frameIdx, cp.hashes.overall, cp.hashes.flow,
				cp.hashes.chara[0], cp.hashes.chara[1]
			);
		}
	}

	// A spectator's mismatch (either side) must never terminate the two
	// players' fight; strict termination requires both peers to be players.
	if (StrictDesyncEnabled() && remote.fromPlayer && client->IsLocalPlayer()) {
		spdlog::error("Desync v2 (strict): terminating match at frame {}", local.frameIdx);
		client->TerminateOnDesync("v2_strict", local.frameIdx);
	}
}

// Ends the fight after a confirmed state divergence, and tells the player
// why: without this the game simply returned to the menu and the table
// paused thirty seconds later. Only a player may end the fight; a spectator
// that diverged is a spectator problem and is logged only.
void SessionClient::TerminateOnDesync(const char* stage, int frameIdx) {
	if (!IsLocalPlayer()) {
		spdlog::error("Desync ({}): local spectator diverged at frame {}; the players' fight continues", stage, frameIdx);
		sf4e::NetplayFacade::PushAlert("Your spectator view diverged from the fight. Leave and watch again.", sf4e::NoticeSeverity::Warning);
		return;
	}
	spdlog::error("Desync ({}): terminating match at frame {}", stage, frameIdx);
	sf4e::NetplayFacade::PushAlert(
		"Match ended: the two games diverged (desync). Export diagnostics from both players.",
		sf4e::NoticeSeverity::Error
	);
	*rSystem::GetReadyState(rSystem::staticMethods.GetSingleton()) = rSystem::RS_ISLEAVING;
}

bool SessionClient::IsLocalPlayer() const {
	for (int i = 0; i < 2 && i < (int)_lobbyData.members.size(); i++) {
		if (_lobbyData.members[i].connId == _cid) {
			return true;
		}
	}
	return false;
}

SessionClient::SessionClient(
	const Callbacks& callbacks,
	std::string sidecarHash,
	uint16_t ggpoPort,
	std::string& name
):
	_callbacks(callbacks),
	_sidecarHash(sidecarHash),
	_name(name),
	_ggpoPort(ggpoPort),
	_connected(false),
	_lobbyData(SessionProtocol::LobbyData::NULL_LOBBY)
{
}

int SessionClient::Connect(std::unique_ptr<session::ClientTransport> transport,
	bool snapshotsEnabled, bool sendHello) {
	Disconnect();
	_transport = std::move(transport);
	_snapshotsEnabled = snapshotsEnabled;
	_helloPending = sendHello;
	_joinRequestPending = false;
	_joinRequestNextStep = 0;
	return _transport && _transport->State() != session::ConnectionState::Failed ? 0 : -1;
}

std::string SessionClient::ServerAddress() const {
	return _transport ? _transport->PeerAddress() : std::string();
}

void SessionClient::Disconnect() {
	if (_transport) _transport->Close();
	_transport.reset();
	_connected = false;
	_helloPending = false;
	_joinRequestPending = false;
	_joinRequestNextStep = 0;
	_gameplayMessages.clear(); _gameplayGeneration = 0; _matchAuthorizationRequired = false;
	_lobbyData = SessionProtocol::LobbyData::NULL_LOBBY;
	_matchData.Clear();
	_cid = {};
	_roomSnapshot = {};
	_roomError.clear();
	_customRoomsSeen = false;
	_nextRoomActionId = 1;
	_resultRetry = {}; _finishRetry = {}; _staleTableRetries = 0;
    _actionReplies.clear();
	_projectionFrozen = false;
	_pendingRoomProjection.reset();
	_queuedGrantProjection.reset();
	_queuedGrantGeneration = 0;
	_appliedRoomProjectionGeneration = 0;
	_selectedRoomTable = 0;
	_outstandingReadyRequestNumber = -1;
	_stepCounter = 0;
	pendingRemoteSnapshots.clear();
	pendingRemoteHashes.clear();
	_roomEvents.clear();
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

bool SessionClient::TakeGameplayMessage(json& message) {
	if (_gameplayMessages.empty()) return false;
	message = std::move(_gameplayMessages.front()); _gameplayMessages.pop_front(); return true;
}

void SessionClient::PrepareForCallbacks()
{
	// Callback ownership belongs to each transport, never a global client.
}

int SessionClient::Step()
{
	if (!_transport) return 0;
	std::vector<session::Message> incoming;
	if (!_transport->Poll(incoming, SESSION_CLIENT_MAX_MESSAGES_PER_POLL) ||
		_transport->State() == session::ConnectionState::Failed ||
		_transport->State() == session::ConnectionState::Closed) {
		_connected = false;
		// Let the owner handle loss after Step returns. Calling the facade
		// here can delete this client while its method is still executing.
		return -1;
	}
	if (_transport->State() != session::ConnectionState::Connected) return 0;
	_connected = true;
	if (_helloPending) {
		json hello = SessionProtocol::SessionHelloMsg();
        if(_customRoomsRequired) {
            SessionProtocol::SessionJoinRequest admission;
            admission.sidecarHash=_sidecarHash; admission.username=_name; admission.port=_ggpoPort;
            admission.customRooms=true; admission.roomProtocol=room::ProtocolVersion; admission.mainFighter=_mainFighter;
            admission.roomChatDelta=true;
            hello["admission"]=admission;
        }
        const auto sent=Send(hello,nullptr);
        if(sent==session::SendResult::NotConnected || sent==session::SendResult::QueueFull) return 0;
        if(sent!=session::SendResult::Queued) return -1;
		_helloPending = false;
	}
	++_stepCounter;
	// Every recipient acknowledges a durable terminal event through the same
	// authenticated room action path. Reuse one action ID for the exact
	// room/table/generation so arbitrary response latency cannot evict its proof.
	for (auto pending = _pendingTerminalAcks.begin(); pending != _pendingTerminalAcks.end();) {
		if (_roomSnapshot.roomEpoch && _roomSnapshot.roomEpoch != pending->roomEpoch) {
			pending = _pendingTerminalAcks.erase(pending);
			continue;
		}
		if (!_customRoomsSeen || !_roomSnapshot.roomEpoch || _stepCounter < pending->nextStep) {
			++pending;
			continue;
		}
		// A reliable send can be accepted by the local transport while its
		// room-result reply is lost during leader recovery. Bound the in-flight
		// wait and retry the same idempotent table/generation acknowledgement;
		// otherwise the Application clears its local queue after the first send
		// and the receipt can remain blocked forever.
		if (!pending->actionId) {
			if (_nextRoomActionId == (std::numeric_limits<std::uint64_t>::max)()) {
				pending->nextStep = _stepCounter + 4;
				++pending;
				continue;
			}
			pending->actionId = _nextRoomActionId++;
		}
		room::Action acknowledgment;
		acknowledgment.kind = room::ActionKind::AcknowledgeTerminal;
		acknowledgment.protocolVersion = room::ProtocolVersion;
		acknowledgment.actionId = pending->actionId;
		acknowledgment.roomEpoch = pending->roomEpoch;
		acknowledgment.table = pending->table;
		acknowledgment.matchGeneration = pending->generation;
		acknowledgment.tableRevision = pending->table < room::TableCount ? _roomSnapshot.tables[pending->table].revision : 0;
		SessionProtocol::RoomActionMessage message;
		message.action = acknowledgment;
		json payload = message;
		if (Send(payload, nullptr) == session::SendResult::Queued) {
			RememberSentRoomAction(acknowledgment);
			pending->nextStep = _stepCounter + 8;
			++pending;
		} else {
			pending->nextStep = _stepCounter + 4;
			++pending;
		}
	}
	for (const auto& message : incoming) {
		json msg;
		try {
			msg = json::parse(message.payload);
		}
		catch (const json::exception&) {
			continue;
		}

		SessionProtocol::MessageType type;
		try {
			msg.at("type").get_to(type);
		}
		catch (const json::exception&) {
			spdlog::info("Client: got a message without a type, or a type that was not a string");
			continue;
		}

		if (type == SessionProtocol::MT_ROOM_SNAPSHOT) {
			SessionProtocol::RoomSnapshotMessage snapshot;
			try { msg.get_to(snapshot); }
			catch (const std::exception&) { _roomError = "invalid_room_snapshot"; return -1; }
			// The owner leaves out chat this client already holds (it advertised
			// roomChatDelta). Keep the current room's chat; a different room has
			// none to keep.
			if (snapshot.chatUnchanged && _roomSnapshot.roomEpoch && snapshot.snapshot.roomEpoch == _roomSnapshot.roomEpoch)
				snapshot.snapshot.chat = _roomSnapshot.chat;
			if (snapshot.snapshot.protocolVersion != room::ProtocolVersion ||
				(snapshot.snapshot.localMember == 0 && _customRoomsRequired)) { _roomError = "incompatible_room_protocol"; return -1; }
            if(_roomSnapshot.roomEpoch && (snapshot.snapshot.roomEpoch!=_roomSnapshot.roomEpoch ||
                snapshot.snapshot.revision<_roomSnapshot.revision)) continue;
			_roomSnapshot = std::move(snapshot.snapshot);
			_customRoomsSeen = true;
			if (_roomSnapshot.localMember != 0) _joinRequestPending = false;
			ReconcileTerminalAcks();
			_roomError.clear();
			ProjectSelectedRoomTable();
			if (_callbacks.OnRoomSnapshot) _callbacks.OnRoomSnapshot(this, _roomSnapshot, _callbacks);
		}
		else if (type == SessionProtocol::MT_ROOM_RESULT) {
			SessionProtocol::RoomResultMessage result;
			try { msg.get_to(result); }
			catch (const std::exception&) { _roomError = "invalid_room_result"; continue; }
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
					continue;
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
		}
		else if (type == SessionProtocol::MT_ROOM_EVENT) {
			SessionProtocol::RoomEventMessage event;
			try { msg.get_to(event); }
			catch (const std::exception&) { _roomError = "invalid_room_event"; continue; }
			const bool lifecycle = event.event.kind == room::Event::Kind::MatchReady ||
				event.event.kind == room::Event::Kind::MatchStarted ||
				event.event.kind == room::Event::Kind::MatchEnded ||
				event.event.kind == room::Event::Kind::RoomClosed ||
				event.event.kind == room::Event::Kind::ResultDisputed;
			if (_roomEvents.size() >= 64) {
				if (!lifecycle) continue; // the accompanying snapshot is authoritative
				auto discard = std::find_if(_roomEvents.begin(), _roomEvents.end(), [&](const room::Event& queued) {
					return queued.kind != room::Event::Kind::MatchReady && queued.kind != room::Event::Kind::MatchStarted &&
						queued.kind != room::Event::Kind::MatchEnded && queued.kind != room::Event::Kind::RoomClosed &&
						queued.kind != room::Event::Kind::ResultDisputed;
				});
				if (discard == _roomEvents.end()) { _roomError = "room_event_overflow"; return -1; }
				_roomEvents.erase(discard);
			}
			_roomEvents.push_back(event.event);
			// MatchEnded is only an outcome notification. The application queues
			// AcknowledgeTerminal after profile persistence and native/helper
			// retirement; spectators and fighters therefore cannot release a
			// durable receipt merely by receiving this event.
			if (_callbacks.OnRoomEvent) _callbacks.OnRoomEvent(this, event.event, _callbacks);
		}
		else if (type == SessionProtocol::MT_GAME_PREPARE || type == SessionProtocol::MT_GAME_CONNECT ||
			type == SessionProtocol::MT_GAME_START || type == SessionProtocol::MT_GAME_END ||
			type == SessionProtocol::MT_GAME_PEER_END) {
			if (!_matchAuthorizationRequired) return -1;
			if (_gameplayMessages.size() >= 32 || message.payload.size() > 16384) return -1;
			if (type == SessionProtocol::MT_GAME_PREPARE) {
				_projectionFrozen = true;
				const auto incomingGeneration = msg.value("generation", std::uint64_t(0));
				if (incomingGeneration != _queuedGrantGeneration) _queuedGrantProjection.reset();
				_queuedGrantGeneration = incomingGeneration;
				if (_queuedGrantGeneration && _pendingRoomProjection &&
					_pendingRoomProjection->matchGeneration == _queuedGrantGeneration) {
					_queuedGrantProjection = std::move(_pendingRoomProjection);
					_pendingRoomProjection.reset();
				} else if (_queuedGrantGeneration &&
					_appliedRoomProjectionGeneration == _queuedGrantGeneration) {
					// The normal direct path applied the projection before the
					// prepare message. Capture that exact state so a later update
					// cannot replace the grant's native inputs.
					SessionProtocol::SessionDataUpdate captured;
					captured.lobbyData = _lobbyData;
					captured.matchData = _matchData;
					captured.matchGeneration = _queuedGrantGeneration;
					_queuedGrantProjection = std::move(captured);
				}
			}
			_gameplayMessages.push_back(std::move(msg));
		}
		else if (type == SessionProtocol::MT_SESSION_HELLO_RESP) {
			SessionProtocol::SessionHelloResp cidMsg;
			try {
				msg.get_to(cidMsg);
			}
			catch (const json::exception&) {
				spdlog::info("Client: couldn't deserialize CID?");
				continue;
			}

			_cid = cidMsg.cid;
			// A nonzero room member proves the recovery bootstrap hello already
			// committed this custom-room admission. Starting the legacy join_req
			// retry loop here can enqueue duplicate full checkpoints while the
			// accompanying room projection is still crossing a relay.
			const bool bootstrapAdmitted = _customRoomsRequired && cidMsg.roomMember != 0;
			_joinRequestPending = !bootstrapAdmitted;
			if (_joinRequestPending) {
				_joinRequestNextStep = _stepCounter;
				TrySendPendingJoinRequest();
			}
		}
		else if (type == SessionProtocol::MT_SESSION_JOINREJ) {
			_joinRequestPending = false;
			SessionProtocol::SessionJoinReject reject;
			try {
				msg.get_to(reject);
			}
			catch (const json::exception&) {
				spdlog::info("Client: couldn't deserialize join rejection?");
				continue;
			}

			spdlog::info("Join rejected, reason: {}", (int)reject.result);
			ErrorType errType = ErrorType::SCE_UNKNOWN;
			switch (reject.result) {
			case SessionProtocol::JoinResult::JR_HASH_INVALID:
				errType = ErrorType::SCE_JOIN_REJECTED_HASH_INVALID;
				break;
			case SessionProtocol::JoinResult::JR_LOBBY_FULL:
				errType = ErrorType::SCE_JOIN_REJECTED_LOBBY_FULL;
				break;
			case SessionProtocol::JoinResult::JR_NAME_TAKEN:
				errType = ErrorType::SCE_JOIN_REJECTED_NAME_TAKEN;
				break;
			case SessionProtocol::JoinResult::JR_REQUEST_INVALID:
				errType = ErrorType::SCE_JOIN_REJECTED_REQUEST_INVALID;
				break;
			default:
				break;
			}
			_callbacks.OnError(errType, this, _callbacks);
			Disconnect();
			return -1;
		}
		else if (type == SessionProtocol::MT_SESSION_DATAUPDATE) {
			SessionProtocol::SessionDataUpdate update;
			try {
				msg.get_to(update);
			}
			catch (const json::exception&) {
				spdlog::info("Client: could not deserialize response");
				continue;
			}
			if (_joinRequestPending && !_customRoomsRequired &&
				std::any_of(update.lobbyData.members.begin(), update.lobbyData.members.end(),
					[&](const SessionProtocol::MemberData& member) { return member.connId == _cid; }))
				_joinRequestPending = false;
			if (!_customRoomsSeen || !_projectionFrozen) {
				_lobbyData = update.lobbyData;
				_matchData = update.matchData;
				_appliedRoomProjectionGeneration = update.matchGeneration;
			} else if (_queuedGrantGeneration && update.matchGeneration == _queuedGrantGeneration &&
				!_queuedGrantProjection) {
				// If transport delivery puts the projection after game_prepare,
				// capture the first matching update as that grant's immutable input.
				_queuedGrantProjection = std::move(update);
			} else if (!_pendingRoomProjection || update.matchGeneration >= _pendingRoomProjection->matchGeneration) {
				// Keep at most one generation-matched native projection while the
				// prior GGPO roster is frozen. The next accepted grant selects it.
				_pendingRoomProjection = std::move(update);
			}

			if (_outstandingReadyRequestNumber > -1) {
				for (int i = 0; i < _lobbyData.members.size() && i < 2; i++) {
					if (_lobbyData.members[i].name == _name) {
						if (_matchData.readyMessageNum[i] == _outstandingReadyRequestNumber) {
							// This contains the ready data, so there's no longer an outstanding request.
							_outstandingReadyRequestNumber = -1;
						}
						break;
					}
				}
			}
		}
		else if (type == SessionProtocol::MT_LOBBY_ALLREADY) {
			if (!_matchAuthorizationRequired && _callbacks.OnReady) _callbacks.OnReady(this, _callbacks);
		}
		else if (type == SessionProtocol::MT_BATTLE_SYNCED) {
			_callbacks.OnBattleSynced(this, _callbacks);
		}
		else if (type == SessionProtocol::MT_BATTLE_SNAPSHOT) {
			SessionProtocol::BattleSnapshot m;
			try {
				msg.get_to(m);
			}
			catch (const json::exception&) {
				spdlog::info("Client: could not deserialize incoming checksum msg");
				continue;
			}

			auto localSnapshotIter = fSystem::snapshotMap.find(m.snapshot.frameIdx);
			if (localSnapshotIter != fSystem::snapshotMap.end()) {
				// This client is ahead and already has a snapshot for this frame.
				// Compare it.
				SessionProtocol::StateSnapshot& localSnapshot = localSnapshotIter->second.first;
				if (bVerboseLogging) {
					spdlog::error("Client: snapshot receipt: valid snapshot @ frame {} on receipt, confirm {}, sent {}", localSnapshot.frameIdx, localSnapshotIter->second.second.confirmed, localSnapshotIter->second.second.sent);
				}
				if (memcmp(&m.snapshot, &localSnapshot, sizeof(SessionProtocol::StateSnapshot)) != 0) {
					spdlog::error("Client: snapshot receipt: Desync detected!");
					ReportSnapshotDivergence("receipt", localSnapshot, m.snapshot);
					TerminateOnDesync("receipt", localSnapshot.frameIdx);
				}

				if (bVerboseLogging) {
					spdlog::error("    - Client: snapshot receipt: valid snapshot @ frame {} on receipt confirmed", localSnapshot.frameIdx);
				}
				localSnapshotIter->second.second.confirmed = true;
				if (localSnapshotIter->second.second.confirmed && localSnapshotIter->second.second.sent) {

					if (bVerboseLogging) {
						spdlog::error("Client: snapshot receipt: erasing local snapshot @ frame {} on receipt due to confirmation+sent", m.snapshot.frameIdx);
					}
					fSystem::snapshotMap.erase(localSnapshotIter);
				}
			}
			else {
				// Opponent's ahead- can't compare yet.
				if (bVerboseLogging) {
					spdlog::error("Client: snapshot receipt: pendingRemoteSnapshots.emplace({})", m.snapshot.frameIdx);
				}
				pendingRemoteSnapshots.emplace(m.snapshot.frameIdx, m.snapshot);
			}
		}
		else if (type == SessionProtocol::MT_BATTLE_HASH) {
			SessionProtocol::BattleHashV2 m;
			try {
				msg.get_to(m);
			}
			catch (json::exception&) {
				spdlog::info("Client: could not deserialize v2 hash msg");
				continue;
			}
			// Always buffer; the aged reconcile pass below compares only
			// once the LOCAL checkpoint is also non-speculative (a local
			// value inside the rollback window could still change).
			if (pendingRemoteHashes.size() >= MAX_PENDING_REMOTE_HASHES) {
				pendingRemoteHashes.erase(pendingRemoteHashes.begin());
			}
			pendingRemoteHashes[m.frameIdx] = m;
		}


		else if (type == SessionProtocol::MT_FORWARD) {
			spdlog::debug("Received forwarded message: {}", msg.dump());
		}
		else {
			spdlog::warn("Client: got unrecognized message type: {}", (int)type);
		}
	}

	// Send all our outstanding local snapshots and compare any to pending
	// snapshots.
	if (_snapshotsEnabled) {
		int mostRecentPredictiveFrame = (
			rSystem::GetNumFramesSimulated_FixedPoint(rSystem::staticMethods.GetSingleton())->integral
		);
		auto localSnapshotIter = fSystem::snapshotMap.begin();
		while (localSnapshotIter != fSystem::snapshotMap.end()) {
			if (mostRecentPredictiveFrame - localSnapshotIter->first < 60) {
				// Snapshot not yet old enough.
				localSnapshotIter++;
				continue;
			}

			if (bVerboseLogging) {
				spdlog::error("Client: snapshot reconciliation: checking snapshot @ {} due to mostRecentPredictiveFrame {}", localSnapshotIter->first, mostRecentPredictiveFrame);
			}

			if (!localSnapshotIter->second.second.sent) {
				if (bVerboseLogging) {
					spdlog::error("Client: snapshot reconciliation: snapshot @ {} not yet sent, confirmed val: {}", localSnapshotIter->first, localSnapshotIter->second.second.confirmed);
				}
				// Snapshot not yet sent. Send it. Only a player's snapshot is
				// authoritative; a spectator compares but never publishes, the
				// same rule the v2 hash exchange already follows.
				localSnapshotIter->second.second.sent = true;
				if (IsLocalPlayer()) {
					SessionProtocol::BattleSnapshot m;
					m.snapshot = localSnapshotIter->second.first;
					json msg = m;
					if (Send(msg, nullptr) != session::SendResult::Queued) {
						spdlog::error("Client: Could not send snapshot update");
					}
				}
			}

			if (!localSnapshotIter->second.second.confirmed) {
				if (bVerboseLogging) {
					spdlog::error("Client: snapshot reconciliation: snapshot @ {} not yet confirmed, sent val: {}", localSnapshotIter->first, localSnapshotIter->second.second.sent);
				}
				auto remoteSnapshotIter = pendingRemoteSnapshots.find(localSnapshotIter->first);
				if (remoteSnapshotIter != pendingRemoteSnapshots.end()) {
					if (bVerboseLogging) {
						spdlog::error("   - Client: snapshot reconciliation: snapshot @ {} got candidate remote snapshot", localSnapshotIter->first);
					}
					// Caught up to the opponent- compare to a snapshot already sent by the opponent.
					SessionProtocol::StateSnapshot& localSnapshot = localSnapshotIter->second.first;
					if (memcmp(&remoteSnapshotIter->second, &localSnapshot, sizeof(SessionProtocol::StateSnapshot)) != 0) {
						spdlog::error("Client: snapshot reconciliation: Desync detected from pending!");
						ReportSnapshotDivergence("reconciliation", localSnapshot, remoteSnapshotIter->second);
						TerminateOnDesync("reconciliation", localSnapshot.frameIdx);
					}
					localSnapshotIter->second.second.confirmed = true;
				}
			}

			if (localSnapshotIter->second.second.confirmed && localSnapshotIter->second.second.sent) {
				if (bVerboseLogging) {
					spdlog::error("   - Client: snapshot reconciliation: snapshot @ {} erased due to being confirmed and sent", localSnapshotIter->first);
				}
				localSnapshotIter = fSystem::snapshotMap.erase(localSnapshotIter);
			}
			else {
				localSnapshotIter++;
			}
		}

		// Read-only game-thread query. No polling or pacing changes here.
		// Only players send; spectators compare the confirmed source stream.
		{
			const bool isPlayer = IsLocalPlayer();
			int confirmedInput = -1;
			if (fSystem::ggpo) ggpo_get_last_confirmed_frame(fSystem::ggpo, &confirmedInput);
			for (int i = 0; i < fSystem::NUM_HASH_CHECKPOINTS; i++) {
				fSystem::HashCheckpoint& cp = fSystem::hashCheckpoints[i];
				if (!cp.valid) {
					continue;
				}
				if (!sf4e::statehash::IsConfirmedCheckpoint(cp.ggpoStateFrame, confirmedInput)) {
					continue;
				}
				if (!cp.sent && isPlayer) {
					SessionProtocol::BattleHashV2 m;
					m.frameIdx = cp.frameIdx;
					m.overall = cp.hashes.overall;
					m.flow = cp.hashes.flow;
					m.chara0 = cp.hashes.chara[0];
					m.chara1 = cp.hashes.chara[1];
					m.fromPlayer = true;
					json hashMsg = m;
					if (Send(hashMsg, nullptr) == session::SendResult::Queued) {
						cp.sent = true;
					}
				}
				auto pendingIter = pendingRemoteHashes.find(cp.frameIdx);
				if (pendingIter != pendingRemoteHashes.end()) {
					ReportHashMismatch(this, cp, pendingIter->second);
					pendingRemoteHashes.erase(pendingIter);
				}
			}
		}
	}
	TrySendPendingJoinRequest();
	return 0;
}

session::SendResult SessionClient::Send(nlohmann::json& msg, int64_t* outMessageNum) {
	if (!_transport) return session::SendResult::NotConnected;
	return _transport->Send(msg.dump(), true, outMessageNum);
}

session::SendResult SessionClient::Lobby_Ready()
{
	if (_customRoomsSeen) {
		return SendRoomAction(TableAction(room::ActionKind::Ready, _selectedRoomTable, _selectedDelay));
	}
	LobbyReady msg;
	json j = msg;
	session::SendResult result = Send(j, &_outstandingReadyRequestNumber);
	if (result != session::SendResult::Queued) {
		spdlog::warn("Client: could not send ready! Result: {}", (int)result);
	}
	return result;
}

session::SendResult SessionClient::Lobby_ReportResults(int loserSide)
{
	if (_customRoomsSeen) {
		room::Action action;
		action.kind = room::ActionKind::RecordResult;
		action.roomEpoch = _roomSnapshot.roomEpoch;
		action.table = _selectedRoomTable;
		action.matchGeneration = _roomSnapshot.tables[_selectedRoomTable].matchGeneration;
		action.result = loserSide == 0 ? room::MatchResult::P2Win : room::MatchResult::P1Win;
		return SendRoomAction(action);
	}
	SessionProtocol::LobbyReportResults r;
	r.loserSide = loserSide;
	json msg = r;
	if (_matchAuthorizationRequired) msg["generation"] = _gameplayGeneration;
	session::SendResult result = Send(msg, nullptr);
	if (result != session::SendResult::Queued) {
		spdlog::warn("Client: could not report results! Result: {}", (int)result);
	}
	return result;
}

session::SendResult SessionClient::Lobby_ResetRematch()
{
	if (_customRoomsSeen) {
		return SendRoomAction(TableAction(room::ActionKind::Unready, _selectedRoomTable, room::Action{}.inputDelay));
	}
	SessionProtocol::LobbyReset msg;
	json j = msg;
	if (_matchAuthorizationRequired) j["generation"] = _gameplayGeneration;
	session::SendResult result = Send(j, nullptr);
	if (result != session::SendResult::Queued) {
		spdlog::warn("Client: could not reset lobby for rematch! Result: {}", (int)result);
	}
	return result;
}

session::SendResult SessionClient::Lobby_SetSettings(
	bool editionSelect,
	int roundCount,
	Dimps::Math::FixedPoint roundTime,
	bool trainingMode
)
{
	if (_customRoomsSeen) {
		room::Action action;
		action.kind = room::ActionKind::SetRules;
		action.roomEpoch = _roomSnapshot.roomEpoch;
		action.revision = _roomSnapshot.revision;
		action.table = _selectedRoomTable;
		action.tableRevision = _roomSnapshot.tables[_selectedRoomTable].revision;
		action.rules = _roomSnapshot.tables[_selectedRoomTable].rules;
		action.rules.editionSelect = editionSelect;
		action.rules.roundCount = static_cast<std::uint8_t>(roundCount);
		action.rules.roundTime = static_cast<std::uint16_t>(roundTime.integral);
		return SendRoomAction(action);
	}
	SessionProtocol::LobbySetSettings msg;
	msg.editionSelect = editionSelect;
	msg.roundCount = roundCount;
	msg.roundTime = roundTime;
	msg.trainingMode = trainingMode;
	json j = msg;
	session::SendResult result = Send(j, nullptr);
	if (result != session::SendResult::Queued) {
		spdlog::warn("Client: could not send lobby settings! Result: {}", (int)result);
	}
	return result;
}

session::SendResult SessionClient::PreBattle_SetEnv(uint32_t rngSeed)
{
	SessionProtocol::PreBattleSetEnv msg;
	msg.rngSeed = rngSeed;
	json j = msg;
	session::SendResult result = Send(j, nullptr);
	if (result != session::SendResult::Queued) {
		spdlog::warn("Client: could not set prebattle environment! Result: {}", (int)result);
	}
	return result;
}

session::SendResult SessionClient::PreBattle_SetChara(const Dimps::GameEvents::VsMode::ConfirmedCharaConditions& chara)
{
	if (!sf4e::selection::Valid(sf4e::selection::FromNative(chara), _lobbyData.editionSelect)) return session::SendResult::InvalidPayload;
	SessionProtocol::PreBattleSetChara msg;
	msg.chara = chara;
	json j = msg;
	session::SendResult result = Send(j, nullptr);
	if (result != session::SendResult::Queued) {
		spdlog::warn("Client: could not set prebattle character! Result: {}", (int)result);
	}
	return result;
}

session::SendResult SessionClient::PreBattle_SetStage(int32_t stageID)
{
	if (!selection::FindStage(stageID)) return session::SendResult::InvalidPayload;
	SessionProtocol::PreBattleSetStage msg;
	msg.stageID = stageID;
	json j = msg;
	session::SendResult result = Send(j, nullptr);
	if (result != session::SendResult::Queued) {
		spdlog::warn("Client: could not set prebattle stage! Result: {}", (int)result);
	}
	return result;
}

session::SendResult SessionClient::Battle_Loaded()
{
	SessionProtocol::BattleLoaded msg;
	json j = msg;
	session::SendResult result = Send(j, nullptr);
	if (result != session::SendResult::Queued) {
		spdlog::warn("Client: could not set battle loaded! Result: {}", (int)result);
	}
	return result;
}
