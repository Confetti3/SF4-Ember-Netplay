#pragma once
// Internals shared by the NetplayRuntime translation units. The runtime is
// one object driven by TickRuntime on the game thread; its phases live in
// separate files by concern:
//   sf4e__NetplayRuntime.cxx           the object, its startup and shutdown, the tick order
//   sf4e__NetplayRuntime__Room.cxx     command dispatch, room events, parked intents
//   sf4e__NetplayRuntime__Match.cxx    the match lifecycle, results and teardown
//   sf4e__NetplayRuntime__Input.cxx    controller capture and the gameplay device
//   sf4e__NetplayRuntime__Status.cxx   the published snapshot, errors, Discord
//   sf4e__NetplayRuntime__Tournament.cxx  playing a bridge-run tournament match
//   sf4e__NetplayRuntime__PublicRooms.cxx the bridge's public room list and admissions
#include "sf4e__NetplayFacade.hxx"
#include "sf4e__RuntimeBridge.hxx"
#include "sf4e__InputDevices.hxx"
#include "../Dimps/Dimps__Selection.hxx"
#include "../Dimps/Dimps__Sound.hxx"
#include "sf4e.hxx"
#include "sf4e__UserApp.hxx"
#include "sf4e__OverlayPrefs.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../platform/HelperClient.hxx"
#include "../session/IrohRoom.hxx"
#include "../session/IrohMatchSession.hxx"
#include "../session/RoomRecoveryRuntime.hxx"
#include "../session/ReadyChime.hxx"
#include "../netplay/AutoDelayCheck.hxx"
#include "../netplay/BoundedMailbox.hxx"
#include "../netplay/MatchResultOutbox.hxx"
#include "../netplay/MatchEndRules.hxx"
#include "../netplay/ParkedIntent.hxx"
#include "../netplay/TournamentPlay.hxx"
#include "../netplay/PublicRooms.hxx"
#include "../netplay/TournamentStatus.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../netplay/ProfileRecordJson.hxx"
#include "../netplay/RoomPreferences.hxx"
#include "../netplay/InputDelayPreference.hxx"
#include "../common/SpectatorPolicy.hxx"
#include "../common/StageCatalog.hxx"
#include "../common/EnvFlag.hxx"
#include "../common/SessionTrace.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"
#include "../common/Localization.hxx"
#include "../platform/LocaleWindows.hxx"
#include "../platform/JoinLinkMailbox.hxx"
#include "../platform/UiPreferencesStore.hxx"
#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <ctime>
#include <spdlog/spdlog.h>
#include "../discord/Ticket.hxx"
#include "../training/TrainingRuntime.hxx"

namespace sf4e { namespace NetplayFacade {
namespace internal {
// The lifecycle trace records only changes. Publish runs every application
// tick, so the traced values are compared here first and the JSON document is
// built only when one of them moved.
struct TraceFields {
    int room = -1, match = -1, control = -1, recovery = -1, router = -1, matchPhase = -1;
    std::string routerError, matchError, probe;
    RouteKind probeRoute = RouteKind::Unknown;
    bool nativeSocket = false, resultPending = false, finishPending = false;
    bool leavePending = false, terminalPending = false, probeBenchmark = false;
    bool authorityWritable = false, readyRequested = false, readyGate = false;
    unsigned probeFailure = 0, probeReplies = 0, probeMissed = 0;
    std::uint64_t probeP50Us = 0, probeP95Us = 0, probeP99Us = 0, probeJitterUs = 0;

    bool operator==(const TraceFields& other) const {
        return room == other.room && match == other.match && control == other.control &&
            recovery == other.recovery && router == other.router && matchPhase == other.matchPhase &&
            routerError == other.routerError && matchError == other.matchError &&
            probe == other.probe && probeRoute == other.probeRoute &&
            nativeSocket == other.nativeSocket && resultPending == other.resultPending &&
            finishPending == other.finishPending && leavePending == other.leavePending &&
            terminalPending == other.terminalPending && probeBenchmark == other.probeBenchmark &&
            authorityWritable == other.authorityWritable && readyRequested == other.readyRequested && readyGate == other.readyGate &&
            probeFailure == other.probeFailure && probeReplies == other.probeReplies &&
            probeMissed == other.probeMissed && probeP50Us == other.probeP50Us &&
            probeP95Us == other.probeP95Us && probeP99Us == other.probeP99Us &&
            probeJitterUs == other.probeJitterUs;
    }
};

using Intent = netplay::ParkedIntent<RuntimeCommand>;

// The networking errors that stay until networking is back. They are kept as a
// kind and a code, and their sentence is made from the catalog when it is
// raised and again when the language changes.
enum class StickyError { None, StartFailed, HelperUnavailable };

struct Runtime {
    StickyError stickyError = StickyError::None;
    std::uint32_t stickyCode = 0;
    std::string stickyText;
    SessionTrace trace;
    std::optional<TraceFields> lastTraceFields;
    std::unique_ptr<platform::HelperClient> discordClient;
    discord::PendingInvite discordInvite;
    // The catalog id of the Discord row's status, resolved when published so a
    // language chosen after it was raised still reads in that language. Until
    // the launcher hands over a companion, none is running: it was missing, or
    // did not start.
    const char* discordStatusId = "discord.unavailable_gameplay_ok";
    std::string discordPublished;
    ULONGLONG discordLastPublish = 0;
    std::array<std::uint64_t,8> discordPresenceKey{};
    bool discordPresenceKeyValid=false;

    input::Assignment input;
    bool inputInitialized = false;
    input::Device matchInput;
    int matchInputSide=-1;
    bool matchInputFault=false;
    std::vector<input::Device> inputDevices;
    platform::ApplicationServices services;
    bool updateClosing = false;
	std::unique_ptr<platform::HelperClient> helper;
	std::shared_ptr<session::IrohRoom> room;
	std::unique_ptr<session::IrohMatchSession> match;
	bool matchEntered = false;
	// The generation whose battle the runtime entered, while matchEntered.
	std::uint64_t enteredGeneration = 0;
	// Set when a netplay battle closes with its GGPO session already retired:
	// the generation that was entered. TickMatch decides what it means.
	std::uint64_t sessionlessCloseGeneration = 0;
	// The generation whose GGPO session was retired by this spectator's own
	// stream failure (not by the game finishing). Read when its battle closes.
	std::uint64_t spectatorStreamFailedGeneration = 0;
	// The lock release that failure owes the room. It outlives the battle: it
	// stays pending, resent as needed, until the room confirms it, the player
	// locks in again, or it expires, whatever the room commits meanwhile.
	netplay::SpectatorLockRelease spectatorLockRelease;
	// When the current grant's entry first waited for the main menu.
	std::uint64_t entryDeferredGeneration = 0;
	ULONGLONG entryDeferredSinceMs = 0;
	bool matchEnded = false;
	bool matchFinishedPending = false;
	bool recoveringMatch = false;
	netplay::MatchResultOutbox resultOutbox;
    std::uint64_t finishActionId=0, finishRetryAt=0;
	std::optional<room::Action> matchFinishedAction;
    bool replacementPending=false;
    bool leaveRequested=false, leaveAcknowledged=false;
    // The room being opened is a public one joined with a ticket, so a refusal or
    // failure is worded for it. Set with the join, cleared when the room closes.
    bool publicJoin=false;
    std::uint64_t leaveActionId=0, leaveRetryAt=0, leaveDeadline=0;
    int selectedDelay=2;
    std::uint64_t nextProbeRequest=1;
    // The interface's last identity request: its ticket, the helper request it
    // became (0 when refused) and the catalog id of a refusal.
    std::uint64_t identityTicket=0, identityRequest=0;
    const char* identityRefusal="";
    // The tournament match being played, its helper requests still awaiting
    // an answer (by request id), and the assignment list's last refresh.
    netplay::tournament::TournamentPlay tournament;
    struct TournamentRequest { netplay::tournament::Output::Kind kind; std::uint64_t generation = 0; };
    std::map<std::uint64_t, TournamentRequest> tournamentRequests;
    std::uint64_t assignmentRequest=0;
    netplay::tournament::AssignmentList assignmentList;
    // The public room list and the last create or ticket request: their helper
    // request ids (0 when none is in flight), when each was sent, and what
    // the interface sees.
    std::uint64_t roomListRequest=0, admissionRequest=0;
    std::uint64_t roomListSentMs=0, admissionSentMs=0;
    netplay::publicrooms::Status publicRooms;
    // Match links from the browser (ember://tournament/open), from a later
    // launcher or the start argument, and the last one the interface was given.
    platform::MatchLinkMailbox matchLinks;
    netplay::tournament::OpenedLink openedLink;
    // Discord connect links (ember://discord/connect) likewise: the service.
    platform::ConnectLinkMailbox connectLinks;
    netplay::tournament::OpenedLink openedConnect;
    // Public room links (ember://room/open) likewise: the service and the room.
    platform::PublicRoomLinkMailbox publicRoomLinks;
    netplay::tournament::OpenedRoomLink openedRoomLink;
    netplay::AutoDelayCheck autoDelayCheck;
    session::RoomRecoveryRuntime recovery;
    std::uint64_t observedAuthorityTerm=0;
	// A terminal receipt is released only after its local outcome has been
	// persisted and native/helper teardown has reached Idle. MatchEnded itself
	// is deliberately insufficient: spectators have no profile write, while a
	// fighter may still own GGPO's socket when the event is delivered.
	bool terminalAckPending = false;
	bool terminalOutcomeConsumed = false;
	std::uint8_t terminalAckTable = 0;
	std::uint64_t terminalAckGeneration = 0;
	std::uint64_t matchFinishedGeneration = 0;
	std::uint8_t matchFinishedTable = 0;
	std::unique_ptr<room::Action> pendingAbort;
	ULONGLONG pendingAbortDeadline = 0;
	// Presses the room could not take yet. A room action (Queue, Watch, chat
	// and the like) gets a short budget behind the authority fence, and none
	// while it waits on the match teardown it triggered. A Ready/Rematch press
	// is armed from the accepted press until the seat flag commits or it
	// fails, so the interface shows one steady state and a stall is reported
	// instead of the press vanishing. A lobby edit waits on the previous
	// match's drain.
	Intent roomActionIntent{3000, Intent::Completion::OnDispatch};
	// Chat parks on its own, so a message never displaces a table action.
	Intent chatIntent{3000, Intent::Completion::OnDispatch};
	// Outlives the helper's teardown bound: a match that fails to close ends the
	// room with its own message before a parked Ready can blame it (F-008).
	Intent readyIntent{session::MatchTeardownTiming::HelperTimeoutMs + 5000, Intent::Completion::OnCommit};
	room::ReadyChime readyChime;
	// The opponent's fighter changed between games; the sequence moves per change.
	room::OpponentFighterWatch opponentFighterWatch;
	std::uint64_t opponentChangeSequence = 0;
	Intent lobbyEditIntent{15000, Intent::Completion::OnDispatch};
	std::string readyFailure;
	std::uint64_t readyFailureSequence = 0;
	// Generation for which "a participant left" was already announced.
	std::uint64_t participantLeftGeneration = 0;
	// The current match's generation once the room has committed its end
	// (any result). Nothing about that match needs the player afterwards.
	std::uint64_t committedEndGeneration = 0;
	netplay::MatchEndLog matchEndLog;
	// `error` is cleared by only a handful of successful actions, so every
	// transient message otherwise stayed pinned for the session and hid save
	// feedback. Transient errors expire; the helper-unavailable startup
	// errors stay because the condition they describe persists.
	std::string errorShown;
	ULONGLONG errorShownAtMs = 0;
	std::unique_ptr<netplay::LobbySettings> pendingLobbySettings;
	ULONGLONG lobbySettingsDeadline = 0;
	netplay::PlayerPreferences preferences;
	netplay::SessionController controller;
	// Shared with the bridge (sf4e__RuntimeBridge), which producers on other
	// threads push through; StopHelper closes it before this object goes.
	std::shared_ptr<bridge::CommandMailbox> commands = std::make_shared<bridge::CommandMailbox>(32, 128 * 1024);
	std::string displayName;
	std::string languagePreference = "auto";
	std::string error;
	bool ready = false;
	bool eventSystemReady = false;
	bool attached = false;
	bool helperLossReported = false;
	bool offlineRequested = false;
	// Room links from the browser (ember://join/...): the launcher's start
	// argument, or one a later launcher handed over. The newest waits here
	// as the https link for the Join screen; the sequence tells it apart.
	platform::JoinLinkMailbox joinLinks;
	std::string pendingJoinLink;
	std::uint64_t pendingJoinSequence = 0;
	// A link joins by itself only if it arrived with no room or match open,
	// and only for a while after it arrived (JoinLinkDirectWindow); the
	// window covers a game that is still starting up.
	bool pendingJoinFree = false;
	std::chrono::steady_clock::time_point pendingJoinArrived{};
};

struct PostPublishState {
    netplay::Snapshot session;
    bool discordCanSwitch=false, canOpenRoom=false;
};

// The runtime object; null until StartHelper and after StopHelper.
extern Runtime* runtime;

// State shared by every phase (sf4e__NetplayRuntime.cxx).
void ResetMatchEntry();
bool LocalIsSpectator();
bool AtMainMenu();
void CloseRoom();
void Apply(netplay::EventKind kind, const std::string& error = {});
bool CanEditLobby();
bool CanBeginReplacement();

// Command dispatch and room events (sf4e__NetplayRuntime__Room.cxx).
void FailReady(const char* reason);
void DrainCommands(bool helperReady);
void DrainRoomEvents();
void PersistTerminalOutcome();
void DrainActionReplies();
void ConfirmLobbySettings();
void ResolvePendingIntents(bool helperReady);
// A room command (host, join, leave) on tournament play's behalf, through
// the same checks as a press.
netplay::DispatchOutcome DispatchTournamentRoomCommand(netplay::Command command);
// The delay a Ready sends: the chosen one, or Auto's.
int ReadyDelay();
// Auto has a recommendation measured against the seated opponent.
bool AutoDelayMeasured();
// Auto is still measuring the seated opponent, so a Ready holds for it.
bool AutoDelayMeasuring();
void TickAutoDelay(bool helperReady);

// The match lifecycle (sf4e__NetplayRuntime__Match.cxx).
void AbortLocalMatch(const char* reason, NoticeSeverity severity = NoticeSeverity::Error);
void RetireFinishedMatch(const char* label);
void RetryPendingAbort();
void RetrySpectatorLockRelease();
// Game thread: the announcer's challenger call at a percent of the game's voice volume.
void PlayChallengerCall(int volumePercent);
void RetryMatchFinished();
void PumpResultOutbox();
void TickMatch();
void ReleaseFinishedMatch();

// Tournament play (sf4e__NetplayRuntime__Tournament.cxx).
void DispatchTournament(const netplay::tournament::Command& command, bool helperReady);
void TickTournament(bool helperReady);
// Hands a match link from the browser to the interface, which opens its row.
void OpenMatchLink(const tournament_link::MatchLink& link);
// `launched`: the link started Ember, so it was just clicked.
void OpenConnectLink(const std::string& bridge, bool launched = false);
// Hands a public room link to the interface, which asks for the room's ticket
// when the player is free. `launched` as for OpenConnectLink.
void OpenPublicRoomLink(const tournament_link::RoomLink& link, bool launched = false);
// A table's end as the room committed it, with how it compares to this game's own capture.
void ObserveTournamentTerminal(const room::Event& event, netplay::MatchResultOutbox::TerminalResult terminal);
netplay::tournament::Status TournamentStatus();

// Public rooms (sf4e__NetplayRuntime__PublicRooms.cxx).
void DispatchPublicRooms(const netplay::tournament::Command& command, bool helperReady);
// True when the answer was to a public room request, which it then took.
bool TakePublicRoomsAnswer(const session::TournamentAnswer& answer);
// Gives up on a request the helper never answered.
void ExpirePublicRooms(std::uint64_t nowMs);

// Controllers (sf4e__NetplayRuntime__Input.cxx).
std::string DeviceName(const input::Device& device);
std::string ControllerLabel(const input::Device& device);
void CaptureMenuInput();

// The published snapshot, errors and Discord (sf4e__NetplayRuntime__Status.cxx).
void FillNetworkDiagnostics(platform::DiagnosticsView& view);
std::string StickyText();
void RaiseStickyError(StickyError kind, std::uint32_t code = 0);
bool StickyRuntimeError(const std::string& error);
PostPublishState Publish();
PostPublishState PublishThrottled();
void PumpDiscordClient();
void PublishAndTickDiscordInvite();
} // namespace internal
using namespace internal;
} } // namespace sf4e::NetplayFacade
