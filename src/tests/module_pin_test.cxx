#include "../platform/ModulePin.hxx"
#include "test_support.hxx"
#include <objbase.h>
#include <cwchar>
#include <cstdlib>
#include <string>

// msimg32.dll: a small system DLL nothing in this process loads on its own.
static const wchar_t* kDll = L"msimg32.dll";

// COM's free-unused-libraries sweep (what unloads a COM server whose DllCanUnloadNow
// says S_OK) with no unload delay, run until `module` is gone or `rounds` ran out.
static void Sweep(int rounds, HMODULE module) {
    for (int i = 0; i < rounds && GetModuleHandleW(L"ComProbeServer.dll") == module; ++i)
        CoFreeUnusedLibrariesEx(0, 0);
}

static std::wstring ProbePath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    return dir.substr(0, dir.find_last_of(L'\\') + 1) + L"ComProbeServer.dll";
}

// Makes the probe a registered in-process server for this user, as XAudio2_7.dll is
// one for the game. The class id is new on every run, and atexit removes the key
// even when a CHECK ends the process early.
static CLSID g_clsid;
static std::wstring g_key;
static void Unregister() { RegDeleteTreeW(HKEY_CURRENT_USER, g_key.c_str()); }
static void Register() {
    CHECK(SUCCEEDED(CoCreateGuid(&g_clsid)));
    wchar_t id[64];
    CHECK(StringFromGUID2(g_clsid, id, 64) > 0);
    g_key = std::wstring(L"Software\\Classes\\CLSID\\") + id;
    std::atexit(Unregister);
    const std::wstring path = ProbePath();
    const std::wstring server = g_key + L"\\InprocServer32";
    CHECK(RegSetKeyValueW(HKEY_CURRENT_USER, server.c_str(), nullptr, REG_SZ,
        path.c_str(), static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
    const wchar_t both[] = L"Both";
    CHECK(RegSetKeyValueW(HKEY_CURRENT_USER, server.c_str(), L"ThreadingModel", REG_SZ, both, sizeof(both)) == ERROR_SUCCESS);
}

// Creates and releases an object of the probe, so COM loads the DLL and tracks it for the sweep.
static HMODULE LoadForCom() {
    IUnknown* object = nullptr;
    CHECK(SUCCEEDED(CoCreateInstance(g_clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown, reinterpret_cast<void**>(&object))));
    HMODULE module = GetModuleHandleW(L"ComProbeServer.dll");
    CHECK(module != nullptr);
    object->Release();
    return module;
}

static void SweepUnloadsAnUnpinnedServer() {
    HMODULE module = LoadForCom();
    Sweep(10, module);
    CHECK_MSG(GetModuleHandleW(L"ComProbeServer.dll") == nullptr, "%s", "the sweep did not unload the probe, so this test shows nothing");
}

static void SweepLeavesAPinnedServerAndItsThread() {
    HMODULE module = LoadForCom();
    typedef bool (*Start)(); typedef long (*Beats)(); typedef void (*Stop)();
    Start start = reinterpret_cast<Start>(GetProcAddress(module, "ProbeStart"));
    Beats beats = reinterpret_cast<Beats>(GetProcAddress(module, "ProbeBeats"));
    Stop stop = reinterpret_cast<Stop>(GetProcAddress(module, "ProbeStop"));
    CHECK(start && beats && stop && start());
    CHECK(sf4e::platform::PinModule(module) == module);
    Sweep(10, module);
    CHECK(GetModuleHandleW(L"ComProbeServer.dll") == module);
    const long before = beats();
    Sleep(50);
    CHECK_MSG(beats() > before, "%s", "the probe's thread stopped running");
    stop();
}

int main() {
    CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
    Register();
    SweepUnloadsAnUnpinnedServer();
    SweepLeavesAPinnedServerAndItsThread();

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
    CoUninitialize();
    std::printf("A pinned module stays loaded.\n");
    return 0;
}
