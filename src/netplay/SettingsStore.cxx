#include "SettingsStore.hxx"
#include "JsonFileStore.hxx"
#include "BoolPreferences.hxx"
#include "../common/ProductIdentity.hxx"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <restartmanager.h>
#include <filesystem>
#include <utility>
#include <vector>

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
// sendProblemReports is Send problem reports from before reports carried the
// small dump; Stable 1.5.0 retires it (BoolPreferences.hxx).
const char* ObsoleteKeys[] = {"sendProblemReports", "relayRoomCode", "relayHostSecret", "relaySessionPort", "sessionPort", "ggpoPort", "hostPort", "joinPort", "relayHost", "relayPort", "brokerUrl", "useRelay", "netMode", "joinAddr", "roomCode", "hostSecret", "brokerBaseUrl", "lastJoinHost", "lastAdvertiseHost", "simpleUi", "defaultConnectMethod"};
void RetireLegacy(Json& document) {
    for (const char* key : ObsoleteKeys) { document["legacyLauncher"].erase(key); document["netplay"].erase(key); }
    auto& overlay = document["overlay"];
    for (const char* key : {"host", "join", "mainMenu", "windows", "debug"}) overlay.erase(key);
}
// With the on/off preferences (BoolPreferences), the keys kept in the netplay section.
const char* NetplayKeys[] = { "inputDelay", "buttonPrompts", "controllerMappings", "buttonLayouts", "autoInputDelay", "autoDelayStyle", "editionSelect", "roundCount", "roundTimeIntegral", "matchHudSize", "matchHudAnchor", "pictureSpot", "pictureMode", "wornTitle", "wornIcon", "gameThemeMenus", "gameThemeBattleHud", "roomColumn", "roomSplit", "matchHudLayout", "matchHudNameOffset", "readySoundVolume", "interfaceScale", "roomDefaults", "country", "area", "speech", "roomFilter", "cardDevice", "cardShowLink", "cardLooking", "cardPins" };

void MergeLauncher(Json& document, const Json& launcher) {
    // Only durable preferences enter the new store. Originals/backups retain
    // legacy data, but room credentials are never copied into active settings.
    auto legacy = launcher;
    for (const char* key : ObsoleteKeys) legacy.erase(key);
    for(const char* key:{"displayName","mainFighter","mainChosen","onlineRecord"})if(legacy.contains(key)) {
        document["profile"][key]=legacy[key];legacy.erase(key);
    }
    const auto netplay = [&](const char* key) {
        if (legacy.contains(key)) { document["netplay"][key] = legacy[key]; legacy.erase(key); }
    };
    for (const char* key : NetplayKeys) netplay(key);
    for (const auto& preference : BoolPreferences) netplay(preference.key);
    document["legacyLauncher"].update(legacy);
    // The card's own switch for the area was folded into the area choice (ReadAreaPreference reads an old one as
    // Hidden); once a save carries the choice, the old key must not outvote it.
    if (launcher.contains("area")) { document["netplay"].erase("cardShowRegion"); document["legacyLauncher"].erase("cardShowRegion"); }
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

// `why` says what kind of failure a false is.
bool LoadDocument(const Path& directory, Json& document, std::string& error, ReadFailure& why) {
    why = ReadFailure::Unreadable;
    std::string bytes;
    bool missing = false;
    if (!ReadBytes(directory / L"settings.json", bytes, missing, error)) return false;
    if (!missing) {
        if (!Parse(bytes, document, error)) { why = ReadFailure::Damaged; return false; }
        if (!ValidDocument(document, error)) { why = ReadFailure::Unsupported; return false; }
        return true;
    }

    Json launcher = Json::object(), overlay = Json::object();
    std::string launcherBytes, overlayBytes;
    bool noLauncher = false, noOverlay = false;
    if (!ReadBytes(directory / L"config.json", launcherBytes, noLauncher, error) ||
        !ReadBytes(directory / L"overlay_prefs.json", overlayBytes, noOverlay, error)) return false;
    // The older files are never set aside: the migration waits for them to be fixed.
    if ((!noLauncher && !Parse(launcherBytes, launcher, error)) ||
        (!noOverlay && !Parse(overlayBytes, overlay, error))) { why = ReadFailure::Unsupported; return false; }
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
    if (ember::Product().settingsBesideModule) {
        // The module this code is linked into, wherever the game loaded it from.
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&SettingsStore::DefaultDirectory), &module)) return {};
        wchar_t file[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(module, file, MAX_PATH);
        if (!length || length >= MAX_PATH) return {};
        return (Path(file).parent_path() / ember::Product().appDataFolder).wstring();
    }
    PWSTR appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &appData))) return {};
    const auto path = Path(appData) / ember::Product().appDataFolder;
    CoTaskMemFree(appData);
    return path.wstring();
}

bool SettingsStore::Access(bool overlay, const Json* update, Json* output, std::string& error, ReadFailure* why) const {
    error.clear();
    ReadFailure ignored = ReadFailure::None;
    ReadFailure& cause = why ? *why : ignored;
    cause = ReadFailure::Unreadable;
    if (directory_.empty() || (update && !update->is_object())) {
        error = "Invalid settings location or values."; return false;
    }
    try {
        const Path directory(directory_);
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) { error = "Cannot create settings directory (system error " + std::to_string(ec.value()) + ")."; return false; }
        // Cross-process exclusion covers the whole read/merge/replace sequence.
        // Handle closure also releases this lock after a crash; no stale-lock timeout.
        Handle lock(CreateFileW((directory / L"settings.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (lock.value == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            error = "Settings are busy or inaccessible (system error " + std::to_string(code) + "). Try saving again.";
            cause = code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION ? ReadFailure::Busy : ReadFailure::Unreadable;
            return false;
        }
        Json document;
        if (!LoadDocument(directory, document, error, cause)) return false;
        cause = ReadFailure::Unreadable;
        RetireLegacy(document);
        if (update) {
            if (overlay) document["overlay"].update(*update);
            else MergeLauncher(document, *update);
            RetireLegacy(document);
            if (!Publish(directory, L"settings.json", document, error)) return false;
            cause = ReadFailure::None;
            return true;
        }
        if (overlay) {
            *output = document["overlay"];
            // Player autosave owns this setting once edited. Preserve P1's
            // overlay startup contract without a second write or stale overwrite.
            if (document["netplay"].contains("controllerLayer")) (*output)["controllerLayer"] = document["netplay"]["controllerLayer"];
        }
        else {
            *output = document["legacyLauncher"];
            output->update(document["netplay"]);
            output->update(document["profile"]);
            if (!output->contains("controllerLayer") && document["overlay"].contains("controllerLayer"))
                (*output)["controllerLayer"] = document["overlay"]["controllerLayer"];
        }
        cause = ReadFailure::None;
        return true;
    } catch (...) {
        error = "Settings could not be processed. The original has been preserved.";
        return false;
    }
}

bool SettingsStore::LoadLauncher(Json& value, std::string& error, ReadFailure* why) const { return Access(false, nullptr, &value, error, why); }
bool SettingsStore::SaveLauncher(const Json& value, std::string& error, ReadFailure* why) const { return Access(false, &value, nullptr, error, why); }
bool SettingsStore::LoadOverlay(Json& value, std::string& error, ReadFailure* why) const { return Access(true, nullptr, &value, error, why); }
bool SettingsStore::SaveOverlay(const Json& value, std::string& error, ReadFailure* why) const { return Access(true, &value, nullptr, error, why); }

bool SettingsStore::KeepDamagedAside(std::wstring& keptAs, std::string& error) const {
    error.clear();
    keptAs.clear();
    if (directory_.empty()) { error = "Invalid settings location."; return false; }
    try {
        const Path directory(directory_);
        Handle lock(CreateFileW((directory / L"settings.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (lock.value == INVALID_HANDLE_VALUE) {
            error = "Settings are busy or inaccessible (system error " + std::to_string(GetLastError()) + ").";
            return false;
        }
        const Path file = directory / L"settings.json";
        std::string bytes;
        bool missing = false;
        if (!ReadBytes(file, bytes, missing, error)) return false;
        Json parsed;
        std::string parseError;
        if (missing || Parse(bytes, parsed, parseError)) { error = "The settings file is not damaged."; return false; }
        SYSTEMTIME now = {};
        GetLocalTime(&now);
        wchar_t stamp[32] = {};
        swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
            static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond));
        const std::wstring base = std::wstring(L"settings.json.damaged-") + stamp;
        Path target = directory / base;
        std::error_code ec;
        for (int n = 1; n < 100 && std::filesystem::exists(target, ec); ++n) target = directory / (base + L"-" + std::to_wstring(n));
        if (!MoveFileExW(file.c_str(), target.c_str(), 0)) {
            error = "Cannot set the damaged settings aside (system error " + std::to_string(GetLastError()) + ").";
            return false;
        }
        const Json fresh = {{"schemaVersion", SchemaVersion}, {"profile", Json::object()}, {"netplay", Json::object()},
            {"overlay", Json::object()}, {"legacyLauncher", Json::object()}};
        if (!Publish(directory, L"settings.json", fresh, error)) {
            // Without a file the next read would migrate the older files again: put the damaged one back.
            MoveFileExW(target.c_str(), file.c_str(), 0);
            return false;
        }
        keptAs = target.filename().wstring();
        return true;
    } catch (...) {
        error = "The damaged settings could not be set aside.";
        return false;
    }
}

namespace {
std::string Utf8(const wchar_t* text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], bytes, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(bytes - 1));
    return out;
}
}

std::string SettingsStore::LockHolder() const {
    // Windows' Restart Manager lists the processes with a file open. It is
    // loaded on demand: it is only ever asked on the way to a log line.
    if (directory_.empty()) return {};
    HMODULE module = LoadLibraryW(L"rstrtmgr.dll");
    if (!module) return {};
    using StartFunction = DWORD (WINAPI*)(DWORD*, DWORD, WCHAR*);
    using RegisterFunction = DWORD (WINAPI*)(DWORD, UINT, LPCWSTR*, UINT, RM_UNIQUE_PROCESS*, UINT, LPCWSTR*);
    using ListFunction = DWORD (WINAPI*)(DWORD, UINT*, UINT*, RM_PROCESS_INFO*, LPDWORD);
    using EndFunction = DWORD (WINAPI*)(DWORD);
    const auto start = reinterpret_cast<StartFunction>(reinterpret_cast<void*>(GetProcAddress(module, "RmStartSession")));
    const auto add = reinterpret_cast<RegisterFunction>(reinterpret_cast<void*>(GetProcAddress(module, "RmRegisterResources")));
    const auto list = reinterpret_cast<ListFunction>(reinterpret_cast<void*>(GetProcAddress(module, "RmGetList")));
    const auto end = reinterpret_cast<EndFunction>(reinterpret_cast<void*>(GetProcAddress(module, "RmEndSession")));
    std::string holders;
    if (start && add && list && end) {
        DWORD session = 0;
        WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
        if (start(&session, 0, key) == ERROR_SUCCESS) {
            const std::wstring file = (Path(directory_) / L"settings.lock").wstring();
            LPCWSTR files[] = {file.c_str()};
            UINT needed = 0, count = 0;
            DWORD reasons = 0;
            if (add(session, 1, files, 0, nullptr, 0, nullptr) == ERROR_SUCCESS &&
                list(session, &needed, &count, nullptr, &reasons) == ERROR_MORE_DATA && needed > 0 && needed < 64) {
                std::vector<RM_PROCESS_INFO> found(needed);
                count = needed;
                if (list(session, &needed, &count, found.data(), &reasons) == ERROR_SUCCESS) {
                    for (UINT i = 0; i < count; ++i) {
                        if (!holders.empty()) holders += ", ";
                        holders += Utf8(found[i].strAppName) + " (process " + std::to_string(found[i].Process.dwProcessId) + ")";
                    }
                }
            }
            end(session);
        }
    }
    FreeLibrary(module);
    return holders;
}

} } // namespace sf4e::netplay
