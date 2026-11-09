#pragma once
// The game under Ember's runtime. The runtime reaches the game only through
// these interfaces: where the game is, its battle and rollback session, its
// controllers, where Ember's settings are saved, and the game-side services
// (Steam, music, Training, replays). The battle hooks call back the other way
// through sf4e__RuntimeEvents.hxx.
//
// Each game defines Game() once, in its own source (SF4's is
// sf4e__Sf4Game.cxx), as GameCatalog.hxx's Catalog() is: the runtime's tests
// run against the game's own definition, and a binary without one fails to
// link instead of running without a game.
//
// Some types here are still SF4's own: the fighter availability and installed
// content (the catalog reaches them in step 5 of EMBER_LIBRARY.md), and the
// pad layer, Training and replay types, which live in the headers named below
// until the library moves out (step 9).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../common/ControllerView.hxx"
#include "../common/SelectionPick.hxx"
#include "../common/SettingsFeatures.hxx"
#include "../common/GamePing.hxx"
#include "../common/MatchResult.hxx"
#include "../common/MenuButtons.hxx"
#include "../common/NoticeSeverity.hxx"
#include "../common/SteamId.hxx"
#include "../netplay/InputAssignment.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include "../netplay/SessionController.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../training/TrainingRuntime.hxx"
#include "sf4e__Gamepads.hxx"
#include "sf4e__ReplayPlayback.hxx"
#include "sf4e__ReplayStore.hxx"

namespace ember { namespace game {

// What this game offers the player. Each flag closes the places the runtime
// offers its feature, named below; the services below stay callable either way.
// Every flag is off until the game sets it, by name.
struct Capabilities {
    // Training from Home or a room: RuntimeGates::canTrain and every Training
    // request (NetplayFacade::TrainingHolds), so Training::Request is never called.
    bool training = false;
    bool replays = false;        // playing replays from the Replays screen (the snapshot's replays.ready)
    // What the Replays screen offers of a replay beyond watching it, keeping
    // it and showing its file, each with `replays`: the game's own replay
    // list (Add to the game, Open the Battle Log), a video of it (Export
    // video, its tab and a set's videos), the frame meter over it (Watch
    // with frame meter, and the playback strip's meter) and its inputs and
    // stats (the Inputs row).
    bool replayGameList = false;
    bool replayVideo = false;
    bool replayMeter = false;
    bool replayInputs = false;
    // Watching a table's games: a room's Watch action, a tournament match's
    // watch and its observer join, and the entry of any game this PC would
    // only watch (queued or watching), which it sits out.
    bool spectators = false;
    bool padSetup = false;       // the pad wizard (a pad's Setup request)
    bool steamProfile = false;   // opening a member's Steam profile (the OpenSteamProfile service)
    bool customContent = false;  // installed custom costumes, colours and stages (InstalledContent)
    // Ember's frame meter over a match: Settings' Match frame meter row,
    // which a game without the meter does not offer.
    bool frameMeter = false;
    // The game features whose Settings rows only some games have
    // (SettingsFeatures.hxx): a game without one is not offered its rows.
    SettingsFeatures settingsFeatures;
};

// Where the game is. Game thread.
class GameState {
public:
    // The build id rooms match on: players with different builds do not meet.
    virtual const std::string& BuildId() const = 0;
    // The game's own menu is in front, ready for a match to start from it.
    // Asked only once the game has said its screens are up
    // (NotifyRuntimeEventSystemReady).
    virtual bool MainMenuInFront() const = 0;
    // The game's screen in front, by name, for logs and the self-test; empty
    // while it is changing or before the game's screens are up.
    virtual std::string ForegroundScreen() const = 0;
    // The game's own language, which the interface's Automatic language
    // follows: a code as ResolveLocale reads it (Localization.hxx), or empty
    // when the game's language is unknown. Asked once, as the runtime starts.
    virtual std::string Language() const = 0;
    // What the player has unlocked of a fighter. Only the game's own code reads
    // it: its catalog's fighterLocked and availablePick, and its select screen.
    virtual sf4e::selection::Availability FighterAvailability(int fighter) const = 0;
    // A random number from the game's own generator.
    virtual std::uint32_t Random() = 0;
    // The announcer's challenger call at `volume` of the game's voice volume;
    // false while the game's sound is not up.
    virtual bool PlayChallengerCall(float volume) = 0;
    // Closes the game (an update is installing).
    virtual void CloseGame() = 0;
protected:
    ~GameState() = default;
};

// What the HUD and the status line read of a live rollback session.
struct LinkSample {
    bool connectionWarning = false;
    bool predictionStalled = false;
    // Until the peer is dropped, or -1 while it is not about to be.
    int disconnectCountdownMs = -1;
    int pingMs = -1;
    int appliedDelay = -1;
    bool ownDelays = false;
    bool spectator = false;
    int spectatorBacklog = -1;
    // This PC plays a fighter in the session (not a spectator's stream).
    bool localFighter = false;
};

// The game's battle and its rollback session. Game thread.
//
// A room match reaches the battle in two steps (EMBER_LIBRARY.md §6, #7):
// Enter starts the battle from the room's match, and the battle, once loaded,
// asks the runtime for its ports and starts the rollback session itself
// (RuntimeEvents: GetRuntimeMatchEndpoints, ReleaseRuntimePortToGgpo). The
// room client the runtime builds tells the battle when every member's battle
// has loaded.
class Battle {
public:
    // Deferred: not yet (no opponent, or away from the main menu).
    // Rejected: the room's match is one the game cannot take; nothing of it
    // was written, and the runtime leaves the game.
    enum class Entry { Deferred, Entered, Rejected };
    // The room's granted match. Its facts are still read from the room client
    // (SessionClient's lobby and match data) until the wire is the library's.
    virtual Entry Enter() = 0;
    virtual void OnSessionSynced() = 0;
    // Drops what an entry armed for the next battle, so no later battle
    // plays the match's seed or rules.
    virtual void DropNextBattle() = 0;

    // A rollback session is live: a game is played, or its finished
    // session still streams to spectators.
    virtual bool Live() const = 0;
    // Retires the session quietly; `label` is for diagnostics.
    virtual void Retire(const char* label) = 0;
    // Aborts a live session with a notice for the player. `left`: the player
    // left the game, which is no failure of this PC's.
    virtual void Abort(const char* reason, sf4e::NoticeSeverity severity, bool left) = 0;
    // The live session met an error it cannot go on from; the battle leaves.
    virtual void Fail() = 0;

    // The game's result, while the battle holds it for the post-match prompt.
    virtual bool ResultHeld() const = 0;
    // The WIN or DRAW screen is up.
    virtual bool ResultScreenShown() const = 0;
    virtual sf4e::room::MatchResult GameResult() const = 0;
    // Both players' rematch choices as the battle's input carries them.
    virtual unsigned RematchChoices() const = 0;
    // The players who declined the rematch over the WIN, as the battle's
    // input carries it (bit 1 = P1, 2 = P2), from the frame the battle
    // decided on until the next WIN or battle: a decline ends the WIN at once.
    // 0 where the input carries no decline (SF4's).
    virtual unsigned RematchDeclines() const = 0;
    virtual unsigned GamesPlayedInPlace() const = 0;

    // Read while Live().
    virtual LinkSample Sample() const = 0;
    // Sample's disconnectCountdownMs alone, for a caller that needs only it.
    virtual int DisconnectCountdownMs() const = 0;
    virtual unsigned RecentRollbackFrames() const = 0;
    // The game's pings, for the delay advice after it.
    virtual sf4e::GamePing::Summary PingSummary() const = 0;
    virtual void PollTelemetry() = 0;
    virtual void ResetPacing() = 0;

    // A finished session kept open so its spectators can finish.
    virtual std::size_t SpectatorStreams() const = 0;
    virtual bool DrainingSpectators() const = 0;
    virtual void SetDrainingSpectators(bool draining) = 0;
    // The frames a spectator's session has received and not played yet;
    // false when the session cannot tell.
    virtual bool SpectatorBacklog(int& frames) const = 0;

    // The desync check: the frame its confirmed state is held at, and
    // dropping its snapshots and checkpoints.
    virtual int VerificationHeldAt() const = 0;
    virtual void ClearVerification() = 0;
    // A fight is running on a healthy session: true with the frame simulated.
    virtual bool FightRunning(int& simulatedFrame) const = 0;
protected:
    ~Battle() = default;
};

// The game's controllers. Game thread.
class Input {
public:
    // The game's pads and the keyboard. The keyboard row's buttons cost a
    // full key scan, which `scanKeyboard` asks for (ScanKeyboard adds it later).
    virtual std::vector<sf4e::input::Device> ReadDevices(bool scanKeyboard = true) = 0;
    virtual void ScanKeyboard(std::vector<sf4e::input::Device>& devices) = 0;
    // The device the game's menu follows.
    virtual sf4e::input::Device MenuDevice(const std::vector<sf4e::input::Device>& devices) = 0;
    virtual void LogInventory(const std::vector<sf4e::input::Device>& devices) = 0;
    // Gives a fighter's side to a device, or takes it back.
    virtual bool AssignToSide(const sf4e::input::Device& device, int side, bool fight) = 0;
    virtual void ReleaseFromSide(int side) = 0;
    virtual bool ReadAssignedInput(const sf4e::input::Device& device, int side, unsigned& mapped, unsigned& raw) = 0;
    // A device's buttons as the game's menu reads them.
    virtual bool ReadController(int deviceType, int deviceIndex, unsigned& held, unsigned* physical) = 0;
    virtual sf4e::input::MenuButtons MenuButtons(int deviceType, int deviceIndex) = 0;
    // The controllers DirectInput reports, for the pad hints.
    virtual std::vector<sf4e::gamepads::NativeController> NativeControllers() = 0;
    // Ember's menu holds the pads now.
    virtual bool MenuTakesPads() = 0;

    // Ember's pad layer: its pads, the pad wizard and the button layouts.
    virtual sf4e::Gamepads::LayerFrame ReadPadLayer() = 0;
    virtual std::optional<sf4e::gamepads::Identity> PadInSlot(int slot) = 0;
    virtual sf4e::gamepads::PromptBrand BrandForDevice(int deviceType, int slot) = 0;
    virtual void SetLayouts(std::vector<sf4e::gamepads::StoredLayout> layouts) = 0;
    virtual void SetupPrompt(const sf4e::gamepads::Identity& identity) = 0;
    virtual void BeginSetup(const sf4e::gamepads::Identity& identity, unsigned padId) = 0;
    virtual void ControlSetup(sf4e::gamepads::SetupCommand command, int step) = 0;
    virtual std::optional<std::string> SaveSetup() = 0;
protected:
    ~Input() = default;
};

// Where Ember's settings are saved. Every game has one, so it is an interface,
// not a capability. Game thread; the game's one writer saves off it.
class Preferences {
public:
    struct Device { int type; int index; std::optional<sf4e::gamepads::Identity> identity; };
    // The gameplay device the settings name, read every tick.
    virtual Device SavedGameplayDevice() = 0;
    virtual std::optional<sf4e::gamepads::Identity> SavedDeviceIdentity() = 0;
    virtual bool SaveGameplayDevice(int type, int index, const std::optional<sf4e::gamepads::Identity>& identity) = 0;
    // Save queues the preferences; Queue also gives the revision Saved
    // answers for.
    virtual bool Save(const sf4e::netplay::PlayerPreferences& preferences) = 0;
    virtual std::uint64_t Queue(const sf4e::netplay::PlayerPreferences& preferences) = 0;
    virtual bool Saved(std::uint64_t revision) = 0;
    virtual bool Pending() = 0;
    virtual std::string Error() = 0;
protected:
    ~Preferences() = default;
};

// The game-side services the runtime drives. Game thread.
class Services {
public:
    virtual sf4e::SteamId LocalSteamId() = 0;
    virtual bool SteamOverlayAvailable() = 0;
    virtual bool OpenSteamProfile(const sf4e::SteamId& id) = 0;
    // The lobby music: endless or the game's own.
    virtual void UseLobbyMusic(bool endless) = 0;
    virtual bool LobbyMusicAvailable() = 0;
    // A sound that reaches a player who is away from the game window.
    virtual void AnnounceAway(bool sound, int volumePercent, const char* what) = 0;
    virtual sf4e::selection::InstalledContentPtr InstalledContent() = 0;
    // Lines the game's log sink dropped, for the diagnostics a problem report carries.
    virtual unsigned long long LogLinesDropped() = 0;
    // The in-game self-test (a development build's): whether one runs, its
    // tick, and its hand on a fighter's input.
    virtual bool SelfTestRunning() = 0;
    virtual void SelfTestTick(bool atMainMenu, const std::string& screen) = 0;
    virtual void SelfTestInput(int side, unsigned& mapped, unsigned& raw) = 0;
protected:
    ~Services() = default;
};

// Offline Training, and the Training call from a room. Game thread.
class Training {
public:
    // Leaves the main menu for Training, for the room session `made`.
    virtual void Request(const sf4e::netplay::Generation& made) = 0;
    virtual bool ControlsAvailable() = 0;
    virtual bool MatchPracticeActive() = 0;
    virtual sf4e::training::Place ReadPlace() = 0;
    virtual void SetCall(const sf4e::training::CallControl& call) = 0;
    virtual void StopCapture() = 0;
protected:
    ~Training() = default;
};

// The replay archive, its playback and its export. Game thread, except where
// a method says otherwise.
class Replays {
public:
    virtual bool Ready() = 0;
    virtual const sf4e::replaystore::Status& Status() = 0;
    virtual void Start(const sf4e::replay::Request& request, bool atMainMenu, bool noRoom) = 0;
    virtual void Tick(bool atMainMenu, bool noRoom) = 0;
    virtual void TickPlayback(int deviceType, int deviceIndex, bool connected) = 0;
    virtual const sf4e::replaytransport::View& Playback() = 0;
    virtual std::shared_ptr<const sf4e::replaylane::Lanes> PlaybackLanes() = 0;

    // The archive's folder, which the Replays screen opens; empty when there
    // is none. Asked once, as the runtime starts.
    virtual std::filesystem::path ArchivePath() = 0;
    // The archive's listing and the Replays screen's entry, which the game
    // makes off the game thread. Any thread asks and reads: the shell's host
    // asks for what its screen shows. The latest listing is null before the
    // first, and the latest detail empty before any.
    virtual void WantListing() = 0;
    virtual std::shared_ptr<const std::vector<sf4e::platform::replays::ArchivedReplay>> LatestListing() = 0;
    virtual void WantDetail(const std::string& file, std::uint64_t revision) = 0;
    virtual sf4e::replayinputs::DetailCompletion LatestDetail() = 0;
    // Ends the listing and waits for one in progress; once, as the runtime stops.
    virtual void StopListing() = 0;
    // The player's mark on a replay, and a tournament's sharing: its set's
    // bridge or the replay archive has it, or the set is closed to it.
    virtual void MarkKept(const std::filesystem::path& file, bool kept) = 0;
    virtual void MarkSent(const std::filesystem::path& file) = 0;
    virtual void MarkSetClosed(const std::filesystem::path& file) = 0;
    // Where a set's pack is saved (empty when it cannot be), and taking a
    // pack's games into the archive: how many the archive holds, 0 for a
    // file that is no whole pack.
    virtual std::filesystem::path SetPackPath(const std::string& matchId) = 0;
    virtual std::size_t ImportSetPack(const std::filesystem::path& pack) = 0;
protected:
    ~Replays() = default;
};

struct Adapter {
    Capabilities caps;
    GameState& state;
    Battle& battle;
    Input& input;
    Preferences& preferences;
    Services& services;
    Training& training;
    Replays& replays;
};

const Adapter& Game();

} } // namespace ember::game
