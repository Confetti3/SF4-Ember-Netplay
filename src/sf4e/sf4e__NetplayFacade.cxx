#include "sf4e__NetplayFacade.hxx"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>

#include "../common/sf4e__DebugLog.hxx"

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../session/sf4e__GgpoRelay.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__Overlay.hxx"
#include "sf4e__UserApp.hxx"

using Dimps::App;
using rMainMenu = Dimps::GameEvents::MainMenu;
using Dimps::Event::EventBase;
using Dimps::Event::EventBaseWithEC;
using Dimps::Event::EventController;
using Dimps::Game::ProgressData;
using Dimps::GameEvents::RootEvent;
using Dimps::Math::FixedPoint;
using fSystem = sf4e::Game::Battle::System;
using fUserApp = sf4e::UserApp;
using fVsBattle = sf4e::GameEvents::VsBattle;

namespace sf4e {

	static NetplayConfig s_config = { 0 };
	static bool s_autoPending = false;
	static bool s_graphicsWarned = false;
	static bool s_deferGgpoClose = false;
	static bool s_deferredGgpoPending = false;
	static DWORD s_deferGgpoCloseUntil = 0;
	static std::deque<std::string> s_alerts;
	static int s_autoStartDwellTicks = 0;
	static int s_framesSinceReady = 0;
	static bool s_gameReady = false;
	static const int kMinFramesAfterReady = 180;
	static const int kMinDwellBeforeAutoStart = 60;

	void NetplayFacade::NotifyGameReady() {
		s_gameReady = true;
		// #region agent log
		debug::AgentLog("H1", "NetplayFacade::NotifyGameReady", "game init complete", "{}");
		// #endregion
	}

	void NetplayFacade::InitFromPayload(const NetplayConfig& cfg) {
		s_config = cfg;
		const char* relayEnv = getenv("SF4E_RELAY");
		if (relayEnv && relayEnv[0] == '0') {
			s_config.useRelay = 0;
		}
		if (cfg.devOverlay != 0) {
			// keep
		}
		else {
			const char* env = getenv("SF4E_NETPLAY_DEV");
			if (env && env[0] == '1') {
				s_config.devOverlay = 1;
			}
		}

		s_autoPending = NetplayConfigIsActive(s_config);
		s_autoStartDwellTicks = 0;
		s_framesSinceReady = 0;
		// #region agent log
		{
			char buf[256];
			snprintf(buf, sizeof(buf),
				"{\"mode\":%d,\"sessionPort\":%u,\"ggpoPort\":%u,\"autoPending\":%d}",
				s_config.mode, s_config.sessionPort, s_config.ggpoPort, s_autoPending ? 1 : 0);
			debug::AgentLog("H6", "NetplayFacade::InitFromPayload", "payload init", buf);
		}
		// #endregion
		if (s_autoPending) {
			spdlog::info("NetplayFacade: auto netplay pending mode={}", s_config.mode);
		}

		Overlay::ConfigureNetplayUi(s_autoPending, s_config.devOverlay != 0);
	}

	const NetplayConfig& NetplayFacade::GetConfig() {
		return s_config;
	}

	bool NetplayFacade::IsDevOverlayEnabled() {
		return s_config.devOverlay != 0;
	}

	bool NetplayFacade::IsRelayEnabled() {
		return s_config.useRelay != 0;
	}

	bool NetplayFacade::IsAutoNetplayPending() {
		return s_autoPending;
	}

	void NetplayFacade::SetLastError(const char* msg) {
		if (msg) {
			spdlog::warn("Netplay: {}", msg);
		}
	}

	void NetplayFacade::PushAlert(const char* msg) {
		if (msg) {
			s_alerts.push_back(msg);
			Overlay::PushNetplayAlert(msg);
		}
	}

	static bool IsOnMainMenu() {
		RootEvent* root = App::GetRootEvent();
		if (!root) {
			return false;
		}
		char* mainMenuQuery[1] = { "MainMenu" };
		return EventBaseWithEC::FindForegroundEvent(root, mainMenuQuery, 1) != nullptr;
	}

	static const char* GetRootForegroundEventName() {
		RootEvent* root = App::GetRootEvent();
		if (!root) {
			return "(no root)";
		}
		EventController* controller = (root->*EventBaseWithEC::publicMethods.GetChildEventController)();
		if (!controller) {
			return "(no controller)";
		}
		EventBase* child = (controller->*EventController::publicMethods.GetForegroundEvent)();
		if (!child) {
			return "(no foreground)";
		}
		return EventBase::GetName(child);
	}

	static void CapturePadIfNeeded(uint8_t& deviceIdx, uint8_t& deviceType) {
		if (deviceIdx != 0xff && deviceType != 0xff) {
			return;
		}
		Dimps::Pad::System* p = Dimps::Pad::System::staticMethods.GetSingleton();
		if (!p) {
			return;
		}
		Dimps::Pad::System::__publicMethods& methods = Dimps::Pad::System::publicMethods;
		deviceIdx = (p->*methods.GetDeviceIndexForPlayer)(0);
		deviceType = (p->*methods.GetDeviceTypeForPlayer)(0);
	}

	void NetplayFacade::TickMainMenu() {
		if (!s_autoPending) {
			return;
		}
		if (!s_gameReady) {
			// #region agent log
			static int s_waitReadyFrames = 0;
			s_waitReadyFrames++;
			if (s_waitReadyFrames == 180 || s_waitReadyFrames == 360) {
				char buf[96];
				snprintf(buf, sizeof(buf), "{\"waitFrames\":%d}", s_waitReadyFrames);
				debug::AgentLog("H12", "NetplayFacade::TickMainMenu", "waiting NotifyGameReady", buf);
			}
			// #endregion
			return;
		}

		s_framesSinceReady++;
		if (s_framesSinceReady < kMinFramesAfterReady) {
			return;
		}

		// #region agent log
		static int s_tickLogCount = 0;
		if (s_tickLogCount < 5) {
			char buf[96];
			snprintf(buf, sizeof(buf), "{\"framesSinceReady\":%d}", s_framesSinceReady);
			debug::AgentLog("H1", "NetplayFacade::TickMainMenu", "tick", buf);
			s_tickLogCount++;
		}
		// #endregion

		// #region agent log
		static int s_foregroundLogCount = 0;
		if (!IsOnMainMenu() && s_foregroundLogCount < 8 && (s_framesSinceReady % 60) == 0) {
			char buf[192];
			snprintf(buf, sizeof(buf),
				"{\"foreground\":\"%s\",\"onMainMenu\":0,\"dwell\":%d}",
				GetRootForegroundEventName(), s_autoStartDwellTicks);
			debug::AgentLog("H7", "NetplayFacade::TickMainMenu", "not on main menu yet", buf);
			s_foregroundLogCount++;
		}
		// #endregion

		s_autoStartDwellTicks++;

		RootEvent* root = App::GetRootEvent();
		if (!root) {
			return;
		}

		// Pad system may not be ready immediately after boot.
		Dimps::Pad::System* padSys = Dimps::Pad::System::staticMethods.GetSingleton();
		if (!padSys) {
			// #region agent log
			if (s_autoStartDwellTicks == 1 || s_autoStartDwellTicks % 30 == 0) {
				char buf[128];
				snprintf(buf, sizeof(buf), "{\"dwell\":%d,\"pad\":0}", s_autoStartDwellTicks);
				debug::AgentLog("H1", "NetplayFacade::TickMainMenu", "waiting for pad", buf);
			}
			// #endregion
			return;
		}

		if (s_autoStartDwellTicks < kMinDwellBeforeAutoStart) {
			// #region agent log
			if (s_autoStartDwellTicks == 1 || s_autoStartDwellTicks % 30 == 0) {
				char buf[128];
				snprintf(buf, sizeof(buf), "{\"dwell\":%d,\"need\":%d}", s_autoStartDwellTicks, kMinDwellBeforeAutoStart);
				debug::AgentLog("H1", "NetplayFacade::TickMainMenu", "auto-start dwell", buf);
			}
			// #endregion
			return;
		}

		CheckGraphicsWarning();

		uint8_t deviceIdx = s_config.deviceIdx;
		uint8_t deviceType = s_config.deviceType;
		CapturePadIfNeeded(deviceIdx, deviceType);
		// #region agent log
		{
			char buf[192];
			snprintf(buf, sizeof(buf),
				"{\"dwell\":%d,\"deviceIdx\":%u,\"deviceType\":%u,\"mode\":%d,\"onMainMenu\":%d,\"foreground\":\"%s\"}",
				s_autoStartDwellTicks, deviceIdx, deviceType, s_config.mode,
				IsOnMainMenu() ? 1 : 0, GetRootForegroundEventName());
			debug::AgentLog("H2", "NetplayFacade::TickMainMenu", "auto-start session", buf);
		}
		// #endregion

		std::string name(s_config.displayName);
		if (name.empty()) {
			name = "Player";
		}

		FixedPoint roundTime = { 0, (short)s_config.roundTimeIntegral };

		if (s_config.mode == (int)NetplayMode::Host) {
			std::string hash = sf4e::sidecarHash;
			char hostAddr[128];

			if (s_config.useCentralSession != 0) {
				snprintf(hostAddr, sizeof(hostAddr), "%s:%u", s_config.sessionHost, s_config.sessionPort);
				fUserApp::StartSession(
					hostAddr,
					s_config.ggpoPort,
					hash,
					name,
					deviceType,
					deviceIdx,
					s_config.inputDelay,
					s_config.useRelay != 0
				);
				Overlay::SetNetplayLobbyVisible(true);
				s_autoPending = false;
				spdlog::info("NetplayFacade: host joined central relay at {}", hostAddr);
			}
			else {
				// Local session server; join via loopback.
				snprintf(hostAddr, sizeof(hostAddr), "127.0.0.1:%u", s_config.sessionPort);

				std::string identity(s_config.sessionHost);
				bool serverOk = fUserApp::StartServer(
					s_config.sessionPort,
					identity,
					hash,
					s_config.editionSelect != 0,
					s_config.roundCount,
					roundTime
				);
				// #region agent log
				{
					char buf[128];
					snprintf(buf, sizeof(buf), "{\"serverOk\":%d,\"port\":%u}", serverOk ? 1 : 0, s_config.sessionPort);
					debug::AgentLog("H4", "NetplayFacade::TickMainMenu", "host StartServer", buf);
				}
				// #endregion
				if (!serverOk) {
					PushAlert("Could not start session server (port in use?).");
					s_autoPending = false;
					return;
				}

				fUserApp::StartSession(
					hostAddr,
					s_config.ggpoPort,
					hash,
					name,
					deviceType,
					deviceIdx,
					s_config.inputDelay,
					s_config.useRelay != 0
				);
				Overlay::SetNetplayLobbyVisible(true);
				s_autoPending = false;
				spdlog::info("NetplayFacade: host started session at {}", hostAddr);
			}
		}
		else if (s_config.mode == (int)NetplayMode::Join) {
			char joinAddr[128];
			snprintf(joinAddr, sizeof(joinAddr), "%s:%u", s_config.sessionHost, s_config.sessionPort);

			std::string hash = sf4e::sidecarHash;
			fUserApp::StartSession(
				joinAddr,
				s_config.ggpoPort,
				hash,
				name,
				deviceType,
				deviceIdx,
				s_config.inputDelay,
				s_config.useRelay != 0
			);
			Overlay::SetNetplayLobbyVisible(true);
			s_autoPending = false;
			spdlog::info("NetplayFacade: join connected to {}", joinAddr);
		}
	}

	void NetplayFacade::TickFrame() {
		if (fUserApp::netplay && fUserApp::netplay->client._useRelay && GgpoRelay::Instance().IsActive()) {
			GgpoRelay::Instance().Pump();
		}

		if (s_deferredGgpoPending && fSystem::ggpo && !ShouldDeferGgpoClose()) {
			ggpo_close_session(fSystem::ggpo);
			fSystem::ggpo = nullptr;
			GgpoRelay::Instance().Reset();
			s_deferredGgpoPending = false;
			s_deferGgpoClose = false;
			spdlog::info("NetplayFacade: closed deferred GGPO session");
		}
	}

	NetplayStatus NetplayFacade::GetStatus() {
		NetplayStatus st;
		st.active = fUserApp::netplay != nullptr || fUserApp::server != nullptr;
		if (fUserApp::netplay) {
			st.connected = fUserApp::netplay->client.IsConnected();
			st.inLobby = st.connected && fSystem::ggpo == nullptr;
			st.inMatch = fSystem::ggpo != nullptr;
			st.inputDelay = fUserApp::netplay->delay;

			for (const auto& m : fUserApp::netplay->client._lobbyData.members) {
				if (m.name != fUserApp::netplay->client._name) {
					strncpy_s(st.opponentName, m.name.c_str(), _TRUNCATE);
					break;
				}
			}

			if (fSystem::ggpo) {
				GGPONetworkStats stats;
				for (int i = 0; i < MAX_SF4E_PROTOCOL_USERS; i++) {
					if (fSystem::players[i].type == GGPO_PLAYERTYPE_REMOTE) {
						if (GGPO_SUCCEEDED(ggpo_get_network_stats(fSystem::ggpo, fSystem::players[i].handle, &stats))) {
							st.pingMs = stats.network.ping;
						}
						break;
					}
				}
			}
		}
		return st;
	}

	void NetplayFacade::ShutdownNetplay(bool closeGgpo) {
		if (closeGgpo && fSystem::ggpo) {
			ggpo_close_session(fSystem::ggpo);
			fSystem::ggpo = nullptr;
		}
		GgpoRelay::Instance().Reset();
		if (fUserApp::netplay) {
			fUserApp::netplay->client.Disconnect();
			fUserApp::netplay.reset();
		}
		if (fUserApp::server) {
			fUserApp::server->Close();
			fUserApp::server.reset();
		}
		s_deferGgpoClose = false;
		s_deferredGgpoPending = false;
	}

	void NetplayFacade::ClearBattleState() {
		fSystem::snapshotMap.clear();
		if (fUserApp::netplay) {
			fUserApp::netplay->client.pendingRemoteSnapshots.clear();
		}
	}

	bool NetplayFacade::ShouldDeferGgpoClose() {
		if (!s_deferGgpoClose) {
			return false;
		}
		if (GetTickCount() < s_deferGgpoCloseUntil) {
			return true;
		}
		s_deferGgpoClose = false;
		return false;
	}

	void NetplayFacade::NotifyMatchEnded() {
		ClearBattleState();
		if (!fUserApp::netplay) {
			return;
		}

		size_t spectators = 0;
		if (fUserApp::netplay->client._lobbyData.members.size() > 2) {
			spectators = fUserApp::netplay->client._lobbyData.members.size() - 2;
		}

		if (spectators > 0) {
			s_deferGgpoClose = true;
			s_deferredGgpoPending = true;
			s_deferGgpoCloseUntil = GetTickCount() + 120000;
			spdlog::info("NetplayFacade: deferring GGPO close for {} spectators", spectators);
		}
	}

	void NetplayFacade::CheckGraphicsWarning() {
		if (s_graphicsWarned || !NetplayConfigIsActive(s_config)) {
			return;
		}
		s_graphicsWarned = true;
		PushAlert(
			"Netplay tip: disable Smooth frame rate in Graphics options if simulation feels wrong."
		);
	}

} // namespace sf4e
