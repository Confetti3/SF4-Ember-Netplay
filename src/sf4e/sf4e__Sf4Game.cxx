// SF4's game under Ember's runtime (sf4e__GameAdapter.hxx). Each method is
// the call the runtime made into SF4, or into SF4 Ember's own hooks, before
// the adapter: the bodies moved here unchanged.

#include "sf4e__GameAdapter.hxx"

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "../Dimps/Dimps__Selection.hxx"
#include "../Dimps/Dimps__Sound.hxx"
#include "sf4e.hxx"
#include "sf4e__AwayAlert.hxx"
#include "sf4e__CustomContent.hxx"
#include "sf4e__DirectInputFilter.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__InputDevices.hxx"
#include "sf4e__MenuMusic.hxx"
#include "sf4e__Overlay.hxx"
#include "sf4e__OverlayPrefs.hxx"
#include "sf4e__Platform.hxx"
#include "sf4e__SelfTest.hxx"
#include "sf4e__Steam.hxx"
#include "sf4e__UserApp.hxx"

namespace {
using fSystem = sf4e::Game::Battle::System;
using rSystem = Dimps::Game::Battle::System;
using namespace ember::game;

class Sf4GameState final : public GameState {
public:
    const std::string& BuildId() const override { return sf4e::sidecarHash; }
    // Asked only after NotifyRuntimeEventSystemReady: platform initialization
    // precedes the event-system singleton, which GetRootEvent dereferences
    // before it can return a root pointer.
    bool MainMenuInFront() const override {
        auto* root = Dimps::App::GetRootEvent();
        if (!root) return false;
        char* query[] = { "MainMenu" };
        return Dimps::Event::EventBaseWithEC::FindForegroundEvent(root, query, 1) != nullptr;
    }
    std::string ForegroundScreen() const override {
        if (!Dimps::App::RootEventReady()) return {};
        auto* root = Dimps::App::GetRootEvent();
        if (!root) return {};
        using Dimps::Event::EventController;
        auto* controller = (root->*Dimps::Event::EventBaseWithEC::publicMethods.GetChildEventController)();
        if (!controller || (controller->*EventController::publicMethods.GetForegroundEventStatus)() != EventController::EQS_FOREGROUND) return {};
        auto* event = (controller->*EventController::publicMethods.GetForegroundEvent)();
        return event ? std::string(Dimps::Event::EventBase::GetName(event)) : std::string();
    }
    sf4e::selection::Availability FighterAvailability(int fighter) const override {
        return Dimps::Selection::ReadAvailability(fighter);
    }
    std::uint32_t Random() override { return sf4e::localRand(); }
    bool PlayChallengerCall(float volume) override {
        return Dimps::Sound::PlaySystemCue(Dimps::Sound::SystemCue::HereComesChallenger, Dimps::Sound::SystemChannel::Voice, volume);
    }
    void CloseGame() override { Dimps::Platform::Main::Close(); }
};

class Sf4Battle final : public Battle {
public:
    Entry Enter() override { return sf4e::UserApp::EnterAuthorizedMatch(); }
    void OnSessionSynced() override { sf4e::GameEvents::VsBattle::bSessionSynced = true; }
    void DropNextBattle() override { sf4e::GameEvents::VsBattle::nextRequest = {}; }

    bool Live() const override { return fSystem::ggpo != nullptr; }
    void Retire(const char* label) override { fSystem::RetireGgpoSession(label); }
    void Abort(const char* reason, sf4e::NoticeSeverity severity, bool left) override {
        fSystem::AbortGgpoMatch(reason, severity, left ? fSystem::AbortCause::Left : fSystem::AbortCause::Failure);
    }
    void Fail() override { fSystem::simGate.OnFatal(); }

    bool ResultHeld() const override { return fSystem::ResultHeld(); }
    bool ResultScreenShown() const override {
        if (!rSystem::staticVars.CurrentBattleFlow) return false;
        const auto flow = *rSystem::staticVars.CurrentBattleFlow;
        return flow == rSystem::BF__MATCH_RESULT || flow == rSystem::BF__DRAW_RESULT;
    }
    sf4e::room::MatchResult GameResult() const override { return fSystem::GameResult(); }
    unsigned RematchChoices() const override { return fSystem::rematchChoices; }
    unsigned GamesPlayedInPlace() const override { return fSystem::GamesPlayedInPlace(); }

    LinkSample Sample() const override {
        LinkSample sample;
        sample.connectionWarning = fSystem::simGate.connectionWarningActive;
        sample.predictionStalled = fSystem::simGate.predictionStalled;
        sample.disconnectCountdownMs = fSystem::DisconnectCountdownMs();
        sample.pingMs = fSystem::matchTelemetry.Ping(GetTickCount64());
        sample.appliedDelay = fSystem::matchTelemetry.appliedDelay;
        sample.ownDelays = fSystem::matchTelemetry.ownDelays;
        sample.spectator = fSystem::matchTelemetry.spectator;
        sample.spectatorBacklog = fSystem::matchTelemetry.backlog;
        sample.localFighter = fSystem::LocalFighter();
        return sample;
    }
    unsigned RecentRollbackFrames() const override { return fSystem::RecentRollbackFrames(); }
    sf4e::GamePing::Summary PingSummary() const override { return fSystem::matchTelemetry.pings.Summarize(); }
    void PollTelemetry() override { fSystem::PollMatchTelemetry(); }
    void ResetPacing() override { fSystem::ResetPacing(); }

    std::size_t SpectatorStreams() const override { return fSystem::SpectatorStreamCount(); }
    bool DrainingSpectators() const override { return fSystem::drainingSpectators; }
    void SetDrainingSpectators(bool draining) override { fSystem::drainingSpectators = draining; }
    bool SpectatorBacklog(int& frames) const override {
        return GGPO_SUCCEEDED(ggpo_get_spectator_backlog(fSystem::ggpo, &frames));
    }

    int VerificationHeldAt() const override { return fSystem::ggpoTimeline.Held(); }
    void ClearVerification() override {
        fSystem::snapshotMap.clear();
        fSystem::ClearHashCheckpoints();
    }
    bool FightRunning(int& simulatedFrame) const override {
        rSystem* system = rSystem::staticMethods.GetSingleton();
        const bool fightRunning =
            fSystem::ggpo &&
            fSystem::simGate.phase == sf4e::gate::PHASE_RUNNING &&
            !fSystem::simGate.fatalError &&
            system &&
            *rSystem::staticVars.CurrentBattleFlow != rSystem::BF__IDLE;
        if (fightRunning) simulatedFrame = rSystem::GetNumFramesSimulated_FixedPoint(system)->integral;
        return fightRunning;
    }
};

class Sf4Input final : public Input {
public:
    std::vector<sf4e::input::Device> ReadDevices(bool scanKeyboard) override { return sf4e::input::ReadDevices(scanKeyboard); }
    void ScanKeyboard(std::vector<sf4e::input::Device>& devices) override { sf4e::input::ScanKeyboard(devices); }
    sf4e::input::Device MenuDevice(const std::vector<sf4e::input::Device>& devices) override { return sf4e::input::MenuDevice(devices); }
    void LogInventory(const std::vector<sf4e::input::Device>& devices) override { sf4e::input::LogInventory(devices); }
    bool AssignToSide(const sf4e::input::Device& device, int side, bool fight) override { return sf4e::input::AssignToSide(device, side, fight); }
    void ReleaseFromSide(int side) override { sf4e::input::ReleaseFromSide(side); }
    bool ReadAssignedInput(const sf4e::input::Device& device, int side, unsigned& mapped, unsigned& raw) override {
        return sf4e::input::ReadAssignedInput(device, side, mapped, raw);
    }
    bool ReadController(int deviceType, int deviceIndex, unsigned& held, unsigned* physical) override {
        return Dimps::Pad::ReadController(deviceType, deviceIndex, held, physical);
    }
    sf4e::input::MenuButtons MenuButtons(int deviceType, int deviceIndex) override { return Dimps::Pad::MenuButtons(deviceType, deviceIndex); }
    std::vector<sf4e::gamepads::NativeController> NativeControllers() override { return sf4e::DirectInputFilter::ReadControllers(); }
    bool MenuTakesPads() override { return sf4e::Overlay::MenuPads().Takes(); }

    sf4e::Gamepads::LayerFrame ReadPadLayer() override { return sf4e::Gamepads::ReadFrame(); }
    std::optional<sf4e::gamepads::Identity> PadInSlot(int slot) override { return sf4e::Gamepads::IdentityForSlot(slot); }
    sf4e::gamepads::PromptBrand BrandForDevice(int deviceType, int slot) override { return sf4e::Gamepads::BrandForDevice(deviceType, slot); }
    void SetLayouts(std::vector<sf4e::gamepads::StoredLayout> layouts) override { sf4e::Gamepads::SetLayouts(std::move(layouts)); }
    void SetupPrompt(const sf4e::gamepads::Identity& identity) override { sf4e::Gamepads::SetupPrompt(identity); }
    void BeginSetup(const sf4e::gamepads::Identity& identity, unsigned padId) override { sf4e::Gamepads::BeginSetup(identity, padId); }
    void ControlSetup(sf4e::gamepads::SetupCommand command, int step) override { sf4e::Gamepads::ControlSetup(command, step); }
    std::optional<std::string> SaveSetup() override { return sf4e::Gamepads::SaveSetup(); }
};

class Sf4Preferences final : public Preferences {
public:
    Device SavedGameplayDevice() override {
        sf4e::OverlayPrefs::Data prefs;
        sf4e::OverlayPrefs::LoadEachTick(prefs);
        return {prefs.deviceType, prefs.deviceIdx, prefs.deviceIdentity};
    }
    std::optional<sf4e::gamepads::Identity> SavedDeviceIdentity() override { return sf4e::OverlayPrefs::DeviceIdentity(); }
    bool SaveGameplayDevice(int type, int index, const std::optional<sf4e::gamepads::Identity>& identity) override {
        return sf4e::OverlayPrefs::SaveGameplayDevice(type, index, identity);
    }
    bool Save(const sf4e::netplay::PlayerPreferences& preferences) override { return sf4e::OverlayPrefs::SavePlayerPreferences(preferences); }
    std::uint64_t Queue(const sf4e::netplay::PlayerPreferences& preferences) override { return sf4e::OverlayPrefs::QueuePlayerPreferences(preferences); }
    bool Saved(std::uint64_t revision) override { return sf4e::OverlayPrefs::PlayerPreferencesSaved(revision); }
    bool Pending() override { return sf4e::OverlayPrefs::PersistencePending(); }
    std::string Error() override { return sf4e::OverlayPrefs::PersistenceError(); }
};

class Sf4Services final : public Services {
public:
    sf4e::SteamId LocalSteamId() override { return sf4e::steam::LocalSteamId(); }
    bool SteamOverlayAvailable() override { return sf4e::steam::OverlayAvailable(); }
    bool OpenSteamProfile(const sf4e::SteamId& id) override { return sf4e::steam::OpenProfileOverlay(id); }
    void UseLobbyMusic(bool endless) override { sf4e::MenuMusic::Use(endless); }
    bool LobbyMusicAvailable() override { return sf4e::MenuMusic::Available(); }
    void AnnounceAway(bool sound, int volumePercent, const char* what) override { sf4e::AwayAlert::Announce(sound, volumePercent, what); }
    sf4e::selection::InstalledContentPtr InstalledContent() override { return sf4e::custom::Installed(); }
    unsigned long long LogLinesDropped() override { return sf4e::Platform::AsyncLogDropped(); }
    bool SelfTestRunning() override { return sf4e::selftest::Running(); }
    void SelfTestTick(bool atMainMenu, const std::string& screen) override { sf4e::selftest::Tick(atMainMenu, screen); }
    void SelfTestInput(int side, unsigned& mapped, unsigned& raw) override { sf4e::selftest::MatchInput(side, mapped, raw); }
};

class Sf4Training final : public Training {
public:
    void Request(const sf4e::netplay::Generation& made) override { sf4e::GameEvents::MainMenu::RequestTraining(made); }
    bool ControlsAvailable() override { return sf4e::training::ControlsAvailable(); }
    bool MatchPracticeActive() override { return sf4e::training::MatchPracticeActive(); }
    sf4e::training::Place ReadPlace() override { return sf4e::training::ReadPlace(); }
    void SetCall(const sf4e::training::CallControl& call) override { sf4e::training::SetCall(call); }
    void StopCapture() override { sf4e::training::StopCapture(); }
};

class Sf4Replays final : public Replays {
public:
    bool Ready() override { return sf4e::replaystore::Ready(); }
    const sf4e::replaystore::Status& Status() override { return sf4e::replaystore::GetStatus(); }
    void Start(const sf4e::replay::Request& request, bool atMainMenu, bool noRoom) override { sf4e::replaystore::Start(request, atMainMenu, noRoom); }
    void Tick(bool atMainMenu, bool noRoom) override { sf4e::replaystore::Tick(atMainMenu, noRoom); }
    void TickPlayback(int deviceType, int deviceIndex, bool connected) override { sf4e::replayplayback::Tick(deviceType, deviceIndex, connected); }
    const sf4e::replaytransport::View& Playback() override { return sf4e::replayplayback::GetView(); }
    std::shared_ptr<const sf4e::replaylane::Lanes> PlaybackLanes() override { return sf4e::replayplayback::GetLanes(); }
};

Sf4GameState s_state;
Sf4Battle s_battle;
Sf4Input s_input;
Sf4Preferences s_preferences;
Sf4Services s_services;
Sf4Training s_training;
Sf4Replays s_replays;
// SF4 offers every feature.
const Adapter s_game = {
    {true, true, true, true, true, true},
    s_state, s_battle, s_input, s_preferences, s_services, s_training, s_replays,
};
}

namespace ember { namespace game {
const Adapter& Game() { return s_game; }
} }
