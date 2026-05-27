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

	struct BrokerHealth {
		bool ok = false;
		bool forceVpsRelay = false;
		char relayHost[64] = { 0 };
	};

	bool FetchBrokerHealth(const char* brokerBaseUrl, BrokerHealth& out);

	struct RelayRoomCreateResult {
		bool ok = false;
		std::string error;
		std::string shortCode;
		std::string hostSecret;
		char relayHost[64] = { 0 };
		uint16_t relayPort = 0;
		uint16_t ggpoPort = 0;
		char matchId[33] = { 0 };
		char roomToken[33] = { 0 };
		char ggpoTransport[32] = { 0 };
	};

	struct ConnectPlanResult {
		bool ok = false;
		std::string error;
		uint8_t ggpoTransport = 0;
		char ggpoRemoteHost[64] = { 0 };
		uint16_t ggpoRemotePort = 0;
		char matchId[33] = { 0 };
		char roomToken[33] = { 0 };
	};

	RelayRoomCreateResult CreateRelayRoom(
		const char* brokerBaseUrl,
		const char* displayName,
		const char* relayHost,
		const char* sidecarHash = nullptr
	);
	bool HeartbeatRelayRoom(const char* brokerBaseUrl, const char* roomCode, const char* hostSecret = nullptr);

	ConnectPlanResult FetchConnectPlan(
		const char* brokerBaseUrl,
		const char* roomCode,
		const char* role,
		const char* hostSecret = nullptr,
		uint16_t ggpoPort = 0
	);

	bool PostRoomEvent(
		const char* brokerBaseUrl,
		const char* roomCode,
		const char* eventType,
		const char* hostSecret,
		const char* transportActive,
		const char* detailJson = nullptr
	);

} // namespace launcher
} // namespace sf4e
