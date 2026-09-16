#include "../netplay/SettingsStore.hxx"
#include "../netplay/SettingsWriter.hxx"
#include "../netplay/RoomPreferences.hxx"
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::cerr << "Check failed at " << __LINE__ << ": " #c << '\n'; std::exit(1); } } while (false)
using Json = nlohmann::json;
using Path = std::filesystem::path;
using sf4e::netplay::SettingsStore;

static void Write(const Path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << bytes;
    file.close();
    CHECK(file.good());
}

static std::string Read(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    CHECK(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int main() {
    // Each run owns a fresh temporary subtree; never consult real AppData.
    const auto root = std::filesystem::temp_directory_path() /
        (L"sf4e-settings-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    CHECK(std::filesystem::create_directory(root));
    const auto path = root / L"migration";
    CHECK(std::filesystem::create_directory(path));
    const Json launcher = {{"displayName", "Saved player"}, {"inputDelay", 3}, {"roundCount", 7},
        {"roundTimeIntegral", 300}, {"editionSelect", 1}, {"sessionPort", 23456},
        {"relayHostSecret", "old-test-secret"}, {"relayRoomCode", "OLD1"}, {"relaySessionPort", 30001},
        {"unknownPreference", "retain"}};
    const Json overlay = {{"stageID", 12}, {"lobby", {{"charaID", 23}, {"color", 7}}},
        {"mainMenu", {{"p2", {{"charaID", 9}}}}}, {"device", {{"idx", 2}, {"type", 1}}}};
    Write(path / L"config.json", launcher.dump(4));
    Write(path / L"overlay_prefs.json", overlay.dump(4));
    SettingsStore store(path.wstring());
    Json result;
    std::string error;
    CHECK(store.LoadLauncher(result, error));
    CHECK(error.empty());
    CHECK(result["displayName"] == "Saved player" && result["inputDelay"] == 3);
    CHECK(!result.contains("relayHostSecret") && !result.contains("relayRoomCode") && !result.contains("relaySessionPort"));
    auto activeOverlay = overlay; activeOverlay.erase("mainMenu");
    CHECK(store.LoadOverlay(result, error) && result == activeOverlay);
    CHECK(store.LoadLauncher(result, error) && !result.contains("sessionPort"));
    CHECK(Read(path / L"config.json.pre-v1.bak") == launcher.dump(4));
    CHECK(Read(path / L"overlay_prefs.json.pre-v1.bak") == overlay.dump(4));
    CHECK(Read(path / L"config.json") == launcher.dump(4));
    CHECK(Read(path / L"overlay_prefs.json") == overlay.dump(4));
    const auto document = Json::parse(Read(path / L"settings.json"));
    CHECK(document["schemaVersion"] == 1 && document["profile"]["displayName"] == "Saved player");
    CHECK(document["netplay"]["roundCount"] == 7 && !document["legacyLauncher"].contains("displayName"));
    CHECK(Read(path / L"settings.json").find("old-test-secret") == std::string::npos);

    sf4e::netplay::PlayerPreferences discordDefaults;
    CHECK(discordDefaults.matchHudSize==1&&!discordDefaults.matchHudRaised);
    discordDefaults.matchHudSize=-1;CHECK(!discordDefaults.Valid());
    discordDefaults.matchHudSize=3;CHECK(!discordDefaults.Valid());
    discordDefaults.matchHudSize=2;CHECK(discordDefaults.Valid());
    CHECK(store.SaveLauncher({{"matchHudSize",2},{"matchHudRaised",true}},error));
    CHECK(store.LoadLauncher(result,error)&&result["matchHudSize"]==2&&result["matchHudRaised"]==true);
    CHECK(Json::parse(Read(path / L"settings.json"))["netplay"]["matchHudSize"]==2);
    CHECK(discordDefaults.discordPresence && discordDefaults.discordInvites);
    CHECK(discordDefaults.inputDelay == 2);
    discordDefaults.inputDelay = 0; CHECK(discordDefaults.Valid());
    discordDefaults.inputDelay = 10; CHECK(discordDefaults.Valid());
    discordDefaults.inputDelay = -1; CHECK(!discordDefaults.Valid());
    discordDefaults.inputDelay = 11; CHECK(!discordDefaults.Valid());
    CHECK(store.SaveLauncher({{"inputDelay", 0}}, error));
    CHECK(store.LoadLauncher(result, error) && result["inputDelay"] == 0);
    CHECK(store.SaveLauncher({{"inputDelay", 3}}, error));
    CHECK(store.SaveLauncher({{"discordPresence",false},{"discordInvites",false}},error));
    CHECK(store.LoadLauncher(result,error));
    CHECK(result["discordPresence"]==false && result["discordInvites"]==false);
    const auto discordSettings=Json::parse(Read(path / L"settings.json"));
    CHECK(discordSettings["netplay"]["discordPresence"]==false);
    CHECK(!discordSettings["legacyLauncher"].contains("discordInvites"));
    // Independent consumers merge into the latest complete document.
    SettingsStore second(path.wstring());
    CHECK(store.SaveLauncher({{"displayName", "Changed"}}, error));
    CHECK(second.SaveOverlay({{"stageID", 29}}, error));
    CHECK(store.LoadLauncher(result, error));
    CHECK(result["displayName"] == "Changed" && result["unknownPreference"] == "retain" && result["inputDelay"] == 3);
    CHECK(store.SaveLauncher({{"mainFighter",1},{"onlineRecord",{{"wins",4},{"losses",2},{"recent",Json::array()}}}},error));
    CHECK(store.LoadLauncher(result,error)&&result["mainFighter"]==1&&result["onlineRecord"]["wins"]==4);
    CHECK(store.SaveLauncher({{"displayName","Changed"}},error));
    CHECK(store.LoadLauncher(result,error)&&result["onlineRecord"]["losses"]==2&&result["mainFighter"]==1);
    CHECK(second.LoadOverlay(result, error));
    CHECK(result["stageID"] == 29 && result["lobby"] == overlay["lobby"]);
    CHECK(Read(path / L"config.json.pre-v1.bak") == launcher.dump(4));
    // Legacy files no longer override current preferences on later startups.
    Write(path / L"config.json", "broken old config");
    CHECK(store.LoadLauncher(result, error) && result["displayName"] == "Changed");

    // Failed atomic replacement leaves the previous document byte-identical.
    const auto saved = Read(path / L"settings.json");
    HANDLE reader = CreateFileW((path / L"settings.json").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(reader != INVALID_HANDLE_VALUE);
    CHECK(!store.SaveLauncher({{"displayName", "Must not publish"}}, error));
    CloseHandle(reader);
    CHECK(Read(path / L"settings.json") == saved);
    for (const auto& entry : std::filesystem::directory_iterator(path))
        CHECK(entry.path().filename().wstring().find(L".pending.") == std::wstring::npos);

    HANDLE lock = CreateFileW((path / L"settings.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    CHECK(!second.SaveOverlay({{"stageID", 0}}, error));
    CloseHandle(lock);
    CHECK(Read(path / L"settings.json") == saved);
    CHECK(second.SaveOverlay({{"stageID", 0}}, error));

    // Unsupported/corrupt/oversized/deep current data must never be repaired by
    // overwriting it with defaults, even when a caller explicitly attempts Save.
    for (const std::string& bad : {std::string("{\"schemaVersion\":2}"), std::string("{"),
        std::string(256 * 1024 + 1, ' '), std::string("{\"deep\":") + std::string(40, '[') + "0" + std::string(40, ']') + "}"}) {
        Write(path / L"settings.json", bad);
        CHECK(!store.LoadLauncher(result, error));
        CHECK(!store.SaveLauncher({{"displayName", "Default"}}, error));
        CHECK(Read(path / L"settings.json") == bad);
    }

    const auto broken = root / L"broken-migration";
    CHECK(std::filesystem::create_directory(broken));
    SettingsStore brokenStore(broken.wstring());
    Write(broken / L"config.json", launcher.dump());
    Write(broken / L"overlay_prefs.json", "{");
    CHECK(!brokenStore.LoadLauncher(result, error));
    CHECK(!std::filesystem::exists(broken / L"settings.json"));
    CHECK(!std::filesystem::exists(broken / L"config.json.pre-v1.bak"));
    Write(broken / L"overlay_prefs.json", overlay.dump());
    Write(broken / L"config.json.pre-v1.bak", "different backup");
    CHECK(!brokenStore.LoadLauncher(result, error));
    CHECK(Read(broken / L"config.json.pre-v1.bak") == "different backup");
    CHECK(!std::filesystem::exists(broken / L"settings.json"));
    Write(broken / L"config.json.pre-v1.bak", launcher.dump());
    CHECK(brokenStore.LoadLauncher(result, error)); // Interrupted migration retry.

    SettingsStore fresh((root / L"fresh").wstring());
    CHECK(fresh.LoadLauncher(result, error) && result.empty());
    CHECK(fresh.LoadOverlay(result, error) && result.empty());
    CHECK(!SettingsStore(L"").LoadLauncher(result, error));

    // Hold the cross-process lock while the UI submits many edits. They remain
    // bounded in one pending slot; releasing the lock lets the latest one save.
    const auto asyncPath = root / L"async";
    SettingsStore asyncStore(asyncPath.wstring());
    CHECK(asyncStore.LoadOverlay(result, error));
    lock = CreateFileW((asyncPath / L"settings.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    sf4e::netplay::SettingsWriter writer(asyncPath.wstring());
    for (int i = 0; i < 100; ++i) CHECK(writer.QueueOverlay({{"stageID", i % 30}, {"revision", i}}));
    const auto launcherRevision = writer.QueueLauncher({{"displayName", "From ImGui"}, {"mainFighter", 4}, {"inputDelay", 7}, {"roundCount", 15}});
    CHECK(launcherRevision != 0);
    const auto deadline = GetTickCount64() + 3000;
    while (writer.GetStatus().error.empty() && GetTickCount64() < deadline) Sleep(1);
    CHECK(writer.GetStatus().pending && !writer.GetStatus().error.empty());
    // Acceptance is not persistence: a failing write must not report the
    // revision as saved, however long the caller waits.
    CHECK(writer.SavedLauncherRevision() < launcherRevision);
    CloseHandle(lock);
    // The worker retries on its own; the revision is reported only once written.
    const auto savedDeadline = GetTickCount64() + 5000;
    while (writer.SavedLauncherRevision() < launcherRevision && GetTickCount64() < savedDeadline) Sleep(1);
    CHECK(writer.SavedLauncherRevision() >= launcherRevision);
    CHECK(asyncStore.LoadLauncher(result, error) && result["displayName"] == "From ImGui");
    writer.Stop();
    CHECK(!writer.GetStatus().pending && writer.GetStatus().error.empty());
    CHECK(!writer.QueueOverlay({{"stageID", 1}}));
    CHECK(asyncStore.LoadOverlay(result, error) && result["revision"] == 99);
    CHECK(asyncStore.LoadLauncher(result, error) && result["displayName"] == "From ImGui" && result["inputDelay"] == 7 && result["roundCount"] == 15);
    // Reopen the store after the writer drains; a portrait must survive a new
    // reader, including a transient lock failure and concurrent overlay writes.
    SettingsStore reopenedProfile(asyncPath.wstring());
    CHECK(reopenedProfile.LoadLauncher(result, error) && result["mainFighter"] == 4);

    // This is the exact fresh subtree created above, never an externally supplied path.
    sf4e::netplay::PlayerPreferences defaults;
    CHECK(sf4e::netplay::ReadRoomPreferences(Json::object(), defaults));
    CHECK(defaults.roomCapacity == 16 && defaults.tableRules.format == sf4e::room::SetFormat::Unlimited &&
        defaults.tableRules.rotation == sf4e::room::RotationMode::WinnerStays);
    defaults.roomName = "Friday room"; defaults.roomCapacity = 12;
    defaults.tableRules.format = sf4e::room::SetFormat::Ft5;
    defaults.tableRules.rotation = sf4e::room::RotationMode::BothRotate;
    CHECK(asyncStore.SaveLauncher({{"roomDefaults", sf4e::netplay::RoomPreferences(defaults)}}, error));
    CHECK(asyncStore.SaveLauncher({{"inputDelay", 4}}, error));
    CHECK(asyncStore.LoadLauncher(result, error));
    sf4e::netplay::PlayerPreferences restored;
    CHECK(sf4e::netplay::ReadRoomPreferences(result, restored));
    CHECK(restored.roomName == "Friday room" && restored.roomCapacity == 12 &&
        restored.tableRules.format == sf4e::room::SetFormat::Unlimited && restored.tableRules.rotation == sf4e::room::RotationMode::WinnerStays);
    for (const Json& invalid : {Json{{"capacity", 17}}, Json{{"capacity", 258}}, Json{{"capacity", 2.5}},
        Json{{"format", 257}}, Json{{"rotation", -1}}, Json{{"name", std::string(65, 'x')}}}) {
        CHECK(!sf4e::netplay::ReadRoomPreferences({{"roomDefaults", invalid}}, restored));
        CHECK(restored.roomName == "Friday room" && restored.roomCapacity == 12);
    }
    CHECK(std::filesystem::equivalent(root.parent_path(), std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
    std::cout << "Settings migration, preservation, independent updates and atomic failure checks passed\n";
}
