#pragma once
#include <windows.h>
#include <string>

namespace sf4e { namespace install {
// A private test package must not replace its local fixes with a public update.
// Only the private packager ships this marker. Recovery/uninstall still work.
inline bool PrivateTestPackage(const wchar_t* directory) {
    if (!directory || !*directory) return false;
    const std::wstring path = std::wstring(directory) + L"\\PRIVATE_TEST_BUILD.txt";
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
} }
