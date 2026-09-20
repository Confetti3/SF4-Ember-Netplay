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

bool ValidDocument(const Json& value, std::string& preference) {
    if (!value.is_object() || !value.contains("schemaVersion") ||
        !value["schemaVersion"].is_number_integer() || value["schemaVersion"] != SchemaVersion ||
        !value.contains("language") || !value["language"].is_string()) return false;
    try { preference = value["language"].get<std::string>(); }
    catch (...) { return false; }
    return loc::ValidPreference(preference);
}

std::string LoadFrom(const Path& directory) noexcept {
    try {
        std::string bytes, error;
        bool missing = false;
        if (directory.empty() || !netplay::json_file::ReadBytes(directory / Filename, bytes, missing, error) || missing)
            return "auto";
        Json value;
        std::string preference;
        if (!netplay::json_file::Parse(bytes, value, error) || !ValidDocument(value, preference)) return "auto";
        return preference;
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

bool SaveTo(const Path& directory, std::string_view preference, std::string& error) noexcept {
    error.clear();
    if (directory.empty() || !loc::ValidPreference(preference)) {
        error = "Invalid language preference.";
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
        if (exists) {
            std::string bytes, readError, existingPreference;
            bool missing = false;
            Json old;
            valid = netplay::json_file::ReadBytes(current, bytes, missing, readError) && !missing &&
                netplay::json_file::Parse(bytes, old, readError) && ValidDocument(old, existingPreference);
            if (!valid && !PreserveInvalid(current, directory / L"ui-preferences.json.malformed.bak", error))
                return false;
        }
        const Json value = {{"schemaVersion", SchemaVersion}, {"language", std::string(preference)}};
        return netplay::json_file::Publish(directory, Filename, value, error);
    } catch (...) {
        error = "UI preferences could not be saved. The previous file has been preserved.";
        return false;
    }
}
}

std::string LoadLanguagePreference() {
    return LoadFrom(netplay::SettingsStore::DefaultDirectory());
}

bool SaveLanguagePreference(std::string_view preference, std::string& error) {
    return SaveTo(netplay::SettingsStore::DefaultDirectory(), preference, error);
}

namespace testing {
std::string LoadLanguagePreferenceFrom(const std::wstring& directory) { return LoadFrom(directory); }
bool SaveLanguagePreferenceTo(const std::wstring& directory, std::string_view preference, std::string& error) {
    return SaveTo(directory, preference, error);
}
}

} } // namespace sf4e::platform
