#include "../common/StartupHandshake.hxx"
#include "../launcher/LaunchOutcome.hxx"
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
    if (std::wstring(mode) == L"ready-exit") CHECK(WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0);
    if (std::wstring(mode) == L"deny-stop") {
        HANDLE limited = nullptr;
        CHECK(DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &limited, SYNCHRONIZE, FALSE, 0));
        const auto stopped = StopOwnedChild(limited, 0, 15);
        CHECK(!stopped.confirmed && stopped.terminateError == ERROR_ACCESS_DENIED && stopped.waitResult == WAIT_TIMEOUT);
        CloseHandle(limited);
    }
    const Result result = Await(channel, pi.hProcess, timeout);
    CHECK(StopOwnedChild(pi.hProcess, 0).confirmed);
    CHECK(StopOwnedChild(pi.hProcess, 0).confirmed); // Already exited is also confirmed.
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return result;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 4) {
        const HANDLE event = reinterpret_cast<HANDLE>(std::stoull(argv[2]));
        const HANDLE mailbox = reinterpret_cast<HANDLE>(std::stoull(argv[3]));
        const std::wstring mode = argv[1];
        if (mode == L"exit") return 23;
        if (mode == L"crash") ExitProcess(0xC0000005u);
        if (mode == L"ready-exit") { Publish(mailbox, event, State::Ready, 0); return 0; }
        if (mode == L"ready") Publish(mailbox, event, State::Ready, 0);
        if (mode == L"fail") Publish(mailbox, event, State::Failed, ERROR_INVALID_DATA);
        if (mode == L"signal-only") SetEvent(event);
        Sleep(10000);
        return 0;
    }
    CHECK(RunChild(L"ready", 5000).outcome == Outcome::Ready);
    const auto failed = RunChild(L"fail", 5000);
    CHECK(failed.outcome == Outcome::SidecarFailed && failed.error == ERROR_INVALID_DATA);
    CHECK(RunChild(L"signal-only", 5000).outcome == Outcome::Incomplete);
    CHECK(RunChild(L"idle", 25).outcome == Outcome::Timeout);
    CHECK(RunChild(L"deny-stop", 25).outcome == Outcome::Timeout);
    const auto exited = RunChild(L"exit", 5000);
    CHECK(exited.outcome == Outcome::EarlyExit && exited.processExit == 23);
    const auto crash = RunChild(L"crash", 5000);
    CHECK(crash.outcome == Outcome::EarlyExit && crash.processExit == 0xC0000005u);
    const auto readyExit = RunChild(L"ready-exit", 5000);
    CHECK(readyExit.outcome == Outcome::EarlyExit && readyExit.processExit == 0);
    CHECK(!StopOwnedChild(INVALID_HANDLE_VALUE, 0).confirmed);
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
    CHECK(duplicated.outcome == Outcome::SidecarFailed && duplicated.error == ERROR_REVISION_MISMATCH);
    CHECK(Await(channel, nullptr, 0).outcome == Outcome::WaitFailed);
    // Error codes cannot turn a reported Sidecar failure into unsupported-game
    // or validation-I/O outcomes; the cause determines the player's guidance.
    using sf4e::launcher::StartupMessage;
    CHECK(std::string(StartupMessage(duplicated)) == "launcher.sidecar_failed");
    CHECK(std::string(StartupMessage({Outcome::ValidationIo, ERROR_SHARING_VIOLATION, 0})) == "launcher.validation_io");
    CHECK(std::string(StartupMessage({Outcome::UnsupportedGame, ERROR_REVISION_MISMATCH, 0})) == "launcher.unsupported_game");
    for (bool early : {false, true}) {
        using sf4e::launcher::ClassifyGameExit;
        const auto crash = ClassifyGameExit(0xC0000005u, false, true, early);
        CHECK(crash.crashed && crash.reportable && std::string(crash.message) == "launcher.game_crashed");
        CHECK(std::string(ClassifyGameExit(0xC0000005u, false, false, early).message) == "launcher.game_crashed_files");
        CHECK(std::string(ClassifyGameExit(0xC0000139u, false, true, early).message) == "launcher.game_wrong_dll");
        CHECK(std::string(ClassifyGameExit(0xC0E90002u, false, true, early).message) == "launcher.game_blocked_by_windows");
        CHECK(!ClassifyGameExit(0xC0000005u, true, true, early).reportable);
        CHECK(std::string(ClassifyGameExit(0xC0000005u, true, true, early).message) == "launcher.steam_runs_as_admin");
    }
    CloseHandle(self);
    return 0;
}
