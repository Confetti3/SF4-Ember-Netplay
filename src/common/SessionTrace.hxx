#pragma once
#include <windows.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace sf4e {
// A small per-process lifecycle trace independent of the optional game logger.
// Only the runtime owner writes it. Callers supply state codes, never invites,
// endpoint addresses, chat, or player names. A bad connection cannot grow it
// beyond 2 MiB or make trace failure interrupt the game.
class SessionTrace {
public:
    ~SessionTrace() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
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
        worker_ = std::thread([this] { Run(); });
        Record(nlohmann::json{{"event", "runtime_started"}});
    }
    // Returns false only when the state was dropped, so a caller that skips
    // unchanged states can offer the same state again on its next tick.
    bool Record(nlohmann::json state) {
        if (file_ == INVALID_HANDLE_VALUE || state == previous_) return true;
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || stopping_ || queue_.size() >= 256) { ++dropped_; return false; }
        previous_ = state;
        LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc);
        queue_.push_back({std::move(state), GetTickCount64(), qpc.QuadPart});
        lock.unlock(); wake_.notify_one();
        return true;
    }
    unsigned long long Dropped() const { return dropped_.load(); }
    double LastWriteMs() const { return lastWriteUs_.load() / 1000.0; }
private:
    void Run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            while (!queue_.empty()) {
                auto record = std::move(queue_.front()); queue_.pop_front(); lock.unlock();
                record.state["qpc"] = record.qpc;
                const auto line = std::to_string(record.tick) + " " + record.state.dump() + "\r\n";
                if (bytes_ + line.size() <= 2 * 1024 * 1024) {
                    LARGE_INTEGER frequency{}, begin{}, end{}; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&begin);
                    DWORD written = 0;
                    const bool success=WriteFile(file_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)!=FALSE;
                    bytes_ += written;
                    if(!success || written!=line.size()) ++dropped_;
                    QueryPerformanceCounter(&end);
                    if (frequency.QuadPart) lastWriteUs_ = static_cast<unsigned long long>((end.QuadPart - begin.QuadPart) * 1000000 / frequency.QuadPart);
                } else ++dropped_;
                lock.lock();
            }
            if (stopping_) return;
        }
    }
    HANDLE file_ = INVALID_HANDLE_VALUE;
    unsigned long long bytes_ = 0;
    nlohmann::json previous_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    struct RecordItem { nlohmann::json state; ULONGLONG tick; LONGLONG qpc; };
    std::deque<RecordItem> queue_;
    std::thread worker_;
    bool stopping_ = false;
    std::atomic<unsigned long long> dropped_{0}, lastWriteUs_{0};
};
}
