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

namespace sf4e {

	static NetplayConfig s_config = { 0 };
	static GgpoTransportStatus s_ggpoTransportStatus = { 0 };
	static GgpoSyncPhase s_ggpoSyncPhase = GgpoSyncPhase::None;
	// Nonzero while P1 holds a finished battle's GGPO session open so its
	// spectators can finish: the tick after which it closes regardless.
	static ULONGLONG s_spectatorDrainUntil = 0;
	// The deadline and the battle hooks' drain flag (fSystem::drainingSpectators) end together.
	static void EndSpectatorDrain() {
		s_spectatorDrainUntil = 0;
		fSystem::drainingSpectators = false;
	}

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
		// Every room runs committed coordination, which recovers the control
		// plane itself, so the loss only tells the player.
		if (wasLost || !IsControlPlaneLost()) return;
		s_verificationLostAtFrame = fSystem::ggpoTimeline.Held();
		// Only a live match has a connection to keep.
		PushAlert(loc::T(IsRuntimeMatchLive() ? "runtime.room_recovering" : "runtime.room_control_recovering"),
			NoticeSeverity::Warning);
	}


	void NetplayFacade::TickFrame() {
        fSystem::PollMatchTelemetry();
        // The hold ends once every spectator has left. Until the session is
        // retired the room's terminal receipt stays open, which also locks
        // the fighter's selection.
        if (DrainingSpectators() && (GetTickCount64() >= s_spectatorDrainUntil || !fSystem::SpectatorStreamCount())) {
            spdlog::info("NetplayFacade: closing deferred GGPO session spectators={}", fSystem::SpectatorStreamCount());
            fSystem::RetireGgpoSession("deferred_close");
            EndSpectatorDrain();
        }
    }

	void NetplayFacade::ShutdownNetplay(bool closeGgpo) {
		if (closeGgpo && fSystem::ggpo) {
			fSystem::RetireGgpoSession("shutdown");
		}
		fSystem::ResetPacing();
		// A match entered but not yet prepared has no session left to play
		// it for: no later battle takes its seed or Training rules.
		fVsBattle::nextRequest = {};
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
		EndSpectatorDrain();
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
		EndSpectatorDrain();
	}

	bool NetplayFacade::DrainingSpectators() {
		return fSystem::drainingSpectators;
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
			EndSpectatorDrain();
			return;
		}
		// The hold only has to deliver P1's last frames: a spectator plays out its
		// backlog after P1's link closes (PollSpectatorExit). SpectatorPolicy drops
		// any spectator with DropQueueFrames or more frames unacknowledged, so a
		// live one has received everything well inside this bound.
		s_spectatorDrainUntil = GetTickCount64() + 10000;
		fSystem::drainingSpectators = true;
		spdlog::info("NetplayFacade: deferring GGPO close for {} spectator streams", spectators);
	}

} // namespace sf4e
