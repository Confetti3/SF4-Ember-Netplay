#include "StreamHostFiles.hxx"
#include "../platform/Utf8.hxx"
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include "StreamHostScripts.hxx"

namespace sf4e { namespace launcher {
namespace {
bool WriteBytes(const std::filesystem::path& file, const void* bytes, size_t size) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    return out.good();
}
}

std::wstring WriteStreamHostFiles() {
    PWSTR appData = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData) != S_OK) return {};
    const std::filesystem::path folder = std::filesystem::path(appData) / L"sf4e" / L"stream-host";
    CoTaskMemFree(appData);
    wchar_t module[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    const std::string emberFolder = platform::WideToUtf8(std::filesystem::path(module).parent_path().wstring());
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error || emberFolder.empty()) return {};
    // Each array ends in a 0 byte the file does not have.
    const bool written =
        WriteBytes(folder / L"EmberStreamHost.cmd", streamHostCmd, sizeof(streamHostCmd) - 1) &&
        WriteBytes(folder / L"EmberStreamHost.ps1", streamHostScript, sizeof(streamHostScript) - 1) &&
        WriteBytes(folder / L"README.md", streamHostReadme, sizeof(streamHostReadme) - 1) &&
        WriteBytes(folder / L"EmberFolder.txt", emberFolder.data(), emberFolder.size());
    return written ? folder.wstring() : std::wstring();
}
} }
