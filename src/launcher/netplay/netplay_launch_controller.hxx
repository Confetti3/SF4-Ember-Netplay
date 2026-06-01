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

#ifdef SF4E_STEAMWORKS_EXPERIMENT
		nlohmann::json MakeSteamBuildInfoJson() const;
		bool GetLocalSteamBuildInfo(char* sidecarHash, int sidecarHashLen, char* buildGit, int buildGitLen) const;
		void GenerateSteamSessionToken(char* outToken, int outTokenLen) const;
		nlohmann::json ConfigureSteamStart(int mode, const nlohmann::json& msg);
#endif



		PersistedSettings& m_settings;

		NetplayConfig& m_outConfig;

		char m_lanIp[64];

		char m_sessionMatchId[33] = { 0 };

		char m_sessionRoomToken[33] = { 0 };

		uint16_t m_sessionGgpoPort = 0;

		bool m_finished = false;

		bool m_cancelled = false;

		bool m_exitForUpdate = false;

#ifdef SF4E_STEAMWORKS_EXPERIMENT
		bool m_steamHasPendingInvite = false;
		uint64_t m_steamPendingPeerId = 0;
		int m_steamPendingVirtualPort = 7;
		char m_steamPendingSessionToken[NETPLAY_STEAM_SESSION_TOKEN_LEN] = { 0 };
		char m_steamPendingSidecarHash[65] = { 0 };
		char m_steamPendingBuildGit[64] = { 0 };
		char m_steamHostSessionToken[NETPLAY_STEAM_SESSION_TOKEN_LEN] = { 0 };
#endif

	};



	bool GetLauncherUiIndexUrl(wchar_t* outUrl, int outUrlChars);



} // namespace launcher

} // namespace sf4e

