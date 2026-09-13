#include "HelperClient.hxx"

#include <limits>
#include <vector>

namespace sf4e { namespace platform {
namespace {
const size_t MaxPayload = 512 * 1024;
const size_t MaxMessages = 128;
const size_t MaxQueuedBytes = 8 * 1024 * 1024;
const DWORD IoTimeoutMs = 15000;
const DWORD HeaderSize = 14;
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
};
}

HelperClient::HelperClient() : outgoing_(MaxMessages, MaxQueuedBytes), incoming_(MaxMessages, MaxQueuedBytes) {
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}
HelperClient::~HelperClient() { Stop(); if (stop_) { CloseHandle(stop_); } }

bool HelperClient::Start(const HelperBootstrap& bootstrap) {
    if (!stop_ || started_ || worker_.joinable() || state_ != HelperState::Stopped || bootstrap.version != 1
        || bootstrap.helperPid == 0 || bootstrap.pipeName[95] != 0 || !bootstrap.pipeName[0]) { return false; }
    const std::wstring name(bootstrap.pipeName), prefix = L"\\\\.\\pipe\\sf4-net-";
    if (name.size() != prefix.size() + 32 || name.compare(0, prefix.size(), prefix) != 0) { return false; }
    for (size_t i = prefix.size(); i < name.size(); ++i) {
        if (!(name[i] >= L'0' && name[i] <= L'9') && !(name[i] >= L'a' && name[i] <= L'f')) { return false; }
    }
    // One worker/client per bootstrap instance. Create a new object on restart.
    started_ = true;
    state_ = HelperState::Connecting;
    worker_ = std::thread(&HelperClient::Run, this, bootstrap);
    return true;
}

bool HelperClient::Send(const std::string& payload, uint64_t* requestId) {
    if (payload.empty() || payload.size() > MaxPayload || state_ != HelperState::Connected) { return false; }
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (nextId_ > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) { return false; }
    HelperMessage message; message.id = nextId_; message.payload = payload;
    if (!outgoing_.TryPush(std::move(message), payload.size())) { return false; }
    if (requestId) { *requestId = nextId_; }
    ++nextId_;
    return true;
}

void HelperClient::Stop() {
    if (stop_) { SetEvent(stop_); }
    if (worker_.joinable()) { worker_.join(); }
    outgoing_.Close(); incoming_.Close();
    state_ = HelperState::Stopped;
}

bool HelperClient::Transfer(HANDLE pipe, void* data, DWORD size, bool write) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.value) { error_ = GetLastError(); return false; }
    DWORD offset = 0;
    const ULONGLONG deadline = GetTickCount64() + IoTimeoutMs;
    while (offset < size) {
        OVERLAPPED operation = {}; operation.hEvent = event.value;
        ResetEvent(event.value);
        DWORD transferred = 0;
        auto buffer = static_cast<uint8_t*>(data) + offset;
        const BOOL success = write ? WriteFile(pipe, buffer, size - offset, &transferred, &operation)
            : ReadFile(pipe, buffer, size - offset, &transferred, &operation);
        if (!success && GetLastError() != ERROR_IO_PENDING) { error_ = GetLastError(); return false; }
        if (!success) {
            HANDLE waits[] = { stop_, event.value };
            const ULONGLONG now = GetTickCount64();
            const DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
            if (WaitForMultipleObjects(2, waits, FALSE, remaining) != WAIT_OBJECT_0 + 1) {
                CancelIoEx(pipe, &operation);
                GetOverlappedResult(pipe, &operation, &transferred, TRUE);
                error_ = ERROR_OPERATION_ABORTED; return false;
            }
            if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE)) { error_ = GetLastError(); return false; }
        }
        if (transferred == 0) { error_ = ERROR_BROKEN_PIPE; return false; }
        offset += transferred;
    }
    return true;
}

bool HelperClient::WriteFrame(HANDLE pipe, const HelperMessage& message) {
    if (message.payload.empty() || message.payload.size() > MaxPayload) { return false; }
    std::vector<uint8_t> bytes(HeaderSize + message.payload.size());
    const uint32_t length = static_cast<uint32_t>(10 + message.payload.size());
    for (int i = 0; i < 4; ++i) { bytes[i] = static_cast<uint8_t>(length >> (24 - 8 * i)); }
    bytes[4] = 0; bytes[5] = 1;
    for (int i = 0; i < 8; ++i) { bytes[6 + i] = static_cast<uint8_t>(message.id >> (56 - 8 * i)); }
    memcpy(bytes.data() + HeaderSize, message.payload.data(), message.payload.size());
    const bool success = Transfer(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), true);
    if (message.id == 1) { SecureZeroMemory(bytes.data(), bytes.size()); }
    return success;
}

bool HelperClient::ReadFrame(HANDLE pipe, HelperMessage& message) {
    uint8_t header[HeaderSize];
    if (!Transfer(pipe, header, HeaderSize, false)) { return false; }
    uint32_t length = 0;
    for (int i = 0; i < 4; ++i) { length = (length << 8) | header[i]; }
    if (length <= 10 || length > MaxPayload + 10 || header[4] != 0 || header[5] != 1) {
        error_ = ERROR_INVALID_DATA; return false;
    }
    message.id = 0;
    for (int i = 0; i < 8; ++i) { message.id = (message.id << 8) | header[6 + i]; }
    if (message.id == 0 || message.id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        error_ = ERROR_INVALID_DATA; return false;
    }
    message.payload.resize(length - 10);
    return Transfer(pipe, &message.payload[0], static_cast<DWORD>(message.payload.size()), false);
}

void HelperClient::Run(HelperBootstrap bootstrap) {
    Handle pipe(INVALID_HANDLE_VALUE);
    const ULONGLONG deadline = GetTickCount64() + IoTimeoutMs;
    do {
        pipe.value = CreateFileW(bootstrap.pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe.value != INVALID_HANDLE_VALUE) { break; }
        const DWORD error = GetLastError();
        if ((error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) || GetTickCount64() >= deadline) { error_ = error; break; }
    } while (WaitForSingleObject(stop_, 5) == WAIT_TIMEOUT);
    bool authenticated = false;
    ULONG pid = 0;
    if (pipe.value != INVALID_HANDLE_VALUE && GetNamedPipeServerProcessId(pipe.value, &pid) && pid == bootstrap.helperPid) {
        HelperMessage request; request.id = 1;
        request.payload.assign(reinterpret_cast<const char*>(bootstrap.nonce), sizeof(bootstrap.nonce));
        HelperMessage response;
        authenticated = WriteFrame(pipe.value, request) && ReadFrame(pipe.value, response) && response.id == 1 && response.payload == "SF4N";
        SecureZeroMemory(&request.payload[0], request.payload.size());
    } else { error_ = ERROR_ACCESS_DENIED; }
    SecureZeroMemory(&bootstrap, sizeof(bootstrap));
    if (!authenticated) { state_ = HelperState::Failed; return; }
    state_ = HelperState::Connected;
    uint64_t lastReceived = 1;
    HelperMessage pendingIncoming;
    bool hasPendingIncoming = false;
    while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
        HelperMessage message;
        // Bound work in each direction so outgoing traffic cannot starve reads.
        for (size_t i = 0; i < 8 && outgoing_.TryPop(message); ++i) {
            if (!WriteFrame(pipe.value, message)) { state_ = HelperState::Failed; return; }
        }
        // The helper may publish a complete checkpoint window and lifecycle
        // events faster than the game thread can consume them. Keep one read
        // frame locally and stop reading the pipe until bounded mailbox space
        // becomes available. This propagates backpressure to Rust without
        // dropping an ordered event or treating temporary saturation as a
        // helper failure. Outgoing acknowledgements above continue to drain.
        if (hasPendingIncoming) {
            const size_t size = pendingIncoming.payload.size();
            if (incoming_.TryPush(pendingIncoming, size)) {
                pendingIncoming = {};
                hasPendingIncoming = false;
            } else {
                WaitForSingleObject(stop_, 2);
                continue;
            }
        }
        DWORD available = 0;
        if (!PeekNamedPipe(pipe.value, nullptr, 0, nullptr, &available, nullptr)) { error_ = GetLastError(); state_ = HelperState::Failed; return; }
        if (available > 0) {
            if (!ReadFrame(pipe.value, message) || message.id <= lastReceived) { error_ = ERROR_INVALID_DATA; state_ = HelperState::Failed; return; }
            lastReceived = message.id;
            const size_t size = message.payload.size();
            if (!incoming_.TryPush(message, size)) {
                pendingIncoming = std::move(message);
                hasPendingIncoming = true;
            }
        } else { WaitForSingleObject(stop_, 2); }
    }
}

} }
