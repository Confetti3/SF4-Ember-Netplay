#pragma once

#include <windows.h>

namespace sf4e { namespace startup {

enum class State : LONG { Pending = 0, Ready = 1, Failed = 2 };
struct Record { LONG state; DWORD error; };
// Stable causes: Win32 values are details, not a substitute for the cause.
enum class Outcome { Ready, UnsupportedGame, ValidationIo, CreateFailed, BootstrapFailed,
    SidecarFailed, Timeout, WaitFailed, Incomplete, EarlyExit };
struct Result { Outcome outcome; DWORD error; DWORD processExit; };
struct StopResult {
    bool confirmed;
    DWORD terminateError;
    DWORD waitResult;
    DWORD waitError;
};
// Only for a child this launcher created. A failed termination can still be
// followed by a natural exit; only the process wait confirms it stopped.
inline StopResult StopOwnedChild(HANDLE process, DWORD exitCode, DWORD timeoutMs = 5000) {
    if (!process || process == INVALID_HANDLE_VALUE) return {false, 0, WAIT_FAILED, ERROR_INVALID_HANDLE};
    const DWORD first = WaitForSingleObject(process, 0);
    if (first == WAIT_OBJECT_0) return {true, 0, first, 0};
    if (first == WAIT_FAILED) return {false, 0, first, GetLastError()};
    const DWORD terminateError = TerminateProcess(process, exitCode) ? 0 : GetLastError();
    const DWORD waited = WaitForSingleObject(process, timeoutMs);
    return {waited == WAIT_OBJECT_0, terminateError, waited, waited == WAIT_FAILED ? GetLastError() : 0};
}

// Unnamed handles are duplicated only into the launched game. The launcher
// owns this view until startup finishes; no pipe/worker wait runs in DllMain.
class Channel {
public:
    HANDLE event = NULL;
    HANDLE mailbox = NULL;
    Record* record = nullptr;
    Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    ~Channel() {
        if (record) UnmapViewOfFile(record);
        if (mailbox) CloseHandle(mailbox);
        if (event) CloseHandle(event);
    }
    bool Create() {
        event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!event) return false;
        mailbox = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(Record), NULL);
        if (!mailbox) return false;
        record = static_cast<Record*>(MapViewOfFile(mailbox, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Record)));
        return record != nullptr;
    }
    bool DuplicateTo(HANDLE process, HANDLE& childEvent, HANDLE& childMailbox) const {
        return DuplicateHandle(GetCurrentProcess(), event, process, &childEvent,
                   EVENT_MODIFY_STATE, FALSE, 0) &&
            DuplicateHandle(GetCurrentProcess(), mailbox, process, &childMailbox,
                   FILE_MAP_READ | FILE_MAP_WRITE, FALSE, 0);
    }
};

// Final report from the sidecar. Close only its copies, never the launcher's.
inline void Publish(HANDLE mailbox, HANDLE event, State state, DWORD error) {
    auto* record = mailbox ? static_cast<Record*>(MapViewOfFile(mailbox, FILE_MAP_WRITE, 0, 0, sizeof(Record))) : nullptr;
    if (record) {
        record->error = error;
        InterlockedExchange(&record->state, static_cast<LONG>(state));
        UnmapViewOfFile(record);
    }
    if (event) { SetEvent(event); CloseHandle(event); }
    if (mailbox) CloseHandle(mailbox);
}

// A service event lets the launcher write a crash dump during startup,
// without another worker or resetting the absolute startup deadline.
template <typename Service>
Result Await(const Channel& channel, HANDLE process, DWORD timeoutMs, HANDLE serviceEvent, Service service) {
    if (!channel.record) return {Outcome::WaitFailed, ERROR_INVALID_HANDLE, 0};
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
        const HANDLE handles[] = {process, channel.event, serviceEvent};
        const DWORD waited = WaitForMultipleObjects(serviceEvent ? 3 : 2, handles, FALSE, remaining);
        if (waited == WAIT_FAILED) return {Outcome::WaitFailed, GetLastError(), 0};
        if (waited == WAIT_TIMEOUT) return {Outcome::Timeout, ERROR_TIMEOUT, 0};
        if (waited == WAIT_OBJECT_0 + 2) { service(); continue; }
        const State state = static_cast<State>(InterlockedCompareExchange(&channel.record->state, 0, 0));
        if (state == State::Failed)
            return {Outcome::SidecarFailed, channel.record->error ? channel.record->error : ERROR_DLL_INIT_FAILED, 0};
        // Even a Ready report does not make an already-exited process live.
        const DWORD alive = WaitForSingleObject(process, 0);
        if (alive == WAIT_FAILED) return {Outcome::WaitFailed, GetLastError(), 0};
        if (alive == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(process, &exitCode)) return {Outcome::WaitFailed, GetLastError(), 0};
            return {Outcome::EarlyExit, ERROR_PROCESS_ABORTED, exitCode};
        }
        if (state == State::Ready) return {Outcome::Ready, ERROR_SUCCESS, 0};
        return {Outcome::Incomplete, ERROR_DLL_INIT_FAILED, 0};
    }
}
inline Result Await(const Channel& channel, HANDLE process, DWORD timeoutMs) {
    return Await(channel, process, timeoutMs, nullptr, [] {});
}

}}
