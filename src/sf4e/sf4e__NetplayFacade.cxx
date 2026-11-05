#include "sf4e__NetplayFacade.hxx"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>

#include <spdlog/spdlog.h>

#include "../common/Localization.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "sf4e__GameAdapter.hxx"
#include "sf4e__RuntimeSession.hxx"

using ember::RuntimeSession;

namespace sf4e {

	static ember::game::Battle& GameBattle() { return ember::game::Game().battle; }

	static NetplayConfig s_config = { 0 };
	static GgpoTransportStatus s_ggpoTransportStatus = { 0 };
	static GgpoSyncPhase s_ggpoSyncPhase = GgpoSyncPhase::None;
	// Nonzero while P1 holds a finished battle's GGPO session open so its
	// spectators can finish: the tick after which it closes regardless.
	static ULONGLONG s_spectatorDrainUntil = 0;
	// The deadline and the battle's drain flag (Battle::DrainingSpectators) end together.
	static void EndSpectatorDrain() {
		s_spectatorDrainUntil = 0;
		GameBattle().SetDrainingSpectators(false);
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
            s_verificationLostAtFrame = GameBattle().VerificationHeldAt();
            // Only a live match has a connection to keep.
            PushAlert(loc::T(IsRuntimeMatchLive() ? "runtime.room_recovering" : "runtime.room_control_recovering"),
                NoticeSeverity::Warning);
            return;
        }

		int simulatedFrame = 0;
		if (!GameBattle().FightRunning(simulatedFrame)) {
			HandleNetplayFailure(reason, true);
			return;
		}

		s_verificationLostAtFrame = simulatedFrame;
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
			PushAlert(reason);
		}
		// Retiring the session is what ends the fight: a netplay battle
		// without its session is orphaned and leaves on its own.
		if (GameBattle().Live()) GameBattle().Fail();
		ShutdownNetplay(closeGgpo);
	}


	void NetplayFacade::TickFrame() {
        GameBattle().PollTelemetry();
        // The hold ends once every spectator has left. Until the session is
        // retired the room's terminal receipt stays open, which also locks
        // the fighter's selection.
        if (DrainingSpectators() && (GetTickCount64() >= s_spectatorDrainUntil || !GameBattle().SpectatorStreams())) {
            spdlog::info("NetplayFacade: closing deferred GGPO session spectators={}", GameBattle().SpectatorStreams());
            GameBattle().Retire("deferred_close");
            EndSpectatorDrain();
        }
    }

	void NetplayFacade::ShutdownNetplay(bool closeGgpo) {
		if (closeGgpo && GameBattle().Live()) {
			GameBattle().Retire("shutdown");
		}
		GameBattle().ResetPacing();
		// A match entered but not yet prepared has no session left to play
		// it for: no later battle takes its seed or Training rules.
		GameBattle().DropNextBattle();
		s_ggpoTransportStatus = { 0 };
		s_ggpoSyncPhase = GgpoSyncPhase::None;
		if (RuntimeSession::netplay) {
			RuntimeSession::netplay->client.Disconnect();
			RuntimeSession::netplay.reset();
		}
		if (RuntimeSession::server) {
			RuntimeSession::server->Close();
			RuntimeSession::server.reset();
		}
		EndSpectatorDrain();
		s_coordinationHealthy = s_sessionClientHealthy = true;
		s_verificationLostAtFrame = -1;
		// Keep an Error visible across the shutdown (it explains why the
		// player is back in the menu); drop transient notices.
		ClearTransientMatchNotice();
	}

	void NetplayFacade::ClearBattleState() {
		GameBattle().ClearVerification();
		if (RuntimeSession::netplay) RuntimeSession::netplay->client.ClearBattleVerification();
	}

	void NetplayFacade::CancelDeferredGgpoClose() {
		EndSpectatorDrain();
	}

	bool NetplayFacade::DrainingSpectators() {
		return GameBattle().DrainingSpectators();
	}

	void NetplayFacade::NotifyMatchEnded() {
		NotifyRuntimeMatchEnded();
		ClearBattleState();
		// The hold exists so P1 can drain the spectator streams it owns. Lobby
		// membership is not that set: a receiving spectator counts the other
		// members and would hold its own session open for nothing, and without
		// a room there is nothing to coordinate spectators through at all.
		const std::size_t spectators = IsControlPlaneLost() || !RuntimeSession::netplay ?
			0 : GameBattle().SpectatorStreams();
		if (!spectators) {
			EndSpectatorDrain();
			return;
		}
		// The hold only has to deliver P1's last frames: a spectator plays out its
		// backlog after P1's link closes (PollSpectatorExit). SpectatorPolicy drops
		// any spectator with DropQueueFrames or more frames unacknowledged, so a
		// live one has received everything well inside this bound.
		s_spectatorDrainUntil = GetTickCount64() + 10000;
		GameBattle().SetDrainingSpectators(true);
		spdlog::info("NetplayFacade: deferring GGPO close for {} spectator streams", spectators);
	}

} // namespace sf4e
