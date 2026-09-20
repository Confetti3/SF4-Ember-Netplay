#include "SettingsStore.hxx"
#include "JsonFileStore.hxx"

#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <utility>

namespace sf4e { namespace netplay {
namespace {
using Json = nlohmann::json;
using Path = std::filesystem::path;
using json_file::Handle;
using json_file::Parse;
using json_file::PreserveBackup;
using json_file::Publish;
using json_file::ReadBytes;
constexpr int SchemaVersion = 1;
const char* ObsoleteKeys[] = {"relayRoomCode", "relayHostSecret", "relaySessionPort", "sessionPort", "ggpoPort", "hostPort", "joinPort", "relayHost", "relayPort", "brokerUrl", "useRelay", "netMode", "joinAddr", "roomCode", "hostSecret", "brokerBaseUrl", "lastJoinHost", "lastAdvertiseHost", "simpleUi", "defaultConnectMethod"};
void RetireLegacy(Json& document) {
    for (const char* key : ObsoleteKeys) document["legacyLauncher"].erase(key);
    auto& overlay = document["overlay"];
    for (const char* key : {"host", "join", "mainMenu", "windows", "debug"}) overlay.erase(key);
}
const char* NetplayKeys[] = { "inputDelay", "editionSelect", "roundCount", "roundTimeIntegral", "showMatchHud", "matchHudSize", "matchHudRaised", "discordPresence", "discordInvites", "interfaceScale", "roomDefaults" };

void MergeLauncher(Json& document, const Json& launcher) {
    // Only durable preferences enter the new store. Originals/backups retain
    // legacy data, but room credentials are never copied into active settings.
    auto legacy = launcher;
    for (const char* key : ObsoleteKeys) legacy.erase(key);
    for(const char* key:{"displayName","mainFighter","onlineRecord"})if(legacy.contains(key)) {
        document["profile"][key]=legacy[key];legacy.erase(key);
    }
    for (const char* key : NetplayKeys) {
        if (legacy.contains(key)) { document["netplay"][key] = legacy[key]; legacy.erase(key); }
    }
    document["legacyLauncher"].update(legacy);
}

bool ValidDocument(const Json& document, std::string& error) {
    if (!document.contains("schemaVersion") || !document["schemaVersion"].is_number_integer() ||
        document["schemaVersion"] != SchemaVersion) {
        error = "Unsupported settings version. The original has been preserved."; return false;
    }
    for (const char* key : { "profile", "netplay", "overlay", "legacyLauncher" }) {
        if (!document.contains(key) || !document[key].is_object()) {
            error = "Invalid settings section. The original has been preserved."; return false;
        }
    }
    return true;
}

bool LoadDocument(const Path& directory, Json& document, std::string& error) {
    std::string bytes;
    bool missing = false;
    if (!ReadBytes(directory / L"settings.json", bytes, missing, error)) return false;
    if (!missing) return Parse(bytes, document, error) && ValidDocument(document, error);

    Json launcher = Json::object(), overlay = Json::object();
    std::string launcherBytes, overlayBytes;
    bool noLauncher = false, noOverlay = false;
    if (!ReadBytes(directory / L"config.json", launcherBytes, noLauncher, error) ||
        !ReadBytes(directory / L"overlay_prefs.json", overlayBytes, noOverlay, error) ||
        (!noLauncher && !Parse(launcherBytes, launcher, error)) ||
        (!noOverlay && !Parse(overlayBytes, overlay, error))) return false;
    document = {{"schemaVersion", SchemaVersion}, {"profile", Json::object()},
        {"netplay", Json::object()}, {"overlay", overlay}, {"legacyLauncher", Json::object()}};
    MergeLauncher(document, launcher);
    // Import durable overlay-only profiles once, before retiring old panels.
    if (!document["profile"].contains("displayName")) {
        for (const char* section : {"host", "join"}) {
            if (overlay.contains(section) && overlay[section].is_object() && overlay[section].contains("name") && overlay[section]["name"].is_string() && !overlay[section]["name"].get<std::string>().empty()) {
                document["profile"]["displayName"] = overlay[section]["name"]; break;
            }
        }
    }
    if (overlay.contains("lobbySettings") && overlay["lobbySettings"].is_object()) {
        const auto& lobby = overlay["lobbySettings"];
        const int counts[] = {1,3,5,7,15,99}, times[] = {30,60,99,300,9999};
        if (!document["netplay"].contains("roundCount") && lobby.contains("roundCountIdx") && lobby["roundCountIdx"].is_number_integer()) {
            const int idx = lobby["roundCountIdx"]; if (idx >= 0 && idx < 6) document["netplay"]["roundCount"] = counts[idx];
        }
        if (!document["netplay"].contains("roundTimeIntegral") && lobby.contains("roundTimeIdx") && lobby["roundTimeIdx"].is_number_integer()) {
            const int idx = lobby["roundTimeIdx"]; if (idx >= 0 && idx < 5) document["netplay"]["roundTimeIntegral"] = times[idx];
        }
        if (!document["netplay"].contains("editionSelect") && lobby.contains("editionSelect") && lobby["editionSelect"].is_boolean()) document["netplay"]["editionSelect"] = lobby["editionSelect"].get<bool>() ? 1 : 0;
    }
    RetireLegacy(document);
    // Read and validate both sources before creating either backup. A retry
    // accepts only byte-identical backups; originals are never renamed/deleted.
    if ((!noLauncher && !PreserveBackup(directory / L"config.json.pre-v1.bak", launcherBytes, error)) ||
        (!noOverlay && !PreserveBackup(directory / L"overlay_prefs.json.pre-v1.bak", overlayBytes, error))) return false;
    return Publish(directory, L"settings.json", document, error);
}
} // namespace

SettingsStore::SettingsStore(std::wstring directory) : directory_(std::move(directory)) {}

std::wstring SettingsStore::DefaultDirectory() {
    PWSTR appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &appData))) return {};
    const auto path = Path(appData) / L"sf4e";
    CoTaskMemFree(appData);
    return path.wstring();
}

bool SettingsStore::Access(bool overlay, const Json* update, Json* output, std::string& error) const {
    error.clear();
    if (directory_.empty() || (update && !update->is_object())) {
        error = "Invalid settings location or values."; return false;
    }
    try {
        const Path directory(directory_);
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) { error = "Cannot create settings directory."; return false; }
        // Cross-process exclusion covers the whole read/merge/replace sequence.
        // Handle closure also releases this lock after a crash; no stale-lock timeout.
        Handle lock(CreateFileW((directory / L"settings.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (lock.value == INVALID_HANDLE_VALUE) { error = "Settings are busy or inaccessible. Try saving again."; return false; }
        Json document;
        if (!LoadDocument(directory, document, error)) return false;
        RetireLegacy(document);
        if (update) {
            if (overlay) document["overlay"].update(*update);
            else MergeLauncher(document, *update);
            RetireLegacy(document);
            return Publish(directory, L"settings.json", document, error);
        }
        if (overlay) *output = document["overlay"];
        else {
            *output = document["legacyLauncher"];
            output->update(document["netplay"]);
            output->update(document["profile"]);
        }
        return true;
    } catch (...) {
        error = "Settings could not be processed. The original has been preserved.";
        return false;
    }
}

bool SettingsStore::LoadLauncher(Json& value, std::string& error) const { return Access(false, nullptr, &value, error); }
bool SettingsStore::SaveLauncher(const Json& value, std::string& error) const { return Access(false, &value, nullptr, error); }
bool SettingsStore::LoadOverlay(Json& value, std::string& error) const { return Access(true, nullptr, &value, error); }
bool SettingsStore::SaveOverlay(const Json& value, std::string& error) const { return Access(true, &value, nullptr, error); }

} } // namespace sf4e::netplay
