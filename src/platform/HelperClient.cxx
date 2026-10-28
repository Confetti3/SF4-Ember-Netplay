#include "HelperClient.hxx"

#include <limits>

namespace sf4e { namespace platform {
using namespace helper_frame;
namespace {
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
};
// The header read the connected worker keeps posted while it waits. Every exit
// cancels it and waits for the cancel to land, so the kernel is done with this
// buffer and OVERLAPPED before they leave the stack and the pipe closes.
struct PendingRead {
    HANDLE pipe;
    OVERLAPPED operation;
    uint8_t header[HeaderSize];
    bool posted;
    explicit PendingRead(HANDLE handle) : pipe(handle), operation(), posted(false) {}
    ~PendingRead() {
        if (!posted) { return; }
        CancelIoEx(pipe, &operation);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &operation, &ignored, TRUE);
    }
};
}

HelperClient::HelperClient() : outgoing_(MaxMessages, MaxQueuedBytes), incoming_(MaxMessages, MaxQueuedBytes) {
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    sendWake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}
HelperClient::~HelperClient() {
    Stop();
    if (stop_) { CloseHandle(stop_); }
    if (sendWake_) { CloseHandle(sendWake_); }
    if (readEvent_) { CloseHandle(readEvent_); }
    if (writeEvent_) { CloseHandle(writeEvent_); }
}

bool HelperClient::Start(const HelperBootstrap& bootstrap) {
    if (!stop_ || !sendWake_ || !readEvent_ || !writeEvent_ || started_ || worker_.joinable() || state_ != HelperState::Stopped
        || bootstrap.version != 1 || bootstrap.helperPid == 0 || bootstrap.pipeName[95] != 0 || !bootstrap.pipeName[0]) { return false; }
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

bool HelperClient::Send(std::string payload, uint64_t* requestId) {
    // Moved into the message first, so its destructor wipes it on every return
    // below; a moved-from short string keeps its characters, so wipe that too.
    OutgoingMessage message; message.payload = std::move(payload);
    WipeText(payload);
    const size_t size = message.payload.size();
    if (size == 0 || size > MaxPayload || state_ != HelperState::Connected) { return false; }
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (nextId_ > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) { return false; }
    message.id = nextId_;
    if (!outgoing_.TryPush(std::move(message), size)) { return false; }
    if (requestId) { *requestId = nextId_; }
    ++nextId_;
    SetEvent(sendWake_);
    return true;
}

void HelperClient::Stop() {
    if (stop_) { SetEvent(stop_); }
    if (worker_.joinable()) { worker_.join(); }
    outgoing_.Close(); incoming_.Close();
    state_ = HelperState::Stopped;
}

// Moves `size` bytes, waiting on the pipe and the stop signal together so a
// blocked transfer ends when the client stops or `deadline` passes.
bool HelperClient::Transfer(HANDLE pipe, void* data, DWORD size, bool write, ULONGLONG deadline) {
    const HANDLE event = write ? writeEvent_ : readEvent_;
    DWORD offset = 0;
    while (offset < size) {
        OVERLAPPED operation = {}; operation.hEvent = event;
        ResetEvent(event);
        DWORD transferred = 0;
        auto buffer = static_cast<uint8_t*>(data) + offset;
        const BOOL success = write ? WriteFile(pipe, buffer, size - offset, &transferred, &operation)
            : ReadFile(pipe, buffer, size - offset, &transferred, &operation);
        if (!success && GetLastError() != ERROR_IO_PENDING) { error_ = GetLastError(); return false; }
        if (!success) {
            HANDLE waits[] = { stop_, event };
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
    // The header and the payload go as two writes on the byte-mode pipe under
    // one deadline. The payload, which may carry the bootstrap nonce or a
    // passphrase, is not copied here; the caller wipes it after the write.
    uint8_t header[HeaderSize];
    EncodeHeader(message.id, message.payload.size(), header);
    const ULONGLONG deadline = GetTickCount64() + IoTimeoutMs;
    return Transfer(pipe, header, HeaderSize, true, deadline)
        && Transfer(pipe, const_cast<char*>(message.payload.data()), static_cast<DWORD>(message.payload.size()), true, deadline);
}

bool HelperClient::ReadFrame(HANDLE pipe, HelperMessage& message, uint8_t (&header)[HeaderSize], DWORD have) {
    if (have < HeaderSize && !Transfer(pipe, header + have, HeaderSize - have, false, GetTickCount64() + IoTimeoutMs)) { return false; }
    uint32_t size = 0;
    if (!DecodeHeader(header, message.id, size)) { error_ = ERROR_INVALID_DATA; return false; }
    message.payload.resize(size);
    return Transfer(pipe, &message.payload[0], size, false, GetTickCount64() + IoTimeoutMs);
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
        // The id 1 exchange stays on the blocking path, before any read is posted.
        HelperMessage request; request.id = 1;
        request.payload.assign(reinterpret_cast<const char*>(bootstrap.nonce), sizeof(bootstrap.nonce));
        HelperMessage response;
        uint8_t header[HeaderSize];
        uint16_t version = 0;
        authenticated = WriteFrame(pipe.value, request) && ReadFrame(pipe.value, response, header, 0)
            && response.id == 1 && ReadAuthenticationAnswer(response.payload, version);
        ipcVersion_ = version;
        SecureZeroMemory(&request.payload[0], request.payload.size());
    } else { error_ = ERROR_ACCESS_DENIED; }
    SecureZeroMemory(&bootstrap, sizeof(bootstrap));
    if (!authenticated) { MarkFailed(); return; }
    state_ = HelperState::Connected;
    uint64_t lastReceived = 1;
    HelperMessage pendingIncoming;
    bool hasPendingIncoming = false;
    // Declared after the pipe, so it is cancelled before the pipe closes.
    PendingRead read(pipe.value);
    while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
        // Bound work in each direction so outgoing traffic cannot starve reads.
        OutgoingMessage sending;
        for (size_t i = 0; i < 8 && outgoing_.TryPop(sending); ++i) {
            const bool written = WriteFrame(pipe.value, sending);
            WipeText(sending.payload);
            if (!written) { MarkFailed(); return; }
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
                NotifyIncoming();
            }
        }
        // One header read stays posted while the worker waits, and none while
        // a frame is held back: that is the backpressure. A read that
        // completes at once still sets its event and is taken below.
        if (!read.posted && !hasPendingIncoming) {
            read.operation = OVERLAPPED(); read.operation.hEvent = readEvent_;
            ResetEvent(readEvent_);
            if (!ReadFile(pipe.value, read.header, HeaderSize, nullptr, &read.operation) && GetLastError() != ERROR_IO_PENDING) {
                error_ = GetLastError(); MarkFailed(); return;
            }
            read.posted = true;
        }
        // Each pass waits for the stop signal, a Send or the helper's output,
        // so a room with nothing happening costs nothing. More than one
        // bounded batch queued: write again before waiting. Only a held-back
        // frame is retried on a timer, since the mailbox's consumer gives no
        // signal when it makes room.
        const DWORD timeout = hasPendingIncoming ? 2 : outgoing_.Empty() ? INFINITE : 0;
        const HANDLE waits[] = { stop_, sendWake_, readEvent_ };
        const DWORD woke = WaitForMultipleObjects(read.posted ? 3 : 2, waits, FALSE, timeout);
        if (woke == WAIT_OBJECT_0) { return; }
        if (woke == WAIT_FAILED) { error_ = GetLastError(); MarkFailed(); return; }
        // Looked at after any wake, since a Send that wins the wait must not
        // starve the read.
        if (!read.posted || WaitForSingleObject(readEvent_, 0) != WAIT_OBJECT_0) { continue; }
        read.posted = false;
        DWORD got = 0;
        if (!GetOverlappedResult(pipe.value, &read.operation, &got, FALSE)) { error_ = GetLastError(); MarkFailed(); return; }
        // The rest of the frame is read as before, under its own deadlines.
        HelperMessage message;
        if (got == 0 || !ReadFrame(pipe.value, message, read.header, got) || message.id <= lastReceived) {
            error_ = ERROR_INVALID_DATA; MarkFailed(); return;
        }
        lastReceived = message.id;
        const size_t size = message.payload.size();
        if (!incoming_.TryPush(message, size)) {
            pendingIncoming = std::move(message);
            hasPendingIncoming = true;
        } else { NotifyIncoming(); }
    }
}

} }
