#pragma once
#include "../ui/ControllerNavigation.hxx"
#include "../common/MenuInputCapture.hxx"
#include "../common/FighterCatalog.hxx"
#include "../discord/Presence.hxx"
#include "../netplay/InputAssignment.hxx"

#include "../common/sf4e__NetplayConfig.hxx"
#include "../platform/HelperProcess.hxx"
#include "../netplay/SessionController.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../netplay/MemberView.hxx"
#include "../platform/ApplicationServices.hxx"
#include "../common/RoomLimits.hxx"
#include "../session/RoomModel.hxx"
#include <vector>
#include <array>
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
		char lastError[256] = { 0 };
        std::string matchNames[2];
        unsigned rollbackFrames = 0;
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
			netplay::PlayerPreferences preferences;
			room::Action roomAction;
            int selectedDelay=-1;
		};
		struct RuntimeSnapshot {
            ui::ControllerSample menuController;
            input::MenuContext menuContext = input::MenuContext::Unavailable;
            std::array<selection::Availability,selection::FighterCount> fighterAvailability;
			netplay::Snapshot session;
			room::Snapshot room;
			bool helperReady = false;
			bool atMainMenu = false;
			bool canOpenRoom = false;
			bool canReplaceRoom = false;
			bool canReady = false;
			bool canEditSelection = false;
            std::string selectionLockReason, readyLockReason;
			bool canEditPreferences = false;
			bool canEditLobby = false;
			bool settingsPending = false;
            int selectedDelay=2, recommendedDelay=-1;
            bool delayLocked=false, canProbe=false, canApplyDelay=false;
            std::string probeStatus, probeRoute;
            std::uint64_t probeP50Us=0, probeP95Us=0, probeP99Us=0, probeJitterUs=0;
            bool probeBenchmark=false;
            unsigned probeSamples=0, probeLost=0, probeSent=0, probeExpected=0;
			netplay::PlayerPreferences preferences;
			netplay::LobbySettings lobbySettings;
			std::string settingsError;
			int localSlot = -1;
			bool offlineRequested = false;
			std::string displayName;
			std::string invitation;
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
		bool SubmitRuntimeCommand(RuntimeCommand command);
		bool IsRuntimeRoomActive();
        bool IsRuntimeRecoveryEnabled();
		void NotifyRuntimeMatchEnded();
		void NotifyRuntimeMatchResult(room::MatchResult result);
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
		NetplayStatus GetStatus();
		void SetLastError(const char* msg);
		void PushAlert(const char* msg);
		void HandleNetplayFailure(const char* reason, bool closeGgpo);

		// Phase 7: room/control-plane failure handling. During an active,
		// healthy, non-tunneled GGPO fight this degrades instead of
		// killing the match: the fight continues on GGPO UDP, room sends
		// stop, verification is marked unavailable, and rematch/results/
		// spectator coordination are disabled. In every other situation it
		// falls back to full HandleNetplayFailure.
		void HandleControlPlaneLoss(const char* reason);
        void RestoreControlPlane();
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
		bool ShouldDeferGgpoClose();
		void NotifyMatchEnded();
	}

} // namespace sf4e
