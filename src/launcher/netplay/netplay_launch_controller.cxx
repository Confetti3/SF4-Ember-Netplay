#include "netplay_launch_controller.hxx"



#include <stdio.h>

#include <stdlib.h>

#include <string.h>



#include <windows.h>

#include <pathcch.h>
#include <shlobj.h>
#include <shellapi.h>
#include <strsafe.h>



#include "../common/sf4e__NetUtil.hxx"

#include "connect_strategy.hxx"

#include "netplay_room_code.hxx"

#include "relay/relay_host_spawn.hxx"

#include "netplay_persist.hxx"

#include "room_broker_client.hxx"

#include "update/github_release_client.hxx"

#include <spdlog/spdlog.h>



namespace sf4e {

namespace launcher {



	static const int kProtocolVersion = 1;

	static void ApplyBrokerUrlFromEnvironment(PersistedSettings& settings) {
		const char* env = getenv("SF4E_BROKER_URL");
		if (!env || !env[0]) {
			return;
		}
		BrokerUrlParts parts;
		if (!ParseBrokerBaseUrl(env, parts)) {
			spdlog::warn("SF4E_BROKER_URL ignored (invalid or disallowed): {}", env);
			return;
		}
		strncpy_s(settings.brokerBaseUrl, env, _TRUNCATE);
	}

	static bool IsAllowedExternalUrl(const char* url) {
		if (!url || !url[0]) {
			return false;
		}
		if (_strnicmp(url, "https://", 8) != 0) {
			return false;
		}
		const char* hostStart = url + 8;
		const char* pathStart = strchr(hostStart, '/');
		char host[256] = { 0 };
		if (pathStart) {
			strncpy_s(host, hostStart, pathStart - hostStart);
		}
		else {
			strncpy_s(host, hostStart, _TRUNCATE);
		}

		char* at = strchr(host, '@');
		if (at) {
			memmove(host, at + 1, strlen(at + 1) + 1);
		}
		char* colon = strchr(host, ':');
		if (colon) {
			*colon = 0;
		}

		if (_stricmp(host, "github.com") == 0 || _stricmp(host, "www.github.com") == 0) {
			return true;
		}
		const size_t hostLen = strlen(host);
		if (hostLen > 10 && _stricmp(host + hostLen - 10, ".github.io") == 0) {
			return true;
		}
		return false;
	}

	static bool RequiresProtocolVersion(const std::string& type) {
		return type == "start"
			|| type == "offlineStart"
			|| type == "applyUpdate"
			|| type == "saveSettings"
			|| type == "openUrl"
			|| type == "createRelayRoom"
			|| type == "relayHeartbeat";
	}

	static const char* DefaultConnectMethodString(uint8_t method) {
		switch (method) {
		case 0:
			return "relay";
		case 2:
			return "autoNat";
		default:
			return "direct";
		}
	}



	NetplayLaunchController::NetplayLaunchController(PersistedSettings& settings, NetplayConfig& outConfig)

		: m_settings(settings), m_outConfig(outConfig) {

		m_lanIp[0] = 0;

		sf4e::DetectLanIPv4(m_lanIp, sizeof(m_lanIp));

		ApplyBrokerUrlFromEnvironment(m_settings);

	}

	static RelayRoomCreateResult CreateRelayRoomWithAdvertise(PersistedSettings& settings, const char* displayName) {
		BrokerHealth health;
		FetchBrokerHealth(settings.brokerBaseUrl, health);

		if (health.forceVpsRelay) {
			char sidecarHash[128] = { 0 };
			if (!HashSidecarDllNextToLauncher(sidecarHash, sizeof(sidecarHash))) {
				RelayRoomCreateResult r;
				r.error = "Could not read Sidecar.dll next to Launcher.exe (required for relay). Reinstall the game package.";
				return r;
			}
			return CreateRelayRoom(settings.brokerBaseUrl, displayName, nullptr, sidecarHash);
		}

		char advertiseHost[NETPLAY_SESSION_HOST_LEN] = { 0 };
		FetchAdvertiseRelayHost(advertiseHost, sizeof(advertiseHost));
		return CreateRelayRoom(settings.brokerBaseUrl, displayName, advertiseHost, nullptr);
	}

	void NetplayLaunchController::ApplyConnectPlanToConfig(const ConnectPlanResult& plan) {
		if (!plan.ok) {
			return;
		}
		m_outConfig.ggpoTransport = plan.ggpoTransport;
		m_outConfig.ggpoRemotePort = plan.ggpoRemotePort;
		strncpy_s(m_outConfig.ggpoRemoteHost, plan.ggpoRemoteHost, _TRUNCATE);
		strncpy_s(m_outConfig.matchId, plan.matchId, _TRUNCATE);
		strncpy_s(m_outConfig.ggpoRoomToken, plan.roomToken, _TRUNCATE);
	}

	void NetplayLaunchController::ConfigureGgpoTransportForRelay(const char* roomCode, const char* role) {
		const char* transportEnv = getenv("SF4E_GGPO_TRANSPORT");
		if (transportEnv && _stricmp(transportEnv, "legacy") == 0) {
			m_outConfig.ggpoTransport = 0;
			return;
		}

		const char* hostSecret =
			(role && strcmp(role, "host") == 0 && m_settings.relayHostSecret[0])
				? m_settings.relayHostSecret
				: nullptr;

		ConnectPlanResult plan = FetchConnectPlan(
			m_settings.brokerBaseUrl,
			roomCode,
			role,
			hostSecret,
			m_settings.ggpoPort
		);
		if (plan.ok) {
			ApplyConnectPlanToConfig(plan);
		}
		else {
			spdlog::warn(
				"Connect plan unavailable for {} ({}); applying relay session fallbacks",
				roomCode,
				plan.error.empty() ? "unknown" : plan.error.c_str()
			);
		}
		if (m_sessionRoomToken[0] && !m_outConfig.ggpoRoomToken[0]) {
			strncpy_s(m_outConfig.ggpoRoomToken, m_sessionRoomToken, _TRUNCATE);
		}
		if (m_sessionMatchId[0]) {
			strncpy_s(m_outConfig.matchId, m_sessionMatchId, _TRUNCATE);
		}
		if (m_sessionGgpoPort > 0 && m_outConfig.ggpoRemotePort == 0) {
			m_outConfig.ggpoRemotePort = m_sessionGgpoPort;
			strncpy_s(m_outConfig.ggpoRemoteHost, m_outConfig.sessionHost, _TRUNCATE);
		}
		// Broker auto / udp_relay plan: ensure Sidecar attempts UDP when we have relay credentials.
		if (
			m_outConfig.ggpoTransport == 0
			&& m_outConfig.ggpoRemotePort > 0
			&& m_outConfig.ggpoRoomToken[0]
			&& (!transportEnv || _stricmp(transportEnv, "legacy") != 0)
		) {
			m_outConfig.ggpoTransport = 1;
		}

		if (role && strcmp(role, "host") == 0) {
			m_outConfig.playerRole = 1;
		}
		else if (role && strcmp(role, "guest") == 0) {
			m_outConfig.playerRole = 2;
		}

		if (transportEnv && _stricmp(transportEnv, "udp") == 0) {
			m_outConfig.ggpoTransport = 1;
		}
		else if (transportEnv && _stricmp(transportEnv, "p2p") == 0) {
			m_outConfig.ggpoTransport = 2;
		}
	}

	static bool IsPrivateOrLocalHost(const char* host, const char* lanIp) {
		if (!host || !host[0]) {
			return true;
		}
		if (_stricmp(host, "127.0.0.1") == 0 || _stricmp(host, "localhost") == 0) {
			return true;
		}
		if (lanIp && lanIp[0] && _stricmp(host, lanIp) == 0) {
			return true;
		}

		unsigned a = 0;
		unsigned b = 0;
		unsigned c = 0;
		unsigned d = 0;
		if (sscanf_s(host, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
			return false;
		}
		if (a == 10) {
			return true;
		}
		if (a == 172 && b >= 16 && b <= 31) {
			return true;
		}
		if (a == 192 && b == 168) {
			return true;
		}
		return false;
	}

	static bool ShouldProbeJoinEndpoint(
		const char* connectMethod,
		const char* roomCode,
		const char* host,
		const char* lanIp
	) {
		(void)connectMethod;
		if (!IsShortRoomCode(roomCode)) {
			return false;
		}
		if (IsPrivateOrLocalHost(host, lanIp)) {
			return false;
		}
		return true;
	}



	nlohmann::json NetplayLaunchController::MakeStateEnvelope() const {

		nlohmann::json j;

		j["v"] = kProtocolVersion;

		j["type"] = "state";

		return j;

	}



	std::string NetplayLaunchController::PreviewRoomCode(const char* shareIp, uint16_t port) const {

		char buf[128];

		if (!shareIp || !shareIp[0]) {

			return "(enter internet address)";

		}

		FormatRoomCode(shareIp, port, buf, sizeof(buf));

		return buf;

	}



	nlohmann::json NetplayLaunchController::BuildStateJson() const {

		nlohmann::json j = MakeStateEnvelope();

		j["displayName"] = m_settings.displayName;

		j["inputDelay"] = m_settings.inputDelay;

		j["sessionPort"] = m_settings.sessionPort;

		j["ggpoPort"] = m_settings.ggpoPort;

		j["lanIp"] = m_lanIp;

		j["lastJoinHost"] = m_settings.lastJoinHost;

		j["advertiseHost"] = m_settings.lastAdvertiseHost[0] ? m_settings.lastAdvertiseHost : m_lanIp;

		j["roomCodePreview"] = PreviewRoomCode(

			m_settings.lastAdvertiseHost[0] ? m_settings.lastAdvertiseHost : m_lanIp,

			m_settings.sessionPort

		);

		if (IsShortRoomCode(m_settings.relayRoomCode)) {

			j["roomCodePreview"] = m_settings.relayRoomCode;

		}

		j["lanRoomCode"] = PreviewRoomCode(m_lanIp, m_settings.sessionPort);

		j["lanAddress"] = j["lanRoomCode"];

		const char* advertiseHost = m_settings.lastAdvertiseHost[0] ? m_settings.lastAdvertiseHost : m_lanIp;

		j["wanAddress"] = PreviewRoomCode(advertiseHost, m_settings.sessionPort);

		j["simpleUi"] = m_settings.simpleUi != 0;

		j["brokerBaseUrl"] = m_settings.brokerBaseUrl;

		j["defaultConnectMethod"] = m_settings.defaultConnectMethod;

		BrokerHealth brokerHealth;
		if (FetchBrokerHealth(m_settings.brokerBaseUrl, brokerHealth)) {
			j["forceVpsRelay"] = brokerHealth.forceVpsRelay;
		}

		j["featureFlags"] = {

			{ "matchmaking", true },

			{ "autoNat", true },

			{ "relayRoom", true },

		};

		char installedVersion[64] = { 0 };

		ReadInstalledVersion(installedVersion, sizeof(installedVersion));

		j["installedVersion"] = installedVersion;

		return j;

	}

	nlohmann::json NetplayLaunchController::HandleWebMessage(const nlohmann::json& msg) {

		const std::string type = msg.value("type", "");

		if (RequiresProtocolVersion(type)) {
			const int version = msg.value("v", 0);
			if (version != kProtocolVersion) {
				nlohmann::json err;
				err["v"] = kProtocolVersion;
				err["type"] = "error";
				err["message"] = "Launcher UI protocol mismatch. Restart the launcher.";
				return err;
			}
		}

		if (type == "getState") {

			return BuildStateJson();

		}

		if (type == "fetchPublicIp") {

			char publicIp[NETPLAY_SESSION_HOST_LEN] = { 0 };

			bool ok = sf4e::FetchPublicIPv4(publicIp, sizeof(publicIp), 5000);

			if (!ok && m_settings.lastAdvertiseHost[0]) {

				strncpy_s(publicIp, m_settings.lastAdvertiseHost, _TRUNCATE);

				ok = true;

			}

			if (ok) {

				strncpy_s(m_settings.lastAdvertiseHost, publicIp, _TRUNCATE);

			}

			nlohmann::json r = MakeStateEnvelope();

			if (!ok) {

				nlohmann::json err;

				err["v"] = kProtocolVersion;

				err["type"] = "error";

				err["message"] = "Could not detect public IP. Enter your public or VPN address manually.";

				return err;

			}

			r["advertiseHost"] = publicIp;

			r["roomCodePreview"] = PreviewRoomCode(publicIp, m_settings.sessionPort);

			return r;

		}



		if (type == "copyText") {

			std::string text = msg.value("text", "");

			bool ok = sf4e::CopyTextToClipboardUtf8(text.c_str());

			nlohmann::json r;

			r["v"] = kProtocolVersion;

			r["type"] = ok ? "copied" : "error";

			if (!ok) {

				r["message"] = "Could not copy to clipboard.";

			}

			return r;

		}

		if (type == "openUrl") {

			std::string url = msg.value("url", "");

			if (url.empty()) {

				nlohmann::json err;

				err["v"] = kProtocolVersion;

				err["type"] = "error";

				err["message"] = "No URL to open.";

				return err;

			}

			if (!IsAllowedExternalUrl(url.c_str())) {

				nlohmann::json err;

				err["v"] = kProtocolVersion;

				err["type"] = "error";

				err["message"] = "Only HTTPS GitHub links can be opened from the launcher.";

				return err;

			}

			wchar_t wUrl[2048] = { 0 };

			MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wUrl, 2048);

			HINSTANCE rc = ShellExecuteW(NULL, L"open", wUrl, NULL, NULL, SW_SHOWNORMAL);

			nlohmann::json r;

			r["v"] = kProtocolVersion;

			r["type"] = (INT_PTR)rc > 32 ? "openedUrl" : "error";

			if ((INT_PTR)rc <= 32) {

				r["message"] = "Could not open link in browser.";

			}

			return r;

		}



		if (type == "previewRoomCode") {

			std::string shareIp = msg.value("advertiseHost", "");

			int port = msg.value("sessionPort", (int)m_settings.sessionPort);

			if (port < 1 || port > 65535) {

				port = m_settings.sessionPort;

			}

			nlohmann::json r = MakeStateEnvelope();

			r["roomCodePreview"] = PreviewRoomCode(shareIp.c_str(), (uint16_t)port);

			return r;

		}

		if (type == "setUiMode") {

			m_settings.simpleUi = msg.value("simpleUi", true) ? 1 : 0;

			nlohmann::json r = BuildStateJson();

			return r;

		}

		if (type == "saveSettings") {

			if (msg.contains("brokerBaseUrl")) {

				std::string broker = msg.value("brokerBaseUrl", m_settings.brokerBaseUrl);

				BrokerUrlParts parts;
				if (!ParseBrokerBaseUrl(broker.c_str(), parts)) {

					nlohmann::json err;

					err["v"] = kProtocolVersion;

					err["type"] = "error";

					err["message"] = "Invalid or disallowed room broker URL. Use a public http(s) address.";

					return err;

				}

				strncpy_s(m_settings.brokerBaseUrl, broker.c_str(), _TRUNCATE);

			}

			if (msg.contains("defaultConnectMethod")) {

				m_settings.defaultConnectMethod = (uint8_t)msg.value("defaultConnectMethod", (int)m_settings.defaultConnectMethod);

			}

			return BuildStateJson();

		}

		if (type == "checkUpdate") {

			UpdateCheckResult check = CheckForUpdate();

			nlohmann::json r;

			r["v"] = kProtocolVersion;

			r["type"] = "updateCheck";

			r["installedVersion"] = check.installedVersion;

			if (!check.ok) {

				r["ok"] = false;

				r["error"] = check.error;

				return r;

			}

			r["ok"] = true;

			r["latestVersion"] = check.latestVersion;

			r["updateAvailable"] = check.updateAvailable;

			r["releaseNotes"] = check.releaseNotes;

			r["releaseUrl"] = check.releaseUrl;

			r["zipDownloadUrl"] = check.zipDownloadUrl;

			r["zipApiUrl"] = check.zipApiUrl;

			return r;

		}

		if (type == "applyUpdate") {

			if (IsGameProcessRunning()) {

				nlohmann::json err;

				err["v"] = kProtocolVersion;

				err["type"] = "error";

				err["message"] = "Close Ultra Street Fighter IV before installing an update.";

				return err;

			}

			UpdateCheckResult check = CheckForUpdate();

			if (!check.ok) {

				nlohmann::json err;

				err["v"] = kProtocolVersion;

				err["type"] = "error";

				err["message"] = check.error;

				return err;

			}

			if (!check.updateAvailable) {

				nlohmann::json err;

				err["v"] = kProtocolVersion;

				err["type"] = "error";

				err["message"] = "No update available.";

				return err;

			}

			ApplyUpdateResult applied = DownloadAndApplyUpdate(
				check.zipDownloadUrl.c_str(),
				check.zipApiUrl.c_str(),
				check.latestVersion.c_str(),
				check.expectedSha256.c_str()
			);

			if (!applied.ok) {

				nlohmann::json err;

				err["v"] = kProtocolVersion;

				err["type"] = "error";

				err["message"] = applied.error;

				return err;

			}

			m_exitForUpdate = true;

			nlohmann::json r;

			r["v"] = kProtocolVersion;

			r["type"] = "updateApply";

			r["ok"] = true;

			r["message"] = "Update downloaded. The launcher will close and restart.";

			return r;

		}

		if (type == "createRelayRoom") {

			std::string name = msg.value("displayName", m_settings.displayName);

			RelayRoomCreateResult created = CreateRelayRoomWithAdvertise(m_settings, name.c_str());

			if (!created.ok) {

				nlohmann::json err;

				err["v"] = kProtocolVersion;

				err["type"] = "error";

				err["message"] = created.error;

				return err;

			}

			strncpy_s(m_settings.relayRoomCode, created.shortCode.c_str(), _TRUNCATE);

			m_settings.relaySessionPort = created.relayPort;

			strncpy_s(m_settings.relayHostSecret, created.hostSecret.c_str(), _TRUNCATE);

			strncpy_s(m_sessionMatchId, created.matchId, _TRUNCATE);

			strncpy_s(m_sessionRoomToken, created.roomToken, _TRUNCATE);

			m_sessionGgpoPort = created.ggpoPort;

			BrokerHealth brokerHealth;
			const bool vpsRelay = FetchBrokerHealth(m_settings.brokerBaseUrl, brokerHealth) && brokerHealth.forceVpsRelay;

			nlohmann::json r = MakeStateEnvelope();

			r["roomCodePreview"] = created.shortCode;

			r["relayHost"] = created.relayHost;

			r["relayPort"] = created.relayPort;

			r["forceVpsRelay"] = vpsRelay;

			char statusMsg[64];
			snprintf(
				statusMsg,
				sizeof(statusMsg),
				"Room live — %s",
				created.shortCode.c_str()
			);
			r["connectionStatus"] = statusMsg;

			return r;

		}

		if (type == "relayHeartbeat") {

			std::string code = msg.value("roomCode", m_settings.relayRoomCode);

			nlohmann::json r = MakeStateEnvelope();

			r["heartbeatOk"] = HeartbeatRelayRoom(
				m_settings.brokerBaseUrl,
				code.c_str(),
				m_settings.relayHostSecret[0] ? m_settings.relayHostSecret : nullptr
			);

			return r;

		}

		if (type == "tryUpnp") {

			int port = msg.value("sessionPort", (int)m_settings.sessionPort);

			HostNatResult nat = TryConfigureHostUpnp((uint16_t)port, m_settings.ggpoPort);

			nlohmann::json r = MakeStateEnvelope();

			r["natStatus"] = nat.status;

			r["natDetail"] = nat.detail;

			r["natOk"] = nat.ok;

			return r;

		}

		if (type == "listRooms") {

			BrokerUrlParts parts;

			nlohmann::json r = MakeStateEnvelope();

			r["rooms"] = nlohmann::json::array();

			if (!ParseBrokerBaseUrl(m_settings.brokerBaseUrl, parts)) {

				r["listError"] = "Room broker URL is not set. Use Advanced → Room broker URL or SF4E_BROKER_URL.";

				return r;

			}

			char body[8192] = { 0 };

			if (!BrokerHttpGet(parts, "/v1/rooms", body, sizeof(body))) {

				r["listError"] = "Could not load open rooms.";

				return r;

			}

			try {

				nlohmann::json j = nlohmann::json::parse(body);

				if (j.contains("rooms") && j["rooms"].is_array()) {

					r["rooms"] = j["rooms"];

				}

			}

			catch (...) {

				r["listError"] = "Invalid room list from service.";

			}

			return r;

		}



		if (type == "cancel") {

			m_cancelled = true;

			m_finished = true;

			return nlohmann::json();

		}



		if (type == "offlineStart") {

			std::string name = msg.value("displayName", m_settings.displayName);

			if (name.empty()) {

				name = "Offline Tester";

			}

			strncpy_s(m_settings.displayName, name.c_str(), _TRUNCATE);



			int delay = msg.value("inputDelay", (int)m_settings.inputDelay);

			if (delay < 1) {

				delay = 1;

			}

			else if (delay > 10) {

				delay = 10;

			}

			m_settings.inputDelay = (uint8_t)delay;



			memset(&m_outConfig, 0, sizeof(m_outConfig));

			m_outConfig.version = SF4E_NETPLAY_CONFIG_VERSION;

			m_outConfig.mode = (int)NetplayMode::Idle;

			strncpy_s(m_outConfig.displayName, m_settings.displayName, _TRUNCATE);

			m_outConfig.inputDelay = m_settings.inputDelay;

			m_outConfig.sessionPort = m_settings.sessionPort;

			m_outConfig.ggpoPort = m_settings.ggpoPort;

			m_outConfig.editionSelect = m_settings.editionSelect;

			m_outConfig.roundCount = m_settings.roundCount;

			m_outConfig.roundTimeIntegral = m_settings.roundTimeIntegral;

			m_outConfig.deviceIdx = 0xff;

			m_outConfig.deviceType = 0xff;

			NetplayConfigApplyGgpoDisconnectDefaults(m_outConfig);

			m_outConfig.devOverlay = msg.value("devOverlay", true) ? 1 : 0;

			spdlog::info(
				"Controller offlineStart displayName={} inputDelay={} devOverlay={} configVersion={}",
				m_outConfig.displayName,
				(int)m_outConfig.inputDelay,
				(int)m_outConfig.devOverlay,
				m_outConfig.version
			);

			m_finished = true;

			m_cancelled = false;

			return nlohmann::json();

		}



		if (type == "start") {

			const std::string modeStr = msg.value("mode", "offline");

			int mode = (int)NetplayMode::Idle;

			if (modeStr == "host") {

				mode = (int)NetplayMode::Host;

			}

			else if (modeStr == "join") {

				mode = (int)NetplayMode::Join;

			}



			std::string name = msg.value("displayName", m_settings.displayName);

			strncpy_s(m_settings.displayName, name.c_str(), _TRUNCATE);



			int delay = msg.value("inputDelay", (int)m_settings.inputDelay);

			if (delay < 1) {

				delay = 1;

			}

			m_settings.inputDelay = (uint8_t)delay;



			int port = msg.value("sessionPort", (int)m_settings.sessionPort);

			if (port < 1 || port > 65535) {

				nlohmann::json err;

				err["v"] = kProtocolVersion;

				err["type"] = "error";

				err["message"] = "Session port must be between 1 and 65535.";

				return err;

			}

			m_settings.sessionPort = (uint16_t)port;



			memset(&m_outConfig, 0, sizeof(m_outConfig));

			m_outConfig.version = SF4E_NETPLAY_CONFIG_VERSION;

			m_outConfig.mode = mode;

			strncpy_s(m_outConfig.displayName, m_settings.displayName, _TRUNCATE);

			m_outConfig.inputDelay = m_settings.inputDelay;

			m_outConfig.sessionPort = m_settings.sessionPort;

			m_outConfig.ggpoPort = m_settings.ggpoPort;

			m_outConfig.editionSelect = m_settings.editionSelect;

			m_outConfig.roundCount = m_settings.roundCount;

			m_outConfig.roundTimeIntegral = m_settings.roundTimeIntegral;

			m_outConfig.trainingMode = msg.value("trainingMode", false) ? 1 : 0;

			m_outConfig.useRelay = m_settings.useRelay;

			m_outConfig.deviceIdx = 0xff;

			m_outConfig.deviceType = 0xff;

			NetplayConfigApplyGgpoDisconnectDefaults(m_outConfig);



			const std::string connectMethod = msg.value("connectMethod",
				DefaultConnectMethodString(m_settings.defaultConnectMethod));
			spdlog::info(
				"Controller start mode={} connectMethod={} inputDelay={} sessionPort={} ggpoPort={}",
				mode,
				connectMethod,
				(int)m_outConfig.inputDelay,
				m_outConfig.sessionPort,
				m_outConfig.ggpoPort
			);

			if (mode == (int)NetplayMode::Host) {

				strncpy_s(m_outConfig.roomKey, "1", _TRUNCATE);

				std::string relayCode = msg.value("relayRoomCode", "");

				if (relayCode.empty() && m_settings.relayRoomCode[0]) {

					relayCode = m_settings.relayRoomCode;

				}

				std::string hostConnectMethod = connectMethod;

				if (hostConnectMethod == "relay") {

					if (relayCode.empty()) {

						nlohmann::json err;

						err["v"] = kProtocolVersion;

						err["type"] = "error";

						err["message"] = "Click Get code before starting.";

						return err;

					}

					{

						JoinRequest lookup;

						lookup.roomCode = relayCode;

						StrategyResult sr = ResolveJoinRelayRoom(lookup, m_settings.brokerBaseUrl);

						if (!sr.ok) {

							nlohmann::json err;

							err["v"] = kProtocolVersion;

							err["type"] = "error";

							err["message"] = sr.error;

							return err;

						}

						strncpy_s(m_outConfig.sessionHost, sr.endpoint.host, _TRUNCATE);

						m_outConfig.sessionPort = sr.endpoint.port;

						m_settings.relaySessionPort = sr.endpoint.port;

						strncpy_s(m_settings.relayRoomCode, relayCode.c_str(), _TRUNCATE);

					}

					m_outConfig.useCentralSession = 1;

					BrokerHealth brokerHealth;
					FetchBrokerHealth(m_settings.brokerBaseUrl, brokerHealth);
					if (brokerHealth.forceVpsRelay) {
						m_outConfig.useCentralSession = 2;
					}

					spdlog::info(
						"Host relay start: code={} endpoint={}:{} centralSession={}",
						relayCode,
						m_outConfig.sessionHost,
						m_outConfig.sessionPort,
						(int)m_outConfig.useCentralSession
					);

					strncpy_s(m_outConfig.relayRoomCode, relayCode.c_str(), _TRUNCATE);

					if (m_outConfig.useCentralSession == 2) {
						ConfigureGgpoTransportForRelay(relayCode.c_str(), "host");
					}

					if (m_outConfig.useCentralSession == 2) {
						// VPS-hosted session relay — host connects outbound; no local RelayHost or UPnP.
					}
					else {
						TryConfigureHostUpnp(m_outConfig.sessionPort, m_settings.ggpoPort);

						if (!SpawnRelayHost(m_outConfig.sessionPort, nullptr)) {

							spdlog::error("SpawnRelayHost failed on port {}", m_outConfig.sessionPort);

							nlohmann::json err;

							err["v"] = kProtocolVersion;

							err["type"] = "error";

							err["message"] = "Could not start RelayHost.exe. Rebuild/install, keep RelayHost.exe next to Launcher.exe, and ensure the session port is free.";

							return err;

						}

						spdlog::info("SpawnRelayHost succeeded on port {} (pid {})", m_outConfig.sessionPort, GetRelayHostPid());
					}

				}

				else {

					m_outConfig.useCentralSession = 0;

					strncpy_s(m_outConfig.sessionHost, m_lanIp, _TRUNCATE);

					std::string advertise = msg.value("advertiseHost", "");

					char advBuf[NETPLAY_SESSION_HOST_LEN] = { 0 };

					strncpy_s(advBuf, advertise.c_str(), _TRUNCATE);

					sf4e::TrimRoomCodeInPlace(advBuf);

					if (advBuf[0]) {

						strncpy_s(m_settings.lastAdvertiseHost, advBuf, _TRUNCATE);

					}

					if (connectMethod == "autoNat" || msg.value("tryUpnp", false)) {

						TryConfigureHostUpnp(m_settings.sessionPort, m_settings.ggpoPort);

					}

				}

			}

			else if (mode == (int)NetplayMode::Join) {

				JoinRequest req;

				req.roomCode = msg.value("joinAddress", "");

				if (req.roomCode.empty()) {

					req.roomCode = msg.value("roomCode", "");

				}

				StrategyResult sr;

				if (connectMethod == "matchmaking") {

					sr = ResolveJoinMatchmaking(req, m_settings.brokerBaseUrl, m_settings.displayName);

				}

				else if (connectMethod == "autoNat") {

					sr = ResolveJoinAutoNat(req);

				}

				else if (IsShortRoomCode(req.roomCode.c_str())) {

					sr = ResolveJoinRelayRoom(req, m_settings.brokerBaseUrl);

				}

				else {

					sr = ResolveJoinDirectIp(req);

				}

				if (!sr.ok) {

					nlohmann::json err;

					err["v"] = kProtocolVersion;

					err["type"] = "error";

					err["message"] = sr.error;

					return err;

				}

				strncpy_s(m_outConfig.sessionHost, sr.endpoint.host, _TRUNCATE);

				m_outConfig.sessionPort = sr.endpoint.port;

				m_outConfig.useCentralSession = 0;

				BrokerHealth joinBrokerHealth;
				const bool vpsRelay =
					FetchBrokerHealth(m_settings.brokerBaseUrl, joinBrokerHealth) && joinBrokerHealth.forceVpsRelay;
				const bool vpsRelayJoin =
					vpsRelay
					&& (IsShortRoomCode(req.roomCode.c_str()) || connectMethod == "matchmaking");
				if (vpsRelayJoin) {
					m_outConfig.useCentralSession = 2;
				}

				if (IsShortRoomCode(req.roomCode.c_str())) {
					strncpy_s(m_outConfig.relayRoomCode, req.roomCode.c_str(), _TRUNCATE);
				}

				if (vpsRelayJoin && m_outConfig.relayRoomCode[0]) {
					ConfigureGgpoTransportForRelay(m_outConfig.relayRoomCode, "guest");
				}

				if (ShouldProbeJoinEndpoint(connectMethod.c_str(), req.roomCode.c_str(), sr.endpoint.host, m_lanIp)) {
					if (!vpsRelay && !sf4e::ProbeRemoteTcpConnect(sr.endpoint.host, sr.endpoint.port, 5000)) {
						nlohmann::json err;

						err["v"] = kProtocolVersion;

						err["type"] = "error";

						char errMsg[512];
						snprintf(
							errMsg,
							sizeof(errMsg),
							"Cannot reach host at %s:%u. Ask the host to forward TCP+UDP %u on their router and allow RelayHost in Windows Firewall. Host must click Start game first.",
							sr.endpoint.host,
							(unsigned)sr.endpoint.port,
							(unsigned)sr.endpoint.port
						);
						err["message"] = errMsg;

						return err;
					}
				}

				if (IsShortRoomCode(req.roomCode.c_str())) {

					strncpy_s(m_settings.lastJoinHost, req.roomCode.c_str(), _TRUNCATE);

				}

				else {

					FormatRoomCode(

						m_outConfig.sessionHost,

						m_outConfig.sessionPort,

						m_settings.lastJoinHost,

						sizeof(m_settings.lastJoinHost)

					);

				}

			}



			if (m_outConfig.useCentralSession == 2 && m_outConfig.relayRoomCode[0]) {
				const char* transportLabel =
					m_outConfig.ggpoTransport == 2
						? "p2p"
						: (m_outConfig.ggpoTransport == 1 ? "udp_relay" : "legacy_session_tunnel");
				PostRoomEvent(
					m_settings.brokerBaseUrl,
					m_outConfig.relayRoomCode,
					"battle_start",
					m_settings.relayHostSecret[0] ? m_settings.relayHostSecret : nullptr,
					transportLabel,
					nullptr
				);
			}

			m_finished = true;

			m_cancelled = false;

			return nlohmann::json();

		}



		nlohmann::json err;

		err["v"] = kProtocolVersion;

		err["type"] = "error";

		err["message"] = "Unknown message type.";

		return err;

	}

} // namespace launcher

} // namespace sf4e

