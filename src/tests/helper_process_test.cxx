#include "../platform/HelperClient.hxx"
#include <cstdio>

using namespace sf4e::platform;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s (win32=%lu)\n", __LINE__, #c, GetLastError()); ++failures; } } while (0)

static bool WaitConnected(HelperClient& client) {
    const ULONGLONG until = GetTickCount64() + 15000;
    while (client.State() == HelperState::Connecting && GetTickCount64() < until) { Sleep(2); }
    return client.State() == HelperState::Connected;
}
static bool WaitMessage(HelperClient& client, const char* text) {
    const ULONGLONG until = GetTickCount64() + 15000;
    while (GetTickCount64() < until) {
        HelperMessage message;
        if (client.TryReceive(message)) {
            if (message.payload.find(text) != std::string::npos) { return true; }
        } else if (client.State() == HelperState::Failed) { return false; }
        Sleep(2);
    }
    return false;
}
struct WindowProbe { DWORD pid; bool found; };
static BOOL CALLBACK FindWindow(HWND window, LPARAM context) {
    auto& probe = *reinterpret_cast<WindowProbe*>(context);
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid == probe.pid && IsWindowVisible(window)) { probe.found = true; }
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { return 2; }
    const std::wstring executable(argv[1]);
    {
        HelperProcess helper;
        CHECK(helper.Start(executable, GetCurrentProcessId()));
        if (!helper.IsRunning()) { std::printf("Helper start error %lu\n", helper.LastError()); return 1; }
        const DWORD pid = helper.Bootstrap().helperPid;
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        CHECK(process != nullptr);
        HelperClient client;
        CHECK(client.Start(helper.Bootstrap()));
        CHECK(WaitConnected(client));
        uint64_t request = 0;
        CHECK(client.Send("{\"type\":\"status\"}", &request) && request == 2);
        CHECK(WaitMessage(client, "\"request_id\":2"));
        WindowProbe probe = { pid, false }; EnumWindows(FindWindow, reinterpret_cast<LPARAM>(&probe));
        CHECK(!probe.found);
        CHECK(client.Send("{\"type\":\"leave\",\"epoch\":999}"));
        CHECK(WaitMessage(client, "stale_epoch"));
        CHECK(helper.IsRunning());
        CHECK(client.Send("{\"type\":\"shutdown\"}"));
        CHECK(WaitMessage(client, "\"type\":\"stopped\""));
        CHECK(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0);
        DWORD exitCode = 99; CHECK(GetExitCodeProcess(process, &exitCode) && exitCode == 0);
        CloseHandle(process); client.Stop(); helper.Stop(0);
        CHECK(!helper.IsRunning());
    }
    // Authentication failures must never reach the command service.
    for (int mode = 0; mode < 3; ++mode) {
        HelperProcess helper;
        CHECK(helper.Start(executable, GetCurrentProcessId() + (mode == 2 ? 1 : 0)));
        HelperBootstrap invalid = helper.Bootstrap();
        if (mode == 0) { invalid.nonce[0] ^= 1; }
        if (mode == 1) { invalid.helperPid = GetCurrentProcessId(); }
        HelperClient client;
        CHECK(client.Start(invalid));
        CHECK(!WaitConnected(client));
        CHECK(client.State() == HelperState::Failed);
        client.Stop(); helper.Stop(0);
        SecureZeroMemory(&invalid, sizeof(invalid));
    }
    // Closing the supervisor's job kills its own child, even before IPC auth.
    HANDLE child = nullptr;
    {
        HelperProcess supervisor;
        CHECK(supervisor.Start(executable, GetCurrentProcessId()));
        child = OpenProcess(SYNCHRONIZE, FALSE, supervisor.Bootstrap().helperPid);
        CHECK(child != nullptr);
    }
    CHECK(WaitForSingleObject(child, 5000) == WAIT_OBJECT_0);
    CloseHandle(child);
    std::printf("HelperProcess: %d failure(s); x86 supervisor/x64 helper, IPC authentication, hidden startup, shutdown and job cleanup checked\n", failures);
    return failures ? 1 : 0;
}
