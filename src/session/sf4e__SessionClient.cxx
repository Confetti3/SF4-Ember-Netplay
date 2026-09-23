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
	_joinRejection.reset();
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
	_resultRetry = {}; _finishRetry = {};
    _actionReplies.clear();
	_projectionFrozen = false;
	_snapshotSendFailing = false;
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

bool SessionClient::TakeGameplayMessage(json& message) {
	if (_gameplayMessages.empty()) return false;
	message = std::move(_gameplayMessages.front()); _gameplayMessages.pop_front(); return true;
}

void SessionClient::PrepareForCallbacks()
{
	// Callback ownership belongs to each transport, never a global client.
}

bool SessionClient::HandleGameplayMessage(json& msg, const session::Message& message, SessionProtocol::MessageType type) {
	if (!_matchAuthorizationRequired) return false;
	if (_gameplayMessages.size() >= 32 || message.payload.size() > 16384) return false;
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
	return true;
}

bool SessionClient::HandleHelloResponse(json& msg) {
	SessionProtocol::SessionHelloResp cidMsg;
	try {
		msg.get_to(cidMsg);
	}
	catch (const json::exception&) {
		spdlog::info("Client: couldn't deserialize CID?");
		return true;
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
	return true;
}

bool SessionClient::HandleJoinReject(json& msg) {
	_joinRequestPending = false;
	SessionProtocol::SessionJoinReject reject;
	try {
		msg.get_to(reject);
	}
	catch (const json::exception&) {
		spdlog::info("Client: couldn't deserialize join rejection?");
		return true;
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
	_joinRejection = errType;
	_callbacks.OnError(errType, this, _callbacks);
	Disconnect();
	return false;
}

bool SessionClient::HandleDataUpdate(json& msg) {
	SessionProtocol::SessionDataUpdate update;
	try {
		msg.get_to(update);
	}
	catch (const json::exception&) {
		spdlog::info("Client: could not deserialize response");
		return true;
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
	return true;
}

bool SessionClient::HandleBattleSnapshot(json& msg) {
	SessionProtocol::BattleSnapshot m;
	try {
		msg.get_to(m);
	}
	catch (const json::exception&) {
		spdlog::info("Client: could not deserialize incoming checksum msg");
		return true;
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
	return true;
}

bool SessionClient::HandleBattleHash(json& msg) {
	SessionProtocol::BattleHashV2 m;
	try {
		msg.get_to(m);
	}
	catch (json::exception&) {
		spdlog::info("Client: could not deserialize v2 hash msg");
		return true;
	}
	// Always buffer; the aged reconcile pass below compares only
	// once the LOCAL checkpoint is also non-speculative (a local
	// value inside the rollback window could still change).
	if (pendingRemoteHashes.size() >= MAX_PENDING_REMOTE_HASHES) {
		pendingRemoteHashes.erase(pendingRemoteHashes.begin());
	}
	pendingRemoteHashes[m.frameIdx] = m;
	return true;
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
			if (!HandleRoomSnapshot(msg)) return -1;
		}
		else if (type == SessionProtocol::MT_ROOM_RESULT) {
			if (!HandleRoomResult(msg)) return -1;
		}
		else if (type == SessionProtocol::MT_ROOM_EVENT) {
			if (!HandleRoomEvent(msg)) return -1;
		}
		else if (type == SessionProtocol::MT_GAME_PREPARE || type == SessionProtocol::MT_GAME_CONNECT ||
			type == SessionProtocol::MT_GAME_START || type == SessionProtocol::MT_GAME_END ||
			type == SessionProtocol::MT_GAME_PEER_END) {
			if (!HandleGameplayMessage(msg, message, type)) return -1;
		}
		else if (type == SessionProtocol::MT_SESSION_HELLO_RESP) {
			if (!HandleHelloResponse(msg)) return -1;
		}
		else if (type == SessionProtocol::MT_SESSION_JOINREJ) {
			if (!HandleJoinReject(msg)) return -1;
		}
		else if (type == SessionProtocol::MT_SESSION_DATAUPDATE) {
			if (!HandleDataUpdate(msg)) return -1;
		}
		else if (type == SessionProtocol::MT_LOBBY_ALLREADY) {
			if (!_matchAuthorizationRequired && _callbacks.OnReady) _callbacks.OnReady(this, _callbacks);
		}
		else if (type == SessionProtocol::MT_BATTLE_SYNCED) {
			_callbacks.OnBattleSynced(this, _callbacks);
		}
		else if (type == SessionProtocol::MT_BATTLE_SNAPSHOT) {
			if (!HandleBattleSnapshot(msg)) return -1;
		}
		else if (type == SessionProtocol::MT_BATTLE_HASH) {
			if (!HandleBattleHash(msg)) return -1;
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
					const bool failed = Send(msg, nullptr) != session::SendResult::Queued;
					if (failed != _snapshotSendFailing)
						spdlog::log(failed ? spdlog::level::err : spdlog::level::info, failed ?
							"Client: Could not send snapshot update; further failures are not logged until one succeeds" :
							"Client: Snapshot updates are sending again");
					_snapshotSendFailing = failed;
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
