#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "sf4e__SessionClient.hxx"
#include "sf4e__SessionProtocol.hxx"

namespace sf4e {

	// Tunnels GGPO UDP via session server (MT_BATTLE_GGPO_FRAME) using loopback virtual ports.
	class GgpoRelay {
	public:
		static GgpoRelay& Instance();

		void Reset();
		bool Start(uint16_t localGgpoPort, SessionClient* client);
		void Pump();

		// Remote GGPO endpoint when relay is active (127.0.0.1:virtualPort).
		bool GetRemoteEndpoint(
			const SessionProtocol::ConnectionID& remoteConn,
			char* outIp,
			int outIpLen,
			uint16_t* outPort
		);

		void InjectFromPeer(
			const SessionProtocol::ConnectionID& src,
			const uint8_t* data,
			uint32_t len
		);

		bool IsActive() const { return _active; }

		struct TransportStats {
			uint64_t outboundFrames = 0;
			uint64_t outboundBytes = 0;
			uint64_t inboundFrames = 0;
			uint64_t inboundBytes = 0;
		};

		TransportStats GetStats() const { return _stats; }
		void ResetStats() { _stats = {}; }

	private:
		GgpoRelay() = default;

		struct VirtualPeer {
			SessionProtocol::ConnectionID connId;
			SOCKET sock = INVALID_SOCKET;
			uint16_t virtualPort = 0;
			sockaddr_in injectFromAddr = { 0 };
		};

		bool EnsureWinsock();
		bool CreateVirtualPeer(const SessionProtocol::ConnectionID& connId, uint16_t virtualPort);
		void CloseSockets();

		bool _active = false;
		bool _winsockInit = false;
		uint16_t _localGgpoPort = 0;
		SessionClient* _client = nullptr;
		std::vector<VirtualPeer> _peers;
		TransportStats _stats = {};
	};

} // namespace sf4e
