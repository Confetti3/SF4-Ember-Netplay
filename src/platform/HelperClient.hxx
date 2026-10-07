#pragma once

#include "HelperProcess.hxx"
#include "../common/TickCount.hxx"
#include "../netplay/BoundedMailbox.hxx"
#include "../common/WipeText.hxx"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace sf4e { namespace platform {

struct HelperMessage { uint64_t id = 0; std::string payload; };
// A command on its way to the helper. It may carry a passphrase, so every
// copy is wiped when it goes, including one a full or closed queue drops.
struct OutgoingMessage : HelperMessage {
    OutgoingMessage() = default;
    OutgoingMessage(const OutgoingMessage&) = default;
    OutgoingMessage(OutgoingMessage&&) = default;
    OutgoingMessage& operator=(const OutgoingMessage&) = default;
    OutgoingMessage& operator=(OutgoingMessage&&) = default;
    ~OutgoingMessage() { WipeText(payload); }
};
enum class HelperState { Stopped, Connecting, Connected, Failed };

// All pipe connection/authentication/I/O happens on this owned worker. Game
// and UI threads only enqueue bounded commands and consume bounded events.
// The frames are the same on every platform; Windows talks to the helper's
// named pipe (HelperClient.cxx), Unix to the stdin and stdout of a helper run
// as `sf4-net --stdio` (HelperClientPosix.cxx).
class HelperClient {
public:
    HelperClient();
    ~HelperClient();
    HelperClient(const HelperClient&) = delete;
    HelperClient& operator=(const HelperClient&) = delete;
#ifdef _WIN32
    bool Start(const HelperBootstrap& bootstrap);
    DWORD LastError() const { return error_.load(); }
#else
    // The helper's stdout (read here) and stdin (written here); both are owned
    // and closed by the client. Connected at once: a stdio helper has no
    // bootstrap or authentication exchange.
    bool Start(int readFd, int writeFd);
    int LastError() const { return error_.load(); } // errno, or 0
#endif
    bool Send(const std::string& payload, uint64_t* requestId = nullptr);
    bool TryReceive(HelperMessage& message) { return incoming_.TryPop(message); }
    // Returns at once while an event waits for TryReceive or the worker has
    // failed, otherwise when either happens or after `timeoutMs`. A consumer
    // that takes a bounded batch with TryReceive and then waits here sleeps
    // only when there is nothing left to take.
    void WaitForIncoming(unsigned timeoutMs) {
        std::unique_lock<std::mutex> lock(arrivalMutex_);
        arrival_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this] { return !incoming_.Empty() || state_.load() == HelperState::Failed; });
    }
    HelperState State() const { return state_.load(); }
    void Stop();
private:
    // The worker, after pushing to incoming_ or setting Failed. Taking the
    // lock orders the change before a waiter's check of it, so the wakeup
    // cannot fall between that check and the wait.
    void NotifyIncoming() {
        { std::lock_guard<std::mutex> lock(arrivalMutex_); }
        arrival_.notify_all();
    }
#ifdef _WIN32
    void Run(HelperBootstrap bootstrap);
    bool Transfer(HANDLE pipe, void* data, DWORD size, bool write);
    bool WriteFrame(HANDLE pipe, const HelperMessage& message);
    bool ReadFrame(HANDLE pipe, HelperMessage& message);
    HANDLE stop_ = nullptr;
    std::atomic<DWORD> error_{ERROR_SUCCESS};
#else
    void Run(int readFd, int writeFd);
    bool Transfer(int fd, void* data, size_t size, bool write);
    bool WriteFrame(int fd, const HelperMessage& message);
    bool ReadFrame(int fd, HelperMessage& message);
    // The worker polls the read end beside the helper's pipe; Stop writes to
    // the other end, and the pipe's own fds are closed only once it has gone.
    int stopPipe_[2] = {-1, -1};
    // Send writes a byte here after queueing, so the worker wakes to write
    // instead of looking at the queue on a timer.
    int wakePipe_[2] = {-1, -1};
    int readFd_ = -1, writeFd_ = -1;
    std::atomic<int> error_{0};
#endif
    std::thread worker_;
    std::atomic<HelperState> state_{HelperState::Stopped};
    std::mutex sendMutex_;
    uint64_t nextId_ = 2;
    bool started_ = false;
    netplay::BoundedMailbox<OutgoingMessage> outgoing_;
    netplay::BoundedMailbox<HelperMessage> incoming_;
    std::mutex arrivalMutex_;
    std::condition_variable arrival_;
};

} }
