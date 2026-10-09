#pragma once

#include <windows.h>

namespace sf4e { namespace startup {

enum class State : LONG { Pending = 0, Ready = 1, Failed = 2 };
struct Record { LONG state; DWORD error; };
struct Result { DWORD error; DWORD processExit; };

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

inline Result Await(const Channel& channel, HANDLE process, DWORD timeoutMs) {
    HANDLE handles[] = {channel.event, process};
    const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, timeoutMs);
    if (waited == WAIT_FAILED) return {GetLastError(), 0};
    if (waited == WAIT_TIMEOUT) return {ERROR_TIMEOUT, 0};
    const State state = static_cast<State>(InterlockedCompareExchange(&channel.record->state, 0, 0));
    if (state == State::Failed)
        return {channel.record->error ? channel.record->error : ERROR_DLL_INIT_FAILED, 0};
    // Even a ready signal does not turn an already-exited game into success.
    if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(process, &exitCode)) return {GetLastError(), 0};
        return {ERROR_PROCESS_ABORTED, exitCode};
    }
    return {static_cast<DWORD>(state == State::Ready ? ERROR_SUCCESS : ERROR_DLL_INIT_FAILED), 0};
}

}}
