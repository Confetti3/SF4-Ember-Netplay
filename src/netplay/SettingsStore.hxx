#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {

// Shared launcher/overlay storage. These are filesystem operations: call from
// startup or a settings worker, never from a networking/simulation callback.
// A busy writer fails without waiting; the caller may retain and retry a save.
class SettingsStore {
public:
    explicit SettingsStore(std::wstring directory);
    static std::wstring DefaultDirectory();

    bool LoadLauncher(nlohmann::json& settings, std::string& error) const;
    bool SaveLauncher(const nlohmann::json& settings, std::string& error) const;
    bool LoadOverlay(nlohmann::json& settings, std::string& error) const;
    bool SaveOverlay(const nlohmann::json& settings, std::string& error) const;

private:
    bool Access(bool overlay, const nlohmann::json* update,
        nlohmann::json* output, std::string& error) const;
    std::wstring directory_;
};

} } // namespace sf4e::netplay
