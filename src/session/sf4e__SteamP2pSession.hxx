#pragma once

#include <cstdint>

namespace sf4e {

	class SessionClient;
	class SessionServer;

	namespace SteamP2pSession {
		bool EnsureSteam();
		void Pump();
		bool IsConnected();
		bool HostBegin(SessionServer* server, int virtualPort);
		bool ConnectHostLocalClient(SessionClient* client);
		bool JoinBegin(SessionClient* client, uint64_t peerSteamId64, int virtualPort);
		void Shutdown();
	}

} // namespace sf4e
