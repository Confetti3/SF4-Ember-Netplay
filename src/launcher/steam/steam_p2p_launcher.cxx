#include "steam_p2p_launcher.hxx"

#include "../../common/install_paths.hxx"

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>
#include <pathcch.h>

#include <steam/steam_api.h>
#include <steam/isteamnetworkingmessages.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include <spdlog/spdlog.h>

#include "../steam_experiment/steam_p2p_payload.hxx"

namespace sf4e {
namespace launcher {
namespace steam_p2p {
namespace {

	struct ProbeState {
		HSteamListenSocket listenSock = k_HSteamListenSocket_Invalid;
		HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
		bool initialized = false;
		bool connected = false;
		bool failed = false;
		bool accepted = false;
		bool acceptIncoming = false;
		std::string lastEvent;
		std::string lastError;
		int virtualPort = 7;
		unsigned long long peerSteamId = 0;
	};

	static ProbeState g_state;

	const char* ConnStateName(ESteamNetworkingConnectionState state) {
		switch (state) {
		case k_ESteamNetworkingConnectionState_None:
			return "None";
		case k_ESteamNetworkingConnectionState_Connecting:
			return "Connecting";
		case k_ESteamNetworkingConnectionState_FindingRoute:
			return "FindingRoute";
		case k_ESteamNetworkingConnectionState_Connected:
			return "Connected";
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			return "ClosedByPeer";
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			return "ProblemDetectedLocally";
		default:
			return "Unknown";
		}
	}

	void SetEvent(const std::string& text) {
		g_state.lastEvent = text;
	}

	void SetError(const std::string& text) {
		g_state.lastError = text;
		g_state.lastEvent = text;
	}

	void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
		if (!info) {
			return;
		}

		std::ostringstream os;
		os << "connection " << ConnStateName(info->m_eOldState) << " -> " << ConnStateName(info->m_info.m_eState);
		if (info->m_info.m_szEndDebug[0]) {
			os << ": " << info->m_info.m_szEndDebug;
		}
		SetEvent(os.str());

		if (g_state.acceptIncoming && info->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting) {
			g_state.conn = info->m_hConn;
			const EResult accept = SteamNetworkingSockets()->AcceptConnection(info->m_hConn);
			g_state.accepted = accept == k_EResultOK;
			if (accept != k_EResultOK) {
				SetError("AcceptConnection failed");
			}
		}

		if (info->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected) {
			g_state.conn = info->m_hConn;
			g_state.connected = true;
			g_state.failed = false;
		}

		if (
			info->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
			info->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally
		) {
			g_state.failed = true;
			g_state.connected = false;
		}
	}

	bool GetLauncherDir(wchar_t* outDir, int outDirChars) {
		return sf4e::install::GetInstallRoot(outDir, outDirChars);
	}

	bool ShouldAllowRuntimeAppIdWrite() {
		char allow[8] = { 0 };
		return GetEnvironmentVariableA("SF4E_ALLOW_STEAM_APPID_WRITE", allow, sizeof(allow)) > 0
			|| !sf4e::install::UsesDllSubdirectory();
	}

	bool EnsureSteamAppIdFileInDir(const wchar_t* dir, bool allowWrite) {
		if (!dir || !dir[0]) {
			return false;
		}
		wchar_t path[MAX_PATH] = { 0 };
		if (FAILED(PathCchCombine(path, MAX_PATH, dir, L"steam_appid.txt"))) {
			return false;
		}
		if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
			return true;
		}
		if (!allowWrite) {
			spdlog::warn(L"steam_appid.txt missing at {} (runtime write disabled for packaged builds)", path);
			return false;
		}
		std::ofstream f(path);
		if (f) {
			f << "45760";
			spdlog::info(L"Created steam_appid.txt dev fallback at {}", path);
			return true;
		}
		spdlog::warn(L"Could not create steam_appid.txt at {}", path);
		return false;
	}

	void EnsureSteamAppIdFile() {
		SetEnvironmentVariableA("SteamAppId", "45760");
		SetEnvironmentVariableA("SteamGameId", "45760");
		const bool allowWrite = ShouldAllowRuntimeAppIdWrite();
		bool foundAppId = false;

		wchar_t root[MAX_PATH] = { 0 };
		if (GetLauncherDir(root, MAX_PATH)) {
			foundAppId = EnsureSteamAppIdFileInDir(root, allowWrite) || foundAppId;
		}

		wchar_t dllDir[MAX_PATH] = { 0 };
		if (sf4e::install::GetPackageDllDirectory(dllDir, MAX_PATH)) {
			foundAppId = EnsureSteamAppIdFileInDir(dllDir, allowWrite) || foundAppId;
		}

		wchar_t cwd[MAX_PATH] = { 0 };
		if (GetCurrentDirectoryW(MAX_PATH, cwd) > 0) {
			foundAppId = EnsureSteamAppIdFileInDir(cwd, allowWrite) || foundAppId;
		}
		if (!foundAppId) {
			spdlog::warn("No steam_appid.txt found; relying on SteamAppId/SteamGameId environment variables");
		}
	}

	bool EnsureSteam() {
		if (g_state.initialized) {
			SteamAPI_RunCallbacks();
			SteamNetworkingSockets()->RunCallbacks();
			return true;
		}

		EnsureSteamAppIdFile();
		if (!SteamAPI_Init()) {
			SetError("SteamAPI_Init failed. Start Steam and use the experimental build folder containing steam_api.dll.");
			wchar_t cwd[MAX_PATH] = { 0 };
			GetCurrentDirectoryW(MAX_PATH, cwd);
			spdlog::warn(L"SteamAPI_Init failed cwd={}", cwd);
			return false;
		}
		if (!SteamUser() || !SteamUser()->BLoggedOn()) {
			SetError("Steam initialized but no logged-in Steam user is available.");
			spdlog::warn("Steam initialized without a logged-in Steam user");
			return false;
		}
		SteamNetworkingUtils()->InitRelayNetworkAccess();
		g_state.initialized = true;
		SetEvent("Steam initialized");
		spdlog::info(
			"Steam initialized for launcher user={} persona={}",
			(unsigned long long)SteamUser()->GetSteamID().ConvertToUint64(),
			SteamFriends() ? SteamFriends()->GetPersonaName() : ""
		);
		return true;
	}

	void Pump() {
		if (!EnsureSteam()) {
			return;
		}
		SteamAPI_RunCallbacks();
		SteamNetworkingSockets()->RunCallbacks();
	}

	nlohmann::json Envelope(const char* responseType) {
		nlohmann::json j;
		j["v"] = 1;
		j["type"] = responseType;
		j["steamExperiment"] = true;
		j["initialized"] = g_state.initialized;
		j["connected"] = g_state.connected;
		j["failed"] = g_state.failed;
		j["accepted"] = g_state.accepted;
		j["virtualPort"] = g_state.virtualPort;
		j["peerSteamId"] = std::to_string(g_state.peerSteamId);
		j["lastEvent"] = g_state.lastEvent;
		j["lastError"] = g_state.lastError;
		if (g_state.initialized && SteamUser()) {
			j["steamId"] = std::to_string(SteamUser()->GetSteamID().ConvertToUint64());
			j["persona"] = SteamFriends() ? SteamFriends()->GetPersonaName() : "";
		}
		return j;
	}

	nlohmann::json DrainMessages() {
		nlohmann::json messages = nlohmann::json::array();
		if (!g_state.initialized) {
			return messages;
		}

		SteamNetworkingMessage_t* msgs[16] = { nullptr };
		const int count = SteamNetworkingMessages()->ReceiveMessagesOnChannel(0, msgs, 16);
		for (int i = 0; i < count; ++i) {
			std::string body((const char*)msgs[i]->m_pData, (size_t)msgs[i]->m_cbSize);
			nlohmann::json item;
			item["fromSteamId"] = std::to_string(msgs[i]->m_identityPeer.GetSteamID64());
			item["bodySize"] = msgs[i]->m_cbSize;

			sf4e::steam_experiment::SteamInvitePayload invite;
			std::string err;
			if (sf4e::steam_experiment::DecodeInvite(body, invite, err)) {
				item["kind"] = "invite";
				item["invite"] = {
					{ "senderSteamId", std::to_string(invite.senderSteamId) },
					{ "virtualPort", invite.virtualPort },
					{ "role", invite.role },
					{ "sidecarHash", invite.sidecarHash },
					{ "buildGit", invite.buildGit },
					{ "sessionToken", invite.sessionToken }
				};
			}
			else {
				item["kind"] = "raw";
				item["body"] = body;
				item["parseError"] = err;
			}

			messages.push_back(item);
			msgs[i]->Release();
		}
		return messages;
	}

	nlohmann::json DrainSocketMessages() {
		nlohmann::json messages = nlohmann::json::array();
		if (g_state.conn == k_HSteamNetConnection_Invalid || !g_state.initialized) {
			return messages;
		}

		ISteamNetworkingMessage* msgs[8] = { nullptr };
		const int count = SteamNetworkingSockets()->ReceiveMessagesOnConnection(g_state.conn, msgs, 8);
		for (int i = 0; i < count; ++i) {
			std::string body((const char*)msgs[i]->m_pData, (size_t)msgs[i]->m_cbSize);
			nlohmann::json item;
			item["body"] = body;
			item["size"] = msgs[i]->m_cbSize;
			messages.push_back(item);
			msgs[i]->Release();
		}
		return messages;
	}

} // namespace

	nlohmann::json BuildStatusJson() {
		EnsureSteam();
		nlohmann::json j = Envelope("steamStatus");
		j["messages"] = DrainMessages();
		j["socketMessages"] = DrainSocketMessages();
		return j;
	}

	nlohmann::json BuildInfoJson() {
		nlohmann::json j = Envelope("steamBuildInfo");
		j["ok"] = true;
		return j;
	}

	nlohmann::json RefreshFriendsJson(bool onlyInSf4) {
		EnsureSteam();
		nlohmann::json j = Envelope("steamFriends");
		j["friends"] = nlohmann::json::array();
		if (!g_state.initialized || !SteamFriends()) {
			return j;
		}

		const int count = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
		spdlog::info("Steam friends refresh onlyInSf4={} count={}", onlyInSf4, count);
		for (int i = 0; i < count; ++i) {
			CSteamID friendId = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
			FriendGameInfo_t gameInfo;
			const bool inGame = SteamFriends()->GetFriendGamePlayed(friendId, &gameInfo);
			const bool inSf4 = inGame && gameInfo.m_gameID.AppID() == 45760;
			if (onlyInSf4 && !inSf4) {
				continue;
			}
			j["friends"].push_back({
				{ "steamId", std::to_string(friendId.ConvertToUint64()) },
				{ "name", SteamFriends()->GetFriendPersonaName(friendId) },
				{ "personaState", (int)SteamFriends()->GetFriendPersonaState(friendId) },
				{ "inSf4", inSf4 }
			});
		}
		return j;
	}

	nlohmann::json PrepareHostJson(
		unsigned long long targetSteamId,
		int virtualPort,
		const char* sidecarHash,
		const char* buildGit,
		const char* sessionToken
	) {
		nlohmann::json invite = SendInviteJson(targetSteamId, virtualPort, sidecarHash, buildGit, sessionToken);
		if (!invite.value("ok", false)) {
			invite["type"] = "steamPrepareHost";
			return invite;
		}
		nlohmann::json listen = ListenJson(virtualPort);
		nlohmann::json j = Envelope("steamPrepareHost");
		j["ok"] = listen.value("ok", false);
		j["inviteOk"] = invite.value("ok", false);
		j["listenOk"] = listen.value("ok", false);
		j["sessionTokenPresent"] = sessionToken && sessionToken[0];
		j["virtualPort"] = virtualPort;
		j["targetSteamId"] = std::to_string(targetSteamId);
		return j;
	}

	nlohmann::json SendInviteJson(unsigned long long targetSteamId, int virtualPort, const char* sidecarHash, const char* buildGit, const char* sessionToken) {
		EnsureSteam();
		nlohmann::json j = Envelope("steamInviteSent");
		if (!g_state.initialized) {
			j["ok"] = false;
			return j;
		}

		sf4e::steam_experiment::SteamInvitePayload payload;
		payload.senderSteamId = SteamUser()->GetSteamID().ConvertToUint64();
		payload.virtualPort = virtualPort;
		payload.role = "host";
		payload.sidecarHash = sidecarHash && sidecarHash[0] ? sidecarHash : "manual-test";
		payload.buildGit = buildGit && buildGit[0] ? buildGit : "manual-test";
		payload.sessionToken = sessionToken && sessionToken[0] ? sessionToken : "manual-test-token";
		std::string err;
		if (!sf4e::steam_experiment::ValidateInvite(payload, err)) {
			j["ok"] = false;
			j["message"] = err;
			return j;
		}

		SteamNetworkingIdentity identity;
		identity.SetSteamID64(targetSteamId);
		const std::string encoded = sf4e::steam_experiment::EncodeInvite(payload);
		EResult result = SteamNetworkingMessages()->SendMessageToUser(
			identity,
			encoded.data(),
			(uint32)encoded.size(),
			k_nSteamNetworkingSend_Reliable,
			0
		);
		j["ok"] = result == k_EResultOK;
		j["result"] = (int)result;
		j["payloadBytes"] = encoded.size();
		SetEvent(result == k_EResultOK ? "Steam invite sent" : "Steam invite send failed");
		return j;
	}

	nlohmann::json PollMessagesJson() {
		Pump();
		nlohmann::json j = Envelope("steamMessages");
		j["messages"] = DrainMessages();
		j["socketMessages"] = DrainSocketMessages();
		return j;
	}

	nlohmann::json ListenJson(int virtualPort) {
		EnsureSteam();
		nlohmann::json j = Envelope("steamListen");
		if (!g_state.initialized) {
			j["ok"] = false;
			return j;
		}
		CloseJson();
		EnsureSteam();
		g_state.acceptIncoming = true;
		g_state.virtualPort = virtualPort;
		SteamNetworkingConfigValue_t opt;
		opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)OnConnectionStatusChanged);
		g_state.listenSock = SteamNetworkingSockets()->CreateListenSocketP2P(virtualPort, 1, &opt);
		const bool ok = g_state.listenSock != k_HSteamListenSocket_Invalid;
		SetEvent(ok ? "Listening for Steam P2P connection" : "CreateListenSocketP2P failed");
		j = Envelope("steamListen");
		j["ok"] = ok;
		return j;
	}

	nlohmann::json ConnectJson(unsigned long long targetSteamId, int virtualPort) {
		EnsureSteam();
		nlohmann::json j = Envelope("steamConnect");
		if (!g_state.initialized) {
			j["ok"] = false;
			return j;
		}
		CloseJson();
		EnsureSteam();
		g_state.acceptIncoming = false;
		g_state.virtualPort = virtualPort;
		g_state.peerSteamId = targetSteamId;
		SteamNetworkingIdentity identity;
		identity.SetSteamID64(targetSteamId);
		SteamNetworkingConfigValue_t opt;
		opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)OnConnectionStatusChanged);
		g_state.conn = SteamNetworkingSockets()->ConnectP2P(identity, virtualPort, 1, &opt);
		const bool ok = g_state.conn != k_HSteamNetConnection_Invalid;
		SetEvent(ok ? "Connecting with Steam P2P" : "ConnectP2P failed");
		j = Envelope("steamConnect");
		j["ok"] = ok;
		return j;
	}

	nlohmann::json CloseJson() {
		if (g_state.initialized && SteamNetworkingSockets()) {
			if (g_state.conn != k_HSteamNetConnection_Invalid) {
				SteamNetworkingSockets()->CloseConnection(g_state.conn, 0, "launcher close", false);
			}
			if (g_state.listenSock != k_HSteamListenSocket_Invalid) {
				SteamNetworkingSockets()->CloseListenSocket(g_state.listenSock);
			}
		}
		g_state.conn = k_HSteamNetConnection_Invalid;
		g_state.listenSock = k_HSteamListenSocket_Invalid;
		g_state.connected = false;
		g_state.failed = false;
		g_state.accepted = false;
		g_state.acceptIncoming = false;
		SetEvent("Steam P2P sockets closed");
		return Envelope("steamClosed");
	}

} // namespace steam_p2p
} // namespace launcher
} // namespace sf4e
