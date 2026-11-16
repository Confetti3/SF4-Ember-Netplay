#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {

// Why the settings could not be read, for the caller that decides whether to
// try again (SavedRead): a busy lock or a file that cannot be read for now may
// come right; a damaged file or one this build cannot take does not.
enum class ReadFailure {
    None,
    Busy,        // another process holds the settings lock
    Unreadable,  // the folder or the file could not be read or written
    Damaged,     // the file is not valid JSON
    Unsupported, // valid, but of a version or shape this build does not take
};

// Shared launcher/overlay storage. These are filesystem operations: call from
// startup or a settings worker, never from a networking/simulation callback.
// A busy writer fails without waiting; the caller may retain and retry a save.
class SettingsStore {
public:
    explicit SettingsStore(std::wstring directory);
    static std::wstring DefaultDirectory();

    // `why`, when given, says why a call failed (None when it did not).
    bool LoadLauncher(nlohmann::json& settings, std::string& error, ReadFailure* why = nullptr) const;
    bool SaveLauncher(const nlohmann::json& settings, std::string& error, ReadFailure* why = nullptr) const;
    bool LoadOverlay(nlohmann::json& settings, std::string& error, ReadFailure* why = nullptr) const;
    bool SaveOverlay(const nlohmann::json& settings, std::string& error, ReadFailure* why = nullptr) const;

    // A settings file that is not valid JSON, kept aside beside it under
    // "settings.json.damaged-<date>-<time>" (`keptAs` is the new name), and a
    // fresh empty one written in its place. A save never does this on its own:
    // it fails and leaves the file as it is. Fails, changing nothing, when the
    // file is missing, unreadable or valid.
    bool KeepDamagedAside(std::wstring& keptAs, std::string& error) const;
    // Who holds the settings lock, by name and process number, when Windows can
    // say; empty when it cannot. For the log, never for a decision.
    std::string LockHolder() const;

private:
    bool Access(bool overlay, const nlohmann::json* update,
        nlohmann::json* output, std::string& error, ReadFailure* why) const;
    std::wstring directory_;
};

} } // namespace sf4e::netplay
