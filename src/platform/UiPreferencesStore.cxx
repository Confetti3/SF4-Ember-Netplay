#include "UiPreferencesStore.hxx"

#include "../common/Localization.hxx"
#include "../netplay/JsonFileStore.hxx"
#include "../netplay/SettingsStore.hxx"
#include <filesystem>
#include <fstream>
#include <cstring>

namespace sf4e { namespace platform {
namespace {
using Json = nlohmann::json;
using Path = std::filesystem::path;
using netplay::json_file::Handle;

constexpr int SchemaVersion = 1;
const wchar_t* Filename = L"ui-preferences.json";
const char* HideGameSettingsCard = "hideGameSettingsCard";

bool ValidDocument(const Json& value, std::string& preference) {
    if (!value.is_object() || !value.contains("schemaVersion") ||
        !value["schemaVersion"].is_number_integer() || value["schemaVersion"] != SchemaVersion ||
        !value.contains("language") || !value["language"].is_string()) return false;
    try { preference = value["language"].get<std::string>(); }
    catch (...) { return false; }
    return loc::ValidPreference(preference);
}

// The stored document, or null when the file is absent or not a valid one.
// Every read goes through here so the fields cannot disagree about validity.
Json LoadValid(const Path& directory) noexcept {
    try {
        std::string bytes, error, preference;
        bool missing = false;
        Json value;
        if (directory.empty() || !netplay::json_file::ReadBytes(directory / Filename, bytes, missing, error) || missing ||
            !netplay::json_file::Parse(bytes, value, error) || !ValidDocument(value, preference)) return Json();
        return value;
    } catch (...) { return Json(); }
}

std::string LoadFrom(const Path& directory) noexcept {
    try {
        const Json value = LoadValid(directory);
        return value.is_null() ? "auto" : value["language"].get<std::string>();
    } catch (...) { return "auto"; }
}

bool SameFile(const Path& left, const Path& right) {
    std::error_code ec;
    if (std::filesystem::file_size(left, ec) != std::filesystem::file_size(right, ec) || ec) return false;
    std::ifstream a(left, std::ios::binary), b(right, std::ios::binary);
    if (!a || !b) return false;
    char aa[16384], bb[16384];
    do {
        a.read(aa, sizeof(aa)); b.read(bb, sizeof(bb));
        if (a.gcount() != b.gcount() || std::memcmp(aa, bb, static_cast<std::size_t>(a.gcount())) != 0) return false;
    } while (a.gcount());
    return a.eof() && b.eof();
}

bool PreserveInvalid(const Path& source, const Path& backup, std::string& error) {
    if (CopyFileW(source.c_str(), backup.c_str(), TRUE)) return true;
    const auto code = GetLastError();
    if ((code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) && SameFile(source, backup)) return true;
    error = code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS ?
        "A different UI-preferences backup exists. Preferences were left unchanged." :
        "The invalid UI-preferences file could not be backed up.";
    return false;
}

// Hidden is absent from files written before the card existed.
bool HiddenFrom(const Path& directory) noexcept {
    try {
        const Json value = LoadValid(directory);
        return value.is_object() && value.value(HideGameSettingsCard, false);
    } catch (...) { return false; }
}

// Merges the patch into a valid file and keeps the rest of it, so the language
// and the hidden card cannot overwrite each other. Field contracts belong to
// the callers below; this only writes what it is given.
bool SaveTo(const Path& directory, const Json& patch, std::string& error) noexcept {
    error.clear();
    if (directory.empty()) {
        error = "No settings directory.";
        return false;
    }
    try {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) { error = "Cannot create settings directory."; return false; }
        Handle lock(CreateFileW((directory / L"ui-preferences.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (lock.value == INVALID_HANDLE_VALUE) {
            error = "UI preferences are busy or inaccessible. Try saving again.";
            return false;
        }
        const Path current = directory / Filename;
        const bool exists = std::filesystem::exists(current, ec) && !ec;
        bool valid = false;
        Json old;
        if (exists) {
            std::string bytes, readError, existingPreference;
            bool missing = false;
            valid = netplay::json_file::ReadBytes(current, bytes, missing, readError) && !missing &&
                netplay::json_file::Parse(bytes, old, readError) && ValidDocument(old, existingPreference);
            if (!valid && !PreserveInvalid(current, directory / L"ui-preferences.json.malformed.bak", error))
                return false;
        }
        Json value = valid ? old : Json{{"schemaVersion", SchemaVersion}, {"language", "auto"}};
        value.update(patch);
        return netplay::json_file::Publish(directory, Filename, value, error);
    } catch (...) {
        error = "UI preferences could not be saved. The previous file has been preserved.";
        return false;
    }
}

bool SaveLanguageTo(const Path& directory, std::string_view preference, std::string& error) noexcept {
    error.clear();
    if (!loc::ValidPreference(preference)) {
        error = "Invalid language preference.";
        return false;
    }
    return SaveTo(directory, {{"language", std::string(preference)}}, error);
}

bool HideCardIn(const Path& directory, std::string& error) noexcept {
    return SaveTo(directory, {{HideGameSettingsCard, true}}, error);
}
}

std::string LoadLanguagePreference() {
    return LoadFrom(netplay::SettingsStore::DefaultDirectory());
}

bool SaveLanguagePreference(std::string_view preference, std::string& error) {
    return SaveLanguageTo(netplay::SettingsStore::DefaultDirectory(), preference, error);
}

bool GameSettingsCardHidden() { return HiddenFrom(netplay::SettingsStore::DefaultDirectory()); }

bool HideGameSettingsCardForever(std::string& error) {
    return HideCardIn(netplay::SettingsStore::DefaultDirectory(), error);
}

namespace testing {
std::string LoadLanguagePreferenceFrom(const std::wstring& directory) { return LoadFrom(directory); }
bool SaveLanguagePreferenceTo(const std::wstring& directory, std::string_view preference, std::string& error) {
    return SaveLanguageTo(directory, preference, error);
}
bool GameSettingsCardHiddenIn(const std::wstring& directory) { return HiddenFrom(directory); }
bool HideGameSettingsCardIn(const std::wstring& directory, std::string& error) {
    return HideCardIn(directory, error);
}
}

} } // namespace sf4e::platform
