#pragma once
// What the game's battle hooks tell Ember's runtime, and what they ask it:
// the calls that go from the game back into the library, the other way from
// the game adapter (sf4e__GameAdapter.hxx). A hook includes this rather than
// the whole facade when these are all it needs. Game thread, unless a
// declaration says otherwise.

#include <array>
#include <cstddef>
#include <cstdint>

#include "../common/MatchResult.hxx"
#include "../common/NoticeSeverity.hxx"
#include "../common/RoomLimits.hxx"
#include "../common/sf4e__NetplayConfig.hxx"
#include "../netplay/SessionController.hxx"

namespace sf4e {
	// Authenticated Iroh loopback bridge endpoint selected for this match.
	struct GgpoTransportStatus {
		char remoteHost[NETPLAY_SESSION_HOST_LEN] = { 0 };
		uint16_t remotePort = 0;
	};

	enum class GgpoSyncPhase : uint8_t {
		None = 0,
		Starting = 1,
		Connected = 2,
		Synchronizing = 3,
		Running = 4,
	};

	namespace NetplayFacade {
		// Called on the game thread when the native main menu first ticks.
		void NotifyRuntimeEventSystemReady();

		// The battle, and its rollback session.
		// Once its battle has loaded, the game starts the rollback session on the
		// ports the room's match session opened: it asks for the endpoints, then
		// takes the local port over (EMBER_LIBRARY.md §6, #7).
		struct RuntimeMatchEndpoints {
			std::uint16_t localPort = 0;
			std::size_t localSlot = 0, participantCount = 0;
			std::array<std::uint16_t, room::MaxMatchParticipants> remotePorts = {};
		};
		bool GetRuntimeMatchEndpoints(RuntimeMatchEndpoints& endpoints);
		// The battle calls it once as its rollback session starts, with the
		// endpoints it was given: the runtime records the match's two sides, whether
		// this PC watches, and the table's score, which the HUD and the replay
		// reads. Before the session's first frame.
		void NoteRuntimeMatchStart(const RuntimeMatchEndpoints& endpoints);
        bool BindRuntimeInput(int localSlot);
        // This player's prompt over the WIN, carried to the other game in the
        // raw input (rematch::LocalBits); never shown to the game.
        bool ReadRuntimeMatchInput(int side,unsigned& mapped,unsigned& raw);
		void ReleaseRuntimePortToGgpo();
		// The room link the battle reports to: whether the runtime holds one
		// (RuntimeSession), and the battle's load, which the room answers with
		// Battle::OnSessionSynced once every member's battle has loaded.
		bool HasRuntimeRoomLink();
		void NotifyRuntimeBattleLoaded();
		// The battle's rollback session is one this PC watches, as its seat was
		// when the session started (IsRuntimeSpectator follows the match's seat now).
		bool IsRuntimeBattleWatched();
		void ReportGgpoTransport(const char* remoteHost, uint16_t remotePort);
		GgpoTransportStatus GetGgpoTransportStatus();
		void NotifyGgpoSyncPhase(GgpoSyncPhase phase);
		void ResetGgpoBattleWatch();
		// A netplay battle closed after its GGPO session was already retired, so
		// NotifyRuntimeMatchEnded had no session to come from.
		void NotifyRuntimeBattleClosedWithoutSession();
		// A spectator's own GGPO stream failed with no committed match result and
		// the session is being retired. Its battle then closes sessionless, and
		// the runtime leaves that game instead of ending the view silently.
		void NotifyRuntimeSpectatorStreamFailed();
		// P1's GGPO dropped the spectator in this match slot. Ends its link so
		// that spectator's client gives up at once (IrohMatchSession::EndSpectatorStream).
		void EndRuntimeSpectatorStream(std::size_t slot);
		// This PC watches the current or last match rather than playing it.
		bool IsRuntimeSpectator();
		// A rollback-confirmed native result, with the save frame it was
		// captured at and the input frame confirmed when it was published.
		// `game` counts the games before it in this battle; a `continued` one
		// was followed by a rematch in place and leaves the match open.
		void NotifyRuntimeMatchResult(room::MatchResult result, std::uint64_t captureFrame, std::uint64_t confirmedFrame,
			unsigned game, bool continued);
		// The WIN this result brings up may restart the battle in place: the
		// table offers instant rematches and the result leaves its set open.
		bool RematchInPlaceOffered(room::MatchResult result);
		// The room has committed the end of the current match (any result).
		// Notices about losing that match's stream or peer are moot then.
		bool IsRuntimeMatchEndCommitted();
		bool IsRuntimeRoomActive();
		// Whether leaving for Training, asked for in session `made`, holds against
		// the live controller, native menu and room (NetplayFacade::TrainingHolds).
		// The runtime asks it before posting a Training request, and the native
		// menu again just before it moves.
		bool TrainingRequestHolds(const netplay::Generation& made);
		bool IsDevOverlayEnabled();

		// Notices for the player and the session's end.
		// Records a notice for the player, read back through
		// NetplayStatus::lastError; the one-argument PushAlert records an Error.
		void PushAlert(const char* msg);
		void PushAlert(const char* msg, NoticeSeverity severity);
		// Drops the current notice (a new session starts).
		void ClearMatchNotice();
		// Drops an Info or Warning notice. An Error stays until a newer notice
		// replaces it or a new session starts (NoticeSeverity), so retiring a
		// session keeps the reason it ended.
		void ClearTransientMatchNotice();
		bool IsControlPlaneLost();
		// Called after the battle fully closes while degraded: logs the
		// unverified interval and returns to a safe disconnected state
		// (never a fake healthy lobby).
		void FinalizeControlPlaneLossAfterBattle();
		void ShutdownNetplay(bool closeGgpo);
		void ClearBattleState();
		void CancelDeferredGgpoClose();
		void NotifyMatchEnded();
	}
} // namespace sf4e
