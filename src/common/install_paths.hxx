#pragma once

#include <windows.h>

namespace sf4e {
namespace install {

// Subfolder beside Launcher.exe that holds runtime DLLs (Qt, Steam, Sidecar, etc.).
extern const wchar_t kDllDirName[];

// Call first in wWinMain before any delay-loaded DLL is used.
bool ConfigureDllSearch();

bool GetInstallRoot(wchar_t* outDir, int outDirChars);
bool GetPackageDllDirectory(wchar_t* outDir, int outDirChars);
bool UsesDllSubdirectory();

// Resolves rel under install root; when dll/ exists, prefers dll\rel then root\rel.
bool ResolveInstallFile(const wchar_t* relative, wchar_t* outPath, int outPathChars);

} // namespace install
} // namespace sf4e
