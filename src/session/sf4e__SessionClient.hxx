#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>

#include "sf4e__SessionProtocol.hxx"

namespace sf4e {
	extern const int SESSION_CLIENT_MAX_MESSAGES_PER_POLL;

	class SessionClient
	{

	public:
		static bool bVerboseLogging;

		enum ErrorType {
			SCE_UNKNOWN,
			SCE_JOIN_REJECTED_HASH_INVALID,
			SCE_JOIN_REJECTED_LOBBY_FULL,
			SCE_JOIN_REJECTED_NAME_TAKEN,
			SCE_JOIN_REJECTED_REQUEST_INVALID,
		};

		struct Callbacks {
			void* data;
			void (*OnError)(ErrorType errorType, SessionClient* const client, const Callbacks& callbacks);
			void (*OnReady)(SessionClient* const client, const Callbacks& callbacks);
			void (*OnBattleSynced)(SessionClient* const client, const Callbacks& callbacks);
		};

		SessionClient(
			const Callbacks& callbacks,
			std::string sidecarHash,
			uint16_t ggpoPort,
			std::string& name
		);
		~SessionClient();

		int Connect(HSteamNetConnection newConn);
		int Connect(const SteamNetworkingIPAddr& serverAddr);
		void Disconnect();
		int Step();
		void PrepareForCallbacks();
		bool IsConnected() const { return _connected; }

		// Lobby data
		std::string _name;
		SessionProtocol::LobbyData _lobbyData;
		SessionProtocol::MatchData _matchData;
		int64_t _outstandingReadyRequestNumber = -1;
		bool _snapshotsEnabled;

		EResult Lobby_Ready();
		EResult Lobby_ReportResults(int loserSide);
		EResult Lobby_ResetRematch();
		EResult Lobby_SetSettings(
			bool editionSelect,
			int roundCount,
			Dimps::Math::FixedPoint roundTime,
			bool trainingMode
		);

		EResult PreBattle_SetEnv(uint32_t rngSeed);
		EResult PreBattle_SetChara(const Dimps::GameEvents::VsMode::ConfirmedCharaConditions& chara);
		EResult PreBattle_SetStage(int32_t stageID);

		EResult Battle_Loaded();

		EResult Punch_SendReady();
		void Punch_Reset();
		bool Punch_GoReceived() const { return _punchGoReceived; }

		EResult Forward(const SessionProtocol::ConnectionID& dest, const nlohmann::json& msg);
		EResult SendGgpoFrame(const SessionProtocol::ConnectionID& dest, const uint8_t* data, uint32_t len);
		void OnGgpoFrameReceived(const SessionProtocol::BattleGgpoFrame& frame);

		struct GgpoTunnelStats {
			uint64_t sendCount = 0;
			uint64_t sendBytes = 0;
			uint64_t recvCount = 0;
			uint64_t recvBytes = 0;
		};
		GgpoTunnelStats GetGgpoTunnelStats() const { return _ggpoTunnelStats; }
		void ResetGgpoTunnelStats() { _ggpoTunnelStats = {}; }

		// Public for testing
		EResult Send(nlohmann::json& msg, int64_t* outMessageNum);
		
		// Connection related data - public for testing
		std::string _sidecarHash;
		uint16_t _ggpoPort;
		bool _useRelay = true;
		SteamNetworkingIPAddr _serverAddr;
		std::map<int, SessionProtocol::StateSnapshot> pendingRemoteSnapshots;
		// Desync v2: remote hash checkpoints received before this client
		// simulated (or aged) the same frame. Bounded — see Step().
		std::map<int, SessionProtocol::BattleHashV2> pendingRemoteHashes;
		SessionProtocol::ConnectionID _cid;

		// True when this client occupies one of the two player slots of the
		// current lobby. Spectator hash mismatches are diagnostics only.
		bool IsLocalPlayer() const;
	private:

		// Connection related data
		Callbacks _callbacks;
		bool _connected = false;
		HSteamNetConnection _conn;
		ISteamNetworkingSockets* _interface;


		void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo);

		static SessionClient* s_pCallbackInstance;
		static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* pInfo);

		GgpoTunnelStats _ggpoTunnelStats = {};
		bool _punchGoReceived = false;
	};
}
