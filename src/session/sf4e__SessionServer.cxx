#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>

#include "sf4e__SessionProtocol.hxx"
#include "sf4e__SessionServer.hxx"

using nlohmann::json;

namespace SessionProtocol = sf4e::SessionProtocol;
using Dimps::Math::FixedPoint;
using sf4e::SessionServer;


const int sf4e::SESSION_SERVER_MAX_MESSAGES_PER_POLL = 200;
SessionServer* SessionServer::s_pCallbackInstance;

SessionServer::SessionServer(std::string identity, std::string sidecarHash, bool editionSelect, int roundCount, FixedPoint roundTime) :
	_identity(identity),
	_sidecarHash(sidecarHash),
	_interface(SteamNetworkingSockets()),
	_dataDirty(false),
	_lobbyData(SessionProtocol::LobbyData::NULL_LOBBY),
	_listenSock(k_HSteamListenSocket_Invalid)
{
	s_pCallbackInstance = this;
	_lobbyData.id = { _identity, "1" };
	_lobbyData.editionSelect = editionSelect;
	_lobbyData.roundCount = roundCount;
	_lobbyData.roundTime = roundTime;
	clients.reserve(MAX_SF4E_PROTOCOL_USERS + 1);
	_pollGroup = _interface->CreatePollGroup();
}

SessionServer::~SessionServer()
{
	if (_listenSock != k_HSteamListenSocket_Invalid) {
		Close();
		_listenSock = k_HSteamListenSocket_Invalid;
	}
}

void SessionServer::AddConnection(HSteamNetConnection newConn) {
	// XXX (adanducci): It is absolutely critical to note that
	// `SetConfigValue`'s interface to set callbacks is _not_ the
	// same as the one used by `CreateListenSocketIP`/`SteamNetworkingConfigValue_t`.
	// 
	// Per the documentation for `SetConfigValue` and the header comment @
	// https://github.com/ValveSoftware/GameNetworkingSockets/blob/62b395172f157ca4f01eea3387d1131400f8d604/include/steam/isteamnetworkingutils.h#L296-L307 :
	//
	// NOTE: When setting pointers (e.g. callback functions), do not pass the function pointer
	// directly. Your argument should be a pointer to a function pointer.
	//
	// `CreateListenSocketIP`/`SteamNetworkingConfigValue_t` just takes the
	// function pointer directly. The failure mode if you pass the function
	// pointer directly is _extremely_ confusing- it just appears to be
	// a segfault in the GNS callback loop.
	void (*callback)(SteamNetConnectionStatusChangedCallback_t*) = SteamNetConnectionStatusChangedCallback;
	SteamNetworkingUtils()->SetConfigValue(
		k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
		k_ESteamNetworkingConfig_Connection,
		newConn,
		k_ESteamNetworkingConfig_Ptr,
		&callback
	);
	_interface->SetConnectionPollGroup(newConn, _pollGroup);
}

int SessionServer::Listen(uint16 nPort) {
	SteamNetworkingIPAddr serverLocalAddr;
	serverLocalAddr.Clear();
	serverLocalAddr.m_port = nPort;
	SteamNetworkingConfigValue_t opt;
	opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)SteamNetConnectionStatusChangedCallback);
	_listenSock = _interface->CreateListenSocketIP(serverLocalAddr, 1, &opt);
	if (_listenSock == k_HSteamListenSocket_Invalid) {
		spdlog::error("Failed to listen on port {}", nPort);
		return -1;
	}
	spdlog::info("Server listening on port {}", nPort);
	return 0;
}

int SessionServer::Step()
{
	ISteamNetworkingMessage* pIncomingMsgs[SESSION_SERVER_MAX_MESSAGES_PER_POLL] = { 0 };
	int numMsgs = _interface->ReceiveMessagesOnPollGroup(_pollGroup, pIncomingMsgs, SESSION_SERVER_MAX_MESSAGES_PER_POLL);
	bool bSendLobbyAllReady = false;
	bool bSendBattleSynced = false;

	if (numMsgs < 0) {
		spdlog::error("Session server error checking for messages: {}", numMsgs);
		return -1;
	}

	for (int i = 0; i < numMsgs; i++) {
		ISteamNetworkingMessage* pIncomingMsg = pIncomingMsgs[i];
		if (!pIncomingMsg) {
			spdlog::error("Client: incoming message enumerated, but not data retrieved");
			return -1;
		}

		HSteamNetConnection conn = pIncomingMsg->m_conn;
		const char* start = (const char*)pIncomingMsg->m_pData;
		json msg;

		try {
			msg = json::parse(start, start + pIncomingMsg->m_cbSize);
		}
		catch (json::exception e) {
			spdlog::info("Server: got a non-json message");
			continue;
		}

		SessionProtocol::MessageType type;
		try {
			msg.at("type").get_to(type);
		}
		catch (json::exception e) {
			spdlog::info("Server: got a message without a type, or a type that was not a string");
			continue;
		}

		if (cidMap.find(conn) == cidMap.end()) {
			if (type == SessionProtocol::MT_SESSION_HELLO) {
				SessionProtocol::SessionHelloResp cidMsg;
				SessionProtocol::ConnectionID newCid;
				cidMsg.cid.host = _identity;
				cidMsg.cid.user = std::to_string(conn);
				cidMap[conn] = cidMsg.cid;
				Respond(conn, json(cidMsg));
			}
			else {
				spdlog::warn("Server: got unrecognized message type: {}", (int)type);
			}
		}
		else {
			SessionProtocol::ConnectionID cid = cidMap[conn];
			if (type == SessionProtocol::MT_FORWARD) {
				SessionProtocol::ForwardMessage fwdMsg;
				try {
					msg.get_to(fwdMsg);
				}
				catch (json::exception e) {
					spdlog::debug("Server: could not deserialize forwarding message");
					continue;
				}

				// If this is a connection ID managed by this server, we can apply
				// additional security- messages with this source address should
				// only be coming from the connection that the address is assigned
				// to.
				if (fwdMsg.src.host == _identity) {
					if (fwdMsg.src.user != std::to_string(conn)) {
						spdlog::debug("Server: dropping fraudulent packet; {} masqueraded as {}", conn, fwdMsg.src.user);
					}
				}

				if (fwdMsg.dest.host != _identity) {
					spdlog::info("Server: cannot forward to nonlocal identity {}@{}, clustering not yet implemented", fwdMsg.dest.user, fwdMsg.dest.host);
				}

				// Check that the destination is in fact the lobby
				auto clientIter = clients.begin();
				for (; clientIter != clients.end(); clientIter++) {
					if (clientIter->data.connId == fwdMsg.dest) {
						break;
					}
				}
				if (clientIter != clients.end()) {
					Respond(clientIter->conn, msg);
				}
				else {
					spdlog::debug("Server: Could not forward to {}@{}: not in known lobby", fwdMsg.dest.user, fwdMsg.dest.host);
				}
			}
			else if (type == SessionProtocol::MT_SESSION_JOINREQ) {
				SessionProtocol::SessionJoinRequest request;
				try {
					msg.get_to(request);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize join request");
					SessionProtocol::SessionJoinReject reject;
					reject.result = SessionProtocol::JR_REQUEST_INVALID;
					json rejectMsg = reject;
					Respond(conn, rejectMsg);
					continue;
				}

				SteamNetworkingIPAddr peerAddr = *(pIncomingMsg->m_identityPeer.GetIPAddr());
				SessionProtocol::JoinResult joinResult = RegisterToWait(conn, request.port, request.sidecarHash, request.username, peerAddr, cid);
				if (joinResult != SessionProtocol::JOIN_OK) {
					spdlog::info("Server: rejecting registration for reason {}", (int)joinResult);
					SessionProtocol::SessionJoinReject reject;
					reject.result = joinResult;
					json rejectMsg = reject;
					Respond(conn, rejectMsg);
					continue;
				}

				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_PREBATTLE_SETCHARA) {
				int side = -1;
				for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side == -1) {
					spdlog::info("Server: sender {} tried to set chara, but is not playing", conn);
					continue;
				}

				SessionProtocol::PreBattleSetChara request;
				try {
					msg.get_to(request);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize SetConditionsRequest");
					continue;
				}
				_matchData.chara[side] = request.chara;
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_PREBATTLE_SETENV) {
				int side = -1;
				for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side != 0) {
					spdlog::info("Server: sender {} tried to set env, but is not P1", conn);
					continue;
				}
				SessionProtocol::PreBattleSetEnv request;
				try {
					msg.get_to(request);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize SetConditionsRequest");
					continue;
				}

				_matchData.rngSeed = request.rngSeed;
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_PREBATTLE_SETSTAGE) {
				int side = -1;
				for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side != 0) {
					spdlog::info("Server: sender {} tried to set stage, but is not P1", conn);
					continue;
				}
				SessionProtocol::PreBattleSetStage request;
				try {
					msg.get_to(request);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize SetConditionsRequest");
					continue;
				}

				_matchData.stageID = request.stageID;
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_LOBBY_SETSETTINGS) {
				int side = -1;
				for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side != 0) {
					spdlog::info("Server: sender {} tried to set lobby settings, but is not P1", conn);
					continue;
				}
				SessionProtocol::LobbySetSettings request;
				try {
					msg.get_to(request);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize LobbySetSettings");
					continue;
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
			else if (type == SessionProtocol::MT_BATTLE_LOADED) {
				bSendBattleSynced = true;
				for (int i = 0; i < clients.size(); i++) {
					if (clients.at(i).conn == conn) {
						clients.at(i).data.flags |= SessionProtocol::MF_BATTLE_LOADED;
					}
					bSendBattleSynced = bSendBattleSynced && (clients.at(i).data.flags & SessionProtocol::MF_BATTLE_LOADED);
				}
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_LOBBY_READY) {
				int side = -1;
				for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side == -1) {
					spdlog::info("Server: sender {} tried to ready, but is not playing", conn);
					continue;
				}

				SessionProtocol::LobbyReady request;
				try {
					msg.get_to(request);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize ReportResultsRequest");
					continue;
				}
				_matchData.readyMessageNum[side] = pIncomingMsg->GetMessageNumber();
				bSendLobbyAllReady = bSendLobbyAllReady || _matchData.IsAllReady();
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_LOBBY_REPORTRESULTS) {
				SessionProtocol::LobbyReportResults request;
				try {
					msg.get_to(request);
				}
				catch (json::exception e) {
					spdlog::info("Server: could not deserialize ReportResultsRequest");
					continue;
				}

				HandleResults(request.loserSide);
				_dataDirty = true;
			}
			else if (type == SessionProtocol::MT_LOBBY_RESET) {
				int side = -1;
				for (int i = 0; i < 2; i++) {
					if (clients.size() > i && clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side == -1) {
					spdlog::info("Server: sender {} tried to reset lobby, but is not playing", conn);
					continue;
				}
				ResetLobbyForRematch();
			}
			else if (type == SessionProtocol::MT_BATTLE_SNAPSHOT) {
				// Forward the snapshot to every other client.
				for (auto clientIter = clients.begin(); clientIter != clients.end(); clientIter++) {
					if (clientIter->conn != conn) {
						_interface->SendMessageToConnection(
							clientIter->conn, (const char*)pIncomingMsg->m_pData, pIncomingMsg->m_cbSize,
							k_nSteamNetworkingSend_Reliable, nullptr
						);
					}
				}
			}
			else if (type == SessionProtocol::MT_PUNCH_READY) {
				int side = -1;
				for (int i = 0; i < 2 && i < (int)clients.size(); i++) {
					if (clients.at(i).conn == conn) {
						side = i;
						break;
					}
				}
				if (side == -1) {
					spdlog::info("Server: sender {} sent punch_ready but is not playing", conn);
					continue;
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
			else if (type == SessionProtocol::MT_BATTLE_GGPO_FRAME) {
				SessionProtocol::BattleGgpoFrame frame;
				try {
					msg.get_to(frame);
				}
				catch (json::exception e) {
					spdlog::debug("Server: could not deserialize GGPO frame");
					continue;
				}

				if (frame.src.host == _identity && frame.src.user != std::to_string(conn)) {
					spdlog::debug("Server: dropping fraudulent GGPO frame");
					continue;
				}

				for (auto clientIter = clients.begin(); clientIter != clients.end(); clientIter++) {
					if (clientIter->data.connId == frame.dest) {
						Respond(clientIter->conn, msg);
						break;
					}
				}
			}
			else {
				spdlog::warn("Server: got unrecognized message type: {}", (int)type);
			}
		}
	}

	for (int i = 0; i < numMsgs; i++) {
		ISteamNetworkingMessage* pIncomingMsg = pIncomingMsgs[i];
		if (pIncomingMsg) {
			pIncomingMsg->Release();
		}
	}

	if (_dataDirty) {
		SessionProtocol::SessionDataUpdate updateMsg;
		updateMsg.lobbyData = _lobbyData;
		updateMsg.matchData = _matchData;
		updateMsg.lobbyData.members.clear();
		for (auto clientIter = clients.begin(); clientIter != clients.end(); clientIter++) {
			updateMsg.lobbyData.members.push_back(clientIter->data);
		}
		BroadcastMessage(json(updateMsg));
		_dataDirty = false;
	}

	if (bSendLobbyAllReady) {
		BroadcastMessage(json(SessionProtocol::LobbyAllReady()));
	}

	if (bSendBattleSynced) {
		BroadcastMessage(json(SessionProtocol::BattleSynced()));
	}

	return 0;
}

void SessionServer::Respond(HSteamNetConnection client, const nlohmann::json& msg) {
	std::string buf = msg.dump();
	_interface->SendMessageToConnection(
		client, buf.c_str(), (uint32)buf.length(),
		k_nSteamNetworkingSend_Reliable, nullptr
	);
}

void SessionServer::BroadcastMessage(const nlohmann::json& msg) {
	// XXX (adanducci) replace SendMessageToConnection with SendMessages for
	// peformance gains, but ensuring low-copy with it is annoyingly difficult.
	std::string buf = msg.dump();
	for (auto clientIter = clients.begin(); clientIter != clients.end(); clientIter++) {
		_interface->SendMessageToConnection(
			clientIter->conn, buf.c_str(), (uint32)buf.length(),
			k_nSteamNetworkingSend_Reliable, nullptr
		);
	}
}

int SessionServer::Close()
{
	spdlog::info("Closing connections...");
	// Close all connections.  We use "linger mode" to ask SteamNetworkingSockets
	// to flush this out and close gracefully.
	for (auto iter = clients.begin(); iter != clients.end(); iter++) {
		_interface->SetConnectionPollGroup(iter->conn, k_HSteamNetPollGroup_Invalid);
		_interface->CloseConnection(iter->conn, 0, "Server Shutdown", true);
	}
	clients.clear();
	_interface->DestroyPollGroup(_pollGroup);
	_pollGroup = k_HSteamNetPollGroup_Invalid;
	if (_listenSock != k_HSteamListenSocket_Invalid) {
		_interface->CloseListenSocket(_listenSock);
		_listenSock = k_HSteamListenSocket_Invalid;
	}
	return 0;
}

void SessionServer::PrepareForCallbacks()
{
	s_pCallbackInstance = this;
}

void SessionServer::ResetBattleSync()
{
	for (int i = 0; i < clients.size(); i++) {
		clients.at(i).data.flags &= (~SessionProtocol::MF_BATTLE_LOADED);
	}
}

void SessionServer::ResetLobbyForRematch()
{
	_matchData.ClearReady();
	ResetBattleSync();
	_punchReady[0] = false;
	_punchReady[1] = false;
	_dataDirty = true;
}

void SessionServer::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
	switch (pInfo->m_info.m_eState)
	{
	case k_ESteamNetworkingConnectionState_ClosedByPeer:
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
	{
		// Ignore if they were not previously connected.  (If they disconnected
		// before we accepted the connection.)
		if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected) {
			// Select appropriate log messages
			const char* pszDebugLogAction;
			if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
			{
				pszDebugLogAction = "problem detected locally";
			}
			else
			{
				// Note that here we could check the reason code to see if
				// it was a "usual" connection or an "unusual" one.
				pszDebugLogAction = "closed by peer";
			}

			spdlog::info("Connection {} {}, reason {}: {}",
				pInfo->m_info.m_szConnectionDescription,
				pszDebugLogAction,
				pInfo->m_info.m_eEndReason,
				pInfo->m_info.m_szEndDebug
			);

			for (auto iter = clients.begin(); iter != clients.end(); iter++) {
				if (iter->conn == pInfo->m_hConn) {
					clients.erase(iter);
					_dataDirty = true;
					break;
				}
			}

			// Clean up the connection.  This is important!
			// The connection is "closed" in the network sense, but
			// it has not been destroyed.  We must close it on our end, too
			// to finish up.  The reason information do not matter in this case,
			// and we cannot linger because it's already closed on the other end,
			// so we just pass 0's.
			_interface->SetConnectionPollGroup(pInfo->m_hConn, k_HSteamNetPollGroup_Invalid);
			_interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
		}

		break;
	}

	case k_ESteamNetworkingConnectionState_Connecting:
	{
		// Try to accept the connection.
		if (_interface->AcceptConnection(pInfo->m_hConn) != k_EResultOK)
		{
			// This could fail.  If the remote host tried to connect, but then
			// disconnected, the connection may already be half closed.  Just
			// destroy whatever we have on our side.
			_interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
			spdlog::error("Can't accept connection.  (It was already closed?)");
			break;
		}

		_interface->SetConnectionPollGroup(pInfo->m_hConn, _pollGroup);
	}

	default:
		break;
	}
}

void SessionServer::SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
	if (!s_pCallbackInstance) {
		return;
	}
	s_pCallbackInstance->OnSteamNetConnectionStatusChanged(pInfo);
}

SessionProtocol::JoinResult SessionServer::RegisterToWait(
	const HSteamNetConnection& conn,
	const uint16_t& port,
	const std::string& sidecarHash,
	const std::string& name,
	const SteamNetworkingIPAddr& peerAddr,
	SessionProtocol::ConnectionID& cid
) {
	if (sidecarHash != _sidecarHash) {
		return SessionProtocol::JR_HASH_INVALID;
	}

	if (clients.size() >= MAX_SF4E_PROTOCOL_USERS) {
		return SessionProtocol::JR_LOBBY_FULL;
	}

	for (auto iter = clients.begin(); iter != clients.end(); iter++) {
		if (iter->data.name == name) {
			return SessionProtocol::JR_NAME_TAKEN;
		}
	}

	char peerAddrStr[SteamNetworkingIPAddr::k_cchMaxString];
	if (peerAddr.IsLocalHost()) {
		peerAddrStr[0] = 0;
	}
	else {
		peerAddr.ToString(peerAddrStr, SteamNetworkingIPAddr::k_cchMaxString, false);
	}
	SessionMember newMember{ {cid, name, peerAddrStr, port}, conn };
	clients.push_back(std::move(newMember));
	return SessionProtocol::JOIN_OK;
}

void SessionServer::HandleResults(int loserIndex) {
	auto loser = clients.begin() + loserIndex;
	clients.push_back(*loser);
	clients.erase(loser);
	_matchData.Clear();
}
