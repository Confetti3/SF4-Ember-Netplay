#pragma once

#include "HelperProcess.hxx"
#include "../netplay/BoundedMailbox.hxx"
#include "../common/WipeText.hxx"
#include <atomic>
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
class HelperClient {
public:
    HelperClient();
    ~HelperClient();
    HelperClient(const HelperClient&) = delete;
    HelperClient& operator=(const HelperClient&) = delete;
    bool Start(const HelperBootstrap& bootstrap);
    bool Send(const std::string& payload, uint64_t* requestId = nullptr);
    bool TryReceive(HelperMessage& message) { return incoming_.TryPop(message); }
    HelperState State() const { return state_.load(); }
    DWORD LastError() const { return error_.load(); }
    void Stop();
private:
    void Run(HelperBootstrap bootstrap);
    bool Transfer(HANDLE pipe, void* data, DWORD size, bool write);
    bool WriteFrame(HANDLE pipe, const HelperMessage& message);
    bool ReadFrame(HANDLE pipe, HelperMessage& message);
    HANDLE stop_ = nullptr;
    std::thread worker_;
    std::atomic<HelperState> state_{HelperState::Stopped};
    std::atomic<DWORD> error_{ERROR_SUCCESS};
    std::mutex sendMutex_;
    uint64_t nextId_ = 2;
    bool started_ = false;
    netplay::BoundedMailbox<OutgoingMessage> outgoing_;
    netplay::BoundedMailbox<HelperMessage> incoming_;
};

} }
