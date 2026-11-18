// A stand-in for XAudio2_7.dll in ModulePinTest: a COM server that says it can
// unload (as XAudio2 2.7 does) while a thread of its own still runs its code.
#include <windows.h>
#include <objbase.h>

static HANDLE g_thread = nullptr;
static volatile LONG g_beats = 0;
static volatile LONG g_stop = 0;

static DWORD WINAPI Engine(void*) {
    while (!g_stop) {
        InterlockedIncrement(&g_beats);
        Sleep(1);
    }
    return 0;
}

// One static object serves as class factory and as the object it makes; nothing
// is counted, which is how a server can say S_OK to DllCanUnloadNow with a
// thread of its own still running.
struct Probe : IClassFactory {
    HRESULT __stdcall QueryInterface(REFIID id, void** out) override {
        if (id == IID_IUnknown || id == IID_IClassFactory) { *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return 2; }
    ULONG __stdcall Release() override { return 1; }
    HRESULT __stdcall CreateInstance(IUnknown* outer, REFIID id, void** out) override {
        if (outer) { *out = nullptr; return CLASS_E_NOAGGREGATION; }
        return QueryInterface(id, out);
    }
    HRESULT __stdcall LockServer(BOOL) override { return S_OK; }
};
static Probe g_probe;

// Exported through com_probe_server.def: combaseapi.h declares these two.
extern "C" {
STDAPI DllCanUnloadNow() { return S_OK; }
STDAPI DllGetClassObject(REFCLSID, REFIID id, void** object) {
    return g_probe.QueryInterface(id, object);
}
__declspec(dllexport) bool ProbeStart() {
    g_thread = CreateThread(nullptr, 0, Engine, nullptr, 0, nullptr);
    return g_thread != nullptr;
}
__declspec(dllexport) long ProbeBeats() { return g_beats; }
__declspec(dllexport) void ProbeStop() {
    g_stop = 1;
    if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = nullptr; }
}
}
