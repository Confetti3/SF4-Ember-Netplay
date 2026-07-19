#include "sf4e__NetplayFacade.hxx"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../common/agent_debug_log.hxx"
#include "../session/sf4e__SessionClient.hxx"
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
using rSystem = Dimps::Game::Battle::System;

namespace sf4e {

	static NetplayConfig s_config = { 0 };
	static char s_brokerGgpoRemoteHost[NETPLAY_SESSION_HOST_LEN] = { 0 };
	static uint16_t s_brokerGgpoRemotePort = 0;
	static GgpoTransportStatus s_ggpoTransportStatus = { 0 };
	static GgpoSyncPhase s_ggpoSyncPhase = GgpoSyncPhase::None;
	static DWORD s_ggpoBattleStartTick = 0;
	static bool s_udpGgpoFallbackTried = false;
	static bool s_autoPending = false;
	static bool s_graphicsWarned = false;
	static bool s_deferGgpoClose = false;
	static bool s_deferredGgpoPending = false;
	static DWORD s_deferGgpoCloseUntil = 0;
	static std::deque<std::string> s_alerts;
	static int s_autoStartDwellTicks = 0;
	static int s_framesSinceReady = 0;
	static bool s_gameReady = false;
	static bool s_padCaptured = false;
	static bool s_configLogged = false;
	static const int kMinFramesAfterReady = 180;
	static const int kMinDwellBeforeAutoStart = 60;

	void NetplayFacade::NotifyGameReady() {
		s_gameReady = true;
		if (!s_configLogged) {
			spdlog::info(
				"NetplayFacade payload ready version={} mode={} displayName={} inputDelay={} devOverlay={} sessionMode={}",
				s_config.version,
				s_config.mode,
				s_config.displayName,
				(int)s_config.inputDelay,
				(int)s_config.devOverlay,
				(int)s_config.useCentralSession
			);
			if (s_config.mode == (int)NetplayMode::Idle && s_config.devOverlay != 0) {
				spdlog::info("NetplayFacade offline test active");
			}
			s_configLogged = true;
		}
	}

	static void CaptureBrokerGgpoEndpoint(const NetplayConfig& cfg) {
		if (cfg.ggpoRemotePort > 0 && cfg.ggpoRemoteHost[0]) {
			s_brokerGgpoRemotePort = cfg.ggpoRemotePort;
			strncpy_s(s_brokerGgpoRemoteHost, cfg.ggpoRemoteHost, _TRUNCATE);
		}
	}

	void NetplayFacade::RestoreBrokerGgpoEndpoint(NetplayConfig& cfg) {
		if (s_brokerGgpoRemotePort > 0 && s_brokerGgpoRemoteHost[0]) {
			cfg.ggpoRemotePort = s_brokerGgpoRemotePort;
			strncpy_s(cfg.ggpoRemoteHost, s_brokerGgpoRemoteHost, _TRUNCATE);
		}
	}

	void NetplayFacade::InitFromPayload(const NetplayConfig& cfg) {
		s_config = cfg;
		CaptureBrokerGgpoEndpoint(cfg);
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
		s_configLogged = false;
		s_padCaptured = s_config.deviceIdx != 0xff && s_config.deviceType != 0xff;
		if (s_autoPending) {
			spdlog::info("NetplayFacade: auto netplay pending mode={}", s_config.mode);
		}

		Overlay::ConfigureNetplayUi(s_autoPending, s_config.devOverlay != 0);
	}

	const NetplayConfig& NetplayFacade::GetConfig() {
		return s_config;
	}

	void NetplayFacade::ApplyGgpoTransportConfig(const NetplayConfig& cfg) {
		s_config.ggpoTransport = cfg.ggpoTransport;
		if (cfg.ggpoTransport != 0) {
			s_config.ggpoRemotePort = cfg.ggpoRemotePort;
			strncpy_s(s_config.ggpoRemoteHost, cfg.ggpoRemoteHost, _TRUNCATE);
		}
		else if (s_brokerGgpoRemotePort > 0 && s_brokerGgpoRemoteHost[0]) {
			s_config.ggpoRemotePort = s_brokerGgpoRemotePort;
			strncpy_s(s_config.ggpoRemoteHost, s_brokerGgpoRemoteHost, _TRUNCATE);
		}
		if (cfg.ggpoRoomToken[0]) {
			strncpy_s(s_config.ggpoRoomToken, cfg.ggpoRoomToken, _TRUNCATE);
		}
	}

	void NetplayFacade::ReportGgpoTransport(
		uint8_t effectiveMode,
		bool legacyTunnelActive,
		const char* remoteHost,
		uint16_t remotePort
	) {
		s_ggpoTransportStatus.effectiveMode = effectiveMode;
		s_ggpoTransportStatus.legacyTunnelActive = legacyTunnelActive;
		s_ggpoTransportStatus.remotePort = remotePort;
		if (remoteHost && remoteHost[0]) {
			strncpy_s(s_ggpoTransportStatus.remoteHost, remoteHost, _TRUNCATE);
		}
		else {
			s_ggpoTransportStatus.remoteHost[0] = '\0';
		}
	}

	GgpoTransportStatus NetplayFacade::GetGgpoTransportStatus() {
		return s_ggpoTransportStatus;
	}

	GgpoSyncPhase NetplayFacade::GetGgpoSyncPhase() {
		return s_ggpoSyncPhase;
	}

	void NetplayFacade::NotifyGgpoSyncPhase(GgpoSyncPhase phase) {
		if (s_ggpoSyncPhase != phase) {
			spdlog::info("NetplayFacade GGPO phase {} -> {}", (int)s_ggpoSyncPhase, (int)phase);
		}
		s_ggpoSyncPhase = phase;
	}

	void NetplayFacade::ResetGgpoBattleWatch() {
		s_ggpoBattleStartTick = GetTickCount();
		s_udpGgpoFallbackTried = false;
		s_ggpoSyncPhase = GgpoSyncPhase::Starting;
	}

	void NetplayFacade::MarkGgpoBattleStarted() {
		s_ggpoBattleStartTick = GetTickCount();
		s_ggpoSyncPhase = GgpoSyncPhase::Starting;
	}

	const char* NetplayFacade::GgpoTransportModeName(uint8_t mode) {
		switch (mode) {
		case 1:
			return "udp_relay";
		case 2:
			return "p2p";
		default:
			return "legacy_session_tunnel";
		}
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

	void NetplayFacade::HandleNetplayFailure(const char* reason, bool closeGgpo) {
		if (reason && reason[0]) {
			PushAlert(reason);
			SetLastError(reason);
		}

		if (fSystem::ggpo) {
			rSystem* system = rSystem::staticMethods.GetSingleton();
			if (system && *rSystem::staticVars.CurrentBattleFlow != rSystem::BF__IDLE) {
				*rSystem::GetReadyState(system) = rSystem::RS_ISLEAVING;
			}
			fSystem::bUpdateAllowed = false;
		}

		ShutdownNetplay(closeGgpo);
	}

	static bool IsOnMainMenu() {
		RootEvent* root = App::GetRootEvent();
		if (!root) {
			return false;
		}
		char* mainMenuQuery[1] = { "MainMenu" };
		return EventBaseWithEC::FindForegroundEvent(root, mainMenuQuery, 1) != nullptr;
	}

	bool NetplayFacade::TryCapturePadForSide(int side, uint8_t& outIdx, uint8_t& outType) {
		Dimps::Pad::System* p = Dimps::Pad::System::staticMethods.GetSingleton();
		if (!p) {
			return false;
		}
		Dimps::Pad::System::__publicMethods& methods = Dimps::Pad::System::publicMethods;
		if (!(p->*methods.CaptureNextMatchingPadToSide)(side, 0x1040, 0xffffffff)) {
			return false;
		}
		outIdx = (uint8_t)(p->*methods.GetDeviceIndexForPlayer)(side);
		outType = (uint8_t)(p->*methods.GetDeviceTypeForPlayer)(side);
		(p->*methods.SetSideHasAssignedController)(side, 0);
		switch (outType) {
		case Dimps::Pad::PADTYPE_RAWINPUT: {
			Dimps::Pad::System_RawInput* raw = Dimps::Pad::System_RawInput::staticMethods.GetSingleton();
			if (raw) {
				(raw->*Dimps::Pad::System_RawInput::publicMethods.SetDeviceInUse)(outIdx, 0);
			}
			break;
		}
		case Dimps::Pad::PADTYPE_XINPUT: {
			Dimps::Pad::System_XInput* xinput = Dimps::Pad::System_XInput::staticMethods.GetSingleton();
			if (xinput) {
				(xinput->*Dimps::Pad::System_XInput::publicMethods.SetDeviceInUse)(outIdx, 0);
			}
			break;
		}
		}
		return true;
	}

	bool NetplayFacade::IsPadCapturePhaseReady() {
		if (!s_autoPending || !s_gameReady) {
			return false;
		}
		if (s_framesSinceReady < kMinFramesAfterReady) {
			return false;
		}
		if (s_autoStartDwellTicks < kMinDwellBeforeAutoStart) {
			return false;
		}
		return Dimps::Pad::System::staticMethods.GetSingleton() != nullptr;
	}

	bool NetplayFacade::IsPadCapturePending() {
		if (!s_autoPending || s_padCaptured) {
			return false;
		}
		if (s_config.deviceIdx != 0xff && s_config.deviceType != 0xff) {
			return false;
		}
		return NetplayFacade::IsPadCapturePhaseReady();
	}

	void NetplayFacade::TickMainMenu() {
		if (!s_autoPending) {
			return;
		}
		if (!s_gameReady) {
			return;
		}

		s_framesSinceReady++;
		if (s_framesSinceReady < kMinFramesAfterReady) {
			return;
		}

		s_autoStartDwellTicks++;

		RootEvent* root = App::GetRootEvent();
		if (!root) {
			return;
		}

		// Pad system may not be ready immediately after boot.
		Dimps::Pad::System* padSys = Dimps::Pad::System::staticMethods.GetSingleton();
		if (!padSys) {
			return;
		}

		if (s_autoStartDwellTicks < kMinDwellBeforeAutoStart) {
			return;
		}

		CheckGraphicsWarning();

		if (!s_padCaptured) {
			uint8_t capturedIdx = 0xff;
			uint8_t capturedType = 0xff;
			if (!NetplayFacade::TryCapturePadForSide(0, capturedIdx, capturedType)) {
				return;
			}
			s_padCaptured = true;
			s_config.deviceIdx = capturedIdx;
			s_config.deviceType = capturedType;
		}

		uint8_t deviceIdx = s_config.deviceIdx;
		uint8_t deviceType = s_config.deviceType;

		std::string name(s_config.displayName);
		if (name.empty()) {
			name = "Player";
		}

		FixedPoint roundTime = { 0, (short)s_config.roundTimeIntegral };

		if (s_config.mode == (int)NetplayMode::Host) {
			std::string hash = sf4e::sidecarHash;
			char hostAddr[128];

			if (s_config.useCentralSession == 2) {
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
				spdlog::info(
					"NetplayFacade: host VPS relay connect {} (broker session {}:{})",
					hostAddr,
					s_config.sessionHost,
					s_config.sessionPort
				);
			}
			else if (s_config.useCentralSession == 1) {
				snprintf(hostAddr, sizeof(hostAddr), "127.0.0.1:%u", s_config.sessionPort);
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
				spdlog::info(
					"NetplayFacade: host relay connect loopback {} (advertised {}:{})",
					hostAddr,
					s_config.sessionHost,
					s_config.sessionPort
				);
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
			std::string hash = sf4e::sidecarHash;
			char joinAddr[128];
			snprintf(joinAddr, sizeof(joinAddr), "%s:%u", s_config.sessionHost, s_config.sessionPort);
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
		fUserApp::TryStartPendingMatch();

		if (fUserApp::netplay && GgpoRelay::Instance().IsActive()) {
			const bool pumpGgpoTunnel = fUserApp::netplay->client._useRelay;
			if (pumpGgpoTunnel) {
				GgpoRelay::Instance().Pump();
			}
		}

		if (
			fSystem::ggpo &&
			!fSystem::bUpdateAllowed &&
			!s_udpGgpoFallbackTried &&
			s_ggpoTransportStatus.effectiveMode == 1 &&
			!s_ggpoTransportStatus.legacyTunnelActive &&
			s_ggpoSyncPhase != GgpoSyncPhase::Running &&
			s_ggpoBattleStartTick != 0 &&
			GetTickCount() - s_ggpoBattleStartTick > 20000
		) {
			s_udpGgpoFallbackTried = true;
			const DWORD waitedMs = GetTickCount() - s_ggpoBattleStartTick;
			spdlog::warn(
				"GgpoTransport: UDP relay GGPO sync timed out after {}ms (phase={} remote={}:{}) — aborting match",
				waitedMs,
				(int)s_ggpoSyncPhase,
				s_ggpoTransportStatus.remoteHost,
				s_ggpoTransportStatus.remotePort
			);
			fSystem::AbortGgpoMatch(
				"GGPO sync timed out — return to lobby and Ready again."
			);
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

	TransportDiagnostics NetplayFacade::GetTransportDiagnostics() {
		TransportDiagnostics td;
		td.ggpoTransport = s_config.ggpoTransport;
		const GgpoRelay::TransportStats relayStats = GgpoRelay::Instance().GetStats();
		td.relayOutboundFrames = relayStats.outboundFrames;
		td.relayOutboundBytes = relayStats.outboundBytes;
		td.relayInboundFrames = relayStats.inboundFrames;
		td.relayInboundBytes = relayStats.inboundBytes;
		if (fUserApp::netplay) {
			const SessionClient::GgpoTunnelStats tunnelStats = fUserApp::netplay->client.GetGgpoTunnelStats();
			td.tunnelSendCount = tunnelStats.sendCount;
			td.tunnelSendBytes = tunnelStats.sendBytes;
			td.tunnelRecvCount = tunnelStats.recvCount;
			td.tunnelRecvBytes = tunnelStats.recvBytes;
		}
		return td;
	}

	void NetplayFacade::ShutdownNetplay(bool closeGgpo) {
		if (closeGgpo && fSystem::ggpo) {
			ggpo_close_session(fSystem::ggpo);
			fSystem::ggpo = nullptr;
		}
		GgpoRelay::Instance().Reset();
		s_ggpoTransportStatus = { 0 };
		s_ggpoSyncPhase = GgpoSyncPhase::None;
		s_ggpoBattleStartTick = 0;
		s_udpGgpoFallbackTried = false;
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
		s_brokerGgpoRemoteHost[0] = '\0';
		s_brokerGgpoRemotePort = 0;
	}

	void NetplayFacade::ClearBattleState() {
		fSystem::snapshotMap.clear();
		if (fUserApp::netplay) {
			fUserApp::netplay->client.pendingRemoteSnapshots.clear();
		}
	}

	void NetplayFacade::CancelDeferredGgpoClose() {
		s_deferGgpoClose = false;
		s_deferredGgpoPending = false;
		s_deferGgpoCloseUntil = 0;
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
			s_deferGgpoClose = false;
			s_deferredGgpoPending = false;
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
		else {
			s_deferGgpoClose = false;
			s_deferredGgpoPending = false;
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
