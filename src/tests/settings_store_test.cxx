#include "../netplay/SettingsStore.hxx"
#include "../netplay/SettingsWriter.hxx"
#include "../netplay/RoomPreferences.hxx"
#include "../netplay/InputDelayPreference.hxx"
#include "../netplay/MatchHudPreference.hxx"
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>

#include "test_support.hxx"
#include "temp_root.hxx"
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

// A profile's input delay as the game loads it from a fresh settings folder
// holding `saved`: the launcher reads the number (netplay_persist.cxx), and
// the game reads Auto beside it.
static sf4e::netplay::PlayerPreferences LoadDelay(const Path& root, const std::wstring& name, const Json& saved) {
    const auto folder = root / name;
    CHECK(std::filesystem::create_directory(folder));
    SettingsStore store(folder.wstring());
    std::string error;
    CHECK(store.SaveLauncher(saved, error));
    Json loaded;
    CHECK(store.LoadLauncher(loaded, error));
    sf4e::netplay::PlayerPreferences value;
    value.inputDelay = sf4e::SavedInputDelay(loaded.value("inputDelay", sf4e::DefaultInputDelay));
    sf4e::netplay::ReadInputDelayPreference(loaded, value);
    return value;
}

// Existing explicit choices survive; absent/invalid flags use manual zero by default.
static void InputDelayMigrations(const Path& root) {
    using sf4e::netplay::PlayerPreferences;
    const auto fresh = LoadDelay(root, L"delay-new", Json::object());
    CHECK(!fresh.autoInputDelay && fresh.inputDelay == 0 && fresh.Valid());
    const auto zero = LoadDelay(root, L"delay-zero", {{"inputDelay", 0}});
    CHECK(!zero.autoInputDelay && zero.inputDelay == 0 && zero.Valid());
    const auto four = LoadDelay(root, L"delay-four", {{"inputDelay", 4}});
    CHECK(!four.autoInputDelay && four.inputDelay == 4 && four.Valid());
    const auto odd = LoadDelay(root, L"delay-odd", {{"inputDelay", 3}, {"autoInputDelay", 0}});
    CHECK(!odd.autoInputDelay && odd.inputDelay == 3);
    for (int invalid : {-1, 11, 99}) {
        PlayerPreferences fallback;
        sf4e::netplay::ReadInputDelayPreference({{"inputDelay", invalid}}, fallback);
        CHECK(fallback.inputDelay == 0 && !fallback.autoInputDelay && fallback.Valid());
    }
    PlayerPreferences fallback;
    sf4e::netplay::ReadInputDelayPreference({{"inputDelay", "3"}}, fallback);
    CHECK(fallback.inputDelay == 0 && !fallback.autoInputDelay);
    for (const bool on : {true, false}) for (int number : {0, 1, 3, 10}) {
        const auto again = LoadDelay(root, L"delay-round-" + std::to_wstring(on) + L"-" + std::to_wstring(number),
            {{"inputDelay", number}, {"autoInputDelay", on}});
        CHECK(again.autoInputDelay == on && again.inputDelay == number && again.Valid());
    }
}

int main() {
    // Each run owns a fresh temporary subtree; never consult real AppData.
    const auto root = MakeTempRoot(L"sf4e-settings-test-");
    CHECK(std::filesystem::create_directory(root));
    const auto path = root / L"migration";
    CHECK(std::filesystem::create_directory(path));
    const Json launcher = {{"displayName", "Saved player"}, {"inputDelay", 3}, {"roundCount", 7},
        {"roundTimeIntegral", 300}, {"editionSelect", 1}, {"sessionPort", 23456},
        {"relayHostSecret", "old-test-secret"}, {"relayRoomCode", "OLD1"}, {"relaySessionPort", 30001},
        {"matchHudNames", 1}, {"unknownPreference", "retain"}};
    const Json overlay = {{"diagnostics", {{"heapCheckInterval", 1}}}, {"stageID", 12}, {"lobby", {{"charaID", 23}, {"color", 7}}},
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
    CHECK(!result.contains("matchHudNames") && result["unknownPreference"] == "retain");
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
    CHECK(discordDefaults.showMatchHud&&discordDefaults.matchHudSize==0&&!discordDefaults.matchHudRaised); // On, Small, normal spacing by default.
    discordDefaults.matchHudSize=-1;CHECK(!discordDefaults.Valid());
    discordDefaults.matchHudSize=3;CHECK(!discordDefaults.Valid());
    discordDefaults.matchHudSize=2;CHECK(discordDefaults.Valid());
    CHECK(store.SaveLauncher({{"matchHudSize",2},{"matchHudRaised",true}},error));
    CHECK(store.LoadLauncher(result,error)&&result["matchHudSize"]==2&&result["matchHudRaised"]==true);
    CHECK(Json::parse(Read(path / L"settings.json"))["netplay"]["matchHudSize"]==2);
    CHECK(discordDefaults.matchHudAnchor==0); // Bottom center by default.
    CHECK(discordDefaults.matchHudNameOffset==0); // The game's default HUD position.
    discordDefaults.matchHudNameOffset=-sf4e::netplay::MaxMatchHudNameOffset-1;CHECK(!discordDefaults.Valid());
    discordDefaults.matchHudNameOffset=sf4e::netplay::MaxMatchHudNameOffset+1;CHECK(!discordDefaults.Valid());
    discordDefaults.matchHudNameOffset=-sf4e::netplay::MaxMatchHudNameOffset;CHECK(discordDefaults.Valid());
    discordDefaults.matchHudNameOffset=0;
    CHECK(store.SaveLauncher({{"matchHudNameOffset",-20}},error));
    CHECK(store.LoadLauncher(result,error)&&result["matchHudNameOffset"]==-20);
    CHECK(sf4e::netplay::ReadMatchHudNameOffset(result)==-20);
    // A profile from before 1.1.0, the wrong type, out of range, or a number past an
    // int's range (which a plain conversion would wrap into range) is Default.
    using sf4e::netplay::ReadMatchHudNameOffset;
    CHECK(ReadMatchHudNameOffset(Json::object())==0&&ReadMatchHudNameOffset({{"matchHudNameOffset","-20"}})==0);
    CHECK(ReadMatchHudNameOffset({{"matchHudNameOffset",61}})==0&&ReadMatchHudNameOffset({{"matchHudNameOffset",-61}})==0);
    CHECK(ReadMatchHudNameOffset({{"matchHudNameOffset",60}})==60&&ReadMatchHudNameOffset({{"matchHudNameOffset",-60}})==-60);
    CHECK(ReadMatchHudNameOffset({{"matchHudNameOffset",4294967298ull}})==0&&ReadMatchHudNameOffset({{"matchHudNameOffset",-4294967294ll}})==0);
    CHECK(ReadMatchHudNameOffset({{"matchHudNameOffset",18446744073709551615ull}})==0&&ReadMatchHudNameOffset({{"matchHudNameOffset",2.0}})==0);
    discordDefaults.matchHudAnchor=-1;CHECK(!discordDefaults.Valid());
    discordDefaults.matchHudAnchor=5;CHECK(!discordDefaults.Valid());
    discordDefaults.matchHudAnchor=4;CHECK(discordDefaults.Valid());
    CHECK(store.SaveLauncher({{"matchHudAnchor",4}},error));
    CHECK(store.LoadLauncher(result,error)&&result["matchHudAnchor"]==4);
    CHECK(Json::parse(Read(path / L"settings.json"))["netplay"]["matchHudAnchor"]==4);
    discordDefaults.matchHudAnchor=0;
    CHECK(discordDefaults.matchHudLayout==1); // Split by default.
    discordDefaults.matchHudLayout=-1;CHECK(!discordDefaults.Valid());
    discordDefaults.matchHudLayout=2;CHECK(!discordDefaults.Valid());
    discordDefaults.matchHudLayout=1;CHECK(discordDefaults.Valid());
    CHECK(store.SaveLauncher({{"matchHudLayout",1}},error));
    CHECK(store.LoadLauncher(result,error)&&result["matchHudLayout"]==1);
    CHECK(Json::parse(Read(path / L"settings.json"))["netplay"]["matchHudLayout"]==1);
    discordDefaults.matchHudLayout=1;
    {
        // A retired preference leaves both the active section and the legacy one, and nothing else does.
        auto saved=Json::parse(Read(path / L"settings.json"));
        saved["netplay"]["matchHudNames"]=1;saved["legacyLauncher"]["matchHudNames"]=1;
        Write(path / L"settings.json",saved.dump(4));
        CHECK(store.LoadLauncher(result,error)&&!result.contains("matchHudNames")&&result["matchHudLayout"]==1&&result["unknownPreference"]=="retain");
        CHECK(store.SaveLauncher({{"inputDelay",3}},error));
        const auto retired=Json::parse(Read(path / L"settings.json"));
        CHECK(!retired["netplay"].contains("matchHudNames")&&!retired["legacyLauncher"].contains("matchHudNames"));
        CHECK(retired["netplay"]["matchHudAnchor"]==4&&retired["legacyLauncher"]["unknownPreference"]=="retain");
    }
    CHECK(discordDefaults.discordPresence && discordDefaults.discordInvites);
    CHECK(discordDefaults.inputDelay == 0);
    discordDefaults.inputDelay = 0; CHECK(discordDefaults.Valid());
    discordDefaults.inputDelay = 1; CHECK(discordDefaults.Valid());
    discordDefaults.inputDelay = 10; CHECK(discordDefaults.Valid());
    discordDefaults.inputDelay = -1; CHECK(!discordDefaults.Valid());
    discordDefaults.inputDelay = 11; CHECK(!discordDefaults.Valid());
    CHECK(store.SaveLauncher({{"inputDelay", 0}}, error));
    CHECK(store.LoadLauncher(result, error) && result["inputDelay"] == 0);
    // Zero is preserved; out of range falls back to zero.
    CHECK(sf4e::SavedInputDelay(result["inputDelay"].get<int>()) == 0);
    CHECK(sf4e::SavedInputDelay(1) == 1 && sf4e::SavedInputDelay(3) == 3 && sf4e::SavedInputDelay(10) == 10);
    CHECK(sf4e::SavedInputDelay(-1) == 0 && sf4e::SavedInputDelay(11) == 0);
    // Auto is opt-in and its explicit flag is saved beside the delay.
    CHECK(!sf4e::netplay::PlayerPreferences().autoInputDelay && !result.contains("autoInputDelay"));
    discordDefaults.inputDelay = 2; discordDefaults.autoInputDelay = false; CHECK(discordDefaults.Valid());
    CHECK(store.SaveLauncher({{"autoInputDelay", false}}, error));
    CHECK(store.LoadLauncher(result, error) && result["autoInputDelay"] == false && result["inputDelay"] == 0);
    const auto autoDelaySettings = Json::parse(Read(path / L"settings.json"));
    CHECK(autoDelaySettings["netplay"]["autoInputDelay"] == false && !autoDelaySettings["legacyLauncher"].contains("autoInputDelay"));
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
    // The launcher keeps the game folder picked in recovery as UTF-8 text.
    const std::string gameDirectory = "E:\\Jogos Instala\xc3\xa7\xc3\xa3o\\Ultra";
    CHECK(store.SaveLauncher({{"gameDirectory", gameDirectory}}, error));
    CHECK(store.LoadLauncher(result, error) && result["gameDirectory"] == gameDirectory && result["displayName"] == "Changed");
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
    // A public room starts from the public default until the player picks others.
    CHECK(defaults.publicTableRules == sf4e::room::PublicRoomRules());
    defaults.roomName = "Friday room"; defaults.roomCapacity = 12; defaults.roomPublic = true;
    defaults.tableRules.format = sf4e::room::SetFormat::Ft10;
    defaults.tableRules.rotation = sf4e::room::RotationMode::BothRotate;
    defaults.publicTableRules.format = sf4e::room::SetFormat::Ft3;
    defaults.publicTableRules.rotation = sf4e::room::RotationMode::LoserStays;
    defaults.publicTableRules.roundCount = 5;
    CHECK(asyncStore.SaveLauncher({{"roomDefaults", sf4e::netplay::RoomPreferences(defaults)}}, error));
    CHECK(asyncStore.SaveLauncher({{"inputDelay", 4}}, error));
    CHECK(asyncStore.LoadLauncher(result, error));
    sf4e::netplay::PlayerPreferences restored;
    CHECK(sf4e::netplay::ReadRoomPreferences(result, restored));
    CHECK(restored.roomName == "Friday room" && restored.roomCapacity == 12 && restored.roomPublic &&
        restored.tableRules.format == sf4e::room::SetFormat::Ft10 && restored.tableRules.rotation == sf4e::room::RotationMode::BothRotate);
    CHECK(restored.publicTableRules == defaults.publicTableRules && !(restored.publicTableRules == restored.tableRules));
    // Room defaults saved before public rules existed keep the public default.
    {
        auto older = result;
        older["roomDefaults"].erase("publicRules");
        sf4e::netplay::PlayerPreferences fromOlder;
        CHECK(sf4e::netplay::ReadRoomPreferences(older, fromOlder) && fromOlder.publicTableRules == sf4e::room::PublicRoomRules() &&
            fromOlder.tableRules.format == sf4e::room::SetFormat::Ft10);
    }
    for (const Json& invalid : {Json{{"capacity", 17}}, Json{{"capacity", 258}}, Json{{"capacity", 2.5}},
        Json{{"format", 257}}, Json{{"format", 11}}, Json{{"rotation", -1}}, Json{{"rotation", 3}}, Json{{"name", std::string(65, 'x')}}, Json{{"public", "yes"}},
        Json{{"publicRules", 2}}, Json{{"publicRules", {{"format", 11}}}}, Json{{"publicRules", {{"rotation", 3}}}}, Json{{"publicRules", {{"roundTime", 10}}}}}) {
        CHECK(!sf4e::netplay::ReadRoomPreferences({{"roomDefaults", invalid}}, restored));
        CHECK(restored.roomName == "Friday room" && restored.roomCapacity == 12);
    }
    InputDelayMigrations(root);
    RemoveTempRoot(root);
    std::cout << "Settings migration, preservation, independent updates and atomic failure checks passed\n";
}
