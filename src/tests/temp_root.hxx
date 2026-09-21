#pragma once

// A per-run directory name under the system temp folder for tests that write
// files. Not in test_support.hxx: this needs C++17 and Windows.
#include <windows.h>
#include <cstdlib>
#include <filesystem>
#include <string>

// The path only; the caller decides whether and how to create it.
inline std::filesystem::path MakeTempRoot(const wchar_t* prefix) {
    return std::filesystem::temp_directory_path() /
        (std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
}

// Never delete anything that is not directly under the temp folder.
inline void RemoveTempRoot(const std::filesystem::path& root) {
    if (!std::filesystem::equivalent(root.parent_path(), std::filesystem::temp_directory_path())) std::exit(1);
    std::filesystem::remove_all(root);
}
