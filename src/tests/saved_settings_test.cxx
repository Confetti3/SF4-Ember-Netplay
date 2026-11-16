// The player's saved settings are never replaced by what the program holds for
// settings it did not read: a read that fails is tried again and nothing is
// written before one succeeds, a change made meanwhile merges over what was
// read, a setting that cannot be read keeps the file's value, and a file that
// is not JSON is kept aside (SettingsGuard.hxx, SavedRead.hxx,
// PlayerPreferencesRead.hxx). Every case runs in a temporary folder.
#include "../netplay/PlayerPreferencesRead.hxx"
#include "../netplay/SavedRead.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../netplay/SettingsWriter.hxx"

#define NOMINMAX
#include <windows.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "test_support.hxx"
#include "temp_root.hxx"

using Json = nlohmann::json;
using Path = std::filesystem::path;
using namespace sf4e::netplay;
using Clock = SavedRead::Clock;

namespace {

std::string Read(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    CHECK(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void Write(const Path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << bytes;
    file.close();
    CHECK(file.good());
}

// The settings lock as another process holds it, until released.
struct HeldLock {
    HANDLE handle;
    explicit HeldLock(const Path& directory) {
        std::filesystem::create_directories(directory);
        handle = CreateFileW((directory / L"settings.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(handle != INVALID_HANDLE_VALUE);
    }
    void Release() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); handle = INVALID_HANDLE_VALUE; }
    ~HeldLock() { Release(); }
};

bool Settle(SavedRead& read) {
    const auto deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        if (read.Poll(Clock::now())) return true;
        Sleep(1);
    }
    return false;
}

const std::vector<Clock::duration> Quick(4, std::chrono::milliseconds(10));

// The writer saves what it was handed and the revision is the proof; the lock is free by now.
void SaveThroughWriter(const Path& directory, const PlayerPreferences& preferences) {
    SettingsWriter writer(directory.wstring());
    const auto revision = writer.QueueLauncher(PlayerPreferencesJson(preferences));
    CHECK(revision != 0);
    const auto deadline = GetTickCount64() + 5000;
    while (writer.SavedLauncherRevision() < revision && GetTickCount64() < deadline) Sleep(1);
    CHECK(writer.SavedLauncherRevision() >= revision && writer.GetStatus().error.empty());
    writer.Stop();
}

Json Saved(const Path& directory) {
    Json saved;
    std::string error;
    CHECK(SettingsStore(directory.wstring()).LoadLauncher(saved, error));
    return saved;
}

// What a run holds before it has read anything: the defaults, every key held, as the runtime starts.
PlayerPreferences UnreadRun() {
    PlayerPreferences preferences;
    ReadRoomPreferences(Json::object(), preferences);
    preferences.guard = GuardEverything(PlayerPreferencesValues(preferences));
    return preferences;
}

bool Near(double a, double b) { return std::fabs(a - b) < 1e-3; }

// The settings of a player who has been here: not the defaults, so a default written over them shows.
Json PlayerSettings() {
    return {{"displayName", "Player 1"}, {"mainFighter", 5}, {"mainChosen", true}, {"inputDelay", 3}, {"autoInputDelay", false},
        {"matchHudSize", 2}, {"readySoundVolume", 50}, {"interfaceScale", 1.2}, {"wornTitle", "organizer"},
        {"roundCount", 5}, {"country", "DE"}};
}

void GuardAlone() {
    PlayerPreferences preferences;
    const auto values = PlayerPreferencesValues(preferences);
    // With nothing held a save carries every setting, as it always has.
    CHECK(PlayerPreferencesJson(preferences) == values && values.contains("mainFighter") && values.contains("interfaceScale"));
    preferences.guard = GuardEverything(values);
    CHECK(PlayerPreferencesJson(preferences).empty());
    preferences.interfaceScale = 0.9f;
    const auto changed = PlayerPreferencesJson(preferences);
    CHECK(changed.size() == 1 && Near(changed["interfaceScale"].get<double>(), 0.9));
    preferences.guard.frozen = true;
    CHECK(PlayerPreferencesJson(preferences).empty());
    // A setting saved as 0 or 1 is the flag the program holds, and 0.7 is the 0.7 a float keeps.
    CHECK(SameSavedValue(Json(1), Json(true)) && SameSavedValue(Json(false), Json(0)) && !SameSavedValue(Json(2), Json(true)));
    CHECK(SameSavedValue(Json(0.7), Json(0.7f)) && !SameSavedValue(Json(0.7), Json(0.8)) && !SameSavedValue(Json("1"), Json(1)));
}

// The lock is held at start, then released; the player changes a setting meanwhile.
void LockHeldThenReleased(const Path& root) {
    const auto directory = root / L"held-then-released";
    SettingsStore store(directory.wstring());
    std::string error;
    CHECK(store.SaveLauncher(PlayerSettings(), error));
    HeldLock held(directory);
    SavedRead read(store, Clock::now(), Quick);
    CHECK(read.state() == SavedRead::State::Pending);
    PlayerPreferences preferences = UnreadRun();
    CHECK(preferences.mainFighter != 5 && preferences.matchHudSize != 2);
    // A change made while nothing is read: only it is saved, by whoever saves.
    preferences.interfaceScale = 0.9f;
    CHECK(PlayerPreferencesJson(preferences).size() == 1);
    held.Release();
    CHECK(Settle(read) && read.state() == SavedRead::State::Read);
    // The late read merges under the change: the file's values for what was not changed, this run's for what was.
    const auto report = ApplySavedSettings(SavedUnderChanges(read.saved(), preferences), preferences);
    CHECK(!report.fieldsFailed && !report.recordInvalid && !report.roomDefaultsInvalid);
    CHECK(preferences.mainFighter == 5 && preferences.matchHudSize == 2 && preferences.readySoundVolume == 50 &&
        preferences.inputDelay == 3 && !preferences.autoInputDelay && preferences.wornTitle == "organizer");
    CHECK(Near(preferences.interfaceScale, 0.9));
    // And what is saved after is the file with that one change.
    SaveThroughWriter(directory, preferences);
    const auto saved = Saved(directory);
    CHECK(saved["mainFighter"] == 5 && saved["matchHudSize"] == 2 && saved["displayName"] == "Player 1" && saved["readySoundVolume"] == 50 &&
        saved["roundCount"] == 5 && saved["country"] == "DE" && Near(saved["interfaceScale"].get<double>(), 0.9));
}

// A save made before the late read ever arrived (the writer got the file first) leaves every other setting as it was.
void SaveBeforeLateRead(const Path& root) {
    const auto directory = root / L"save-first";
    SettingsStore store(directory.wstring());
    std::string error;
    CHECK(store.SaveLauncher(PlayerSettings(), error));
    PlayerPreferences preferences = UnreadRun();
    preferences.readySound = !preferences.readySound;
    preferences.matchHudAnchor = 3;
    SaveThroughWriter(directory, preferences);
    const auto saved = Saved(directory);
    CHECK(saved["mainFighter"] == 5 && saved["mainChosen"] == true && saved["matchHudSize"] == 2 && saved["inputDelay"] == 3 &&
        saved["autoInputDelay"] == false && saved["displayName"] == "Player 1");
    CHECK(saved["mainFighter"] == 5 && saved["matchHudAnchor"] == 3 && saved["readySound"] == preferences.readySound);
    CHECK(saved["displayName"] == "Player 1" && saved["roundCount"] == 5 && saved["readySoundVolume"] == 50);
}

// A lock never released: nothing is written, and the read gives up once.
void LockNeverReleased(const Path& root) {
    const auto directory = root / L"never-released";
    SettingsStore store(directory.wstring());
    std::string error;
    CHECK(store.SaveLauncher(PlayerSettings(), error));
    const auto before = Read(directory / L"settings.json");
    HeldLock held(directory);
    SavedRead read(store, Clock::now(), Quick);
    CHECK(Settle(read) && read.state() == SavedRead::State::GaveUp && read.failure() == ReadFailure::Busy);
    // The runtime freezes the preferences, once, and tells the player.
    PlayerPreferences preferences = UnreadRun();
    preferences.guard.frozen = true;
    preferences.interfaceScale = 0.9f;
    preferences.mainFighter = 9;
    CHECK(PlayerPreferencesJson(preferences).empty());
    CHECK(!read.Poll(Clock::now()) && read.state() == SavedRead::State::GaveUp);
    held.Release();
    SaveThroughWriter(directory, preferences);
    CHECK(Read(directory / L"settings.json") == before);
}

// One setting that cannot be read does not take the others with it, and keeps the file's own value.
void OneBadField(const Path& root) {
    const auto directory = root / L"bad-field";
    SettingsStore store(directory.wstring());
    std::string error;
    Json settings = PlayerSettings();
    settings["matchHudSize"] = "large";          // the wrong type: it throws as it is read
    settings["roomColumn"] = 100000;             // out of range
    settings["pictureMode"] = 77;                // an unknown choice, from another version
    settings["readySound"] = "loud";             // not a flag, in the middle of the flags
    settings["welcomed"] = false;                // a flag after it that reads fine
    settings["futureSetting"] = {{"mode", "x"}, {"levels", {1, 2, 3}}};
    CHECK(store.SaveLauncher(settings, error));
    SavedRead read(store, Clock::now(), Quick);
    CHECK(read.state() == SavedRead::State::Read);
    PlayerPreferences preferences;
    preferences.lobby = {};
    const auto report = ApplySavedSettings(read.saved(), preferences);
    CHECK(report.fieldsFailed);
    // The others were read.
    CHECK(preferences.mainFighter == 5 && preferences.readySoundVolume == 50 && preferences.inputDelay == 3 && preferences.wornTitle == "organizer");
    CHECK(!preferences.welcomed && Near(preferences.interfaceScale, 1.2) && preferences.country.ToString() == "DE");
    // The ones that could not be keep their defaults, and are held.
    CHECK(preferences.matchHudSize == PlayerPreferences{}.matchHudSize && preferences.readySound == PlayerPreferences{}.readySound);
    for (const char* key : {"matchHudSize", "roomColumn", "pictureMode", "readySound"}) CHECK(preferences.guard.held.count(key) == 1);
    CHECK(preferences.guard.held.count("mainFighter") == 0 && preferences.guard.held.count("interfaceScale") == 0);
    // A save of another setting writes it, and leaves those as the file has them, and the setting of a newer version too.
    preferences.matchHudAnchor = 2;
    SaveThroughWriter(directory, preferences);
    auto saved = Saved(directory);
    CHECK(saved["matchHudSize"] == "large" && saved["roomColumn"] == 100000 && saved["pictureMode"] == 77 && saved["readySound"] == "loud");
    CHECK(saved["matchHudAnchor"] == 2 && saved["mainFighter"] == 5 && saved["futureSetting"] == settings["futureSetting"]);
    // The player changing one of them writes it.
    preferences.matchHudSize = 1;
    SaveThroughWriter(directory, preferences);
    saved = Saved(directory);
    CHECK(saved["matchHudSize"] == 1 && saved["roomColumn"] == 100000 && saved["futureSetting"] == settings["futureSetting"]);
    // A reader that does not know the keys (an older build) loses none of them either: they are the store's own.
    CHECK(SettingsStore(directory.wstring()).SaveLauncher({{"displayName", "Player 1"}}, error));
    CHECK(Saved(directory)["futureSetting"] == settings["futureSetting"]);
}

// A file that is not JSON is kept aside before a fresh one is written.
void DamagedFile(const Path& root) {
    const auto directory = root / L"damaged";
    std::filesystem::create_directories(directory);
    Write(directory / L"settings.json", "{\"displayName\": \"Player 1\", \"mainFighter\": ");
    SettingsStore store(directory.wstring());
    SavedRead read(store, Clock::now(), Quick);
    CHECK(read.state() == SavedRead::State::Damaged && !read.keptAside().empty());
    CHECK(Read(directory / read.keptAside()) == "{\"displayName\": \"Player 1\", \"mainFighter\": ");
    PlayerPreferences preferences;
    preferences.lobby = {};
    // Nothing held: a damaged file has nothing the program could write over.
    ApplySavedSettings(read.saved(), preferences);
    CHECK(preferences.guard.held.empty() && !preferences.guard.frozen);
    preferences.interfaceScale = 0.9f;
    SaveThroughWriter(directory, preferences);
    CHECK(Near(Saved(directory)["interfaceScale"].get<double>(), 0.9));
    CHECK(Read(directory / read.keptAside()) == "{\"displayName\": \"Player 1\", \"mainFighter\": ");
}

// The main chosen last time is still the main after a start whose read failed,
// another setting changed, and a restart.
void MainSurvivesAFailedStart(const Path& root) {
    const auto directory = root / L"main-survives";
    SettingsStore store(directory.wstring());
    std::string error;
    PlayerPreferences first;
    first.mainFighter = 7;
    CHECK(store.SaveLauncher(PlayerPreferencesJson(first), error));
    CHECK(Saved(directory)["mainFighter"] == 7);
    {
        // The second run: the lock is busy at start and for a while; the player changes the interface size.
        HeldLock held(directory);
        SavedRead read(store, Clock::now(), Quick);
        CHECK(read.state() == SavedRead::State::Pending);
        PlayerPreferences run = UnreadRun();
        CHECK(run.mainFighter != 7);
        run.interfaceScale = 1.1f;
        held.Release();
        SaveThroughWriter(directory, run);
        CHECK(Settle(read) && read.state() == SavedRead::State::Read);
        ApplySavedSettings(SavedUnderChanges(read.saved(), run), run);
        CHECK(run.mainFighter == 7 && Near(run.interfaceScale, 1.1));
    }
    // The third run reads it as saved.
    SavedRead third(store, Clock::now(), Quick);
    CHECK(third.state() == SavedRead::State::Read);
    PlayerPreferences restarted;
    restarted.lobby = {};
    ApplySavedSettings(third.saved(), restarted);
    CHECK(restarted.mainFighter == 7 && Near(restarted.interfaceScale, 1.1));
    // And where the read never succeeded in the second run, nothing was written: the main is there still.
    {
        HeldLock held(directory);
        SavedRead read(store, Clock::now(), Quick);
        CHECK(Settle(read) && read.state() == SavedRead::State::GaveUp);
        PlayerPreferences run = UnreadRun();
        run.guard.frozen = true;
        run.interfaceScale = 0.8f;
        CHECK(PlayerPreferencesJson(run).empty());
    }
    SavedRead fourth(store, Clock::now(), Quick);
    PlayerPreferences again;
    again.lobby = {};
    ApplySavedSettings(fourth.saved(), again);
    CHECK(again.mainFighter == 7 && Near(again.interfaceScale, 1.1));
}

// A read late, after a change to the same setting the file holds, takes the change.
void ChangeWinsOverTheFile(const Path& root) {
    const auto directory = root / L"change-wins";
    SettingsStore store(directory.wstring());
    std::string error;
    CHECK(store.SaveLauncher(PlayerSettings(), error));
    SavedRead read(store, Clock::now(), Quick);
    PlayerPreferences run = UnreadRun();
    run.mainFighter = 9;
    run.matchHudSize = 1;
    const auto merged = SavedUnderChanges(read.saved(), run);
    CHECK(merged["mainFighter"] == 9 && merged["matchHudSize"] == 1 && merged["readySoundVolume"] == 50 && merged["displayName"] == "Player 1");
    ApplySavedSettings(merged, run);
    CHECK(run.mainFighter == 9 && run.matchHudSize == 1 && run.readySoundVolume == 50);
}

}

int main() {
    const auto root = MakeTempRoot(L"sf4e-saved-settings-");
    std::filesystem::create_directories(root);
    GuardAlone();
    LockHeldThenReleased(root);
    SaveBeforeLateRead(root);
    LockNeverReleased(root);
    OneBadField(root);
    DamagedFile(root);
    MainSurvivesAFailedStart(root);
    ChangeWinsOverTheFile(root);
    RemoveTempRoot(root);
    std::cout << "Saved settings: failed reads retried, nothing written before one succeeds, changes merged, bad fields and damaged files kept\n";
    return 0;
}
