#pragma once
#include <windows.h>

namespace sf4e { namespace platform {
// Whether this process runs with an administrator token. Updates install as
// the normal user, so only Normal may stage or run the Updater. Wine reports
// its user as elevated for every process, so it counts as Normal there;
// Unknown when Windows cannot say, which is not taken as Normal.
enum class Elevation { Normal, Elevated, Unknown };
inline Elevation ProcessElevation() {
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return Elevation::Normal;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return Elevation::Unknown;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL read = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    if (!read) return Elevation::Unknown;
    return elevation.TokenIsElevated ? Elevation::Elevated : Elevation::Normal;
}
} }
