#include "sf4e__NetplayFacade.hxx"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>

#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../common/agent_debug_log.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"
#include "../session/sf4e__SessionClient.hxx"
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
	static GgpoTransportStatus s_ggpoTransportStatus = { 0 };
	static GgpoSyncPhase s_ggpoSyncPhase = GgpoSyncPhase::None;
	static bool s_deferGgpoClose = false;
	static bool s_deferredGgpoPending = false;
	static ULONGLONG s_deferGgpoCloseUntil = 0;

	// The single current notice. Earlier builds queued alerts here but
	// nothing read the queue and GetStatus never filled lastError, so every
	// in-match message was lost. One slot is enough: the newest event is
	// the one the player needs, and severity decides how long it stays.
	struct MatchNotice {
		std::string text;
		NoticeSeverity severity = NoticeSeverity::Info;
		ULONGLONG shownAtMs = 0;
	};
	static MatchNotice s_notice;
	static const ULONGLONG kInfoNoticeMs = 6000;
	static const ULONGLONG kWarningNoticeMs = 12000;

	void NetplayFacade::NotifyGameReady() { NotifyRuntimeGameReady(); }

	void NetplayFacade::InitFromPayload(const NetplayConfig& cfg) {
        s_config = cfg;
        s_config.mode = static_cast<int>(NetplayMode::Idle);
        s_config.useRelay = 0;
    }

	const NetplayConfig& NetplayFacade::GetConfig() {
		return s_config;
	}

	void NetplayFacade::ReportGgpoTransport(
		const char* remoteHost,
		uint16_t remotePort
	) {
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
		s_ggpoSyncPhase = GgpoSyncPhase::Starting;
	}

	void NetplayFacade::MarkGgpoBattleStarted() {
		s_ggpoSyncPhase = GgpoSyncPhase::Starting;
	}

	bool NetplayFacade::IsDevOverlayEnabled() {
#ifdef SF4E_DEVELOPER_UI
        return true;
#else
        return false;
#endif
    }

	void NetplayFacade::PushAlert(const char* msg, NoticeSeverity severity) {
		if (!msg || !msg[0]) return;
		s_notice.text.assign(msg, (std::min)(strlen(msg), size_t(255)));
		s_notice.severity = severity;
		s_notice.shownAtMs = GetTickCount64();
		spdlog::info("Netplay notice ({}): {}", (int)severity, s_notice.text);
	}

	void NetplayFacade::SetLastError(const char* msg) { PushAlert(msg, NoticeSeverity::Error); }

	void NetplayFacade::PushAlert(const char* msg) { PushAlert(msg, NoticeSeverity::Error); }

	void NetplayFacade::ClearMatchNotice() {
		s_notice = MatchNotice();
	}

	static void ExpireNotice(ULONGLONG now) {
		if (s_notice.text.empty()) return;
		const ULONGLONG age = now - s_notice.shownAtMs;
		if ((s_notice.severity == NoticeSeverity::Info && age >= kInfoNoticeMs) ||
			(s_notice.severity == NoticeSeverity::Warning && age >= kWarningNoticeMs)) {
			s_notice = MatchNotice();
		}
	}

	static bool s_controlPlaneLost = false;
	static int s_verificationLostAtFrame = -1;

	bool NetplayFacade::IsControlPlaneLost() {
		return s_controlPlaneLost;
	}

	int NetplayFacade::GetVerificationLostFrame() {
		return s_verificationLostAtFrame;
	}

	void NetplayFacade::HandleControlPlaneLoss(const char* reason) {
		if (s_controlPlaneLost) {
			// Already degraded; nothing further to do (and no alert spam).
			return;
		}

        if (IsRuntimeRecoveryEnabled()) {
            s_controlPlaneLost = true;
            s_verificationLostAtFrame = fSystem::lastGgpoSaveFrame;
            PushAlert("Room control is recovering. Your match connection is retained.", NoticeSeverity::Warning);
            return;
        }

		rSystem* system = rSystem::staticMethods.GetSingleton();
		const bool fightRunning =
			fSystem::ggpo &&
			fSystem::simGate.phase == sf4e::gate::PHASE_RUNNING &&
			!fSystem::simGate.fatalError &&
			system &&
			*rSystem::staticVars.CurrentBattleFlow != rSystem::BF__IDLE;

		if (!fightRunning) {
			HandleNetplayFailure(reason, true);
			return;
		}

		s_controlPlaneLost = true;
		s_verificationLostAtFrame =
			rSystem::GetNumFramesSimulated_FixedPoint(system)->integral;
		spdlog::warn(
			"Netplay: control plane lost at frame {} — continuing fight on GGPO UDP; "
			"verification, results, rematch, and spectator coordination disabled",
			s_verificationLostAtFrame
		);
		// One clear warning. The SessionClient connection is already closed
		// (Step() no-ops, snapshot/hash sends stop); the netplay objects are
		// kept alive until the fight ends so nothing dangles, and no
		// reconnection is attempted — room identity and lobby IDs are
		// ephemeral, so a new client could not safely resume this lobby.
		PushAlert("Room connection lost. The fight continues, but rematch and results are disabled.", NoticeSeverity::Warning);
	}

	void NetplayFacade::FinalizeControlPlaneLossAfterBattle() {
		if (IsRuntimeRecoveryEnabled()) return;
		if (!s_controlPlaneLost) {
			return;
		}
		spdlog::warn(
			"Netplay: match ended after control-plane loss; snapshot/hash verification "
			"was unavailable from frame {} to match end — that interval is UNVERIFIED",
			s_verificationLostAtFrame
		);
		PushAlert("Returned to the menu: the room connection was lost during the match.");
		// Full teardown to a safe disconnected state (also resets the
		// degraded flags via ShutdownNetplay).
		ShutdownNetplay(true);
	}

    void NetplayFacade::RestoreControlPlane() {
        // Recovery cannot retroactively verify frames from the lost interval.
        s_controlPlaneLost = false;
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
			fSystem::simGate.OnFatal();
			fSystem::bUpdateAllowed = false;
		}

		ShutdownNetplay(closeGgpo);
	}


	void NetplayFacade::TickFrame() {
        fSystem::PollMatchTelemetry();
        if (s_deferredGgpoPending && fSystem::ggpo && !ShouldDeferGgpoClose()) {
            fSystem::RetireGgpoSession("deferred_close");
            s_deferredGgpoPending = s_deferGgpoClose = false;
        }
    }

	NetplayStatus NetplayFacade::GetStatus() {
		NetplayStatus st;
		const ULONGLONG now = GetTickCount64();
		ExpireNotice(now);
		if (!s_notice.text.empty()) {
			strncpy_s(st.lastError, s_notice.text.c_str(), _TRUNCATE);
			st.lastErrorSeverity = s_notice.severity;
		}
		if (fSystem::ggpo) {
			st.connectionWarning = fSystem::simGate.connectionWarningActive;
			st.predictionStalled = fSystem::simGate.predictionStalled;
			st.disconnectCountdownMs = fSystem::DisconnectCountdownMs();
		}
		st.active = fUserApp::netplay != nullptr || fUserApp::server != nullptr;
		if (fUserApp::netplay) {
			st.connected = fUserApp::netplay->client.IsConnected();
			st.inLobby = st.connected && fSystem::ggpo == nullptr;
			st.inMatch = fSystem::ggpo != nullptr;
			st.inputDelay = fUserApp::netplay->delay;
            for (int side = 0; side < 2; ++side) st.matchNames[side] = fUserApp::netplay->matchNames[side];
            st.rollbackFrames = fSystem::RecentRollbackFrames();

			for (const auto& m : fUserApp::netplay->client._lobbyData.members) {
				if (m.name != fUserApp::netplay->client._name) {
					strncpy_s(st.opponentName, m.name.c_str(), _TRUNCATE);
					break;
				}
			}

            if (fSystem::ggpo) {
                st.pingMs = fSystem::matchTelemetry.Ping(GetTickCount64());
                st.appliedDelay = fSystem::matchTelemetry.appliedDelay;
                st.spectator = fSystem::matchTelemetry.spectator;
            }
		}
		return st;
	}

	void NetplayFacade::ShutdownNetplay(bool closeGgpo) {
		if (closeGgpo && fSystem::ggpo) {
			fSystem::RetireGgpoSession("shutdown");
		}
		fSystem::pacer.Reset();
		s_ggpoTransportStatus = { 0 };
		s_ggpoSyncPhase = GgpoSyncPhase::None;
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
		s_controlPlaneLost = false;
		s_verificationLostAtFrame = -1;
		// Keep an Error visible across the shutdown (it explains why the
		// player is back in the menu); drop transient notices.
		if (s_notice.severity != NoticeSeverity::Error) ClearMatchNotice();
	}

	void NetplayFacade::ClearBattleState() {
		fSystem::snapshotMap.clear();
		fSystem::ClearHashCheckpoints();
		if (fUserApp::netplay) {
			fUserApp::netplay->client.pendingRemoteSnapshots.clear();
			fUserApp::netplay->client.pendingRemoteHashes.clear();
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
		if (GetTickCount64() < s_deferGgpoCloseUntil) {
			return true;
		}
		s_deferGgpoClose = false;
		return false;
	}

	void NetplayFacade::NotifyMatchEnded() {
		NotifyRuntimeMatchEnded();
		ClearBattleState();
		if (s_controlPlaneLost) {
			// Without a room there is nothing to coordinate spectators
			// through; never hold the GGPO session open on stale lobby data.
			s_deferGgpoClose = false;
			s_deferredGgpoPending = false;
			return;
		}
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
			s_deferGgpoCloseUntil = GetTickCount64() + 120000;
			spdlog::info("NetplayFacade: deferring GGPO close for {} spectators", spectators);
		}
		else {
			s_deferGgpoClose = false;
			s_deferredGgpoPending = false;
		}
	}

} // namespace sf4e
