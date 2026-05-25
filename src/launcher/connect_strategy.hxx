#pragma once

#include <cstdint>
#include <string>

namespace sf4e {
namespace launcher {

	struct JoinEndpoint {
		char host[64] = { 0 };
		uint16_t port = 0;
	};

	struct JoinRequest {
		std::string roomCode;
	};

	struct StrategyResult {
		bool ok = false;
		std::string error;
		JoinEndpoint endpoint;
	};

	class INetplayConnectStrategy {
	public:
		virtual ~INetplayConnectStrategy() {}
		virtual StrategyResult ResolveJoinEndpoint(const JoinRequest& request) = 0;
	};

	StrategyResult ResolveJoinDirectIp(const JoinRequest& request);
	StrategyResult ResolveJoinRelayRoom(const JoinRequest& request, const char* brokerBaseUrl);
	StrategyResult ResolveJoinMatchmaking(const JoinRequest& request, const char* brokerBaseUrl, const char* displayName);
	StrategyResult ResolveJoinAutoNat(const JoinRequest& request);

	struct HostNatResult {
		bool ok = false;
		std::string status; // plain-language for UI
		std::string detail;
	};

	HostNatResult TryConfigureHostUpnp(uint16_t sessionPort, uint16_t ggpoPort);

	struct RelayRoomCreateResult {
		bool ok = false;
		std::string error;
		std::string shortCode;
		char relayHost[64] = { 0 };
		uint16_t relayPort = 0;
	};

	RelayRoomCreateResult CreateRelayRoom(const char* brokerBaseUrl, const char* displayName, const char* relayHost);

} // namespace launcher
} // namespace sf4e
