#include "GameDisplaySettings.hxx"

#define NOMINMAX
#include <windows.h>
#include <ShlObj.h>
#include <fstream>
#include <iterator>
#include <string>

namespace sf4e { namespace platform {

const gameconfig::DisplaySettings& GameDisplaySettings() {
    static const auto settings = [] {
        std::string bytes;
        PWSTR documents = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents) == S_OK) {
            std::ifstream file(std::wstring(documents) + L"\\CAPCOM\\SUPERSTREETFIGHTERIV\\config.ini", std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(file), {});
        }
        CoTaskMemFree(documents);
        return gameconfig::ParseDisplaySettings(bytes);
    }();
    return settings;
}

} } // namespace sf4e::platform
