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

const char* SessionClient::JoinRejectionKey(ErrorType type) {
	switch (type) {
	case SCE_JOIN_REJECTED_HASH_INVALID: return "runtime.build_mismatch";
	case SCE_JOIN_REJECTED_LOBBY_FULL: return "runtime.room_full";
	case SCE_JOIN_REJECTED_NAME_TAKEN: return "room.reject.name_taken";
	default: return "runtime.room_request_failed";
	}
}

// A public room's host locks the room by refusing as full, and removes or
// turns away a player by an invalid request: both say so in the room's own words.
const char* SessionClient::PublicJoinRejectionKey(ErrorType type) {
	switch (type) {
	case SCE_JOIN_REJECTED_LOBBY_FULL: return "public.failure.full_or_locked";
	case SCE_JOIN_REJECTED_REQUEST_INVALID: return "public.failure.turned_away";
	default: return JoinRejectionKey(type);
	}
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
	_roomSnapshotSequence = NextRoomSnapshotSequence();
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
        if(_customRoomsRequired) hello["admission"]=BuildJoinRequest();
        _transport->BeginAdmission();
        const auto sent=Send(hello,nullptr);
        if(sent==session::SendResult::NotConnected || sent==session::SendResult::QueueFull) return 0;
        if(sent!=session::SendResult::Queued) return -1;
		_helloPending = false;
	}
	++_stepCounter;
	PumpTerminalAcks();
	for (const auto& message : incoming) {
		json msg;
		// A room transport hands over the payload it already parsed to
		// authorize it; it equals json::parse(message.payload).
		if (message.body) msg = std::move(*message.body);
		else {
			try {
				msg = json::parse(message.payload);
			}
			catch (const json::exception&) {
				continue;
			}
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

	if (_snapshotsEnabled) StepBattleVerification();
	TrySendPendingJoinRequest();
	return 0;
}

session::SendResult SessionClient::Send(nlohmann::json& msg, int64_t* outMessageNum) {
	if (!_transport) return session::SendResult::NotConnected;
	return _transport->Send(msg.dump(), true, outMessageNum);
}

// The legacy lobby and pre-battle messages: sent, and a refused send logged
// as "could not <what>".
session::SendResult SessionClient::SendLogged(nlohmann::json& msg, const char* what, int64_t* outMessageNum) {
	session::SendResult result = Send(msg, outMessageNum);
	if (result != session::SendResult::Queued) {
		spdlog::warn("Client: could not {}! Result: {}", what, (int)result);
	}
	return result;
}

session::SendResult SessionClient::Lobby_Ready()
{
	if (_customRoomsSeen) {
		auto ready = TableAction(room::ActionKind::Ready, _selectedRoomTable, _selectedDelay);
		ready.lowerLag = _selectedLowerLag;
		return SendRoomAction(ready);
	}
	LobbyReady msg;
	json j = msg;
	return SendLogged(j, "send ready", &_outstandingReadyRequestNumber);
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
	return SendLogged(msg, "report results");
}

session::SendResult SessionClient::Lobby_ResetRematch()
{
	SessionProtocol::LobbyReset msg;
	json j = msg;
	if (_matchAuthorizationRequired) j["generation"] = _gameplayGeneration;
	return SendLogged(j, "reset lobby for rematch");
}

session::SendResult SessionClient::Lobby_SetSettings(
	bool editionSelect,
	int roundCount,
	Dimps::Math::FixedPoint roundTime
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
	json j = msg;
	return SendLogged(j, "send lobby settings");
}

session::SendResult SessionClient::PreBattle_SetEnv(uint32_t rngSeed)
{
	SessionProtocol::PreBattleSetEnv msg;
	msg.rngSeed = rngSeed;
	json j = msg;
	return SendLogged(j, "set prebattle environment");
}

session::SendResult SessionClient::PreBattle_SetChara(const Dimps::GameEvents::VsMode::ConfirmedCharaConditions& chara)
{
	if (!sf4e::selection::Valid(sf4e::selection::FromNative(chara), _lobbyData.editionSelect)) return session::SendResult::InvalidPayload;
	SessionProtocol::PreBattleSetChara msg;
	msg.chara = chara;
	json j = msg;
	return SendLogged(j, "set prebattle character");
}

session::SendResult SessionClient::PreBattle_SetStage(int32_t stageID)
{
	if (!selection::ValidStage(stageID)) return session::SendResult::InvalidPayload;
	SessionProtocol::PreBattleSetStage msg;
	msg.stageID = stageID;
	json j = msg;
	return SendLogged(j, "set prebattle stage");
}

session::SendResult SessionClient::Battle_Loaded()
{
	SessionProtocol::BattleLoaded msg;
	json j = msg;
	return SendLogged(j, "set battle loaded");
}
