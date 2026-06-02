#include "steam_p2p_launcher.hxx"

#include "../../common/install_paths.hxx"

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
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
	static bool g_relayReady = false;

	bool EnsureSteam();

	void SetEvent(const std::string& text);

	void SetError(const std::string& text);

	void AcceptPendingMessageSessions() {
		if (!g_state.initialized || !SteamNetworkingMessages() || !SteamFriends()) {
			return;
		}
		const int count = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
		for (int i = 0; i < count; ++i) {
			const CSteamID friendId = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
			SteamNetworkingIdentity identity;
			identity.SetSteamID(friendId);
			SteamNetworkingMessages()->AcceptSessionWithUser(identity);
		}
	}

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

	const char* EResultName(EResult result) {
		switch (result) {
		case k_EResultOK:
			return "OK";
		case k_EResultFail:
			return "Fail";
		case k_EResultNoConnection:
			return "NoConnection";
		case k_EResultInvalidParam:
			return "InvalidParam";
		case k_EResultAccessDenied:
			return "AccessDenied";
		case k_EResultLimitExceeded:
			return "LimitExceeded";
		case k_EResultNotLoggedOn:
			return "NotLoggedOn";
		default:
			return "Unknown";
		}
	}

	const char* RelayAvailabilityName(ESteamNetworkingAvailability avail) {
		switch (avail) {
		case k_ESteamNetworkingAvailability_Current:
			return "Current";
		case k_ESteamNetworkingAvailability_Waiting:
			return "Waiting";
		case k_ESteamNetworkingAvailability_Attempting:
			return "Attempting";
		case k_ESteamNetworkingAvailability_Retrying:
			return "Retrying";
		case k_ESteamNetworkingAvailability_Failed:
			return "Failed";
		case k_ESteamNetworkingAvailability_CannotTry:
			return "CannotTry";
		default:
			return "Unknown";
		}
	}

	bool WaitForRelayNetwork(int timeoutMs) {
		if (!SteamNetworkingUtils()) {
			SetError("Steam networking utils unavailable");
			return false;
		}
		SteamNetworkingUtils()->InitRelayNetworkAccess();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (std::chrono::steady_clock::now() < deadline) {
			SteamAPI_RunCallbacks();
			if (SteamNetworkingSockets()) {
				SteamNetworkingSockets()->RunCallbacks();
			}
			SteamRelayNetworkStatus_t status = {};
			const ESteamNetworkingAvailability avail = SteamNetworkingUtils()->GetRelayNetworkStatus(&status);
			if (avail == k_ESteamNetworkingAvailability_Current) {
				g_relayReady = true;
				return true;
			}
			if (avail == k_ESteamNetworkingAvailability_Failed || avail == k_ESteamNetworkingAvailability_CannotTry) {
				std::ostringstream os;
				os << "Steam relay network unavailable (" << RelayAvailabilityName(avail) << ")";
				if (status.m_debugMsg[0]) {
					os << ": " << status.m_debugMsg;
				}
				SetError(os.str());
				return false;
			}
			Sleep(50);
		}
		SetError("Timed out waiting for Steam relay network (P2P invites need relay access)");
		return false;
	}

	bool EnsureRelayReady() {
		if (g_relayReady) {
			return true;
		}
		if (!EnsureSteam()) {
			return false;
		}
		return WaitForRelayNetwork(20000);
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
		AcceptPendingMessageSessions();
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
		AcceptPendingMessageSessions();
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
			sf4e::steam_experiment::SteamLaunchReadyPayload launchReady;
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
			else if (sf4e::steam_experiment::DecodeLaunchReady(body, launchReady, err)) {
				item["kind"] = "launch_ready";
				item["launchReady"] = {
					{ "senderSteamId", std::to_string(launchReady.senderSteamId) },
					{ "sessionToken", launchReady.sessionToken }
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

	void SetEvent(const std::string& text) {
		g_state.lastEvent = text;
	}

	void SetError(const std::string& text) {
		g_state.lastError = text;
		g_state.lastEvent = text;
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
			invite["inviteOk"] = false;
			invite["listenOk"] = false;
			return invite;
		}
		nlohmann::json listen = ListenJson(virtualPort);
		nlohmann::json j = Envelope("steamPrepareHost");
		const bool listenOk = listen.value("ok", false);
		j["ok"] = listenOk;
		j["inviteOk"] = true;
		j["listenOk"] = listenOk;
		j["sessionTokenPresent"] = sessionToken && sessionToken[0];
		if (sessionToken && sessionToken[0]) {
			j["sessionToken"] = sessionToken;
		}
		j["virtualPort"] = virtualPort;
		j["targetSteamId"] = std::to_string(targetSteamId);
		return j;
	}

	nlohmann::json SendInviteJson(unsigned long long targetSteamId, int virtualPort, const char* sidecarHash, const char* buildGit, const char* sessionToken) {
		EnsureSteam();
		nlohmann::json j = Envelope("steamInviteSent");
		if (!g_state.initialized) {
			j["ok"] = false;
			j["message"] = g_state.lastError.empty() ? "Steam not initialized" : g_state.lastError;
			return j;
		}
		if (!EnsureRelayReady()) {
			j["ok"] = false;
			j["message"] = g_state.lastError;
			SetEvent("Steam invite send failed");
			return j;
		}
		if (targetSteamId == 0) {
			j["ok"] = false;
			j["message"] = "Invalid target SteamID";
			SetEvent("Steam invite send failed");
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
		SteamNetworkingMessages()->AcceptSessionWithUser(identity);
		const std::string encoded = sf4e::steam_experiment::EncodeInvite(payload);
		const int sendFlags = k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_AutoRestartBrokenSession;
		EResult result = SteamNetworkingMessages()->SendMessageToUser(
			identity,
			encoded.data(),
			(uint32)encoded.size(),
			sendFlags,
			0
		);
		j["ok"] = result == k_EResultOK;
		j["result"] = (int)result;
		j["resultName"] = EResultName(result);
		j["payloadBytes"] = encoded.size();
		j["targetSteamId"] = std::to_string(targetSteamId);
		if (result != k_EResultOK) {
			std::ostringstream os;
			os << "Steam invite send failed (" << EResultName(result) << "=" << (int)result << ")";
			SteamNetConnectionInfo_t info = {};
			const ESteamNetworkingConnectionState sessionState =
				SteamNetworkingMessages()->GetSessionConnectionInfo(identity, &info, nullptr);
			os << " session=" << ConnStateName(sessionState);
			if (info.m_szEndDebug[0]) {
				os << " " << info.m_szEndDebug;
			}
			os << ". Joiner must run this same launcher build with Steam open on the Join tab.";
			j["message"] = os.str();
			SetError(os.str());
			SetEvent("Steam invite send failed");
			spdlog::warn(
				"SendMessageToUser failed target={} result={} session={} relayReady={}",
				targetSteamId,
				(int)result,
				ConnStateName(sessionState),
				g_relayReady
			);
		}
		else {
			j["message"] = "Invite queued — joiner must have this launcher open with Steam running";
			SetEvent("Steam invite sent");
			spdlog::info(
				"SendMessageToUser ok target={} bytes={} sender={}",
				targetSteamId,
				encoded.size(),
				payload.senderSteamId
			);
		}
		return j;
	}

	static unsigned long long LaunchReadySenderFromItem(const nlohmann::json& item) {
		if (item.contains("launchReady") && item["launchReady"].is_object()) {
			const auto& lr = item["launchReady"];
			if (lr.contains("senderSteamId")) {
				if (lr["senderSteamId"].is_string()) {
					try {
						return std::stoull(lr["senderSteamId"].get<std::string>());
					}
					catch (...) {
						return 0;
					}
				}
				if (lr["senderSteamId"].is_number_unsigned()) {
					return lr["senderSteamId"].get<unsigned long long>();
				}
			}
		}
		if (item.contains("fromSteamId")) {
			if (item["fromSteamId"].is_string()) {
				try {
					return std::stoull(item["fromSteamId"].get<std::string>());
				}
				catch (...) {
					return 0;
				}
			}
			if (item["fromSteamId"].is_number_unsigned()) {
				return item["fromSteamId"].get<unsigned long long>();
			}
		}
		return 0;
	}

	static std::string LaunchReadySessionTokenFromItem(const nlohmann::json& item) {
		if (item.contains("launchReady") && item["launchReady"].is_object()) {
			return item["launchReady"].value("sessionToken", "");
		}
		return {};
	}

	static bool ItemIsLaunchReadyFromPeer(
		const nlohmann::json& item,
		unsigned long long expectedPeerSteamId,
		const char* expectedSessionToken
	) {
		if (
			expectedPeerSteamId == 0
			|| !expectedSessionToken
			|| expectedSessionToken[0] == '\0'
			|| item.value("kind", "") != "launch_ready"
		) {
			return false;
		}
		if (LaunchReadySenderFromItem(item) != expectedPeerSteamId) {
			return false;
		}
		return LaunchReadySessionTokenFromItem(item) == expectedSessionToken;
	}

	bool PollPeerLaunchReady(unsigned long long expectedPeerSteamId, const char* sessionToken, int pumpAttempts) {
		if (expectedPeerSteamId == 0 || !sessionToken || sessionToken[0] == '\0' || !g_state.initialized) {
			return false;
		}
		for (int attempt = 0; attempt < pumpAttempts; ++attempt) {
			Pump();
			nlohmann::json batch = DrainMessages();
			if (!batch.is_array()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(40));
				continue;
			}
			for (const auto& item : batch) {
				if (ItemIsLaunchReadyFromPeer(item, expectedPeerSteamId, sessionToken)) {
					spdlog::info(
						"ReceiveLaunchReady from={} (pump attempt {})",
						expectedPeerSteamId,
						attempt + 1
					);
					return true;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(40));
		}
		return false;
	}

	nlohmann::json DrainLaunchReadyMessagesJson() {
		int drained = 0;
		if (g_state.initialized) {
			for (int pass = 0; pass < 16; ++pass) {
				Pump();
				nlohmann::json batch = DrainMessages();
				if (!batch.is_array() || batch.empty()) {
					break;
				}
				for (const auto& item : batch) {
					if (item.value("kind", "") == "launch_ready") {
						drained++;
					}
				}
			}
		}
		nlohmann::json j = Envelope("steamLaunchReadyDrained");
		j["drained"] = drained;
		return j;
	}

	nlohmann::json ConfirmPeerLaunchReadyJson(
		unsigned long long targetSteamId,
		const char* sessionToken,
		int pumpAttempts
	) {
		nlohmann::json j = Envelope("steamConfirmPeerLaunch");
		if (!sessionToken || sessionToken[0] == '\0' || targetSteamId == 0) {
			j["ok"] = false;
			j["peerLaunchReady"] = false;
			j["message"] = "Missing peer SteamID or session token for launch confirm";
			return j;
		}
		ResendLaunchReadyJson(targetSteamId, sessionToken, 4);
		const bool peerReady = PollPeerLaunchReady(targetSteamId, sessionToken, pumpAttempts);
		j["ok"] = peerReady;
		j["peerLaunchReady"] = peerReady;
		if (!peerReady) {
			j["message"] = "Opponent launch-ready not confirmed yet";
		}
		return j;
	}

	nlohmann::json SendLaunchReadyJson(unsigned long long targetSteamId, const char* sessionToken) {
		EnsureSteam();
		nlohmann::json j = Envelope("steamLaunchReady");
		if (!g_state.initialized) {
			j["ok"] = false;
			j["message"] = g_state.lastError.empty() ? "Steam not initialized" : g_state.lastError;
			return j;
		}
		if (!EnsureRelayReady()) {
			j["ok"] = false;
			j["message"] = g_state.lastError;
			return j;
		}
		if (targetSteamId == 0) {
			j["ok"] = false;
			j["message"] = "Invalid peer SteamID for launch ready";
			return j;
		}
		if (!sessionToken || sessionToken[0] == '\0') {
			j["ok"] = false;
			j["message"] = "Missing session token for launch ready";
			return j;
		}

		sf4e::steam_experiment::SteamLaunchReadyPayload payload;
		payload.senderSteamId = SteamUser()->GetSteamID().ConvertToUint64();
		payload.sessionToken = sessionToken;
		SteamNetworkingIdentity identity;
		identity.SetSteamID64(targetSteamId);
		SteamNetworkingMessages()->AcceptSessionWithUser(identity);
		const std::string encoded = sf4e::steam_experiment::EncodeLaunchReady(payload);
		const int sendFlags = k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_AutoRestartBrokenSession;
		const EResult result = SteamNetworkingMessages()->SendMessageToUser(
			identity,
			encoded.data(),
			(uint32)encoded.size(),
			sendFlags,
			0
		);
		j["ok"] = result == k_EResultOK;
		j["result"] = (int)result;
		j["resultName"] = EResultName(result);
		j["targetSteamId"] = std::to_string(targetSteamId);
		if (result != k_EResultOK) {
			j["message"] = std::string("Launch ready send failed (") + EResultName(result) + ")";
			SetEvent("Launch ready send failed");
		}
		else {
			j["message"] = "Launch ready sent — waiting for opponent";
			SetEvent("You are ready to launch");
			spdlog::info("SendLaunchReady ok target={}", targetSteamId);
		}
		return j;
	}

	nlohmann::json ResendLaunchReadyJson(unsigned long long targetSteamId, const char* sessionToken, int repeatCount) {
		nlohmann::json last = SendLaunchReadyJson(targetSteamId, sessionToken);
		if (!last.value("ok", false) || repeatCount <= 1) {
			return last;
		}
		for (int i = 1; i < repeatCount; ++i) {
			Pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(60));
			last = SendLaunchReadyJson(targetSteamId, sessionToken);
		}
		return last;
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
		if (!EnsureRelayReady()) {
			j["ok"] = false;
			j["message"] = g_state.lastError;
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
		if (!EnsureRelayReady()) {
			j["ok"] = false;
			j["message"] = g_state.lastError;
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
