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
            PushAlert(loc::T("runtime.room_recovering"), NoticeSeverity::Warning);
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
			"Netplay: control plane lost at frame {} — continuing fight on GGPO UDP; "
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
			"was unavailable from frame {} to match end — that interval is UNVERIFIED",
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
            // Spectators carry their table too, so everyone watching sees the same count.
            const auto& room = fUserApp::netplay->client.GetRoomSnapshot();
            for (const auto& m : room.members)
                if (m.id == room.localMember && m.table >= 0) {
                    st.hasMatchScore = true;
                    for (int side = 0; side < 2; ++side) st.matchScore[side] = room.tables[m.table].score[side];
                }
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
		// SpectatorPolicy drops a spectator more than DropQueueFrames behind,
		// so a live one finishes well inside this bound.
		s_spectatorDrainUntil = GetTickCount64() + 10000;
		spdlog::info("NetplayFacade: deferring GGPO close for {} spectator streams", spectators);
	}

} // namespace sf4e
