#pragma once
#include "../ui/ControllerNavigation.hxx"
#include "../ui/ChatTranscript.hxx"
#include "../common/MenuInputCapture.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/StageCatalog.hxx"
#include "../discord/Presence.hxx"
#include "../netplay/InputAssignment.hxx"

#include "../common/sf4e__NetplayConfig.hxx"
#include "../common/NetworkRoute.hxx"
#include "../common/MatchSide.hxx"
#include "../platform/HelperProcess.hxx"
#include "../netplay/SessionController.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../netplay/MemberView.hxx"
#include "../netplay/IdentityView.hxx"
#include "../netplay/IdentityRequest.hxx"
#include "../netplay/PublicRooms.hxx"
#include "../netplay/TournamentStatus.hxx"
#include "../platform/ApplicationServices.hxx"
#include "../common/RoomLimits.hxx"
#include "../session/RoomModel.hxx"
#include <optional>
#include "../common/NoticeSeverity.hxx"
#include <vector>
#include <array>
#include <memory>
#include "../Dimps/Dimps__GameEvents.hxx"

namespace sf4e {

	struct NetplayStatus {
		bool active = false;
		bool connected = false;
		bool inLobby = false;
		bool inMatch = false;
		int pingMs = -1;
		uint8_t inputDelay = 0;
		char opponentName[NETPLAY_DISPLAY_NAME_LEN] = { 0 };
		// The most recent netplay notice (connection events, aborts, room
		// loss, join rejections), or empty.
		char lastError[256] = { 0 };
		NoticeSeverity lastErrorSeverity = NoticeSeverity::Info;
        MatchSide matchSides[2];
        // The pair's running win count from its room table; unset outside a room.
        bool hasMatchScore = false;
        std::uint32_t matchScore[2] = { 0, 0 };
        unsigned rollbackFrames = 0;
        int appliedDelay = -1;
        bool spectator = false;
        // Members watching the local member's table (the HUD's "Watching N"); from the room snapshot.
        int spectators = 0;
        // Live GGPO link state for the match HUD.
        bool connectionWarning = false;   // GGPO CONNECTION_INTERRUPTED active
        bool predictionStalled = false;   // GGPO refused local input this frame
        int disconnectCountdownMs = -1;   // time until GGPO drops the peer, or -1
	};

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
		struct RuntimeCommand {
			netplay::Command command;
            platform::ServiceAction service = platform::ServiceAction::None;
            input::Action inputAction = input::Action::None;
    discord::InviteAction discordAction = discord::InviteAction::None;
    std::uint64_t discordRevision = 0;
			std::string displayName;
			Dimps::GameEvents::VsMode::ConfirmedCharaConditions character = {};
			int stage = 0;
			selection::StageMask randomStageExcluded = 0;
			netplay::PlayerPreferences preferences;
			room::Action roomAction;
            int selectedDelay=-1;
            int previewSoundVolume=-1;
            // An identity or bridge request; op None when the command is something else.
            netplay::IdentityRequest identity;
            // Ask the helper for the room's short link (IrohRoom::RequestShortInvitation).
            bool shortInvitation=false;
            // Refresh the assignment list, play a match or stop; op None otherwise.
            netplay::tournament::Command tournament;
            // With JoinInvite: the signed ticket (JSON) of a public room's admission, or empty for a private room.
            std::string publicTicket;
            // With the JoinInvite of a public room just created: the table rules chosen on Create.
            std::optional<room::Rules> createdRules;
		};
		struct RuntimeSnapshot {
            ui::ControllerSample menuController;
            input::MenuContext menuContext = input::MenuContext::Unavailable;
            std::array<selection::Availability,selection::FighterCount> fighterAvailability;
			netplay::Snapshot session;
			room::Snapshot room;
			// When `room` arrived (GetTickCount64); idle times count on from it.
			std::uint64_t roomReceivedMs = 0;
			// The newest room events for the chat, oldest first.
			std::vector<ui::RoomNotice> roomNotices;
			bool helperReady = false;
			bool atMainMenu = false;
			// A started match is waiting for the player to return to the main menu.
			bool matchWaitsForMenu = false;
			bool canOpenRoom = false;
			bool canReplaceRoom = false;
			bool canReady = false;
			// The environment accepts a Ready send now (no in-flight request
			// considered). The runtime drains a parked press against this.
			bool readyGate = false;
			// A Ready press is being carried through drain, fence, send and
			// commit. The interface shows one steady "Readying up..." state.
			bool readyRequested = false;
			// Set when a press could not be honoured; the sequence changes per failure.
			std::string readyFailure;
			std::uint64_t readyFailureSequence = 0;
			// The fighter the opponent changed to since this player last
			// readied (-1: none); the sequence changes per change.
			int opponentChangedFighter = -1;
			std::uint64_t opponentChangeSequence = 0;
			bool canEditSelection = false;
            std::string selectionLockReason;
			bool canEditPreferences = false;
			bool canEditLobby = false;
			bool settingsPending = false;
            int selectedDelay=2, recommendedDelay=-1;
            bool autoDelayMeasured=false;
            // The opponent's Ready delay, or -1 until they Ready.
            int opponentDelay=-1;
            bool delayLocked=false, canProbe=false, canApplyDelay=false;
            std::string probeStatus;
            RouteKind probeRoute=RouteKind::Unknown;
            std::string probeRelay, probeOpponent;
            NatClass probeOpponentNat=NatClass::Unknown;
            NetworkSummary netReport;
            std::uint64_t probeP50Us=0, probeP95Us=0, probeP99Us=0, probeJitterUs=0;
            bool probeBenchmark=false;
            unsigned probeSamples=0, probeLost=0, probeSent=0, probeExpected=0;
			netplay::PlayerPreferences preferences;
			netplay::LobbySettings lobbySettings;
			std::string settingsError;
			std::string languagePreference = "auto";
			int localSlot = -1;
			bool offlineRequested = false;
			std::string displayName;
			std::string invitation;
			// The room's short link once the helper has one; see IrohRoom.
			std::string shortInvitation;
			bool shortInvitationPending = false;
			std::uint64_t shortInvitationFailures = 0;
			// The newest room link opened from the browser, as an https link.
			std::string pendingJoinLink;
			std::uint64_t pendingJoinSequence = 0;
			bool pendingJoinDirect = false;
			std::string helperError;
            std::string gameplayInputError;
            std::vector<netplay::MemberView> members;
            netplay::NetworkAvailability network = netplay::NetworkAvailability::Starting;
            platform::ServiceSnapshot services;
            std::string controller;
    bool discordPending = false, discordConfirm = false, discordCanSwitch = false;
    std::uint64_t discordRevision = 0;
    std::string discordStatus;
            input::Capture inputCapture = input::Capture::Idle;
            input::Device inputDevice;
            bool canChangeController = false, controllerReady = false;
            // The identity as the helper last reported it. identityTicket is the
            // interface's last request the runtime handled, and identityRequest
            // the helper request it became, or 0 when it was refused, with the
            // catalog id of why in identityRefusal.
            netplay::IdentityView identity;
            std::uint64_t identityTicket = 0, identityRequest = 0;
            std::string identityRefusal;
            // The tournament match being played and the player's assignments.
            netplay::tournament::Status tournament;
            // The bridge's public rooms and the last admission request's answer.
            netplay::publicrooms::Status publicRooms;
		};
		// Everything the overlay draws from, built on the game thread at the end
		// of each outer tick and never changed afterwards. It owns its values:
		// nothing in it points into live game, session or GGPO state, so the
		// drawing thread can hold it across a whole frame while the game thread
		// moves on, shuts a session down or replaces the room.
		struct PresentationSnapshot {
			// The latest room view; republished less often than this envelope.
			std::shared_ptr<const RuntimeSnapshot> runtime;
			NetplayStatus netplay;
			// A GGPO session exists (fighting, spectating or draining spectators).
			bool ggpoSessionActive = false;
			std::uint64_t sequence = 0;
		};
		// Configure copies POD under the loader lock. Start/Stop run from the
		// normal platform lifecycle; TickRuntime runs only on the game thread.
		void ConfigureHelper(const platform::HelperBootstrap& bootstrap, uint32_t startupError);
		void ConfigureDiscord(const platform::HelperBootstrap& bootstrap);
        void StartHelper();
		void NotifyRuntimeGameReady();
		// Called on the game thread when the native main menu first ticks.
		void NotifyRuntimeEventSystemReady();
		void StopHelper();
		void TickRuntime();
		RuntimeSnapshot GetRuntimeSnapshot();
		// The published snapshot itself, shared rather than copied. Per-frame
		// readers (overlay, input) use this; the snapshot is immutable once
		// published and is never null. Safe on any thread, before StartHelper
		// and after StopHelper.
		std::shared_ptr<const RuntimeSnapshot> GetRuntimeSnapshotShared();
		// The overlay's frame input, on any thread; never null.
		std::shared_ptr<const PresentationSnapshot> GetPresentationSnapshotShared();
		// Game thread, once at the end of every outer tick: expires notices and
		// publishes the next PresentationSnapshot.
		void PublishPresentationFrame();
		// Any thread. False when the command is malformed, the queue is full,
		// or no runtime is accepting commands.
		bool SubmitRuntimeCommand(RuntimeCommand command);
		bool IsRuntimeRoomActive();
		// The room being opened is a public one joined with a ticket, so the host's
		// refusals are worded for it.
		bool IsRuntimePublicJoin();
        bool IsRuntimeRecoveryEnabled();
		void NotifyRuntimeMatchEnded();
		// A netplay battle closed after its GGPO session was already retired, so
		// NotifyRuntimeMatchEnded had no session to come from.
		void NotifyRuntimeBattleClosedWithoutSession();
		// A spectator's own GGPO stream failed with no committed match result and
		// the session is being retired. Its battle then closes sessionless, and
		// the runtime leaves that game instead of ending the view silently.
		void NotifyRuntimeSpectatorStreamFailed();
		// A rollback-confirmed native result, with the save frame it was
		// captured at and the input frame confirmed when it was published.
		void NotifyRuntimeMatchResult(room::MatchResult result, std::uint64_t captureFrame, std::uint64_t confirmedFrame);
		// The room has committed the end of the current match (any result).
		// Notices about losing that match's stream or peer are moot then.
		bool IsRuntimeMatchEndCommitted();
		// A match is being prepared or played, or its GGPO session is live for
		// the match (not only draining spectators).
		bool IsRuntimeMatchLive();
		// Retires a spectator's view of a match whose end the room committed,
		// once its stream is played out or its exit bound passed. Called after
		// the outer tick's GGPO poll.
		void PollSpectatorExit();
		struct RuntimeMatchEndpoints {
			std::uint16_t localPort = 0;
			std::size_t localSlot = 0, participantCount = 0;
			std::array<std::uint16_t, room::MaxMatchParticipants> remotePorts = {};
		};
		bool GetRuntimeMatchEndpoints(RuntimeMatchEndpoints& endpoints);
        bool BindRuntimeInput(int localSlot);
        bool ReadRuntimeMatchInput(int side,unsigned& mapped,unsigned& raw);
		void ReleaseRuntimePortToGgpo();
		void InitFromPayload(const NetplayConfig& cfg);
		const NetplayConfig& GetConfig();
		void ReportGgpoTransport(const char* remoteHost, uint16_t remotePort);
		GgpoTransportStatus GetGgpoTransportStatus();
		GgpoSyncPhase GetGgpoSyncPhase();
		void NotifyGgpoSyncPhase(GgpoSyncPhase phase);
		void ResetGgpoBattleWatch();
		void MarkGgpoBattleStarted();
		bool IsDevOverlayEnabled();
		void NotifyGameReady();
		void TickFrame();
		// Game thread only: samples live netplay state. Other threads read the
		// sample PublishPresentationFrame published.
		NetplayStatus GetStatus();
		// Game thread only. Records a notice for the player, read back through
		// NetplayStatus::lastError; SetLastError and the one-argument PushAlert
		// record an Error.
		void SetLastError(const char* msg);
		void PushAlert(const char* msg);
		void PushAlert(const char* msg, NoticeSeverity severity);
		// The notice for a confirmed state divergence, worded in the player's
		// language: an Error when the fight itself ended, a Warning when only
		// a spectator's own view diverged.
		void PushDesyncNotice(bool spectatorOnly);
		// Drops the current notice (a new session starts).
		void ClearMatchNotice();
		// Drops an Info or Warning notice. An Error stays until a newer notice
		// replaces it or a new session starts (NoticeSeverity), so retiring a
		// session keeps the reason it ended.
		void ClearTransientMatchNotice();
		void HandleNetplayFailure(const char* reason, bool closeGgpo);

		// Phase 7: room and control-plane failure handling. The room
		// coordination and the session client each report their health, and
		// the control plane counts as lost while either one is unhealthy.
		// With runtime recovery enabled the loss raises the recovering
		// notice. Otherwise, during an active, healthy, non-tunneled GGPO
		// fight it degrades instead of killing the match: the fight
		// continues on GGPO UDP, room sends stop, verification is marked
		// unavailable, and rematch, results and spectator coordination are
		// disabled. In every other situation it falls back to full
		// HandleNetplayFailure.
		// Callers report a cause's health whenever they observe it. The loss
		// handling runs once on the edge into loss; the edge out of it only
		// clears the state.
		enum class ControlPlaneCause { Coordination, SessionClient };
		void ObserveControlPlane(ControlPlaneCause cause, bool healthy, const char* reason);
		bool IsControlPlaneLost();
		// Frame at which snapshot/hash verification became unavailable
		// (-1 when the control plane is healthy).
		int GetVerificationLostFrame();
		// Called after the battle fully closes while degraded: logs the
		// unverified interval and returns to a safe disconnected state
		// (never a fake healthy lobby).
		void FinalizeControlPlaneLossAfterBattle();
		void ShutdownNetplay(bool closeGgpo);
		void ClearBattleState();
		void CancelDeferredGgpoClose();
		// True while P1 keeps a finished battle's GGPO session open only to
		// drain spectator streams. The fight is over; its peers' events are not.
		bool DrainingSpectators();
		void NotifyMatchEnded();
	}

} // namespace sf4e
