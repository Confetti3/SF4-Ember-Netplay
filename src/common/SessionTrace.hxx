#pragma once
#include <windows.h>
#include <string>

namespace sf4e {
// A small per-process lifecycle trace independent of the optional game logger.
// Only the runtime owner writes it. Callers supply state codes, never invites,
// endpoint addresses, chat, or player names. A bad connection cannot grow it
// beyond 2 MiB or make trace failure interrupt the game.
class SessionTrace {
public:
    ~SessionTrace() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    void Open(const std::wstring& directory) {
        CreateDirectoryW(directory.c_str(), nullptr);
        const auto logs = directory + L"\\logs";
        CreateDirectoryW(logs.c_str(), nullptr);
        const auto path = logs + L"\\session-" + std::to_wstring(GetCurrentProcessId()) + L".log";
        file_ = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file_, &size)) bytes_ = static_cast<unsigned long long>(size.QuadPart);
        Record("runtime_started");
    }
    void Record(const std::string& state) {
        if (file_ == INVALID_HANDLE_VALUE || state == previous_) return;
        previous_ = state;
        const auto line = std::to_string(GetTickCount64()) + " " + state + "\r\n";
        if (bytes_ + line.size() > 2 * 1024 * 1024) return;
        DWORD written = 0;
        if (WriteFile(file_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) bytes_ += written;
    }
private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    unsigned long long bytes_ = 0;
    std::string previous_;
};
}
