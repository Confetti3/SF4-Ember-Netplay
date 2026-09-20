#include "JsonFileStore.hxx"

#include <atomic>
#include <stdexcept>

namespace sf4e { namespace netplay { namespace json_file {

using Path = std::filesystem::path;
using Json = nlohmann::json;

Handle::~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }

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
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 0 ||
        size.QuadPart > static_cast<LONGLONG>(MaximumBytes)) {
        error = "Settings file exceeds the size limit.";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
        read != bytes.size()) {
        error = "Cannot read complete settings file.";
        return false;
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
            error = "Cannot create settings file.";
            return false;
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
    if (backup == original) return true;
    error = "A different migration backup exists. Settings were left unchanged.";
    return false;
}

bool Publish(const Path& directory, const std::wstring& filename,
             const Json& document, std::string& error) {
    std::string bytes;
    try { bytes = document.dump(2) + "\n"; }
    catch (...) { error = "Settings contain invalid text."; return false; }
    if (bytes.size() > MaximumBytes) {
        error = "Settings exceed the size limit.";
        return false;
    }
    static std::atomic<unsigned long> serial{0};
    Path pending;
    for (int attempt = 0; attempt < 16; ++attempt) {
        pending = directory / (filename + L".pending." + std::to_wstring(GetCurrentProcessId()) +
            L"." + std::to_wstring(++serial));
        if (WriteNew(pending, bytes, error)) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        pending.clear();
    }
    if (pending.empty()) {
        error = "Cannot reserve a settings save file.";
        return false;
    }
    if (!MoveFileExW(pending.c_str(), (directory / filename).c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(pending.c_str());
        error = "Cannot replace settings. The previous file has been preserved.";
        return false;
    }
    error.clear();
    return true;
}

} } } // namespace sf4e::netplay::json_file
