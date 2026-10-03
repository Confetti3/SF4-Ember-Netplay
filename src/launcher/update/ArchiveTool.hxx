#pragma once
#include <windows.h>
#include <string>

namespace sf4e { namespace launcher { namespace detail {
// The launcher is x86. System32 is redirected under WOW64, but Windows' tar
// is a native system executable. Never search PATH or the package for it.
inline std::wstring ArchiveToolPath() {
    BOOL wow64 = FALSE;
    if (!IsWow64Process(GetCurrentProcess(), &wow64)) return {};
    wchar_t directory[MAX_PATH] = {};
    const UINT length = wow64 ? GetWindowsDirectoryW(directory, MAX_PATH) : GetSystemDirectoryW(directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    return std::wstring(directory) + (wow64 ? L"\\Sysnative\\tar.exe" : L"\\tar.exe");
}
} } }
