#pragma once



#include <string>



#include <nlohmann/json.hpp>



#include "../common/sf4e__NetplayConfig.hxx"
#include "connect_strategy.hxx"
#include "netplay_persist.hxx"



namespace sf4e {

namespace launcher {



	class NetplayLaunchController {

	public:

		NetplayLaunchController(PersistedSettings& settings, NetplayConfig& outConfig);



		bool IsFinished() const { return m_finished; }

		bool WasCancelled() const { return m_cancelled; }

		bool ShouldExitForUpdate() const { return m_exitForUpdate; }



		nlohmann::json BuildStateJson() const;

		nlohmann::json HandleWebMessage(const nlohmann::json& msg);



	private:

		nlohmann::json MakeStateEnvelope() const;

		void ApplyConnectPlanToConfig(const ConnectPlanResult& plan);

		void ConfigureGgpoTransportForRelay(const char* roomCode, const char* role);

		std::string PreviewRoomCode(const char* shareIp, uint16_t port) const;



		PersistedSettings& m_settings;

		NetplayConfig& m_outConfig;

		char m_lanIp[64];

		char m_sessionMatchId[33] = { 0 };

		char m_sessionRoomToken[33] = { 0 };

		uint16_t m_sessionGgpoPort = 0;

		bool m_finished = false;

		bool m_cancelled = false;

		bool m_exitForUpdate = false;

	};



} // namespace launcher

} // namespace sf4e
