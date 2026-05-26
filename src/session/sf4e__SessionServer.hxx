#pragma once

#include <map>
#include <string>
#include <vector>

#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <nlohmann/json.hpp>

#include "../Dimps/Dimps__Math.hxx"
#include "sf4e__SessionProtocol.hxx"

namespace sf4e {
	extern const int SESSION_SERVER_MAX_MESSAGES_PER_POLL;

	class SessionServer
	{
	private:
		// A binary blob usable by any host for routing messages to this
		// server. This is most likely an IP address and port, a hostname
		// and port, or in extreme cases an overlay network's concept of
		// addressing (ex. an index in a service discovery protocol).
		// Identities of clients connected to the server are prefixed with
		// this identity. This allows all servers in a cluster to forward
		// messages to any user connected to any server in the cluster-
		// just send it to the prefixed identity, and that host will take
		// care of the rest.
		std::string _identity;

		// Connection related data
		std::string _sidecarHash;
		HSteamListenSocket _listenSock;
		HSteamNetPollGroup _pollGroup;
		ISteamNetworkingSockets* _interface;

		// Connection callbacks and message utilities
		static SessionServer* s_pCallbackInstance;
		static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo);
		void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);
		void BroadcastMessage(nlohmann::json& msg);
		void Respond(HSteamNetConnection client, nlohmann::json& msg);

		// Direct lobby data manipulation utilities
		SessionProtocol::JoinResult RegisterToWait(
			const HSteamNetConnection& conn,
			const uint16_t& port,
			const std::string& sidecarHash,
			const std::string& name,
			const SteamNetworkingIPAddr& peerAddr,
			SessionProtocol::ConnectionID& cid
		);
		void HandleResults(int loserSide);

	public:
		SessionServer(
			std::string identity,
			std::string sidecarHash,
			bool editionSelect,
			int roundCount,
			Dimps::Math::FixedPoint roundTime
		);
		~SessionServer();

		void AddConnection(HSteamNetConnection newConn);
		int Listen(uint16 nPort);
		int Step();
		int Close();
		void PrepareForCallbacks();
		void ResetBattleSync();
		void ResetLobbyForRematch();

		size_t ConnectedClientCount() const { return clients.size(); }

		typedef struct SessionMember {
			SessionProtocol::MemberData data;
			HSteamNetConnection conn;
		} SessionMember;

		std::map<HSteamNetConnection, SessionProtocol::ConnectionID> cidMap;
		std::vector<SessionMember> clients;

		// Lobby data: Public for visibility into tests only.
		bool _dataDirty;
		SessionProtocol::LobbyData _lobbyData;
		SessionProtocol::MatchData _matchData;
	};
}