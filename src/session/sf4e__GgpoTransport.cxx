#include "sf4e__GgpoTransport.hxx"

#include "../common/sf4e__NetUtil.hxx"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include <spdlog/spdlog.h>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace sf4e {

	static bool EnsureWinsock() {
		static bool initialized = false;
		if (initialized) {
			return true;
		}
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			spdlog::error("GgpoTransport: WSAStartup failed");
			return false;
		}
		initialized = true;
		return true;
	}

	static bool SendProbeAndWait(
		SOCKET sock,
		const sockaddr_in& dest,
		const char* sendBuf,
		int sendLen,
		char* recvBuf,
		int recvBufLen,
		int timeoutMs
	) {
		if (sendto(sock, sendBuf, sendLen, 0, (sockaddr*)&dest, sizeof(dest)) == SOCKET_ERROR) {
			return false;
		}

		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(sock, &readfds);
		timeval tv;
		tv.tv_sec = timeoutMs / 1000;
		tv.tv_usec = (timeoutMs % 1000) * 1000;
		int ready = select(0, &readfds, nullptr, nullptr, &tv);
		if (ready <= 0) {
			return false;
		}

		sockaddr_in from = {};
		int fromLen = sizeof(from);
		int n = recvfrom(sock, recvBuf, recvBufLen, 0, (sockaddr*)&from, &fromLen);
		return n > 0;
	}

	GgpoTransportMode GgpoTransport::ParseTransportEnv(uint8_t configTransport) {
		const char* env = getenv("SF4E_GGPO_TRANSPORT");
		if (env && env[0]) {
			if (_stricmp(env, "legacy") == 0) {
				return GgpoTransportMode::LegacySessionTunnel;
			}
			if (_stricmp(env, "udp") == 0) {
				return GgpoTransportMode::UdpRelay;
			}
			if (_stricmp(env, "p2p") == 0) {
				return GgpoTransportMode::P2p;
			}
		}
		return (GgpoTransportMode)configTransport;
	}

	const char* GgpoTransport::TransportModeLabel(GgpoTransportMode mode) {
		switch (mode) {
		case GgpoTransportMode::UdpRelay:
			return "udp_relay";
		case GgpoTransportMode::P2p:
			return "p2p";
		default:
			return "legacy_session_tunnel";
		}
	}

	bool GgpoTransport::RegisterWithUdpRelay(
		const char* relayHost,
		uint16_t relayPort,
		const char* roomTokenHex,
		int timeoutMs
	) {
		if (!relayHost || !relayHost[0] || !roomTokenHex || strlen(roomTokenHex) != 32) {
			return false;
		}
		if (!EnsureWinsock()) {
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
		dest.sin_port = htons(relayPort);
		char resolvedHost[64] = { 0 };
		if (!ResolveHostToIPv4(relayHost, resolvedHost, sizeof(resolvedHost))) {
			spdlog::warn("GgpoTransport: could not resolve UDP relay host '{}'", relayHost);
			closesocket(sock);
			return false;
		}
		if (inet_pton(AF_INET, resolvedHost, &dest.sin_addr) != 1) {
			spdlog::warn("GgpoTransport: invalid UDP relay address '{}'", resolvedHost);
			closesocket(sock);
			return false;
		}

		char packet[4 + 32] = { 'S', 'F', '4', 'G' };
		memcpy(packet + 4, roomTokenHex, 32);

		const DWORD deadline = GetTickCount() + (DWORD)timeoutMs;
		while (GetTickCount() < deadline) {
			char response[8] = { 0 };
			if (SendProbeAndWait(sock, dest, packet, sizeof(packet), response, sizeof(response), 500)) {
				if (memcmp(response, "SF4R", 4) == 0) {
					closesocket(sock);
					spdlog::info("GgpoTransport: UDP relay registration OK {}:{}", relayHost, relayPort);
					return true;
				}
			}
			Sleep(100);
		}

		closesocket(sock);
		spdlog::warn("GgpoTransport: UDP relay registration timed out {}:{}", relayHost, relayPort);
		return false;
	}

	bool GgpoTransport::TryP2pPunch(
		const char* peerHost,
		uint16_t peerPort,
		uint16_t localGgpoPort,
		int timeoutMs
	) {
		if (!peerHost || !peerHost[0] || peerPort == 0) {
			return false;
		}
		if (!EnsureWinsock()) {
			return false;
		}

		SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (sock == INVALID_SOCKET) {
			return false;
		}

		sockaddr_in bindAddr = {};
		bindAddr.sin_family = AF_INET;
		bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
		bindAddr.sin_port = htons(localGgpoPort);
		if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
			closesocket(sock);
			return false;
		}

		u_long nonBlocking = 1;
		ioctlsocket(sock, FIONBIO, &nonBlocking);

		sockaddr_in peer = {};
		peer.sin_family = AF_INET;
		peer.sin_port = htons(peerPort);
		if (inet_pton(AF_INET, peerHost, &peer.sin_addr) != 1) {
			closesocket(sock);
			return false;
		}

		const char punchPayload[] = "SF4E_PUNCH";
		const DWORD deadline = GetTickCount() + (DWORD)timeoutMs;
		while (GetTickCount() < deadline) {
			sendto(sock, punchPayload, sizeof(punchPayload) - 1, 0, (sockaddr*)&peer, sizeof(peer));

			fd_set readfds;
			FD_ZERO(&readfds);
			FD_SET(sock, &readfds);
			timeval tv = { 0, 100000 };
			if (select(0, &readfds, nullptr, nullptr, &tv) > 0) {
				char buf[64];
				sockaddr_in from = {};
				int fromLen = sizeof(from);
				int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
				if (n > 0) {
					closesocket(sock);
					spdlog::info("GgpoTransport: P2P punch response from {}:{}", peerHost, peerPort);
					return true;
				}
			}
		}

		closesocket(sock);
		spdlog::warn("GgpoTransport: P2P punch timed out {}:{}", peerHost, peerPort);
		return false;
	}

	NatProbeResult GgpoTransport::NatProbe(const char* brokerHost, uint16_t probePort, int timeoutMs) {
		NatProbeResult result;
		if (!brokerHost || !brokerHost[0] || probePort == 0) {
			return result;
		}
		if (!EnsureWinsock()) {
			return result;
		}

		SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (sock == INVALID_SOCKET) {
			return result;
		}

		u_long nonBlocking = 1;
		ioctlsocket(sock, FIONBIO, &nonBlocking);

		sockaddr_in dest = {};
		dest.sin_family = AF_INET;
		dest.sin_port = htons(probePort);
		char resolvedHost[64] = { 0 };
		if (!ResolveHostToIPv4(brokerHost, resolvedHost, sizeof(resolvedHost))) {
			closesocket(sock);
			return result;
		}
		if (inet_pton(AF_INET, resolvedHost, &dest.sin_addr) != 1) {
			closesocket(sock);
			return result;
		}

		const char probe[] = "SF4E_PROBE";
		char response[256] = { 0 };
		if (!SendProbeAndWait(sock, dest, probe, (int)sizeof(probe) - 1, response, sizeof(response) - 1, timeoutMs)) {
			closesocket(sock);
			return result;
		}
		closesocket(sock);

		try {
			// Minimal JSON parse without nlohmann in this TU
			const char* ipKey = strstr(response, "\"observedIp\"");
			const char* portKey = strstr(response, "\"observedPort\"");
			if (ipKey) {
				const char* q1 = strchr(ipKey, '"');
				if (q1) {
					q1 = strchr(q1 + 1, '"');
					if (q1) {
						const char* q2 = strchr(q1 + 1, '"');
						if (q2 && q2 - q1 - 1 < (int)sizeof(result.observedIp)) {
							memcpy(result.observedIp, q1 + 1, q2 - q1 - 1);
							result.observedIp[q2 - q1 - 1] = '\0';
						}
					}
				}
			}
			if (portKey) {
				const char* colon = strchr(portKey, ':');
				if (colon) {
					result.observedPort = (uint16_t)atoi(colon + 1);
				}
			}
			result.ok = result.observedIp[0] != '\0' && result.observedPort > 0;
		}
		catch (...) {
			result.ok = false;
		}
		return result;
	}

	GgpoTransportMode GgpoTransport::PrepareForBattle(NetplayConfig& cfg) {
		GgpoTransportMode requested = ParseTransportEnv(cfg.ggpoTransport);
		const char* env = getenv("SF4E_GGPO_TRANSPORT");
		const bool autoMode = env && _stricmp(env, "auto") == 0;

		if (requested == GgpoTransportMode::LegacySessionTunnel) {
			cfg.ggpoTransport = (uint8_t)GgpoTransportMode::LegacySessionTunnel;
			return GgpoTransportMode::LegacySessionTunnel;
		}

		if (requested == GgpoTransportMode::P2p) {
			if (cfg.ggpoRemoteHost[0] && cfg.ggpoRemotePort > 0) {
				if (TryP2pPunch(cfg.ggpoRemoteHost, cfg.ggpoRemotePort, cfg.ggpoPort)) {
					cfg.ggpoTransport = (uint8_t)GgpoTransportMode::P2p;
					return GgpoTransportMode::P2p;
				}
			}
			if (!autoMode) {
				spdlog::warn("GgpoTransport: P2P failed, falling back to legacy tunnel");
				cfg.ggpoTransport = (uint8_t)GgpoTransportMode::LegacySessionTunnel;
				return GgpoTransportMode::LegacySessionTunnel;
			}
			requested = GgpoTransportMode::UdpRelay;
		}

		if (requested == GgpoTransportMode::UdpRelay) {
			const char* relayHost = cfg.ggpoRemoteHost[0] ? cfg.ggpoRemoteHost : cfg.sessionHost;
			uint16_t relayPort = cfg.ggpoRemotePort > 0 ? cfg.ggpoRemotePort : 0;
			if (!relayHost[0] || relayPort == 0) {
				spdlog::warn(
					"GgpoTransport: UDP relay missing endpoint (host='{}' port={})",
					relayHost ? relayHost : "",
					relayPort
				);
			}
			else if (!cfg.ggpoRoomToken[0]) {
				spdlog::warn("GgpoTransport: UDP relay missing room token");
			}
			else if (RegisterWithUdpRelay(relayHost, relayPort, cfg.ggpoRoomToken)) {
				strncpy_s(cfg.ggpoRemoteHost, relayHost, _TRUNCATE);
				cfg.ggpoRemotePort = relayPort;
				cfg.ggpoTransport = (uint8_t)GgpoTransportMode::UdpRelay;
				return GgpoTransportMode::UdpRelay;
			}
			spdlog::warn("GgpoTransport: UDP relay failed, falling back to legacy tunnel");
		}

		cfg.ggpoTransport = (uint8_t)GgpoTransportMode::LegacySessionTunnel;
		cfg.ggpoRemoteHost[0] = '\0';
		cfg.ggpoRemotePort = 0;
		return GgpoTransportMode::LegacySessionTunnel;
	}

} // namespace sf4e
