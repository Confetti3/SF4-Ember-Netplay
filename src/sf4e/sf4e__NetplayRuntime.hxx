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
#include "sf4e__ReplayStore.hxx"
#include "sf4e__RuntimeBridge.hxx"
#include "sf4e__InputDevices.hxx"
#include "../netplay/ControllerInventory.hxx"
#include "../common/ControllerHints.hxx"
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
#include "../session/RecoveryProgress.hxx"
#include "../session/ReadyChime.hxx"
#include "../session/TrainingCall.hxx"
#include "../common/ReplayImportQueue.hxx"
#include "../netplay/AutoDelayCheck.hxx"
#include "../netplay/CreatedRules.hxx"
#include "../netplay/BoundedMailbox.hxx"
#include "../netplay/Revisioned.hxx"
#include "../netplay/MatchResultOutbox.hxx"
#include "../netplay/MatchEndRules.hxx"
#include "../netplay/ParkedIntent.hxx"
#include "../netplay/TournamentPlay.hxx"
#include "../netplay/WatchPlay.hxx"
#include "../netplay/OwedAbort.hxx"
#include "../netplay/PublicRooms.hxx"
#include "../netplay/BridgeFlights.hxx"
#include "../netplay/CardRequests.hxx"
#include "../netplay/FriendPresence.hxx"
#include "../netplay/FriendRequests.hxx"
#include "../netplay/FriendVisits.hxx"
#include "../netplay/FriendsView.hxx"
#include "../netplay/ArchiveRequests.hxx"
#include "../netplay/CardBundle.hxx"
#include "../netplay/ProfileRequests.hxx"
#include "../netplay/ProfileWire.hxx"
#include "../netplay/LevelRequests.hxx"
#include "../netplay/RankingRequests.hxx"
#include "../netplay/TournamentStatus.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../netplay/ProfileRecordJson.hxx"
#include "../netplay/RoomPreferences.hxx"
#include "../netplay/InputDelayPreference.hxx"
#include "../netplay/MatchHudPreference.hxx"
#include "../netplay/ButtonPromptPreference.hxx"
#include "../netplay/CountryPreferenceJson.hxx"
#include "../netplay/SpeechPreferenceJson.hxx"
#include "../netplay/CardPreferenceJson.hxx"
#include "../netplay/PublicRoomFilterJson.hxx"
#include "../netplay/BoolPreferenceJson.hxx"
#include "../common/SpectatorPolicy.hxx"
#include "../common/StageCatalog.hxx"
#include "../common/EnvFlag.hxx"
#include "../common/SessionTrace.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"
#include "../common/Localization.hxx"
#include "../platform/LocaleWindows.hxx"
#include "../platform/JoinLinkMailbox.hxx"
#include "../platform/UiPreferencesStore.hxx"
#include "../platform/ReplaySender.hxx"
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <variant>
#include <ctime>
#include <spdlog/spdlog.h>
#include "../discord/Ticket.hxx"
#include "../training/TrainingRuntime.hxx"

namespace sf4e { namespace NetplayFacade {
namespace internal {
void LogGameplayController(const input::Device& device, const char* reason);
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

// A parked command waits under one of these (Runtime::readyIntent and the rest).
using Intent = netplay::ParkedIntent<RuntimeRequest>;
// The game's form of the pick a Ready carries.
inline Dimps::GameEvents::VsMode::ConfirmedCharaConditions NativePick(const selection::Pick& pick) {
    Dimps::GameEvents::VsMode::ConfirmedCharaConditions native{};
    selection::ToNative(pick, native);
    return native;
}

// The networking errors that stay until networking is back. They are kept as a
// kind and a code, and their sentence is made from the catalog when it is
// raised and again when the language changes.
enum class StickyError { None, StartFailed, HelperUnavailable };

// What the player is doing, as Discord's presence and friends' presence are both described from it
// (sf4e__NetplayRuntime__Status.cxx): the key moves when anything the description is made from does. `shown`: whether the
// presence and its invitation are given out (Discord's follow the player's settings, friends' are always on).
using PresenceKey = std::array<std::uint64_t, 11>;
struct PresenceShown { bool presence = true, invites = true; };

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
    PresenceKey discordPresenceKey{};
    bool discordPresenceKeyValid=false;

    input::Assignment input;
    bool inputInitialized = false;
    std::optional<gamepads::Identity> setupAdoption;
    std::string padHint;
    ULONGLONG padHintUntil = 0;
    gamepads::ControllerInventory padInventory;
    gamepads::ControllerHints padHints;
    std::vector<gamepads::PadView> pads;
    gamepads::Layer padLayer = gamepads::Layer::Failed;
    bool runningControllerLayer = true;
    std::optional<gamepads::PromptBrand> loggedPromptBrand;
    gamepads::ButtonPrompts loggedButtonPrompts = gamepads::ButtonPrompts::Automatic;
    // The device bound to the local fighter for the match, its side, and
    // whether its last read failed. CloseRoom resets the binding whole.
    struct MatchInputBinding {
        input::Device matchInput;
        int matchInputSide=-1;
        bool matchInputFault=false;
    } matchBinding;
    std::vector<input::Device> inputDevices;
    platform::ApplicationServices services;
    bool updateClosing = false;
	std::unique_ptr<platform::HelperClient> helper;
	std::shared_ptr<session::IrohRoom> room;
	std::unique_ptr<session::IrohMatchSession> match;
	// The entry of the granted match's battle. ResetMatchEntry resets it whole.
	struct MatchEntry {
		bool matchEntered = false;
		// The generation whose battle the runtime entered, while matchEntered, and
		// the table this member was at when it entered (the game WatchedGameNow names).
		std::uint64_t enteredGeneration = 0;
		int enteredTable = -1;
		// Set when a netplay battle closes with its GGPO session already retired:
		// the generation that was entered. TickMatch decides what it means.
		std::uint64_t sessionlessCloseGeneration = 0;
		// The generation whose GGPO session was retired by this spectator's own
		// stream failure (not by the game finishing). Read when its battle closes.
		std::uint64_t spectatorStreamFailedGeneration = 0;
		// When the current grant's entry first waited for the main menu.
		std::uint64_t entryDeferredGeneration = 0;
		ULONGLONG entryDeferredSinceMs = 0;
	} entry;
	// The post-match prompt opened over this battle's WIN. It stays over the
	// battle's close and, after a Rematch, over the main menu while the next
	// game is prepared: until the main menu is back with no Rematch coming, or
	// the next battle is entered.
	bool promptOverBattle = false;
	// When this WIN's prompt became due, for as long as the fighter is asked
	// (rematch::LocalBits); 0 outside such a WIN.
	ULONGLONG promptDueSinceMs = 0;
	// The lock release that failure owes the room. It outlives the battle: it
	// stays pending, resent as needed, until the room confirms it, the player
	// locks in again, or it expires, whatever the room commits meanwhile.
	netplay::SpectatorLockRelease spectatorLockRelease;
	// Lock-ins this member lost because its own view failed while locked in
	// (RuntimeSnapshot::lockInDrops). Only ever grows, so the shell tells each once.
	std::uint64_t lockInDrops = 0;
	bool recoveringMatch = false;
	netplay::MatchResultOutbox resultOutbox;
	// The current match's end and terminal receipt. CloseRoom resets it whole.
	struct MatchTeardown {
		bool matchEnded = false;
		bool matchFinishedPending = false;
		std::uint64_t finishActionId=0, finishRetryAt=0;
		std::optional<room::Action> matchFinishedAction;
		// A terminal receipt is released only after its local outcome has been
		// persisted and native/helper teardown has reached Idle. MatchEnded itself
		// is deliberately insufficient: spectators have no profile write, while a
		// fighter may still own GGPO's socket when the event is delivered.
		bool terminalAckPending = false;
		bool terminalOutcomeConsumed = false;
		std::uint8_t terminalAckTable = 0;
		std::uint64_t terminalAckGeneration = 0;
		std::uint64_t matchFinishedGeneration = 0;
		std::unique_ptr<room::Action> pendingAbort;
		ULONGLONG pendingAbortDeadline = 0;
		// The current match's generation once the room has committed its end
		// (any result). Nothing about that match needs the player afterwards.
		std::uint64_t committedEndGeneration = 0;
		// Generation for which "a participant left" was already announced.
		std::uint64_t participantLeftGeneration = 0;
		netplay::MatchEndLog matchEndLog;
	} teardown;
	// Not part of MatchTeardown: CloseRoom does not reset it.
	std::uint8_t matchFinishedTable = 0;
    bool replacementPending=false;
    // What the room this PC hosts is created as: the player's choices on
    // Create, the closed room's for a replacement, the default for a room it joins.
    room::Charter charter;
    // The player's Leave of the room. CloseRoom resets it whole.
    struct RoomLeave {
        bool leaveRequested=false, leaveAcknowledged=false;
        std::uint64_t leaveActionId=0, leaveRetryAt=0, leaveDeadline=0;
    } leave;
    // Set with the join, cleared when the room closes.
    JoinKind join=JoinKind::Private;
    int selectedDelay=2;
    std::uint64_t nextProbeRequest=1;
    // The interface's last identity request: its ticket, the helper request it
    // became (0 when refused) and the catalog id of a refusal.
    std::uint64_t identityTicket=0, identityRequest=0;
    const char* identityRefusal="";
    // The tournament match being played, its helper requests still awaiting
    // an answer (by request id), and the assignment list's last refresh.
    netplay::tournament::TournamentPlay tournament;
    // The abort of this fighter's tournament game, owed to the bridge if the
    // room never ends it (SettleOwedAbort).
    netplay::tournament::OwedAbort owedAbort;
    struct TournamentRequest { netplay::tournament::Output::Kind kind; std::uint64_t generation = 0; };
    std::map<std::uint64_t, TournamentRequest> tournamentRequests;
    // The tournament match being watched, and its helper requests awaiting an answer.
    netplay::tournament::WatchPlay watch;
    std::map<std::uint64_t, netplay::tournament::WatchOutput::Kind> watchRequests;
    std::uint64_t assignmentRequest=0;
    netplay::tournament::AssignmentList assignmentList;
    // The replay being sent to a bridge or the replay archive (SendSetReplays).
    platform::replays::ReplaySender replaySend;
    // Save set pack's helper request (0 when none) and its file, and what the
    // Replays screen is told of the last pack saved or imported.
    std::uint64_t packRequest = 0;
    std::filesystem::path packPath;
    struct SetNotice { std::string text; bool error = false; std::uint64_t count = 0; } setNotice;
    // What the last batch of imported replay files came to (the batch count itself is the queue's).
    struct ImportNotice { std::string text; bool error = false; } importNotice;
    // The bridge requests awaiting an answer, with what each does with it:
    // the public rooms' and friends' (the other requests above and below keep
    // their own bookkeeping).
    netplay::BridgeFlights flights;
    // The public room list and the last create or ticket request: the flight
    // of the create or ticket (0 when none), which a new one supersedes, and
    // what the interface sees.
    std::uint64_t admissionRequest=0;
    netplay::publicrooms::Status publicRooms;
    // Rankings: the requests in flight and what the interface sees.
    netplay::rankings::Requests rankings;
    // The public replay archive's browser: the page in flight and the listing so far.
    netplay::archive::Requests archive;
    // The second slot of the archive browser: the sets of the profile open. Never the Archive tab's listing.
    netplay::archive::Requests profileSets;
    // Other players' profiles: the one open and the Find a player search, with their requests in flight
    // (sf4e__NetplayRuntime__Levels.cxx).
    netplay::profiles::Requests profiles;
    // The player's own card as the bridge keeps it, and what the save measures: the connection (read at most once
    // a minute) and the relay region last known (sf4e__NetplayRuntime__Levels.cxx).
    netplay::profiles::Saver cardSaver;
    struct CardSignals {
        NetworkLink link = NetworkLink::Unknown;
        bool linkRead = false;
        std::uint64_t linkMs = 0;
        std::string region;
    } cardSignals;
    // The players this PC fought in public rooms lately, kept beside the preferences and never sent
    // (sf4e__NetplayRuntime__Social.cxx).
    netplay::profiles::RecentList recentOpponents;
    // The player's own fighter levels: the read in flight and the last answer.
    netplay::levels::Requests levels;
    // Other players' cards in a public room: the look-up in flight and the answers.
    netplay::levels::Cards cards;
    // The player's friends: the sync or action in flight and the last state, the
    // visit to a friend's public room under way, and whether the player's room
    // can be offered a friend now (sf4e__NetplayRuntime__Social.cxx).
    netplay::friends::Requests friends;
    netplay::friends::Visit friendVisit;
    bool friendsCanInvite = false;
    // What friends are told, described again only when the key of what it is made from moves (or the invitation
    // offered runs out, at `refreshAt`, unix seconds); `revision` moves when the report itself changes.
    struct FriendsPresence {
        bool valid = false;
        PresenceKey key{};
        netplay::friends::Report report;
        netplay::friends::Offer offer;
        std::uint64_t refreshAt = 0, revision = 0;
    } friendsPresence;
    // Match links from the browser (ember://tournament/open), from a later
    // launcher or the start argument, and the last one the interface was given.
    platform::MatchLinkMailbox matchLinks;
    netplay::tournament::OpenedLink openedLink;
    // Whether the player was free when the last match link arrived (it started
    // Ember, or came idle at the main menu with no tournament match under way),
    // and when: it plays its match by itself only for a while (OpenedLink::direct).
    bool openedLinkFree = false;
    std::chrono::steady_clock::time_point openedLinkArrived{};
    // Discord connect links (ember://discord/connect) likewise: the service.
    platform::ConnectLinkMailbox connectLinks;
    // Replay links (common/ReplayLink.hxx) from the launcher, and the file
    // the last one asked for: a question on the Replays screen until the
    // player answers it (RuntimeSnapshot::replays.link).
    platform::ReplayLinkMailbox replayLinks;
    std::string replayLinkAsked;
    netplay::tournament::OpenedLink openedConnect;
    // Public room links (ember://room/open) likewise: the service and the room.
    platform::PublicRoomLinkMailbox publicRoomLinks;
    netplay::tournament::OpenedRoomLink openedRoomLink;
    netplay::AutoDelayCheck autoDelayCheck;
    // The game that just ended, until TickAutoDelay takes it.
    std::optional<netplay::PlayedGame> finishedGame;
    // The table rules chosen when creating a public room, set on its tables
    // once the creator is in it as host (TickCreatedRules).
    netplay::CreatedRules createdRules;
    session::RoomRecoveryRuntime recovery;
    session::RecoveryProgress recoveryProgress;
    std::uint64_t observedAuthorityTerm=0;
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
	// A player waiting in Training is called to their table. The sequences
	// move when the table is to be shown and when a Ready is to be sent for
	// the player; both are the shell's to act on, since a Ready carries the
	// selection the shell holds.
	room::CallOwner trainingCall;
	std::uint64_t trainingCallSequence = 0, trainingReadySequence = 0;
	// When the room was last told whether the player is in Training.
	std::uint64_t trainingSaidAtMs = 0;
	// When the room was last told the player is typing; zero once they stopped typing.
	std::uint64_t typingSaidAtMs = 0;
	// The post-match answer last said to the room, and when (SayPostMatch).
	room::PostMatchSaid postMatchSaid;
	std::uint64_t postMatchSaidAtMs = 0;
	// The newest RoomNoticesKept room events for the chat, numbered from 1 by
	// roomNoticeSequence. The interface takes each once by its number.
	static constexpr std::size_t RoomNoticesKept = 16;
	std::deque<ui::RoomNotice> roomNotices;
	std::uint64_t roomNoticeSequence = 0;
	Intent lobbyEditIntent{15000, Intent::Completion::OnDispatch};
	std::string readyFailure;
	std::uint64_t readyFailureSequence = 0;
	// `error` is cleared by only a handful of successful actions, so every
	// transient message otherwise stayed pinned for the session and hid save
	// feedback. Transient errors expire; the helper-unavailable startup
	// errors stay because the condition they describe persists.
	std::string errorShown;
	ULONGLONG errorShownAtMs = 0;
	std::unique_ptr<netplay::LobbySettings> pendingLobbySettings;
	ULONGLONG lobbySettingsDeadline = 0;
	// Read as `preferences`; changed only through EditPreferences(), which moves
	// the revision that PublishFingerprint compares, so no writer can leave a
	// throttled fight showing old settings.
	netplay::Revisioned<netplay::PlayerPreferences> preferencesStore;
	const netplay::PlayerPreferences& preferences = preferencesStore.Get();
	netplay::PlayerPreferences& EditPreferences() { return preferencesStore.Edit(); }
	netplay::SessionController controller;
	// Shared with the bridge (sf4e__RuntimeBridge), which producers on other
	// threads push through; StopHelper closes it before this object goes.
	std::shared_ptr<bridge::CommandMailbox> commands = std::make_shared<bridge::CommandMailbox>(32, 128 * 1024);
	std::string displayName;
	std::string languagePreference = "auto";
	bool checkUpdatesAtLaunch = true;
	// Windows' region, read once at start: what Automatic shows.
	CountryCode automaticCountry;
	std::string error;
	bool ready = false;
	bool eventSystemReady = false;
	bool attached = false;
	bool helperLossReported = false;
	bool offlineRequested = false;
	// Launcher --host-public-room: the shell keeps a public room open.
	bool hostPublicRoom = false;
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

// What the player may do now, decided in one place. Publish copies it into the
// snapshot the interface draws (same field names); Dispatch and the other
// game-thread decisions take it fresh where they decide (CurrentGates), so a
// press meets the state it is judged by, not the last published snapshot,
// which is a tick old and up to six in a throttled fight.
struct RuntimeGates {
    bool canEditPreferences = false, canEditSelection = false, canEditLobby = false, canChangeController = false;
    bool readyGate = false, canReady = false, canOpenRoom = false, canTrain = false, discordCanSwitch = false;
    bool delayLocked = false, canProbe = false, canApplyDelay = false;
    // The Ultra changes wherever the selection does, and also over the WIN
    // while the post-match prompt asks this fighter: the next game's Ready,
    // which carries it, has not gone yet.
    bool canChangeUltra = false;
    int localSlot = -1;
    int selectedDelay = 2;
    // The seated table's rules, else the lobby's, else the player's own: what a
    // Ready checks the fighter selection against (the snapshot's lobbySettings).
    bool editionSelect = true;
};
// Changes nothing: it reads the runtime, the room client and the battle hooks.
// `state`, `atMainMenu`, `controllerReady` and `helperReady` are passed so that
// Publish judges by exactly the values it publishes.
RuntimeGates ComputeGates(const netplay::Snapshot& state, bool atMainMenu, bool controllerReady, bool helperReady);
// ComputeGates from the state as it is now.
RuntimeGates CurrentGates();

// The runtime object; null until StartHelper and after StopHelper.
extern Runtime* runtime;

// State shared by every phase (sf4e__NetplayRuntime.cxx).
void ResetMatchEntry();
bool LocalIsSpectator();
// The game this PC only watches now: the entered generation and table while a
// spectator's stream is live, else empty.
netplay::WatchedGame WatchedGameNow();
bool AtMainMenu();
// Tests only: makes AtMainMenu answer `value`; nullopt restores the native check.
// Do not Publish while it is true: Publish at the main menu reads the game's pads.
void SetAtMainMenuForTest(std::optional<bool> value);
// The post-match prompt's WIN (or DRAW) screen (sf4e__NetplayRuntime__Status.cxx).
bool PostMatchPromptDue();
void CloseRoom();
void Apply(netplay::EventKind kind, const std::string& error = {});
bool CanEditLobby();
bool CanBeginReplacement();
// Training from the room: at the main menu, with no game of the player's
// starting or running and no Ready of theirs given or on its way. A game
// that is over leaves PostMatch behind until the next one, as it does for
// a Ready. Never from a closed room, nor as a watcher of a game starting
// (a locked-in spectator whose table holds its start) or being played.
// The snapshot publishes it, and a Room TrainingEntry is judged by it.
bool CanTrain(const netplay::Snapshot& session, bool atMainMenu, const room::Snapshot& room, std::uint64_t retired);
// The game generation this PC was in and has fully left: its session idle, native
// GGPO gone and any recovery ended (MatchRecovered). 0 while none, or while that
// teardown is still under way.
std::uint64_t RetiredMatchGeneration();

// Command dispatch and room events (sf4e__NetplayRuntime__Room.cxx).
void FailReady(const char* reason);
void DrainCommands(bool helperReady);
void DrainRoomEvents();
void PersistTerminalOutcome();
void DrainActionReplies();
void ConfirmLobbySettings();
void ResolvePendingIntents(bool helperReady);
// A room command (host, join, leave) on tournament play's behalf, through
// the same checks as a press, for the controller's generation now.
netplay::DispatchOutcome DispatchTournamentRoomCommand(RuntimeRequest command);
// A joined room ended for the player. True when watching a match took it as
// its own news (a notice, then a fresh ask), so the generic error is not shown.
bool EndWatchedRoom(netplay::tournament::WatchRoomEnd end);
// The delay a Ready sends: the chosen one, or Auto's.
int ReadyDelay();
// Auto has a recommendation measured against the seated opponent.
bool AutoDelayMeasured();
// Auto is still measuring the seated opponent, so a Ready holds for it.
bool AutoDelayMeasuring();
void TickAutoDelay(bool helperReady);
void TickCreatedRules(bool helperReady);

// The match lifecycle (sf4e__NetplayRuntime__Match.cxx).
// How a cancelled game's GGPO session ends: aborted if one is live, with a
// reason the player reads (empty says nothing); left by the player, aborted
// the same way after the room was told, so it is no failure of this PC's; or
// retired quietly under a diagnostics label.
struct GgpoEnd {
    enum class How { Abort, Leave, Retire };
    const char* text;
    NoticeSeverity severity;
    How how;
    static GgpoEnd Abort(const char* reason, NoticeSeverity severity = NoticeSeverity::Error) { return {reason, severity, How::Abort}; }
    static GgpoEnd Leave(const char* reason) { return {reason, NoticeSeverity::Info, How::Leave}; }
    static GgpoEnd Retire(const char* label) { return {label, NoticeSeverity::Info, How::Retire}; }
};
// The one way this PC gives up its part in the current game. Nothing its
// entry armed for the battle request survives (battle::NextRequest), the
// spectator drain hold is dropped, the GGPO session ends as `end` says, and
// the match session aborts. Reporting to the room, leaving the room and
// recovery stay with the caller.
void CancelLocalMatch(GgpoEnd end);
// CancelLocalMatch, and the game is recovered from (recoveringMatch).
void AbortLocalMatch(GgpoEnd end);
void AbortLocalMatch(const char* reason, NoticeSeverity severity = NoticeSeverity::Error);
// The room ended this PC's game with no result: retire it and say so.
void EndUnscoredMatch(room::MatchResult result);
void RetireFinishedMatch(const char* label);
// The player's leave of the game this PC watches (commands::
// LeaveWatchedGame), decided against the stream and the room as they are when
// it runs (netplay::LeaveOfWatchedGame): nothing once that view is gone, only
// the rest of the stream once the room has finished the game, otherwise an
// Unwatch scoped to the game and the player's own end of the stream. Deferred
// when the room's control could not take that Unwatch yet.
netplay::DispatchOutcome LeaveWatchedGame(const netplay::WatchedGame& game);
void RetryPendingAbort();
void SettleOwedAbort();
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
void OpenMatchLink(const tournament_link::MatchLink& link, bool launched = false);
// `launched`: the link started Ember, so it was just clicked.
void OpenConnectLink(const std::string& bridge, bool launched = false);
// Runs a request from the Replays screen, or the player's answer to a
// replay link, and once a tick takes links and moves the
// replay operation on (sf4e__ReplayStore.hxx).
void RunReplayRequest(const replay::Request& request);
void TickReplays();
// The replay files to import, from a link, a drop or the picker alike. It is not the runtime's:
// a file may be dropped from the window-message thread while the runtime is being deleted, so
// it lives to the end of the process, and its waiting files are kept across a restart.
replay_import::Queue& ReplayImports();
void KeepPendingImports();
void RestorePendingImports();
void ApplyGameDisplay();
// What the Replays screen is told of the last set pack saved or imported.
void SaySet(std::string text, bool error);
// Hands a public room link to the interface, which asks for the room's ticket
// when the player is free. `launched` as for OpenConnectLink.
void OpenPublicRoomLink(const tournament_link::RoomLink& link, bool launched = false);
// A table's end as the room committed it, with how it compares to this game's own capture.
void ObserveTournamentTerminal(const room::Event& event, netplay::MatchResultOutbox::TerminalResult terminal);
netplay::tournament::Status TournamentStatus();

// Public rooms (sf4e__NetplayRuntime__PublicRooms.cxx).
// Sends one bridge request on the tournament channel; the id the helper will
// answer under, or 0 when it was not sent.
std::uint64_t SendBridgeRequest(const nlohmann::json& request);
// Sends one bridge request and has its answer go to `handler`, through
// runtime->flights, which also ends it when the helper cannot take it or never
// answers (netplay/BridgeFlights.hxx). `helperReady` false is "cannot take it".
std::uint64_t SendFlight(const nlohmann::json& request, bool helperReady, netplay::BridgeFlights::Handler handler);
void DispatchPublicRooms(const netplay::tournament::Command& command, bool helperReady);
// Asks for a public room's ticket exactly as its Join does. `done` is told how
// it ended: the failure (the bridge's code, empty when none) and the admission.
using AdmissionDone = std::function<void(const std::string& failure, std::optional<netplay::publicrooms::Admission> admission)>;
void RequestTicket(const std::string& bridgeId, const std::string& roomId, bool helperReady, AdmissionDone done);

// Rankings (sf4e__NetplayRuntime__Rankings.cxx), the same way.
void DispatchRankings(const netplay::tournament::Command& command, bool helperReady);
bool TakeRankingsAnswer(const session::TournamentAnswer& answer);
void ExpireRankings(std::uint64_t nowMs);

// The public replay archive (sf4e__NetplayRuntime__Archive.cxx): a page of its
// listing, and a listed game downloaded and then watched.
void DispatchArchive(const commands::ArchiveList& command, bool helperReady);
void DispatchArchive(const commands::ArchiveWatch& command, bool helperReady);

// Fighter levels (sf4e__NetplayRuntime__Levels.cxx): the player's own, read on
// the menu's ask, once at start and after a set this PC fought.
void DispatchLevels(const netplay::tournament::Command& command, bool helperReady);
bool TakeLevelsAnswer(const session::TournamentAnswer& answer);
// Expires lost reads, notes sets that ended, and starts the reads that are due.
void TickLevels(bool helperReady, std::uint64_t nowMs);
// Other players' cards (same file): the public room's members' tokens are
// looked up when one joins and 8 s after a set in the room ends.
bool TakeCardsAnswer(const session::TournamentAnswer& answer);
void TickCards(bool helperReady, std::uint64_t nowMs);
// The friend the Friends tab has focused: their card is looked up with the members' (Cards::WantFriend).
void DispatchCards(const commands::FriendCardWanted& command);
// Other players' profiles and the player's own card (same file). The interface's commands; the profile and
// search the helper answers through flights; and the tick that sends what is due: the look-ups of the profile
// open, the search, and the player's card when it changed (netplay/ProfileRequests.hxx, netplay/CardBundle.hxx).
void DispatchProfile(const commands::ProfileOpen& command);
void DispatchProfile(const commands::ProfileClose& command);
void DispatchProfile(const commands::PlayerSearch& command);
void TickProfiles(bool helperReady, std::uint64_t nowMs);
// A friends action went through: the profile open is looked up again, and the card the action named shows what
// it made of the player (the member cards' Refresh and Relate).
void NoteProfileAction(const std::string& token, netplay::levels::Relation relation);

// Friends (sf4e__NetplayRuntime__Social.cxx): the interface's commands, the
// helper's answers, the presence sent each tick it is due, and what the
// interface reads. A Join, a Watch or an invitation of a friend's is carried
// out here too (netplay/FriendVisits.hxx).
void DispatchFriends(const commands::FriendsVisit& command);
void DispatchFriends(const commands::FriendsLeave& command);
void DispatchFriends(commands::FriendAction action, bool helperReady);
void DispatchFriends(const commands::FriendJoin& command);
void DispatchFriends(const commands::FriendWatch& command);
void DispatchFriends(const commands::FriendTakeInvite& command);
void DispatchFriends(const commands::FriendAnswerVisit& command);
void TickSocial(bool helperReady, std::uint64_t nowMs);
// A game this PC fought in a public room ended: its opponent joins the recent opponents (kept 7 days, saved
// beside the preferences). Nothing for a private room or an opponent without a card token.
void NoteOpponent(const room::Snapshot& snapshot, int table, int seat);
// Reads the saved recent opponents at start.
void LoadRecentOpponents();
// The key of what the player's presence is made from, and the Discord input it is described from (Status.cxx): the one
// rule for Discord's and friends'. `atMainMenu` is the caller's one reading of AtMainMenu() for the tick.
PresenceKey PresenceKeyOf(const netplay::Snapshot& session, const room::Snapshot& current, bool atMainMenu, PresenceShown shown);
discord::PresenceInput PresenceInputOf(const netplay::Snapshot& session, const room::Snapshot& current, bool atMainMenu, PresenceShown shown);
// Tells the friends' service this player is leaving, best effort, as the game closes.
void SayFriendsGoodbye();
netplay::friends::View FriendsStatus();

// Controllers (sf4e__NetplayRuntime__Input.cxx).
std::string DeviceName(const input::Device& device);
std::string ControllerLabel(const input::Device& device);
void CaptureMenuInput();
void HandleControllerRequest(const gamepads::ControllerRequest& request);
void SaveRoomSettings(const netplay::PlayerPreferences& from);
bool CanChooseControllerNow();
// The buttons that confirm and go back in Ember's menus for a device: the
// binding of its pad's Ember layout (runtime->pads), else the game's own.
input::MenuButtons MenuButtonsFor(int deviceType, int deviceIndex);
// The buttons that record and play in Training for a device: those its pad's
// Ember layout names, none for a pad without one.
input::TrainingButtons TrainingButtonsFor(int deviceType, int deviceIndex);

// The published snapshot, errors and Discord (sf4e__NetplayRuntime__Status.cxx).
void FillNetworkDiagnostics(platform::DiagnosticsView& view);
std::string StickyText();
void RaiseStickyError(StickyError kind, std::uint32_t code = 0);
bool StickyRuntimeError(const std::string& error);
PostPublishState Publish();
PostPublishState PublishThrottled();
void PumpDiscordClient();
void PublishAndTickDiscordInvite();
// Whether an invitation to another room must ask first: this PC is in a room,
// plays offline, or is in a game (the rule a Discord join is asked by).
bool BusyForInvitation(const netplay::Snapshot& state);
// Keeps the helper's ping subscription in step with the room board.
void TickMemberLatency();
// Whether the overlay drew the fighter selector lately in room `roomGeneration` (NoteFighterSelector).
bool FighterSelectorShown(std::uint64_t roomGeneration);
// Whether the shell noted a chat draft being written lately in room `roomGeneration` (NoteChatTyping).
bool ChatTypingShown(std::uint64_t roomGeneration);
// The answer to the post-match prompt the shell is wanting said (NotePostMatchAnswer); not active once the shell stops noting one.
room::PostMatchSaid PostMatchWanted();
// The pick is Random (NoteRandomFighter): each game is rolled for, so a
// rematch leaves the battle instead of replaying its fighters in place.
bool RandomFighterChosen();
// Each tick: rolls the next game's fighter when it is owed
// (room::RandomFighterPick), and sends the room host a roll the room does not
// show yet while the player sits between games (room::RandomRollToShow), at
// most once a second.
void TickRandomFighter();
// The rolled fighter (-1 before a roll) and the count of rolls, for the snapshot.
struct RandomFighterRoll { int fighter = -1; std::uint64_t serial = 0; };
RandomFighterRoll CurrentRandomFighter();
// A Ready or Rematch of a Random pick takes the rolled fighter as it is
// dispatched, fresh or parked.
void WithRandomFighter(commands::Ready& ready);
// Tests only: the roll draws from `roll` instead of a random engine; an empty
// one restores the engine.
void SetRandomFighterRollForTest(std::function<int()> roll);
} // namespace internal
using namespace internal;
} } // namespace sf4e::NetplayFacade
