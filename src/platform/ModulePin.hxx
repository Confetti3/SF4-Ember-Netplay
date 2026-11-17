#pragma once
#include <windows.h>

// Keeps a system DLL loaded for the rest of the process: a pinned module is
// never unloaded, whatever its reference count does.
namespace sf4e { namespace platform {

// Loads the system DLL `systemDll` (a file name in the system folder, not a
// path) and pins that module. Only the system folder is searched, so a copy
// beside the game or in the package folder is never the one pinned. Returns the
// module, or null with the error from the failing call kept for GetLastError.
inline HMODULE PinSystemModule(const wchar_t* systemDll) {
    HMODULE loaded = LoadLibraryExW(systemDll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loaded) return nullptr;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(loaded), &pinned)) {
        const DWORD error = GetLastError();
        FreeLibrary(loaded);
        SetLastError(error);
        return nullptr;
    }
    FreeLibrary(loaded); // the pin holds the module; this drops only the reference taken above
    return pinned;
}

} }
