// HelperClient over the stdin and stdout of a helper run as `sf4-net --stdio`
// (rust/sf4-net/src/stdio.rs). The frames are those of HelperClient.cxx's named
// pipe, without the id 1 authentication exchange: the parent made the pipes.
#include "HelperClient.hxx"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <limits>
#include <vector>
#include <poll.h>
#include <unistd.h>

namespace sf4e { namespace platform {
namespace {
const size_t MaxPayload = 512 * 1024;
const size_t MaxMessages = 128;
const size_t MaxQueuedBytes = 8 * 1024 * 1024;
const uint64_t IoTimeoutMs = 15000;
const size_t HeaderSize = 14;

void CloseFd(int& fd) { if (fd >= 0) { close(fd); fd = -1; } }
}

HelperClient::HelperClient() : outgoing_(MaxMessages, MaxQueuedBytes), incoming_(MaxMessages, MaxQueuedBytes) {
    if (pipe(stopPipe_) != 0) { stopPipe_[0] = stopPipe_[1] = -1; }
}
HelperClient::~HelperClient() { Stop(); CloseFd(stopPipe_[0]); CloseFd(stopPipe_[1]); }

bool HelperClient::Start(int readFd, int writeFd) {
    if (stopPipe_[0] < 0 || started_ || worker_.joinable() || state_ != HelperState::Stopped || readFd < 0 || writeFd < 0) { return false; }
    // A helper that has gone must fail a write, not end this process.
    signal(SIGPIPE, SIG_IGN);
    started_ = true;
    readFd_ = readFd; writeFd_ = writeFd;
    state_ = HelperState::Connected;
    worker_ = std::thread(&HelperClient::Run, this, readFd, writeFd);
    return true;
}

bool HelperClient::Send(const std::string& payload, uint64_t* requestId) {
    if (payload.empty() || payload.size() > MaxPayload || state_ != HelperState::Connected) { return false; }
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (nextId_ > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) { return false; }
    OutgoingMessage message; message.id = nextId_; message.payload = payload;
    if (!outgoing_.TryPush(std::move(message), payload.size())) { return false; }
    if (requestId) { *requestId = nextId_; }
    ++nextId_;
    return true;
}

// Closing the helper's stdin is its order to stop (stdio.rs), so a command
// still queued when the client stops is not needed.
void HelperClient::Stop() {
    if (stopPipe_[1] >= 0) { const char byte = 0; while (write(stopPipe_[1], &byte, 1) < 0 && errno == EINTR) {} }
    if (worker_.joinable()) { worker_.join(); }
    CloseFd(writeFd_); CloseFd(readFd_);
    outgoing_.Close(); incoming_.Close();
    state_ = HelperState::Stopped;
}

// Moves `size` bytes, waiting on the pipe and the stop signal together so a
// blocked transfer ends when the client stops or the deadline passes.
bool HelperClient::Transfer(int fd, void* data, size_t size, bool write) {
    size_t offset = 0;
    const uint64_t deadline = GetTickCount64() + IoTimeoutMs;
    while (offset < size) {
        pollfd waits[2] = {};
        waits[0].fd = stopPipe_[0]; waits[0].events = POLLIN;
        waits[1].fd = fd; waits[1].events = write ? POLLOUT : POLLIN;
        const uint64_t now = GetTickCount64();
        const int remaining = now < deadline ? static_cast<int>(deadline - now) : 0;
        const int ready = poll(waits, 2, remaining);
        if (ready < 0) { if (errno == EINTR) continue; error_ = errno; return false; }
        if (ready == 0 || waits[0].revents) { error_ = ready == 0 ? ETIMEDOUT : ECANCELED; return false; }
        auto buffer = static_cast<uint8_t*>(data) + offset;
        const ssize_t moved = write ? ::write(fd, buffer, size - offset) : ::read(fd, buffer, size - offset);
        if (moved < 0) { if (errno == EINTR || errno == EAGAIN) continue; error_ = errno; return false; }
        if (moved == 0) { error_ = EPIPE; return false; }
        offset += static_cast<size_t>(moved);
    }
    return true;
}

bool HelperClient::WriteFrame(int fd, const HelperMessage& message) {
    if (message.payload.empty() || message.payload.size() > MaxPayload) { return false; }
    std::vector<uint8_t> bytes(HeaderSize + message.payload.size());
    const uint32_t length = static_cast<uint32_t>(10 + message.payload.size());
    for (int i = 0; i < 4; ++i) { bytes[i] = static_cast<uint8_t>(length >> (24 - 8 * i)); }
    bytes[4] = 0; bytes[5] = 1;
    for (int i = 0; i < 8; ++i) { bytes[6 + i] = static_cast<uint8_t>(message.id >> (56 - 8 * i)); }
    memcpy(bytes.data() + HeaderSize, message.payload.data(), message.payload.size());
    const bool success = Transfer(fd, bytes.data(), bytes.size(), true);
    // A frame may carry a passphrase; none outlives its write.
    WipeText(reinterpret_cast<char*>(bytes.data()), bytes.size());
    return success;
}

bool HelperClient::ReadFrame(int fd, HelperMessage& message) {
    uint8_t header[HeaderSize];
    if (!Transfer(fd, header, HeaderSize, false)) { return false; }
    uint32_t length = 0;
    for (int i = 0; i < 4; ++i) { length = (length << 8) | header[i]; }
    if (length <= 10 || length > MaxPayload + 10 || header[4] != 0 || header[5] != 1) { error_ = EBADMSG; return false; }
    message.id = 0;
    for (int i = 0; i < 8; ++i) { message.id = (message.id << 8) | header[6 + i]; }
    if (message.id == 0 || message.id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) { error_ = EBADMSG; return false; }
    message.payload.resize(length - 10);
    return Transfer(fd, &message.payload[0], message.payload.size(), false);
}

// The same loop as the Windows worker: bounded writes, then one read when the
// helper has written, with one frame held back while the incoming mailbox is
// full so the helper feels the backpressure instead of losing an event.
void HelperClient::Run(int readFd, int writeFd) {
    uint64_t lastReceived = 1;
    HelperMessage pendingIncoming;
    bool hasPendingIncoming = false;
    const auto stopping = [this](int waitMs) {
        pollfd stop = {}; stop.fd = stopPipe_[0]; stop.events = POLLIN;
        return poll(&stop, 1, waitMs) > 0;
    };
    while (!stopping(0)) {
        HelperMessage message;
        OutgoingMessage sending;
        for (size_t i = 0; i < 8 && outgoing_.TryPop(sending); ++i) {
            const bool written = WriteFrame(writeFd, sending);
            WipeText(sending.payload);
            if (!written) { state_ = HelperState::Failed; return; }
        }
        if (hasPendingIncoming) {
            const size_t size = pendingIncoming.payload.size();
            if (incoming_.TryPush(pendingIncoming, size)) {
                pendingIncoming = {};
                hasPendingIncoming = false;
            } else {
                stopping(2);
                continue;
            }
        }
        pollfd readable = {}; readable.fd = readFd; readable.events = POLLIN;
        const int ready = poll(&readable, 1, 0);
        if (ready < 0 && errno != EINTR) { error_ = errno; state_ = HelperState::Failed; return; }
        if (ready > 0) {
            // POLLHUP with nothing to read is the helper gone: ReadFrame fails on it.
            if (!ReadFrame(readFd, message) || message.id <= lastReceived) { if (!error_) error_ = EBADMSG; state_ = HelperState::Failed; return; }
            lastReceived = message.id;
            const size_t size = message.payload.size();
            if (!incoming_.TryPush(message, size)) {
                pendingIncoming = std::move(message);
                hasPendingIncoming = true;
            }
        } else { stopping(2); }
    }
}

} }
