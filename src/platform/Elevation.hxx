#pragma once
#include <windows.h>

namespace sf4e { namespace platform {
// Whether a process runs with an administrator token. Updates install as the
// normal user, so only Normal may stage or run the Updater. Wine reports its
// user as elevated for every process, so it counts as Normal there; Unknown
// when Windows cannot say, which is not taken as Normal.
enum class Elevation { Normal, Elevated, Unknown };
inline Elevation ProcessElevation(HANDLE process = GetCurrentProcess()) {
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return Elevation::Normal;
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return Elevation::Unknown;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL read = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    if (!read) return Elevation::Unknown;
    return elevation.TokenIsElevated ? Elevation::Elevated : Elevation::Normal;
}
// The running Steam client's elevation, found the way steam_api finds the
// client: the process id Steam writes under HKCU. A game started by a normal
// process cannot reach a Steam that runs as administrator, and its
// SteamAPI_Init fails. Unknown when Steam is not running or cannot be read.
inline Elevation SteamElevation() {
    DWORD pid = 0, size = sizeof(pid);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"pid",
            RRF_RT_REG_DWORD, nullptr, &pid, &size) != ERROR_SUCCESS || !pid) return Elevation::Unknown;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return Elevation::Unknown;
    // The id outlives a Steam that did not exit cleanly; only steam.exe counts.
    wchar_t image[MAX_PATH] = {};
    DWORD length = MAX_PATH;
    const wchar_t* name = nullptr;
    if (QueryFullProcessImageNameW(process, 0, image, &length))
        for (name = image + length; name > image && name[-1] != L'\\' && name[-1] != L'/'; --name) {}
    const Elevation result = name && lstrcmpiW(name, L"steam.exe") == 0 ? ProcessElevation(process) : Elevation::Unknown;
    CloseHandle(process);
    return result;
}
} }
