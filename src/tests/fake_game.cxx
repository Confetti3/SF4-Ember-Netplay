// The game the runtime's tests run under (fake_game.hxx): every interface of
// sf4e__GameAdapter.hxx answered with nothing of SF4's, and its catalog
// (fake_catalog.hxx). Its only behaviour is what fake_game.hxx lets a test
// set and read.
#include "fake_game.hxx"
#include "fake_catalog.hxx"

#include "../common/InstalledContent.hxx"
#include "../sf4e/sf4e__AbsentGameFeatures.hxx"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace ember::game;
using ember::test::Fake;

class FakeState final : public GameState {
public:
    const std::string& BuildId() const override { static const std::string id = "fake-build"; return id; }
    bool MainMenuInFront() const override { return Fake().mainMenu; }
    std::string ForegroundScreen() const override { return Fake().mainMenu ? "MainMenu" : ""; }
    std::string Language() const override { return {}; }
    sf4e::selection::Availability FighterAvailability(int) const override { return {}; }
    std::uint32_t Random() override { return 4; }
    bool PlayChallengerCall(float) override { return false; }
    void CloseGame() override {}
};

class FakeBattle final : public Battle {
public:
    Entry Enter() override { ++Fake().battleEntries; return Fake().entry; }
    void OnSessionSynced() override {}
    void DropNextBattle() override {}
    bool Live() const override { return false; }
    void Retire(const char*) override {}
    void Abort(const char*, sf4e::NoticeSeverity, bool) override {}
    void Fail() override {}
    bool ResultHeld() const override { return false; }
    bool ResultScreenShown() const override { return false; }
    sf4e::room::MatchResult GameResult() const override { return sf4e::room::MatchResult::Abort; }
    unsigned RematchChoices() const override { return 0; }
    unsigned GamesPlayedInPlace() const override { return 0; }
    LinkSample Sample() const override { return {}; }
    int DisconnectCountdownMs() const override { return -1; }
    unsigned RecentRollbackFrames() const override { return 0; }
    sf4e::GamePing::Summary PingSummary() const override { return {}; }
    void PollTelemetry() override {}
    void ResetPacing() override {}
    std::size_t SpectatorStreams() const override { return 0; }
    bool DrainingSpectators() const override { return false; }
    void SetDrainingSpectators(bool) override {}
    bool SpectatorBacklog(int&) const override { return false; }
    int VerificationHeldAt() const override { return -1; }
    void ClearVerification() override {}
    bool FightRunning(int&) const override { return false; }
};

class FakeInput final : public Input {
public:
    std::vector<sf4e::input::Device> ReadDevices(bool) override { return {}; }
    void ScanKeyboard(std::vector<sf4e::input::Device>&) override {}
    sf4e::input::Device MenuDevice(const std::vector<sf4e::input::Device>&) override { return {}; }
    void LogInventory(const std::vector<sf4e::input::Device>&) override {}
    bool AssignToSide(const sf4e::input::Device&, int, bool) override { return true; }
    void ReleaseFromSide(int) override {}
    bool ReadAssignedInput(const sf4e::input::Device&, int, unsigned& mapped, unsigned& raw) override { mapped = raw = 0; return false; }
    bool ReadController(int, int, unsigned& held, unsigned* physical) override { held = 0; if (physical) *physical = 0; return false; }
    sf4e::input::MenuButtons MenuButtons(int, int) override { return {}; }
    std::vector<sf4e::gamepads::NativeController> NativeControllers() override { return {}; }
    bool MenuTakesPads() override { return false; }
    sf4e::Gamepads::LayerFrame ReadPadLayer() override { return {}; }
    std::optional<sf4e::gamepads::Identity> PadInSlot(int) override { return std::nullopt; }
    sf4e::gamepads::PromptBrand BrandForDevice(int, int) override { return {}; }
    void SetLayouts(std::vector<sf4e::gamepads::StoredLayout>) override {}
    void SetupPrompt(const sf4e::gamepads::Identity&) override {}
    void BeginSetup(const sf4e::gamepads::Identity&, unsigned) override {}
    void ControlSetup(sf4e::gamepads::SetupCommand, int) override {}
    std::optional<std::string> SaveSetup() override { return std::nullopt; }
};

class FakePreferences final : public Preferences {
public:
    Device SavedGameplayDevice() override { return {0, 0, std::nullopt}; }
    std::optional<sf4e::gamepads::Identity> SavedDeviceIdentity() override { return std::nullopt; }
    bool SaveGameplayDevice(int, int, const std::optional<sf4e::gamepads::Identity>&) override { return true; }
    bool Save(const sf4e::netplay::PlayerPreferences&) override { return true; }
    std::uint64_t Queue(const sf4e::netplay::PlayerPreferences&) override { return ++revision_; }
    bool Saved(std::uint64_t revision) override { return revision <= revision_; }
    bool Pending() override { return false; }
    std::string Error() override { return {}; }
private:
    std::uint64_t revision_ = 0;
};

class FakeServices final : public Services {
public:
    sf4e::SteamId LocalSteamId() override { return {}; }
    bool SteamOverlayAvailable() override { return false; }
    bool OpenSteamProfile(const sf4e::SteamId&) override { return false; }
    void UseLobbyMusic(bool) override {}
    bool LobbyMusicAvailable() override { return false; }
    void AnnounceAway(bool, int, const char*) override {}
    sf4e::selection::InstalledContentPtr InstalledContent() override { return std::make_shared<const sf4e::selection::InstalledContent>(); }
    unsigned long long LogLinesDropped() override { return 0; }
    bool SelfTestRunning() override { return false; }
    void SelfTestTick(bool, const std::string&) override {}
    void SelfTestInput(int, unsigned&, unsigned&) override {}
};

class FakeTraining final : public Training {
public:
    void Request(const sf4e::netplay::Generation&) override { ++Fake().trainingRequests; }
    bool ControlsAvailable() override { return false; }
    bool MatchPracticeActive() override { return false; }
    sf4e::training::Place ReadPlace() override { return {}; }
    void SetCall(const sf4e::training::CallControl&) override {}
    void StopCapture() override {}
};

Capabilities Everything() {
    Capabilities caps;
    caps.training = true;
    caps.replays = true;
    caps.spectators = true;
    caps.padSetup = true;
    caps.steamProfile = true;
    caps.customContent = true;
    return caps;
}

FakeState s_state;
FakeBattle s_battle;
FakeInput s_input;
FakePreferences s_preferences;
FakeServices s_services;
FakeTraining s_training;
NoReplays s_replays;
Adapter s_game = {Everything(), s_state, s_battle, s_input, s_preferences, s_services, s_training, s_replays};
}

namespace ember {
const GameCatalog& Catalog() {
    static constexpr GameCatalog catalog = test::TestCatalog(test::CatalogFighters);
    static_assert(Usable(catalog), "The fake game's catalog must be usable");
    return catalog;
}
namespace game {
const Adapter& Game() { return s_game; }
}
namespace test {
FakeGame& Fake() { static FakeGame fake; return fake; }
game::Capabilities& FakeCapabilities() { return s_game.caps; }
void ResetFake() {
    Fake() = FakeGame{};
    s_game.caps = Everything();
}
}
} // namespace ember
