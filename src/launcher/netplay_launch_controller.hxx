#pragma once



#include <string>



#include <nlohmann/json.hpp>



#include "../common/sf4e__NetplayConfig.hxx"

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

		std::string PreviewRoomCode(const char* shareIp, uint16_t port) const;



		PersistedSettings& m_settings;

		NetplayConfig& m_outConfig;

		char m_lanIp[64];

		bool m_finished = false;

		bool m_cancelled = false;

		bool m_exitForUpdate = false;

	};



	bool GetLauncherUiIndexUrl(wchar_t* outUrl, int outUrlChars);



} // namespace launcher

} // namespace sf4e

