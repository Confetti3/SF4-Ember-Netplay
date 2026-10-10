#include "../netplay/SettingsStore.hxx"
#include "../netplay/SettingsWriter.hxx"
#include "../netplay/RoomPreferences.hxx"
#include "../netplay/InputDelayPreference.hxx"
#include "../netplay/MatchHudPreference.hxx"
#include "../netplay/JsonFileStore.hxx"
#include "../platform/DurableFile.hxx"
#include "../netplay/BoolPreferenceJson.hxx"
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <algorithm>
#include <functional>
#include <iterator>

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

static std::size_t Entries(const Path& folder) {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(folder), std::filesystem::directory_iterator{}));
}

// The byte-level reader and writers under settings, replays and the updater.
static void DurableFiles(const Path& root) {
    namespace durable = sf4e::durable;
    const auto folder = root / L"durable";
    CHECK(std::filesystem::create_directory(folder));
    const auto file = folder / L"file.bin";

    // Missing is only an absent file; empty and exactly-at-the-bound files read.
    CHECK(durable::ReadBounded(file, 8).status == durable::ReadStatus::Missing);
    Write(file, "");
    auto read = durable::ReadBounded(file, 8);
    CHECK(read.status == durable::ReadStatus::Read && read.bytes.empty());
    Write(file, "12345678");
    read = durable::ReadBounded(file, 8);
    CHECK(read.status == durable::ReadStatus::Read && std::string(read.bytes.begin(), read.bytes.end()) == "12345678");
    CHECK(durable::ReadBounded(file, 7).status == durable::ReadStatus::TooLarge);
    CHECK(durable::ReadBounded(folder, 8).status == durable::ReadStatus::CannotOpen);
    // Larger than one read chunk, so the bound holds across several reads.
    const std::string large(200 * 1024 + 3, 'x');
    Write(file, large);
    read = durable::ReadBounded(file, large.size());
    CHECK(read.status == durable::ReadStatus::Read && read.bytes.size() == large.size());
    CHECK(durable::ReadBounded(file, large.size() - 1).status == durable::ReadStatus::TooLarge);
    HANDLE held = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    CHECK(durable::ReadBounded(file, large.size()).status == durable::ReadStatus::CannotOpen);
    CloseHandle(held);
    CHECK(std::filesystem::remove(file));

    // Create-only publication writes a new file, then refuses an existing one,
    // leaving it intact and removing its own temporary.
    const std::string first = "first", second = "second";
    CHECK(durable::PublishCreateOnly(file, durable::PartialPath(file), first.data(), first.size()));
    CHECK(Read(file) == first);
    const auto refused = durable::PublishCreateOnly(file, durable::PartialPath(file), second.data(), second.size());
    CHECK(!refused && refused.failed == durable::WriteStep::Move);
    CHECK(Read(file) == first && Entries(folder) == 1);

    // Replacement replaces, unless the destination cannot be replaced.
    CHECK(durable::PublishReplace(file, durable::PartialPath(file), second.data(), second.size()));
    CHECK(Read(file) == second && Entries(folder) == 1);
    held = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    const auto blocked = durable::PublishReplace(file, durable::PartialPath(file), first.data(), first.size());
    CloseHandle(held);
    CHECK(!blocked && blocked.failed == durable::WriteStep::Move);
    CHECK(Read(file) == second && Entries(folder) == 1);

    // A temporary name that is already taken belongs to someone else: the
    // call reports it, so a caller can retry, and touches neither file.
    const auto taken = folder / L"taken.tmp";
    Write(taken, "someone else's");
    const auto occupied = durable::PublishReplace(file, taken, first.data(), first.size());
    CHECK(!occupied && occupied.failed == durable::WriteStep::Create && occupied.error == ERROR_FILE_EXISTS);
    CHECK(Read(taken) == "someone else's" && Read(file) == second);

    // A failed write reports its own nonzero error and leaves no file. The
    // system refuses to read bytes from an unmapped buffer.
    const auto failed = folder / L"failed.bin";
    const auto unreadable = durable::WriteNew(failed, nullptr, 16);
    CHECK(!unreadable && unreadable.failed == durable::WriteStep::Write && unreadable.error != ERROR_SUCCESS);
    CHECK(!std::filesystem::exists(failed));
    // More than one write call can take is refused before anything is created.
    if constexpr (sizeof(std::size_t) > sizeof(DWORD)) {
        const auto oversize = durable::WriteNew(failed, first.data(), static_cast<std::size_t>(MAXDWORD) + 1);
        CHECK(!oversize && oversize.failed == durable::WriteStep::Write && oversize.error == ERROR_FILE_TOO_LARGE);
        CHECK(!std::filesystem::exists(failed));
    }

    // Settings keep their own missing-file policy: a missing folder is an error.
    std::string bytes, error;
    bool missing = false;
    CHECK(sf4e::netplay::json_file::ReadBytes(folder / L"absent.json", bytes, missing, error) && missing);
    CHECK(!sf4e::netplay::json_file::ReadBytes(folder / L"absent" / L"settings.json", bytes, missing, error) && !missing);
    CHECK(error == "Cannot read settings file.");
}

// The on/off preferences keep their stored keys and defaults: a settings file
// with each key true, false or missing loads as that value or the default, and
// saving it again writes the same keys to the netplay section.
static void BoolPreferenceRoundTrips(const Path& root) {
    using sf4e::netplay::PlayerPreferences;
    const struct { const char* key; bool PlayerPreferences::* member; bool fallback; } expected[] = {
        {"showMatchHud", &PlayerPreferences::showMatchHud, true}, {"matchHudRaised", &PlayerPreferences::matchHudRaised, false},
        {"readySound", &PlayerPreferences::readySound, true}, {"trainingAutoReady", &PlayerPreferences::trainingAutoReady, false},
        {"matchFrameMeter", &PlayerPreferences::matchFrameMeter, false}, {"backgroundPlay", &PlayerPreferences::backgroundPlay, false},
        {"recordWatched", &PlayerPreferences::recordWatched, true}, {"discordPresence", &PlayerPreferences::discordPresence, true},
        {"discordInvites", &PlayerPreferences::discordInvites, true}, {"sendProblemReports", &PlayerPreferences::sendProblemReports, false}};
    CHECK(std::size(sf4e::netplay::BoolPreferences) == std::size(expected));
    for (const auto& entry : expected) {
        const auto found = std::find_if(std::begin(sf4e::netplay::BoolPreferences), std::end(sf4e::netplay::BoolPreferences),
            [&](const auto& preference) { return std::string(preference.key) == entry.key; });
        CHECK(found != std::end(sf4e::netplay::BoolPreferences) && found->member == entry.member && PlayerPreferences().*entry.member == entry.fallback);
    }
    int folder = 0;
    // `saved` is the netplay section of a settings file; `want` gives each key's loaded value, -1 for its default.
    const auto check = [&](const Json& saved, const std::function<int(std::size_t)>& want) {
        const auto source = root / (L"bool-load-" + std::to_wstring(folder)), copy = root / (L"bool-save-" + std::to_wstring(folder++));
        CHECK(std::filesystem::create_directory(source) && std::filesystem::create_directory(copy));
        Write(source / L"settings.json", Json{{"schemaVersion", 1}, {"profile", Json::object()}, {"netplay", saved},
            {"overlay", Json::object()}, {"legacyLauncher", Json::object()}}.dump(4));
        std::string error;
        Json loaded;
        CHECK(SettingsStore(source.wstring()).LoadLauncher(loaded, error));
        PlayerPreferences value;
        sf4e::netplay::ReadBoolPreferences(loaded, value);
        for (std::size_t i = 0; i < std::size(expected); ++i)
            CHECK(value.*expected[i].member == (want(i) < 0 ? expected[i].fallback : want(i) == 1));
        Json written;
        sf4e::netplay::WriteBoolPreferences(value, written);
        CHECK(SettingsStore(copy.wstring()).SaveLauncher(written, error));
        const auto document = Json::parse(Read(copy / L"settings.json"));
        PlayerPreferences again;
        CHECK(SettingsStore(copy.wstring()).LoadLauncher(loaded, error));
        sf4e::netplay::ReadBoolPreferences(loaded, again);
        for (const auto& entry : expected) {
            CHECK(document["netplay"][entry.key] == value.*entry.member && !document["legacyLauncher"].contains(entry.key));
            CHECK(again.*entry.member == value.*entry.member);
        }
    };
    for (const bool on : {true, false}) {
        Json saved = Json::object();
        for (const auto& entry : expected) saved[entry.key] = on;
        check(saved, [&](std::size_t) { return on ? 1 : 0; });
    }
    check(Json::object(), [](std::size_t) { return -1; });
    // One key away from its default at a time, so each key reaches its own member.
    for (std::size_t changed = 0; changed < std::size(expected); ++changed)
        check({{expected[changed].key, !expected[changed].fallback}}, [&](std::size_t i) { return i == changed ? !expected[i].fallback : -1; });
    // A value of another type is not read as on or off.
    PlayerPreferences odd;
    bool threw = false;
    try { sf4e::netplay::ReadBoolPreferences({{"readySound", 1}}, odd); } catch (const nlohmann::json::exception&) { threw = true; }
    CHECK(threw);
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
    CHECK(!discordDefaults.sendProblemReports && !result.value("sendProblemReports",false));
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
    // The first crash report builds kept their opt-in as "crashReports". On, it
    // only prepared a preview, so it never turns on sending without asking:
    // only off carries over, and the old key goes either way.
    const auto oldOptIn=[&](const Json& value) {
        auto old=Json::parse(Read(path/L"settings.json"));
        old["netplay"].erase("sendProblemReports"); old["legacyLauncher"].erase("sendProblemReports"); old["netplay"]["crashReports"]=value;
        Write(path/L"settings.json",old.dump());
    };
    for (const bool value : {true, false}) {
        oldOptIn(value);
        CHECK(store.LoadLauncher(result,error) && !result.contains("crashReports"));
        CHECK(value ? !result.contains("sendProblemReports") : result.contains("sendProblemReports") && result["sendProblemReports"]==false);
        sf4e::netplay::PlayerPreferences loaded; sf4e::netplay::ReadBoolPreferences(result,loaded);
        CHECK(!loaded.sendProblemReports);
        CHECK(store.SaveLauncher({{"displayName","Migrated"}},error));
        const auto saved=Json::parse(Read(path/L"settings.json"));
        CHECK(!saved["netplay"].contains("crashReports") && !saved["legacyLauncher"].contains("crashReports"));
        CHECK(value ? !saved["netplay"].contains("sendProblemReports") : saved["netplay"]["sendProblemReports"]==false);
    }
    // An old key arriving through a save never turns sending on either.
    CHECK(store.SaveLauncher({{"crashReports",true}},error));
    CHECK(store.LoadLauncher(result,error) && !result.value("sendProblemReports",false) && !result.contains("crashReports"));
    // A value under the current key wins over an old one, either way round.
    CHECK(store.SaveLauncher({{"sendProblemReports",true}},error));
    CHECK(store.SaveLauncher({{"crashReports",false}},error));
    CHECK(store.LoadLauncher(result,error) && result.value("sendProblemReports",false) && !result.contains("crashReports"));
    {
        auto both=Json::parse(Read(path/L"settings.json"));
        both["netplay"]["sendProblemReports"]=false; both["netplay"]["crashReports"]=true;
        Write(path/L"settings.json",both.dump());
        CHECK(store.LoadLauncher(result,error) && !result.value("sendProblemReports",true) && !result.contains("crashReports"));
    }
    CHECK(!Json::parse(Read(path/L"settings.json"))["legacyLauncher"].contains("crashReports"));
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
    DurableFiles(root);
    BoolPreferenceRoundTrips(root);
    RemoveTempRoot(root);
    std::cout << "Settings migration, preservation, independent updates and atomic failure checks passed\n";
}
