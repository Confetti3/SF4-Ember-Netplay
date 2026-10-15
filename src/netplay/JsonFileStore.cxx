#include "JsonFileStore.hxx"
#include "../platform/DurableFile.hxx"

#include <atomic>
#include <stdexcept>

namespace sf4e { namespace netplay { namespace json_file {

using Path = std::filesystem::path;
using Json = nlohmann::json;

Handle::~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }

bool ReadBytes(const Path& path, std::string& bytes, bool& missing, std::string& error) {
    auto read = durable::ReadBounded(path, MaximumBytes);
    // Only a file absent from its folder is missing. A missing folder is an
    // error, so a lost drive never reads as a fresh profile.
    std::error_code ignored;
    missing = read.status == durable::ReadStatus::Missing && std::filesystem::is_directory(path.parent_path(), ignored);
    switch (read.status) {
    case durable::ReadStatus::Read: bytes.assign(read.bytes.begin(), read.bytes.end()); return true;
    case durable::ReadStatus::TooLarge: error = "Settings file exceeds the size limit."; return false;
    case durable::ReadStatus::CannotRead: error = "Cannot read complete settings file."; return false;
    default:
        if (!missing) error = "Cannot read settings file.";
        return missing;
    }
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

static const char* WriteError(const durable::WriteResult& written) {
    switch (written.failed) {
    case durable::WriteStep::Create: return "Cannot create settings file.";
    case durable::WriteStep::Write: return "Cannot flush settings file.";
    default: return "Cannot replace settings. The previous file has been preserved.";
    }
}

bool WriteNew(const Path& path, const std::string& bytes, std::string& error) {
    const auto written = durable::WriteNew(path, bytes.data(), bytes.size());
    if (!written) error = WriteError(written);
    return static_cast<bool>(written);
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
    for (int attempt = 0; attempt < 16; ++attempt) {
        const Path pending = directory / (filename + L".pending." + std::to_wstring(GetCurrentProcessId()) +
            L"." + std::to_wstring(++serial));
        const auto written = durable::PublishReplace(directory / filename, pending, bytes.data(), bytes.size());
        if (written) { error.clear(); return true; }
        if (written.failed != durable::WriteStep::Create ||
            (written.error != ERROR_FILE_EXISTS && written.error != ERROR_ALREADY_EXISTS)) {
            error = WriteError(written);
            return false;
        }
    }
    error = "Cannot reserve a settings save file.";
    return false;
}

} } } // namespace sf4e::netplay::json_file
