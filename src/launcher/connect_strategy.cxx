#include "connect_strategy.hxx"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nlohmann/json.hpp>

#include "../common/sf4e__NetUtil.hxx"
#include "netplay_room_code.hxx"
#include "room_broker_client.hxx"
#include "upnp_portmap.hxx"

namespace sf4e {
namespace launcher {

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
			r.error = "Enter a room code (example SF4-A1B2).";
			return r;
		}

		char token[16] = { 0 };
		if (!ParseShortRoomCode(request.roomCode.c_str(), token, sizeof(token))) {
			r.error = "Invalid room code. Use SF4-XXXX from your host.";
			return r;
		}

		BrokerUrlParts parts;
		if (!ParseBrokerBaseUrl(brokerBaseUrl, parts)) {
			r.error = "Room service URL is not configured. Set broker URL in Advanced or SF4E_BROKER_URL.";
			return r;
		}

		char path[128];
		snprintf(path, sizeof(path), "/v1/rooms/SF4-%s", token);

		char body[4096] = { 0 };
		if (!BrokerHttpGet(parts, path, body, sizeof(body))) {
			r.error = "Could not reach the room service. Check your internet or try again later.";
			return r;
		}

		try {
			nlohmann::json j = nlohmann::json::parse(body);
			if (j.value("error", "").size()) {
				r.error = j.value("message", "Room not found or expired.");
				return r;
			}
			std::string host = j.value("host", "");
			int port = j.value("port", 0);
			if (host.empty() || port < 1 || port > 65535) {
				r.error = "Room service returned an invalid address.";
				return r;
			}
			strncpy_s(r.endpoint.host, host.c_str(), _TRUNCATE);
			r.endpoint.port = (uint16_t)port;
			r.ok = true;
			return r;
		}
		catch (...) {
			r.error = "Room service returned invalid data.";
			return r;
		}
	}

	StrategyResult ResolveJoinMatchmaking(const JoinRequest& request, const char* brokerBaseUrl, const char* displayName) {
		(void)request;
		StrategyResult r;

		BrokerUrlParts parts;
		if (!ParseBrokerBaseUrl(brokerBaseUrl, parts)) {
			r.error = "Matchmaking requires a room service URL (broker).";
			return r;
		}

		nlohmann::json req;
		req["displayName"] = displayName && displayName[0] ? displayName : "Player";
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
		bool tcpOk = TryMapUpnpPort(sessionPort, sessionPort, "TCP", "SF4 Enhanced session");
		bool udpSession = TryMapUpnpPort(sessionPort, sessionPort, "UDP", "SF4 Enhanced session UDP");
		bool udpGgpo = TryMapUpnpPort(ggpoPort, ggpoPort, "UDP", "SF4 Enhanced GGPO");

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

	RelayRoomCreateResult CreateRelayRoom(const char* brokerBaseUrl, const char* displayName, const char* relayHost) {
		RelayRoomCreateResult r;

		BrokerUrlParts parts;
		if (!ParseBrokerBaseUrl(brokerBaseUrl, parts)) {
			r.error = "Room service URL is not configured.";
			return r;
		}

		nlohmann::json req;
		req["displayName"] = displayName && displayName[0] ? displayName : "Host";
		if (relayHost && relayHost[0]) {
			req["relayHost"] = relayHost;
		}
		std::string reqBody = req.dump();

		char body[4096] = { 0 };
		if (!BrokerHttpPostJson(parts, "/v1/rooms", reqBody.c_str(), body, sizeof(body))) {
			r.error = "Could not create a relay room. Is the room service running?";
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
				r.error = "Room service returned incomplete room data.";
				return r;
			}
			r.shortCode = code;
			strncpy_s(r.relayHost, host.c_str(), _TRUNCATE);
			r.relayPort = (uint16_t)port;
			r.ok = true;
			return r;
		}
		catch (...) {
			r.error = "Room service returned invalid data.";
			return r;
		}
	}

} // namespace launcher
} // namespace sf4e
