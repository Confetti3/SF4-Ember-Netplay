#include "connect_strategy.hxx"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "../common/sf4e__NetUtil.hxx"
#include "netplay_room_code.hxx"
#include "relay_host_spawn.hxx"
#include "room_broker_client.hxx"
#include "upnp_portmap.hxx"

namespace sf4e {
namespace launcher {

	static std::string BrokerMessageFromBody(const char* body) {
		if (!body || !body[0]) {
			return "";
		}
		try {
			nlohmann::json j = nlohmann::json::parse(body);
			return j.value("message", "");
		}
		catch (...) {
			return "";
		}
	}

	static std::string FormatBrokerPostError(
		const char* brokerBaseUrl,
		const sf4e::HttpRequestResult& http,
		const char* body
	) {
		const char* url = brokerBaseUrl && brokerBaseUrl[0] ? brokerBaseUrl : "room broker";
		if (http.error == sf4e::HttpErrorKind::Timeout) {
			return std::string("Room broker unreachable at ") + url + ". Check your internet or try again later.";
		}
		if (http.error == sf4e::HttpErrorKind::ConnectFailed) {
			return std::string("Room broker refused connection at ") + url + ".";
		}
		if (http.error == sf4e::HttpErrorKind::HttpStatus) {
			std::string detail = BrokerMessageFromBody(body);
			if (!detail.empty()) {
				return detail;
			}
			char buf[160];
			snprintf(buf, sizeof(buf), "Room broker at %s returned HTTP %d.", url, http.statusCode);
			return buf;
		}
		return std::string("Could not reach room broker at ") + url + ". Is the broker running?";
	}

	StrategyResult ResolveJoinDirectIp(const JoinRequest& request) {
		StrategyResult r;
		if (request.roomCode.empty()) {
			r.error = "Enter a room code or host address.";
			return r;
		}
		if (IsShortRoomCode(request.roomCode.c_str())) {
			r.error = "Short room codes require Relay mode. Switch to Relay or paste IP:port in Advanced.";
			return r;
		}
		uint16_t port = 0;
		if (!ParseRoomCode(request.roomCode.c_str(), r.endpoint.host, sizeof(r.endpoint.host), &port)) {
			r.error = "Invalid address. Use SF4-XXXX (relay) or IP:port in Advanced.";
			return r;
		}
		r.endpoint.port = port;
		r.ok = true;
		return r;
	}

	StrategyResult ResolveJoinRelayRoom(const JoinRequest& request, const char* brokerBaseUrl) {
		StrategyResult r;
		if (request.roomCode.empty()) {
			r.error = "Enter a room code (example SF4-A1B2C3D4E5F67890).";
			return r;
		}

		char normalizedCode[32] = { 0 };
		if (!NormalizeRelayRoomCode(request.roomCode.c_str(), normalizedCode, sizeof(normalizedCode))) {
			r.error = "Invalid room code. Use the SF4- code from your host.";
			return r;
		}

		BrokerUrlParts parts;
		if (!ParseBrokerBaseUrl(brokerBaseUrl, parts)) {
			r.error = "Room broker URL is not set. Use Advanced → Room broker URL or SF4E_BROKER_URL.";
			return r;
		}

		char path[160];
		snprintf(path, sizeof(path), "/v1/rooms/%s", normalizedCode);

		char body[4096] = { 0 };
		if (!BrokerHttpGet(parts, path, body, sizeof(body))) {
			r.error = "Cannot reach the room broker. Check your internet or broker URL.";
			return r;
		}

		try {
			nlohmann::json j = nlohmann::json::parse(body);
			if (j.value("error", "").size()) {
				r.error = j.value("message", "Room not found or expired. Ask the host to create a new SF4- room code.");
				return r;
			}
			std::string host = j.value("host", "");
			int port = j.value("port", 0);
			if (host.empty() || port < 1 || port > 65535) {
				r.error = "Room broker returned an invalid address.";
				return r;
			}
			strncpy_s(r.endpoint.host, host.c_str(), _TRUNCATE);
			r.endpoint.port = (uint16_t)port;
			r.ok = true;
			return r;
		}
		catch (...) {
			r.error = "Room broker returned invalid data.";
			return r;
		}
	}

	StrategyResult ResolveJoinMatchmaking(const JoinRequest& request, const char* brokerBaseUrl, const char* displayName) {
		(void)request;
		StrategyResult r;

		BrokerUrlParts parts;
		if (!ParseBrokerBaseUrl(brokerBaseUrl, parts)) {
			r.error = "Matchmaking requires a room broker URL.";
			return r;
		}

		BrokerHealth health;
		const bool vpsRelay = FetchBrokerHealth(brokerBaseUrl, health) && health.forceVpsRelay;

		nlohmann::json req;
		req["displayName"] = displayName && displayName[0] ? displayName : "Player";
		if (vpsRelay) {
			char sidecarHash[128] = { 0 };
			if (!HashSidecarDllNextToLauncher(sidecarHash, sizeof(sidecarHash))) {
				r.error = "Could not read Sidecar.dll next to Launcher.exe (required for relay). Reinstall the game package.";
				return r;
			}
			req["sidecarHash"] = sidecarHash;
		}
		std::string reqBody = req.dump();

		char body[4096] = { 0 };
		if (!BrokerHttpPostJson(parts, "/v1/queue/join", reqBody.c_str(), body, sizeof(body))) {
			r.error = "Could not reach matchmaking. Try Host with a room code instead.";
			return r;
		}

		try {
			nlohmann::json j = nlohmann::json::parse(body);
			if (j.value("status", "") == "waiting") {
				r.error = "Still looking for an opponent. Try again in a few seconds.";
				return r;
			}
			if (j.value("error", "").size()) {
				r.error = j.value("message", "No match available right now.");
				return r;
			}
			std::string host = j.value("host", "");
			int port = j.value("port", 0);
			std::string code = j.value("code", "");
			if (!code.empty()) {
				// Matched players join the relay room together.
				JoinRequest jr;
				jr.roomCode = code;
				return ResolveJoinRelayRoom(jr, brokerBaseUrl);
			}
			if (host.empty() || port < 1) {
				r.error = "Matchmaking did not return a room.";
				return r;
			}
			strncpy_s(r.endpoint.host, host.c_str(), _TRUNCATE);
			r.endpoint.port = (uint16_t)port;
			r.ok = true;
			return r;
		}
		catch (...) {
			r.error = "Matchmaking returned invalid data.";
			return r;
		}
	}

	StrategyResult ResolveJoinAutoNat(const JoinRequest& request) {
		(void)request;
		StrategyResult r;
		r.error = "Auto NAT runs on the host when you start a game. Join with a room code instead.";
		return r;
	}

	HostNatResult TryConfigureHostUpnp(uint16_t sessionPort, uint16_t ggpoPort) {
		HostNatResult r;
		bool tcpOk = TryMapUpnpPort(sessionPort, sessionPort, "TCP", "SF4 Netplay Launcher session");
		bool udpSession = TryMapUpnpPort(sessionPort, sessionPort, "UDP", "SF4 Netplay Launcher session UDP");
		bool udpGgpo = TryMapUpnpPort(ggpoPort, ggpoPort, "UDP", "SF4 Netplay Launcher GGPO");

		if (tcpOk && udpSession) {
			r.ok = true;
			r.status = "Router configured";
			r.detail = "UPnP opened session ports. Remote friends can try your room code; use Relay if it still fails.";
		}
		else if (tcpOk || udpSession) {
			r.ok = true;
			r.status = "Router partially configured";
			r.detail = "Some ports were opened. Prefer Relay hosting if joiners cannot connect.";
		}
		else {
			r.ok = false;
			r.status = "Router not configured";
			r.detail = "UPnP is unavailable (common on CGNAT). Use Relay room hosting — no port forward needed.";
		}
		(void)udpGgpo;
		return r;
	}

	bool FetchBrokerHealth(const char* brokerBaseUrl, BrokerHealth& out) {
		out = BrokerHealth();

		BrokerUrlParts parts;
		if (!ParseBrokerBaseUrl(brokerBaseUrl, parts)) {
			return false;
		}

		char body[4096] = { 0 };
		if (!BrokerHttpGet(parts, "/v1/health", body, sizeof(body), 5000)) {
			return false;
		}

		try {
			nlohmann::json j = nlohmann::json::parse(body);
			out.ok = j.value("ok", false);
			out.forceVpsRelay = j.value("forceVpsRelay", false);
			std::string relayHost = j.value("relayHost", "");
			if (!relayHost.empty()) {
				strncpy_s(out.relayHost, relayHost.c_str(), _TRUNCATE);
			}
			return out.ok;
		}
		catch (...) {
			return false;
		}
	}

	RelayRoomCreateResult CreateRelayRoom(
		const char* brokerBaseUrl,
		const char* displayName,
		const char* relayHost,
		const char* sidecarHash
	) {
		RelayRoomCreateResult r;

		BrokerUrlParts parts;
		if (!ParseBrokerBaseUrl(brokerBaseUrl, parts)) {
			r.error = "Room broker URL is not set. Use Advanced → Room broker URL or SF4E_BROKER_URL.";
			return r;
		}

		nlohmann::json req;
		req["displayName"] = displayName && displayName[0] ? displayName : "Host";
		if (sidecarHash && sidecarHash[0]) {
			req["sidecarHash"] = sidecarHash;
		}
		else if (relayHost && relayHost[0]) {
			req["relayHost"] = relayHost;
		}
		std::string reqBody = req.dump();

		char body[4096] = { 0 };
		sf4e::HttpRequestResult httpResult;
		if (!BrokerHttpPostJson(parts, "/v1/rooms", reqBody.c_str(), body, sizeof(body), 8000, &httpResult)) {
			r.error = FormatBrokerPostError(brokerBaseUrl, httpResult, body);
			return r;
		}

		try {
			nlohmann::json j = nlohmann::json::parse(body);
			if (j.value("error", "").size()) {
				r.error = j.value("message", "Could not create room (service full or unavailable).");
				return r;
			}
			std::string code = j.value("code", "");
			std::string host = j.value("host", "");
			int port = j.value("port", 0);
			if (code.empty() || host.empty() || port < 1) {
				r.error = "Room broker returned incomplete room data.";
				return r;
			}
			r.shortCode = code;
			r.hostSecret = j.value("hostSecret", "");
			strncpy_s(r.relayHost, host.c_str(), _TRUNCATE);
			r.relayPort = (uint16_t)port;
			std::string ggpoTransport = j.value("ggpoTransport", "");
			if (!ggpoTransport.empty()) {
				strncpy_s(r.ggpoTransport, ggpoTransport.c_str(), _TRUNCATE);
			}
			int ggpoPort = j.value("ggpoPort", 0);
			if (ggpoPort > 0) {
				r.ggpoPort = (uint16_t)ggpoPort;
			}
			std::string matchId = j.value("matchId", "");
			if (!matchId.empty()) {
				strncpy_s(r.matchId, matchId.c_str(), _TRUNCATE);
			}
			std::string roomToken = j.value("roomToken", "");
			if (!roomToken.empty()) {
				strncpy_s(r.roomToken, roomToken.c_str(), _TRUNCATE);
			}
			r.ok = true;
			return r;
		}
		catch (...) {
			r.error = "Room broker returned invalid data.";
			return r;
		}
	}

	bool HeartbeatRelayRoom(const char* brokerBaseUrl, const char* roomCode, const char* hostSecret) {
		if (!roomCode || !roomCode[0]) {
			return false;
		}

		char normalizedCode[32] = { 0 };
		if (!NormalizeRelayRoomCode(roomCode, normalizedCode, sizeof(normalizedCode))) {
			return false;
		}

		BrokerUrlParts parts;
		if (!ParseBrokerBaseUrl(brokerBaseUrl, parts)) {
			return false;
		}

		char path[160];
		snprintf(path, sizeof(path), "/v1/rooms/%s/heartbeat", normalizedCode);

		nlohmann::json req;
		if (hostSecret && hostSecret[0]) {
			req["hostSecret"] = hostSecret;
		}
		std::string reqBody = req.dump();

		char body[512] = { 0 };
		if (!BrokerHttpPostJson(parts, path, reqBody.c_str(), body, sizeof(body))) {
			return false;
		}

		try {
			nlohmann::json j = nlohmann::json::parse(body);
			if (j.contains("heartbeatOk")) {
				return j.value("heartbeatOk", false);
			}
			return j.value("ok", false);
		}
		catch (...) {
			return false;
		}
	}

	static uint8_t TransportFromString(const std::string& transport) {
		if (transport == "udp_relay") {
			return 1;
		}
		if (transport == "p2p") {
			return 2;
		}
		return 0;
	}

	static bool EnsureConnectWinsock() {
		static bool initialized = false;
		if (initialized) {
			return true;
		}
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			return false;
		}
		initialized = true;
		return true;
	}

	static bool SendBrokerNatProbe(const char* brokerHost, uint16_t probePort) {
		if (!brokerHost || !brokerHost[0] || probePort == 0) {
			return false;
		}
		if (!EnsureConnectWinsock()) {
			return false;
		}

		SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (sock == INVALID_SOCKET) {
			return false;
		}

		u_long nonBlocking = 1;
		ioctlsocket(sock, FIONBIO, &nonBlocking);

		sockaddr_in dest = {};
		dest.sin_family = AF_INET;
		dest.sin_port = htons(probePort);
		char resolvedHost[64] = { 0 };
		if (!sf4e::ResolveHostToIPv4(brokerHost, resolvedHost, sizeof(resolvedHost))) {
			closesocket(sock);
			return false;
		}
		if (inet_pton(AF_INET, resolvedHost, &dest.sin_addr) != 1) {
			closesocket(sock);
			return false;
		}

		const char probe[] = "SF4E_PROBE";
		if (sendto(sock, probe, sizeof(probe) - 1, 0, (sockaddr*)&dest, sizeof(dest)) == SOCKET_ERROR) {
			closesocket(sock);
			return false;
		}

		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(sock, &readfds);
		timeval tv = { 2, 0 };
		const bool ok = select(0, &readfds, nullptr, nullptr, &tv) > 0;
		closesocket(sock);
		return ok;
	}

	static void ApplyConnectPlanJson(const nlohmann::json& j, ConnectPlanResult& out) {
		std::string transport = j.value("transport", "legacy_session_tunnel");
		out.ggpoTransport = TransportFromString(transport);
		std::string host = j.value("ggpoRemoteHost", j.value("host", ""));
		if (host.empty() && transport == "p2p") {
			host = j.value("peerHost", "");
		}
		if (!host.empty()) {
			strncpy_s(out.ggpoRemoteHost, host.c_str(), _TRUNCATE);
		}
		int remotePort = j.value("ggpoRemotePort", 0);
		if (remotePort <= 0) {
			remotePort = j.value("ggpoPort", 0);
		}
		if (remotePort <= 0 && transport == "p2p") {
			remotePort = j.value("peerPort", 0);
		}
		if (remotePort > 0) {
			out.ggpoRemotePort = (uint16_t)remotePort;
		}
		std::string matchId = j.value("matchId", "");
		if (!matchId.empty()) {
			strncpy_s(out.matchId, matchId.c_str(), _TRUNCATE);
		}
		std::string roomToken = j.value("roomToken", "");
		if (!roomToken.empty()) {
			strncpy_s(out.roomToken, roomToken.c_str(), _TRUNCATE);
		}
		out.ok = true;
	}

	ConnectPlanResult FetchConnectPlan(
		const char* brokerBaseUrl,
		const char* roomCode,
		const char* role,
		const char* hostSecret,
		uint16_t ggpoPort
	) {
		ConnectPlanResult result;
		if (!roomCode || !roomCode[0]) {
			result.error = "Room code required for connect plan.";
			return result;
		}

		char normalizedCode[32] = { 0 };
		if (!NormalizeRelayRoomCode(roomCode, normalizedCode, sizeof(normalizedCode))) {
			result.error = "Invalid room code.";
			return result;
		}

		BrokerUrlParts parts;
		if (!ParseBrokerBaseUrl(brokerBaseUrl, parts)) {
			result.error = "Room broker URL is not set.";
			return result;
		}

		char path[256];
		snprintf(
			path,
			sizeof(path),
			"/v1/rooms/%s/connect-plan?role=%s",
			normalizedCode,
			(role && role[0]) ? role : "guest"
		);

		char body[4096] = { 0 };
		if (!BrokerHttpGet(parts, path, body, sizeof(body))) {
			result.error = "Could not fetch connect plan from broker.";
			return result;
		}

		try {
			nlohmann::json j = nlohmann::json::parse(body);
			if (j.value("error", "").size()) {
				result.error = j.value("message", "Connect plan unavailable.");
				return result;
			}
			ApplyConnectPlanJson(j, result);
			if (ggpoPort > 0 && role && role[0]) {
				const uint16_t probePort = (uint16_t)j.value("natProbePort", 8790);
				if (!SendBrokerNatProbe(parts.host, probePort)) {
					spdlog::warn(
						"Connect plan: NAT probe to {}:{} failed; keeping broker transport plan without endpoint registration",
						parts.host,
						probePort
					);
					return result;
				}

				nlohmann::json reg;
				reg["role"] = role;
				if (hostSecret && hostSecret[0]) {
					reg["hostSecret"] = hostSecret;
				}
				reg["ggpoPort"] = ggpoPort;
				reg["natType"] = "unknown";
				char regPath[160];
				snprintf(regPath, sizeof(regPath), "/v1/rooms/%s/register-endpoint", normalizedCode);
				char regBody[4096] = { 0 };
				sf4e::HttpRequestResult regHttp;
				if (!BrokerHttpPostJson(parts, regPath, reg.dump().c_str(), regBody, sizeof(regBody), 8000, &regHttp)) {
					spdlog::warn(
						"Connect plan: endpoint registration failed ({}); keeping broker transport plan",
						BrokerMessageFromBody(regBody)
					);
					return result;
				}
				try {
					nlohmann::json regResp = nlohmann::json::parse(regBody);
					if (regResp.value("error", "").size()) {
						spdlog::warn(
							"Connect plan: endpoint registration rejected ({}); keeping broker transport plan",
							regResp.value("message", "unknown")
						);
						return result;
					}
					if (regResp.contains("connectPlan")) {
						ApplyConnectPlanJson(regResp["connectPlan"], result);
					}
				}
				catch (...) {
					spdlog::warn("Connect plan: invalid register-endpoint response; keeping broker transport plan");
				}
			}
			return result;
		}
		catch (...) {
			result.error = "Invalid connect plan from broker.";
			return result;
		}
	}

	bool PostRoomEvent(
		const char* brokerBaseUrl,
		const char* roomCode,
		const char* eventType,
		const char* hostSecret,
		const char* transportActive,
		const char* detailJson
	) {
		if (!roomCode || !roomCode[0] || !eventType || !eventType[0]) {
			return false;
		}

		char normalizedCode[32] = { 0 };
		if (!NormalizeRelayRoomCode(roomCode, normalizedCode, sizeof(normalizedCode))) {
			return false;
		}

		BrokerUrlParts parts;
		if (!ParseBrokerBaseUrl(brokerBaseUrl, parts)) {
			return false;
		}

		nlohmann::json req;
		req["type"] = eventType;
		if (hostSecret && hostSecret[0]) {
			req["hostSecret"] = hostSecret;
		}
		if (transportActive && transportActive[0]) {
			req["transportActive"] = transportActive;
		}
		if (detailJson && detailJson[0]) {
			try {
				req["detail"] = nlohmann::json::parse(detailJson);
			}
			catch (...) {
				req["detail"] = nlohmann::json::object();
			}
		}

		char path[160];
		snprintf(path, sizeof(path), "/v1/rooms/%s/events", normalizedCode);
		char body[512] = { 0 };
		return BrokerHttpPostJson(parts, path, req.dump().c_str(), body, sizeof(body));
	}

} // namespace launcher
} // namespace sf4e
