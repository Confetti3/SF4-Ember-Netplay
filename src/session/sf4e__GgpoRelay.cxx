#include "sf4e__GgpoRelay.hxx"

#include <algorithm>

#include <spdlog/spdlog.h>

namespace sf4e {

	static const uint16_t GGPO_RELAY_VIRTUAL_BASE = 40000;

	GgpoRelay& GgpoRelay::Instance() {
		static GgpoRelay s;
		return s;
	}

	bool GgpoRelay::EnsureWinsock() {
		if (_winsockInit) {
			return true;
		}
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			spdlog::error("GgpoRelay: WSAStartup failed");
			return false;
		}
		_winsockInit = true;
		return true;
	}

	void GgpoRelay::Reset() {
		CloseSockets();
		_active = false;
		_localGgpoPort = 0;
		_client = nullptr;
		_peers.clear();
	}

	void GgpoRelay::CloseSockets() {
		for (auto& peer : _peers) {
			if (peer.sock != INVALID_SOCKET) {
				closesocket(peer.sock);
				peer.sock = INVALID_SOCKET;
			}
		}
	}

	bool GgpoRelay::CreateVirtualPeer(const SessionProtocol::ConnectionID& connId, uint16_t virtualPort) {
		if (!EnsureWinsock()) {
			return false;
		}

		SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s == INVALID_SOCKET) {
			spdlog::error("GgpoRelay: socket() failed");
			return false;
		}

		sockaddr_in bindAddr = { 0 };
		bindAddr.sin_family = AF_INET;
		bindAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bindAddr.sin_port = htons(virtualPort);
		if (bind(s, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
			spdlog::error("GgpoRelay: bind({}) failed err={}", virtualPort, WSAGetLastError());
			closesocket(s);
			return false;
		}

		u_long nonBlocking = 1;
		ioctlsocket(s, FIONBIO, &nonBlocking);

		VirtualPeer peer;
		peer.connId = connId;
		peer.sock = s;
		peer.virtualPort = virtualPort;
		peer.injectFromAddr.sin_family = AF_INET;
		peer.injectFromAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		peer.injectFromAddr.sin_port = htons(virtualPort);
		_peers.push_back(peer);
		return true;
	}

	bool GgpoRelay::Start(uint16_t localGgpoPort, SessionClient* client) {
		Reset();
		if (!client || localGgpoPort == 0) {
			return false;
		}
		if (!EnsureWinsock()) {
			return false;
		}

		_localGgpoPort = localGgpoPort;
		_client = client;
		_active = true;

		uint16_t nextVirtual = GGPO_RELAY_VIRTUAL_BASE;
		for (const auto& member : client->_lobbyData.members) {
			if (member.connId == client->_cid) {
				continue;
			}
			if (!CreateVirtualPeer(member.connId, nextVirtual)) {
				Reset();
				return false;
			}
			nextVirtual++;
		}

		spdlog::info("GgpoRelay: started localGgpoPort={} peers={}", localGgpoPort, _peers.size());
		return true;
	}

	bool GgpoRelay::GetRemoteEndpoint(
		const SessionProtocol::ConnectionID& remoteConn,
		char* outIp,
		int outIpLen,
		uint16_t* outPort
	) {
		if (!_active) {
			return false;
		}
		for (const auto& peer : _peers) {
			if (peer.connId == remoteConn) {
				strncpy_s(outIp, outIpLen, "127.0.0.1", _TRUNCATE);
				*outPort = peer.virtualPort;
				return true;
			}
		}
		return false;
	}

	void GgpoRelay::InjectFromPeer(
		const SessionProtocol::ConnectionID& src,
		const uint8_t* data,
		uint32_t len
	) {
		if (!_active || !data || len == 0) {
			return;
		}

		sockaddr_in localGgpoAddr = { 0 };
		localGgpoAddr.sin_family = AF_INET;
		localGgpoAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		localGgpoAddr.sin_port = htons(_localGgpoPort);

		for (auto& peer : _peers) {
			if (peer.connId == src && peer.sock != INVALID_SOCKET) {
				_stats.inboundFrames++;
				_stats.inboundBytes += (uint64_t)len;
				sendto(
					peer.sock,
					(const char*)data,
					len,
					0,
					(sockaddr*)&localGgpoAddr,
					sizeof(localGgpoAddr)
				);
				return;
			}
		}
	}

	void GgpoRelay::Pump() {
		if (!_active || !_client) {
			return;
		}

		char buf[2048];
		sockaddr_in localGgpoAddr = { 0 };
		localGgpoAddr.sin_family = AF_INET;
		localGgpoAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		localGgpoAddr.sin_port = htons(_localGgpoPort);

		for (auto& peer : _peers) {
			if (peer.sock == INVALID_SOCKET) {
				continue;
			}

			// Outbound: GGPO -> virtual port -> session tunnel
			for (;;) {
				sockaddr_in from = { 0 };
				int fromLen = sizeof(from);
				int n = recvfrom(peer.sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
				if (n == SOCKET_ERROR) {
					int err = WSAGetLastError();
					if (err == WSAEWOULDBLOCK) {
						break;
					}
					break;
				}
				if (n <= 0) {
					break;
				}
				_stats.outboundFrames++;
				_stats.outboundBytes += (uint64_t)n;
				_client->SendGgpoFrame(peer.connId, (const uint8_t*)buf, (uint32_t)n);
			}

			// Inbound from tunnel is handled in SessionClient::OnGgpoFrameReceived
		}
	}

} // namespace sf4e
