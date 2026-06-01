#ifdef SF4E_STEAMWORKS_EXPERIMENT

#include <fstream>
#include <string>

#include <windows.h>
#include <pathcch.h>

#include <steam/steam_api.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#endif

#include "sf4e__SteamP2pSession.hxx"

#include "sf4e__SessionClient.hxx"
#include "sf4e__SessionServer.hxx"

#include <spdlog/spdlog.h>

#ifdef SF4E_STEAMWORKS_EXPERIMENT

namespace sf4e {
namespace SteamP2pSession {
namespace {

	struct State {
		bool initialized = false;
		bool connected = false;
		bool acceptIncoming = false;
		HSteamListenSocket listenSock = k_HSteamListenSocket_Invalid;
		HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
		HSteamNetConnection hostLocalClientConn = k_HSteamNetConnection_Invalid;
		SessionServer* server = nullptr;
	};

	static State g_state;

	void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
		if (!info) {
			return;
		}

		if (g_state.acceptIncoming && info->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting) {
			if (g_state.conn == k_HSteamNetConnection_Invalid) {
				g_state.conn = info->m_hConn;
			}
			const EResult accept = SteamNetworkingSockets()->AcceptConnection(info->m_hConn);
			if (accept != k_EResultOK) {
				spdlog::error("SteamP2pSession: AcceptConnection failed");
			}
			else if (g_state.server) {
				g_state.server->AddConnection(info->m_hConn);
				spdlog::info("SteamP2pSession: accepted peer connection");
			}
		}

		if (info->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected) {
			g_state.conn = info->m_hConn;
			g_state.connected = true;
		}

		if (
			info->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
			info->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally
		) {
			g_state.connected = false;
		}
	}

	bool GetModuleDir(wchar_t* outDir, int outDirChars) {
		HMODULE mod = nullptr;
		if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCWSTR)&GetModuleDir,
			&mod
		)) {
			return false;
		}
		if (GetModuleFileNameW(mod, outDir, outDirChars) == 0) {
			return false;
		}
		return SUCCEEDED(PathCchRemoveFileSpec(outDir, outDirChars));
	}

	void EnsureSteamAppIdFile() {
		SetEnvironmentVariableA("SteamAppId", "45760");
		SetEnvironmentVariableA("SteamGameId", "45760");
		wchar_t dir[MAX_PATH] = { 0 };
		if (!GetModuleDir(dir, MAX_PATH)) {
			return;
		}
		wchar_t path[MAX_PATH] = { 0 };
		if (FAILED(PathCchCombine(path, MAX_PATH, dir, L"steam_appid.txt"))) {
			return;
		}
		if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
			return;
		}
		char allow[8] = { 0 };
		if (GetEnvironmentVariableA("SF4E_ALLOW_STEAM_APPID_WRITE", allow, sizeof(allow)) == 0) {
			spdlog::warn("SteamP2pSession: steam_appid.txt missing; relying on inherited SteamAppId/SteamGameId env");
			return;
		}
		std::ofstream f(path);
		if (f) {
			f << "45760";
			spdlog::info(L"SteamP2pSession: created steam_appid.txt dev fallback at {}", path);
		}
	}

} // namespace

	bool EnsureSteam() {
		if (g_state.initialized) {
			SteamAPI_RunCallbacks();
			SteamNetworkingSockets()->RunCallbacks();
			return true;
		}

		EnsureSteamAppIdFile();
		if (!SteamAPI_Init()) {
			spdlog::error("SteamP2pSession: SteamAPI_Init failed");
			return false;
		}
		if (!SteamUser() || !SteamUser()->BLoggedOn()) {
			spdlog::error("SteamP2pSession: Steam user not logged on");
			return false;
		}
		SteamNetworkingUtils()->InitRelayNetworkAccess();
		g_state.initialized = true;
		return true;
	}

	void Pump() {
		if (!g_state.initialized) {
			return;
		}
		SteamAPI_RunCallbacks();
		SteamNetworkingSockets()->RunCallbacks();
	}

	bool IsConnected() {
		return g_state.connected;
	}

	void Shutdown() {
		if (g_state.initialized && SteamNetworkingSockets()) {
			if (g_state.hostLocalClientConn != k_HSteamNetConnection_Invalid) {
				SteamNetworkingSockets()->CloseConnection(g_state.hostLocalClientConn, 0, "shutdown", false);
			}
			if (g_state.conn != k_HSteamNetConnection_Invalid) {
				SteamNetworkingSockets()->CloseConnection(g_state.conn, 0, "shutdown", false);
			}
			if (g_state.listenSock != k_HSteamListenSocket_Invalid) {
				SteamNetworkingSockets()->CloseListenSocket(g_state.listenSock);
			}
		}
		g_state = State();
	}

	bool HostBegin(SessionServer* server, int virtualPort) {
		if (!EnsureSteam() || !server) {
			return false;
		}
		Shutdown();
		EnsureSteam();

		g_state.server = server;
		g_state.acceptIncoming = true;

		HSteamNetConnection hostServerConn = k_HSteamNetConnection_Invalid;
		HSteamNetConnection hostClientConn = k_HSteamNetConnection_Invalid;
		if (!SteamNetworkingSockets()->CreateSocketPair(&hostServerConn, &hostClientConn, false, nullptr, nullptr)) {
			spdlog::error("SteamP2pSession: CreateSocketPair failed");
			return false;
		}
		server->AddConnection(hostServerConn);
		g_state.hostLocalClientConn = hostClientConn;

		SteamNetworkingConfigValue_t opt;
		opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)OnConnectionStatusChanged);
		g_state.listenSock = SteamNetworkingSockets()->CreateListenSocketP2P(virtualPort, 1, &opt);
		if (g_state.listenSock == k_HSteamListenSocket_Invalid) {
			spdlog::error("SteamP2pSession: CreateListenSocketP2P failed");
			Shutdown();
			return false;
		}
		spdlog::info("SteamP2pSession: listening on virtual port {}", virtualPort);
		return true;
	}

	bool ConnectHostLocalClient(SessionClient* client) {
		if (!client || g_state.hostLocalClientConn == k_HSteamNetConnection_Invalid) {
			return false;
		}
		if (client->Connect(g_state.hostLocalClientConn) != 0) {
			spdlog::error("SteamP2pSession: host local client connect failed");
			return false;
		}
		client->PrepareForCallbacks();
		return true;
	}

	bool JoinBegin(SessionClient* client, uint64_t peerSteamId64, int virtualPort) {
		if (!EnsureSteam() || !client || peerSteamId64 == 0) {
			return false;
		}
		Shutdown();
		EnsureSteam();

		g_state.acceptIncoming = false;
		SteamNetworkingIdentity identity;
		identity.SetSteamID64(peerSteamId64);
		SteamNetworkingConfigValue_t opt;
		opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)OnConnectionStatusChanged);
		g_state.conn = SteamNetworkingSockets()->ConnectP2P(identity, virtualPort, 1, &opt);
		if (g_state.conn == k_HSteamNetConnection_Invalid) {
			spdlog::error("SteamP2pSession: ConnectP2P failed");
			return false;
		}

		for (int i = 0; i < 300; ++i) {
			Pump();
			if (g_state.connected) {
				break;
			}
			Sleep(10);
		}
		if (!g_state.connected) {
			spdlog::warn("SteamP2pSession: ConnectP2P still connecting; continuing");
		}

		if (client->Connect(g_state.conn) != 0) {
			spdlog::error("SteamP2pSession: SessionClient connect failed");
			return false;
		}
		client->PrepareForCallbacks();
		return true;
	}

} // namespace SteamP2pSession
} // namespace sf4e

#else

namespace sf4e {
namespace SteamP2pSession {

	bool EnsureSteam() { return false; }
	void Pump() {}
	bool IsConnected() { return false; }
	bool HostBegin(SessionServer*, int) { return false; }
	bool ConnectHostLocalClient(SessionClient*) { return false; }
	bool JoinBegin(SessionClient*, uint64_t, int) { return false; }
	void Shutdown() {}

} // namespace SteamP2pSession
} // namespace sf4e

#endif
