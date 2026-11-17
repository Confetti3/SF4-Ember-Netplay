#include "../platform/ModulePin.hxx"
#include "test_support.hxx"
#include <cwchar>

// msimg32.dll: a small system DLL nothing in this process loads on its own.
static const wchar_t* kDll = L"msimg32.dll";

int main() {
    CHECK_MSG(GetModuleHandleW(kDll) == nullptr, "%s", "msimg32.dll was already loaded by the test process");

    // Without the pin, dropping the last reference unloads the module.
    HMODULE plain = LoadLibraryW(kDll);
    CHECK(plain != nullptr);
    FreeLibrary(plain);
    CHECK(GetModuleHandleW(kDll) == nullptr);

    // With the pin, extra releases (what COM's free-unused-libraries sweep does) leave it mapped.
    HMODULE pinned = sf4e::platform::PinSystemModule(kDll);
    CHECK(pinned != nullptr);
    HMODULE again = LoadLibraryW(kDll);
    CHECK(again == pinned);
    FreeLibrary(again);
    FreeLibrary(again);
    FreeLibrary(pinned);
    CHECK(GetModuleHandleW(kDll) == pinned);

    // The pinned module is the one in the system folder.
    wchar_t path[MAX_PATH], system[MAX_PATH];
    CHECK(GetModuleFileNameW(pinned, path, MAX_PATH) > 0 && GetSystemDirectoryW(system, MAX_PATH) > 0);
    CHECK(_wcsnicmp(path, system, wcslen(system)) == 0);

    // A module that does not exist is reported, not pinned.
    CHECK(sf4e::platform::PinSystemModule(L"sf4e-no-such-module.dll") == nullptr);
    std::printf("A pinned module stays loaded.\n");
    return 0;
}
