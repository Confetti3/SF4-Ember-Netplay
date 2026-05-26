#include "netplay_launch_controller.hxx"



#include <stdio.h>

#include <stdlib.h>

#include <string.h>



#include <windows.h>

#include <pathcch.h>
#include <shlobj.h>
#include <strsafe.h>



#include "../common/sf4e__NetUtil.hxx"

#include "connect_strategy.hxx"

#include "netplay_room_code.hxx"

#include "relay_host_spawn.hxx"

#include "netplay_persist.hxx"

#include "room_broker_client.hxx"

#include "github_release_client.hxx"

#include <spdlog/spdlog.h>



namespace sf4e {

namespace launcher {



	static const int kProtocolVersion = 1;

	static void ApplyBrokerUrlFromEnvironment(PersistedSettings& settings) {
		const char* env = getenv("SF4E_BROKER_URL");
		if (env && env[0]) {
			strncpy_s(settings.brokerBaseUrl, env, _TRUNCATE);
		}
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
		char advertiseHost[NETPLAY_SESSION_HOST_LEN] = { 0 };
		FetchAdvertiseRelayHost(advertiseHost, sizeof(advertiseHost));
		return CreateRelayRoom(settings.brokerBaseUrl, displayName, advertiseHost);
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

		j["lanRoomCode"] = PreviewRoomCode(m_lanIp, m_settings.sessionPort);

		j["lanAddress"] = j["lanRoomCode"];

		const char* advertiseHost = m_settings.lastAdvertiseHost[0] ? m_settings.lastAdvertiseHost : m_lanIp;

		j["wanAddress"] = PreviewRoomCode(advertiseHost, m_settings.sessionPort);

		j["simpleUi"] = m_settings.simpleUi != 0;

		j["brokerBaseUrl"] = m_settings.brokerBaseUrl;

		j["defaultConnectMethod"] = m_settings.defaultConnectMethod;

		if (m_settings.relayRoomCode[0]) {

			j["roomCodePreview"] = m_settings.relayRoomCode;

		}

		if (m_settings.relaySessionPort > 0) {

			j["relayPort"] = m_settings.relaySessionPort;

		}

		j["featureFlags"] = {

			{ "matchmaking", true },

			{ "autoNat", true },

			{ "relayRoom", true }

		};

		char installedVersion[64] = { 0 };

		ReadInstalledVersion(installedVersion, sizeof(installedVersion));

		j["installedVersion"] = installedVersion;

		return j;

	}



	nlohmann::json NetplayLaunchController::HandleWebMessage(const nlohmann::json& msg) {

		const std::string type = msg.value("type", "");

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

			std::string zipUrl = msg.value("zipDownloadUrl", "");

			std::string latestTag = msg.value("latestVersion", "");

			if (zipUrl.empty() || latestTag.empty()) {

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

				zipUrl = check.zipDownloadUrl;

				latestTag = check.latestVersion;

			}

			ApplyUpdateResult applied = DownloadAndApplyUpdate(zipUrl.c_str(), latestTag.c_str());

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

			SavePersistedSettings(m_settings);

			nlohmann::json r = MakeStateEnvelope();

			r["roomCodePreview"] = created.shortCode;

			r["relayHost"] = created.relayHost;

			r["relayPort"] = created.relayPort;

			r["connectionStatus"] = "Relay room ready — share the code with your opponent.";

			return r;

		}

		if (type == "relayHeartbeat") {

			std::string code = msg.value("roomCode", m_settings.relayRoomCode);

			nlohmann::json r = MakeStateEnvelope();

			r["heartbeatOk"] = HeartbeatRelayRoom(m_settings.brokerBaseUrl, code.c_str());

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

				r["listError"] = "Room service URL is not configured.";

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

			m_outConfig.useRelay = m_settings.useRelay;

			m_outConfig.deviceIdx = 0xff;

			m_outConfig.deviceType = 0xff;



			const std::string connectMethod = msg.value("connectMethod",
				DefaultConnectMethodString(m_settings.defaultConnectMethod));

			if (mode == (int)NetplayMode::Host) {

				strncpy_s(m_outConfig.roomKey, "1", _TRUNCATE);

				std::string relayCode = msg.value("relayRoomCode", "");

				if (relayCode.empty() && m_settings.relayRoomCode[0]) {

					relayCode = m_settings.relayRoomCode;

				}

				std::string hostConnectMethod = connectMethod;

				if (hostConnectMethod != "relay" && IsShortRoomCode(relayCode.c_str())) {

					spdlog::info("Host start: forcing relay mode for room code {}", relayCode);

					hostConnectMethod = "relay";

				}

				if (hostConnectMethod == "relay") {

					if (relayCode.empty()) {

						RelayRoomCreateResult created = CreateRelayRoomWithAdvertise(m_settings, m_settings.displayName);

						if (!created.ok) {

							nlohmann::json err;

							err["v"] = kProtocolVersion;

							err["type"] = "error";

							err["message"] = created.error;

							return err;

						}

						relayCode = created.shortCode;

						strncpy_s(m_settings.relayRoomCode, relayCode.c_str(), _TRUNCATE);

						m_settings.relaySessionPort = created.relayPort;

						strncpy_s(m_outConfig.sessionHost, created.relayHost, _TRUNCATE);

						m_outConfig.sessionPort = created.relayPort;

					}

					else {

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

					spdlog::info(
						"Host relay start: code={} endpoint={}:{} centralSession=1",
						relayCode,
						m_outConfig.sessionHost,
						m_outConfig.sessionPort
					);

					TryConfigureHostUpnp(m_outConfig.sessionPort, m_settings.ggpoPort);

					if (!SpawnRelayHost(m_outConfig.sessionPort, nullptr)) {

						spdlog::error("SpawnRelayHost failed on port {}", m_outConfig.sessionPort);

						nlohmann::json err;

						err["v"] = kProtocolVersion;

						err["type"] = "error";

						err["message"] = "Could not start RelayHost.exe. Rebuild/install and keep RelayHost.exe next to Launcher.exe.";

						return err;

					}

					spdlog::info("SpawnRelayHost succeeded on port {} (pid {})", m_outConfig.sessionPort, GetRelayHostPid());

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

				else if (connectMethod == "relay" || IsShortRoomCode(req.roomCode.c_str())) {

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



	bool GetLauncherUiIndexUrl(wchar_t* outUrl, int outUrlChars) {

		if (!outUrl || outUrlChars <= 0) {

			return false;

		}

		wchar_t launcherDir[MAX_PATH] = { 0 };

		wchar_t indexPath[MAX_PATH] = { 0 };

		GetModuleFileNameW(NULL, launcherDir, MAX_PATH);

		PathCchRemoveFileSpec(launcherDir, MAX_PATH);

		if (FAILED(PathCchCombine(indexPath, MAX_PATH, launcherDir, L"launcher-ui\\index.html"))) {

			return false;

		}

		wchar_t url[MAX_PATH * 2] = { 0 };

		StringCchPrintfW(url, (int)(sizeof(url) / sizeof(url[0])), L"file:///%s", indexPath);

		for (wchar_t* p = url; *p; ++p) {

			if (*p == L'\\') {

				*p = L'/';

			}

		}

		StringCchCopyW(outUrl, outUrlChars, url);

		return true;

	}



} // namespace launcher

} // namespace sf4e

