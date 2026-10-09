#include "../common/StartupHandshake.hxx"
#include "test_support.hxx"
#include <cstdint>
#include <string>

using namespace sf4e::startup;

static Result RunChild(const wchar_t* mode, DWORD timeout) {
    Channel channel;
    CHECK(channel.Create());
    CHECK(SetHandleInformation(channel.event, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
    CHECK(SetHandleInformation(channel.mailbox, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
    wchar_t path[32768];
    CHECK(GetModuleFileNameW(NULL, path, 32768));
    std::wstring command = std::wstring(L"\"") + path + L"\" " + mode + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(channel.event)) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(channel.mailbox));
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    CHECK(CreateProcessW(path, &command[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi));
    const Result result = Await(channel, pi.hProcess, timeout);
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) CHECK(TerminateProcess(pi.hProcess, 0));
    CHECK(WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return result;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 4) {
        const HANDLE event = reinterpret_cast<HANDLE>(std::stoull(argv[2]));
        const HANDLE mailbox = reinterpret_cast<HANDLE>(std::stoull(argv[3]));
        const std::wstring mode = argv[1];
        if (mode == L"exit") return 23;
        if (mode == L"ready") Publish(mailbox, event, State::Ready, 0);
        if (mode == L"fail") { Publish(mailbox, event, State::Failed, ERROR_DLL_INIT_FAILED); return 24; }
        if (mode == L"signal-only") SetEvent(event);
        Sleep(10000);
        return 0;
    }
    CHECK(RunChild(L"ready", 5000).error == ERROR_SUCCESS);
    CHECK(RunChild(L"fail", 5000).error == ERROR_DLL_INIT_FAILED);
    CHECK(RunChild(L"signal-only", 5000).error == ERROR_DLL_INIT_FAILED);
    CHECK(RunChild(L"idle", 25).error == ERROR_TIMEOUT);
    const auto exited = RunChild(L"exit", 5000);
    CHECK(exited.error == ERROR_PROCESS_ABORTED && exited.processExit == 23);
    // Exercise the same non-inheritable handle duplication used by the launcher.
    Channel channel;
    CHECK(channel.Create());
    HANDLE event = NULL, mailbox = NULL;
    CHECK(channel.DuplicateTo(GetCurrentProcess(), event, mailbox));
    Publish(mailbox, event, State::Failed, ERROR_REVISION_MISMATCH);
    // WaitForMultipleObjects needs a real process handle, not the -1 pseudo handle.
    HANDLE self = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    CHECK(self);
    const auto duplicated = Await(channel, self, 1000);
    CHECK(duplicated.error == ERROR_REVISION_MISMATCH);
    CloseHandle(self);
    return 0;
}
