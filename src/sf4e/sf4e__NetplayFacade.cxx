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
#include "../common/Localization.hxx"
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
	// Nonzero while P1 holds a finished battle's GGPO session open so its
	// spectators can finish: the tick after which it closes regardless.
	static ULONGLONG s_spectatorDrainUntil = 0;

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

	// Last health reported by each cause. The control plane is lost while
	// either is false.
	static bool s_coordinationHealthy = true;
	static bool s_sessionClientHealthy = true;
	static int s_verificationLostAtFrame = -1;

	bool NetplayFacade::IsControlPlaneLost() {
		return !(s_coordinationHealthy && s_sessionClientHealthy);
	}

	int NetplayFacade::GetVerificationLostFrame() {
		return s_verificationLostAtFrame;
	}

	void NetplayFacade::ObserveControlPlane(ControlPlaneCause cause, bool healthy, const char* reason) {
		const bool wasLost = IsControlPlaneLost();
		(cause == ControlPlaneCause::Coordination ? s_coordinationHealthy : s_sessionClientHealthy) = healthy;
		// Only the edge into loss is handled. Recovery just clears the state,
		// because it cannot retroactively verify frames from the lost interval.
		// A loss outside a running fight ends in HandleNetplayFailure, whose
		// shutdown resets both causes to healthy.
		if (wasLost || !IsControlPlaneLost()) return;

        if (IsRuntimeRecoveryEnabled()) {
            s_verificationLostAtFrame = fSystem::lastGgpoSaveFrame;
            // Only a live match has a connection to keep.
            PushAlert(loc::T(IsRuntimeMatchLive() ? "runtime.room_recovering" : "runtime.room_control_recovering"),
                NoticeSeverity::Warning);
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

		s_verificationLostAtFrame =
			rSystem::GetNumFramesSimulated_FixedPoint(system)->integral;
		spdlog::warn(
			"Netplay: control plane lost at frame {}, continuing fight on GGPO UDP; "
			"verification, results, rematch, and spectator coordination disabled",
			s_verificationLostAtFrame
		);
		// One clear warning. The SessionClient connection is already closed
		// (Step() no-ops, snapshot/hash sends stop); the netplay objects are
		// kept alive until the fight ends so nothing dangles, and no
		// reconnection is attempted, because room identity and lobby IDs are
		// ephemeral and a new client could not safely resume this lobby.
		PushAlert(loc::T("runtime.room_lost_fight_continues"), NoticeSeverity::Warning);
	}

	void NetplayFacade::FinalizeControlPlaneLossAfterBattle() {
		if (IsRuntimeRecoveryEnabled()) return;
		if (!IsControlPlaneLost()) {
			return;
		}
		spdlog::warn(
			"Netplay: match ended after control-plane loss; snapshot/hash verification "
			"was unavailable from frame {} to match end; that interval is UNVERIFIED",
			s_verificationLostAtFrame
		);
		PushAlert(loc::T("runtime.returned_room_lost"));
		// Full teardown to a safe disconnected state (also resets the
		// degraded flags via ShutdownNetplay).
		ShutdownNetplay(true);
	}

	void NetplayFacade::HandleNetplayFailure(const char* reason, bool closeGgpo) {
		if (reason && reason[0]) {
			SetLastError(reason);
		}
		// Retiring the session is what ends the fight: a netplay battle
		// without its session is orphaned and leaves on its own.
		if (fSystem::ggpo) fSystem::simGate.OnFatal();
		ShutdownNetplay(closeGgpo);
	}


	void NetplayFacade::TickFrame() {
        fSystem::PollMatchTelemetry();
        // The hold ends once every spectator has left. Until the session is
        // retired the room's terminal receipt stays open, which also locks
        // the fighter's selection.
        if (DrainingSpectators() && (GetTickCount64() >= s_spectatorDrainUntil || !fSystem::SpectatorStreamCount())) {
            spdlog::info("NetplayFacade: closing deferred GGPO session spectators={}", fSystem::SpectatorStreamCount());
            fSystem::RetireGgpoSession("deferred_close");
            s_spectatorDrainUntil = 0;
        }
    }

	void NetplayFacade::ShutdownNetplay(bool closeGgpo) {
		if (closeGgpo && fSystem::ggpo) {
			fSystem::RetireGgpoSession("shutdown");
		}
		fSystem::ResetPacing();
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
		s_spectatorDrainUntil = 0;
		s_coordinationHealthy = s_sessionClientHealthy = true;
		s_verificationLostAtFrame = -1;
		// Keep an Error visible across the shutdown (it explains why the
		// player is back in the menu); drop transient notices.
		ClearTransientMatchNotice();
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
		s_spectatorDrainUntil = 0;
	}

	bool NetplayFacade::DrainingSpectators() {
		return s_spectatorDrainUntil && fSystem::ggpo;
	}

	void NetplayFacade::NotifyMatchEnded() {
		NotifyRuntimeMatchEnded();
		ClearBattleState();
		// The hold exists so P1 can drain the spectator streams it owns. Lobby
		// membership is not that set: a receiving spectator counts the other
		// members and would hold its own session open for nothing, and without
		// a room there is nothing to coordinate spectators through at all.
		const std::size_t spectators = IsControlPlaneLost() || !fUserApp::netplay ?
			0 : fSystem::SpectatorStreamCount();
		if (!spectators) {
			s_spectatorDrainUntil = 0;
			return;
		}
		// The hold only has to deliver P1's last frames: a spectator plays out its
		// backlog after P1's link closes (PollSpectatorExit). SpectatorPolicy drops
		// any spectator with DropQueueFrames or more frames unacknowledged, so a
		// live one has received everything well inside this bound.
		s_spectatorDrainUntil = GetTickCount64() + 10000;
		spdlog::info("NetplayFacade: deferring GGPO close for {} spectator streams", spectators);
	}

} // namespace sf4e
