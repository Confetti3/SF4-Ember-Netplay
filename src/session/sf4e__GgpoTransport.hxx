#pragma once

#include <cstdint>

#include "../common/sf4e__NetplayConfig.hxx"

namespace sf4e {

	enum class GgpoTransportMode : uint8_t {
		LegacySessionTunnel = 0,
		UdpRelay = 1,
		P2p = 2,
	};

	struct NatProbeResult {
		bool ok = false;
		char observedIp[64] = { 0 };
		uint16_t observedPort = 0;
	};

	namespace GgpoTransport {
		GgpoTransportMode ParseTransportEnv(uint8_t configTransport);
		const char* TransportModeLabel(GgpoTransportMode mode);

		bool RegisterWithUdpRelay(
			const char* relayHost,
			uint16_t relayPort,
			const char* roomTokenHex,
			int timeoutMs = 3000
		);

		bool TryP2pPunch(
			const char* peerHost,
			uint16_t peerPort,
			uint16_t localGgpoPort,
			int timeoutMs = 2000
		);

		NatProbeResult NatProbe(const char* brokerHost, uint16_t probePort, int timeoutMs = 2000);

		// Apply env override and prepare config; returns effective mode (may downgrade on failure).
		GgpoTransportMode PrepareForBattle(NetplayConfig& cfg);
	}

} // namespace sf4e
