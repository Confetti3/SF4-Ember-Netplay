#include "SettingsStore.hxx"

#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace sf4e { namespace netplay {
namespace {
using Json = nlohmann::json;
using Path = std::filesystem::path;
constexpr DWORD MaximumBytes = 256 * 1024;
constexpr int SchemaVersion = 1;
const char* ObsoleteKeys[] = {"relayRoomCode", "relayHostSecret", "relaySessionPort", "sessionPort", "ggpoPort", "hostPort", "joinPort", "relayHost", "relayPort", "brokerUrl", "useRelay", "netMode", "joinAddr", "roomCode", "hostSecret", "brokerBaseUrl", "lastJoinHost", "lastAdvertiseHost", "simpleUi", "defaultConnectMethod"};
void RetireLegacy(Json& document) {
    for (const char* key : ObsoleteKeys) document["legacyLauncher"].erase(key);
    auto& overlay = document["overlay"];
    for (const char* key : {"host", "join", "mainMenu", "windows", "debug"}) overlay.erase(key);
}
const char* NetplayKeys[] = { "inputDelay", "editionSelect", "roundCount", "roundTimeIntegral", "showMatchHud", "matchHudSize", "matchHudRaised", "discordPresence", "discordInvites", "interfaceScale", "roomDefaults" };

struct Handle {
    HANDLE value;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

// The same size/depth limits apply to current settings and migration inputs.
// No parser exception is logged: it may contain a legacy room capability.
bool ReadBytes(const Path& path, std::string& bytes, bool& missing, std::string& error) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    missing = false;
    if (file.value == INVALID_HANDLE_VALUE) {
        missing = GetLastError() == ERROR_FILE_NOT_FOUND;
        if (!missing) error = "Cannot read settings file.";
        return missing;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 0 || size.QuadPart > MaximumBytes) {
        error = "Settings file exceeds the size limit."; return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) {
        error = "Cannot read complete settings file."; return false;
    }
    return true;
}

bool Parse(const std::string& bytes, Json& value, std::string& error) {
    try {
        value = Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 32) throw std::runtime_error("Settings nesting limit");
            return true;
        });
        if (value.is_object()) return true;
    } catch (...) {}
    error = "Settings file is malformed. The original has been preserved.";
    return false;
}

bool WriteNew(const Path& path, const std::string& bytes, std::string& error) {
    bool ok = false;
    {
        Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.value == INVALID_HANDLE_VALUE) {
            error = "Cannot create settings file."; return false;
        }
        DWORD written = 0;
        ok = WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size() && FlushFileBuffers(file.value);
    }
    if (!ok) {
        DeleteFileW(path.c_str());
        error = "Cannot flush settings file.";
    }
    return ok;
}

bool PreserveBackup(const Path& path, const std::string& original, std::string& error) {
    std::string backup;
    bool missing = false;
    if (!ReadBytes(path, backup, missing, error)) return false;
    if (missing) return WriteNew(path, original, error);
    if (backup == original) return true; // Retry after an interrupted migration.
    error = "A different migration backup exists. Settings were left unchanged.";
    return false;
}

bool Publish(const Path& directory, const Json& document, std::string& error) {
    std::string bytes;
    try { bytes = document.dump(2) + "\n"; }
    catch (...) { error = "Settings contain invalid text."; return false; }
    if (bytes.size() > MaximumBytes) { error = "Settings exceed the size limit."; return false; }
    static std::atomic<unsigned long> serial{0};
    Path pending;
    // CREATE_NEW also protects against a stale file left by a reused PID.
    for (int attempt = 0; attempt < 16; ++attempt) {
        pending = directory / (L"settings.json.pending." + std::to_wstring(GetCurrentProcessId()) +
            L"." + std::to_wstring(++serial));
        if (WriteNew(pending, bytes, error)) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        pending.clear();
    }
    if (pending.empty()) { error = "Cannot reserve a settings save file."; return false; }
    // Same-directory rename publishes one complete document. A failed replace
    // leaves the previous document in place, including when a reader holds it.
    if (!MoveFileExW(pending.c_str(), (directory / L"settings.json").c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(pending.c_str());
        error = "Cannot replace settings. The previous file has been preserved.";
        return false;
    }
    error.clear();
    return true;
}

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
    return Publish(directory, document, error);
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
            return Publish(directory, document, error);
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
