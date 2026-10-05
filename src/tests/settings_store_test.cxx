#include "../netplay/SettingsStore.hxx"
#include "../netplay/SettingsWriter.hxx"
#include "../netplay/RoomPreferences.hxx"
#include "../netplay/InputDelayPreference.hxx"
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
    value.inputDelay = sf4e::SavedInputDelay(loaded.value("inputDelay", 2));
    sf4e::netplay::ReadInputDelayPreference(loaded, value);
    return value;
}

// Auto is kept apart from the number, so the 0 to 1 migration and Auto never
// stand for one another. Each case is a settings folder as that version left it.
static void InputDelayMigrations(const Path& root) {
    using sf4e::netplay::PlayerPreferences;
    // 1.0.x saved 0 before 0 was withdrawn: Auto, with 1 kept as the number.
    const auto zero = LoadDelay(root, L"delay-zero", {{"inputDelay", 0}});
    CHECK(zero.autoInputDelay && zero.inputDelay == 1 && zero.Valid());
    // 1.0.x saved a number: Auto until a number is chosen again; the number is kept.
    const auto four = LoadDelay(root, L"delay-four", {{"inputDelay", 4}});
    CHECK(four.autoInputDelay && four.inputDelay == 4 && four.Valid());
    // Auto chosen, or a new profile saved: Auto, whatever number sits beside it.
    const auto automatic = LoadDelay(root, L"delay-auto", {{"inputDelay", 1}, {"autoInputDelay", true}});
    CHECK(automatic.autoInputDelay && automatic.inputDelay == 1);
    // A number chosen after Auto existed stays that number, 1 included, which
    // is also where a migrated 0 lands: the flag tells them apart.
    for (int chosen : {1, 2, 5, sf4e::MaximumInputDelay}) {
        const auto manual = LoadDelay(root, L"delay-chosen-" + std::to_wstring(chosen), {{"inputDelay", chosen}, {"autoInputDelay", false}});
        CHECK(!manual.autoInputDelay && manual.inputDelay == chosen && manual.Valid());
    }
    // A chosen number written beside a stray 0 (this version never writes
    // one) still loads as manual at the smallest delay, never as 0.
    const auto stray = LoadDelay(root, L"delay-stray", {{"inputDelay", 0}, {"autoInputDelay", false}});
    CHECK(!stray.autoInputDelay && stray.inputDelay == 1);
    // A value the game never writes under autoInputDelay is Auto.
    const auto odd = LoadDelay(root, L"delay-odd", {{"inputDelay", 3}, {"autoInputDelay", 0}});
    CHECK(odd.autoInputDelay && odd.inputDelay == 3);
    // Out of range or the wrong type keeps the default number.
    PlayerPreferences fallback;
    sf4e::netplay::ReadInputDelayPreference({{"inputDelay", 99}, {"autoInputDelay", false}}, fallback);
    CHECK(fallback.inputDelay == 2 && !fallback.autoInputDelay);
    sf4e::netplay::ReadInputDelayPreference({{"inputDelay", "3"}}, fallback);
    CHECK(fallback.inputDelay == 2 && fallback.autoInputDelay);
    // Saving and loading keeps each choice as it was: the game writes both keys.
    for (const bool on : {true, false}) for (int number : {1, 3}) {
        const auto again = LoadDelay(root, L"delay-round-" + std::to_wstring(on) + L"-" + std::to_wstring(number),
            {{"inputDelay", number}, {"autoInputDelay", on}});
        CHECK(again.autoInputDelay == on && again.inputDelay == number);
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
    CHECK(discordDefaults.inputDelay == 2);
    discordDefaults.inputDelay = 0; CHECK(!discordDefaults.Valid());
    discordDefaults.inputDelay = 1; CHECK(discordDefaults.Valid());
    discordDefaults.inputDelay = 10; CHECK(discordDefaults.Valid());
    discordDefaults.inputDelay = -1; CHECK(!discordDefaults.Valid());
    discordDefaults.inputDelay = 11; CHECK(!discordDefaults.Valid());
    CHECK(store.SaveLauncher({{"inputDelay", 0}}, error));
    CHECK(store.LoadLauncher(result, error) && result["inputDelay"] == 0);
    // A 0 saved before it was withdrawn plays at 1; out of range is the default.
    CHECK(sf4e::SavedInputDelay(result["inputDelay"].get<int>()) == 1);
    CHECK(sf4e::SavedInputDelay(1) == 1 && sf4e::SavedInputDelay(3) == 3 && sf4e::SavedInputDelay(10) == 10);
    CHECK(sf4e::SavedInputDelay(-1) == 2 && sf4e::SavedInputDelay(11) == 2);
    // Auto is on until a number is chosen and absent from a profile saved
    // before it existed. Turning it off is saved beside the delay.
    CHECK(sf4e::netplay::PlayerPreferences().autoInputDelay && !result.contains("autoInputDelay"));
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
    defaults.tableRules.format = sf4e::room::SetFormat::Ft5;
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
        restored.tableRules.format == sf4e::room::SetFormat::Ft5 && restored.tableRules.rotation == sf4e::room::RotationMode::BothRotate);
    CHECK(restored.publicTableRules == defaults.publicTableRules && !(restored.publicTableRules == restored.tableRules));
    // Room defaults saved before public rules existed keep the public default.
    {
        auto older = result;
        older["roomDefaults"].erase("publicRules");
        sf4e::netplay::PlayerPreferences fromOlder;
        CHECK(sf4e::netplay::ReadRoomPreferences(older, fromOlder) && fromOlder.publicTableRules == sf4e::room::PublicRoomRules() &&
            fromOlder.tableRules.format == sf4e::room::SetFormat::Ft5);
    }
    for (const Json& invalid : {Json{{"capacity", 17}}, Json{{"capacity", 258}}, Json{{"capacity", 2.5}},
        Json{{"format", 257}}, Json{{"format", 4}}, Json{{"rotation", -1}}, Json{{"rotation", 3}}, Json{{"name", std::string(65, 'x')}}, Json{{"public", "yes"}},
        Json{{"publicRules", 2}}, Json{{"publicRules", {{"format", 4}}}}, Json{{"publicRules", {{"rotation", 3}}}}, Json{{"publicRules", {{"roundTime", 10}}}}}) {
        CHECK(!sf4e::netplay::ReadRoomPreferences({{"roomDefaults", invalid}}, restored));
        CHECK(restored.roomName == "Friday room" && restored.roomCapacity == 12);
    }
    InputDelayMigrations(root);
    RemoveTempRoot(root);
    std::cout << "Settings migration, preservation, independent updates and atomic failure checks passed\n";
}
