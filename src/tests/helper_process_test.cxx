#include "../platform/HelperClient.hxx"
#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>
#include "test_support.hxx"
#define EXPECT_WITH_WIN32(c) EXPECT_MSG(c, "win32=%lu", GetLastError())

using namespace sf4e::platform;

static bool WaitConnected(HelperClient& client) {
    const ULONGLONG until = GetTickCount64() + 15000;
    while (client.State() == HelperState::Connecting && GetTickCount64() < until) { Sleep(2); }
    return client.State() == HelperState::Connected;
}
static bool WaitMessage(HelperClient& client, const char* text) {
    const ULONGLONG until = GetTickCount64() + 15000;
    while (GetTickCount64() < until) {
        HelperMessage message;
        if (client.TryReceive(message)) {
            if (message.payload.find(text) != std::string::npos) { return true; }
        } else if (client.State() == HelperState::Failed) { return false; }
        Sleep(2);
    }
    return false;
}
struct WindowProbe { DWORD pid; bool found; };
static BOOL CALLBACK FindWindow(HWND window, LPARAM context) {
    auto& probe = *reinterpret_cast<WindowProbe*>(context);
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid == probe.pid && IsWindowVisible(window)) { probe.found = true; }
    return TRUE;
}

// The helper's end of the named pipe, played by this process, for what a real
// helper cannot be made to do on cue: flood events, split a header, send a bad
// frame or close its end. Every transfer gives up after 10 s, so a client that
// never writes fails the check instead of hanging the test.
class FakePipe {
public:
    FakePipe() {
        static unsigned instance = 0;
        wchar_t name[96] = {};
        swprintf_s(name, L"\\\\.\\pipe\\sf4-net-%08lx%08lx%016llx", GetCurrentProcessId(), ++instance, GetTickCount64());
        pipe_ = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 1 << 20, 1 << 20, 0, nullptr);
        bootstrap_.helperPid = GetCurrentProcessId();
        wcscpy_s(bootstrap_.pipeName, name);
        for (size_t i = 0; i < sizeof(bootstrap_.nonce); ++i) { bootstrap_.nonce[i] = static_cast<uint8_t>(i * 7 + 1); }
    }
    ~FakePipe() { Close(); }
    FakePipe(const FakePipe&) = delete;
    FakePipe& operator=(const FakePipe&) = delete;
    // Starts `client` on this pipe and answers its id 1 exchange with `answer`.
    bool Connect(HelperClient& client, const std::string& answer = helper_frame::AuthenticationAnswer()) {
        if (pipe_ == INVALID_HANDLE_VALUE || !client.Start(bootstrap_)) { return false; }
        OVERLAPPED operation = {}; operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool connected = ConnectNamedPipe(pipe_, &operation) != FALSE;
        if (!connected) {
            const DWORD error = GetLastError();
            DWORD ignored = 0;
            connected = error == ERROR_PIPE_CONNECTED
                || (error == ERROR_IO_PENDING && GetOverlappedResult(pipe_, &operation, &ignored, TRUE));
        }
        CloseHandle(operation.hEvent);
        uint64_t id = 0; std::string nonce;
        if (!connected || !Read(id, nonce) || id != 1 || nonce.size() != sizeof(bootstrap_.nonce) || !Write(1, answer)) { return false; }
        const ULONGLONG until = GetTickCount64() + 5000;
        while (client.State() == HelperState::Connecting && GetTickCount64() < until) { Sleep(1); }
        return client.State() == HelperState::Connected;
    }
    static std::string Frame(uint64_t id, const std::string& payload) {
        uint8_t header[helper_frame::HeaderSize];
        helper_frame::EncodeHeader(id, payload.size(), header);
        return std::string(reinterpret_cast<const char*>(header), sizeof(header)) + payload;
    }
    bool Write(uint64_t id, const std::string& payload) {
        std::string frame = Frame(id, payload);
        return WriteRaw(frame.data(), frame.size());
    }
    bool WriteRaw(const char* data, size_t size) {
        return Transfer(const_cast<char*>(data), static_cast<DWORD>(size), true);
    }
    bool Read(uint64_t& id, std::string& payload) {
        uint8_t header[helper_frame::HeaderSize];
        uint32_t size = 0;
        if (!Transfer(header, sizeof(header), false) || !helper_frame::DecodeHeader(header, id, size)) { return false; }
        payload.resize(size);
        return Transfer(&payload[0], size, false);
    }
    void Close() { if (pipe_ != INVALID_HANDLE_VALUE) { CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; } }

private:
    bool Transfer(void* data, DWORD size, bool write) {
        OVERLAPPED operation = {}; operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD offset = 0;
        bool ok = operation.hEvent != nullptr;
        while (ok && offset < size) {
            auto buffer = static_cast<uint8_t*>(data) + offset;
            DWORD done = 0;
            ResetEvent(operation.hEvent);
            const BOOL started = write ? WriteFile(pipe_, buffer, size - offset, &done, &operation)
                : ReadFile(pipe_, buffer, size - offset, &done, &operation);
            if (!started && GetLastError() != ERROR_IO_PENDING) { ok = false; break; }
            if (WaitForSingleObject(operation.hEvent, 10000) != WAIT_OBJECT_0) {
                CancelIoEx(pipe_, &operation);
                GetOverlappedResult(pipe_, &operation, &done, TRUE);
                ok = false; break;
            }
            ok = GetOverlappedResult(pipe_, &operation, &done, FALSE) && done > 0;
            offset += done;
        }
        if (operation.hEvent) { CloseHandle(operation.hEvent); }
        return ok;
    }
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HelperBootstrap bootstrap_;
};

// Takes the next event, waiting up to 5 s for one.
static bool NextEvent(HelperClient& client, HelperMessage& message) {
    const ULONGLONG until = GetTickCount64() + 5000;
    while (GetTickCount64() < until) {
        if (client.TryReceive(message)) { return true; }
        if (client.State() == HelperState::Failed) { return false; }
        client.WaitForIncoming(100);
    }
    return false;
}
static bool WaitFailed(HelperClient& client) {
    const ULONGLONG until = GetTickCount64() + 5000;
    while (client.State() != HelperState::Failed && GetTickCount64() < until) { client.WaitForIncoming(100); }
    return client.State() == HelperState::Failed;
}

// The connected worker waits on events, not on a timer: a Send wakes it to
// write, the helper's frame wakes it to read, a full mailbox stops the reads
// without losing or reordering a frame, and Stop ends a posted read.
static void FakePipeCases() {
    // Command and event round trips: each Send must wake the worker, or the
    // fake's read runs out.
    {
        FakePipe fake;
        HelperClient client;
        EXPECT_WITH_WIN32(fake.Connect(client));
        EXPECT_WITH_WIN32(client.HelperIpcVersion() == helper_frame::IpcVersion);
        const std::string command = "{\"type\":\"status\"}";
        const ULONGLONG started = GetTickCount64();
        for (uint64_t round = 0; round < 50; ++round) {
            uint64_t id = 0, got = 0;
            std::string payload;
            EXPECT_WITH_WIN32(client.Send(command, &id) && id == round + 2);
            EXPECT_WITH_WIN32(fake.Read(got, payload) && got == id && payload == command);
            EXPECT_WITH_WIN32(fake.Write(round + 2, "{\"type\":\"reply\"}"));
            HelperMessage message;
            EXPECT_WITH_WIN32(NextEvent(client, message) && message.id == round + 2 && message.payload == "{\"type\":\"reply\"}");
        }
        const ULONGLONG took = GetTickCount64() - started;
        std::printf("fake helper: 50 command and event round trips took %llu ms\n", took);
        EXPECT_WITH_WIN32(took < 5000);
        const ULONGLONG stopping = GetTickCount64();
        client.Stop();
        EXPECT_WITH_WIN32(GetTickCount64() - stopping < 1000);
        EXPECT_WITH_WIN32(client.State() == HelperState::Stopped);
        // Stopping while a read is posted is not an error.
        EXPECT_WITH_WIN32(client.LastError() == ERROR_SUCCESS);
    }
    // More events than the mailbox holds, written while nobody takes them:
    // the worker holds one back and stops reading, commands still go out,
    // and every event arrives once and in order when the consumer catches up.
    {
        FakePipe fake;
        HelperClient client;
        EXPECT_WITH_WIN32(fake.Connect(client));
        const uint64_t count = 300;
        bool written = true;
        std::thread writer([&fake, &written, count] {
            for (uint64_t id = 2; id < 2 + count; ++id) {
                written = fake.Write(id, "{\"type\":\"event\",\"n\":" + std::to_string(id) + "}") && written;
            }
        });
        Sleep(500);
        EXPECT_WITH_WIN32(client.State() == HelperState::Connected);
        uint64_t sent = 0, got = 0;
        std::string payload;
        EXPECT_WITH_WIN32(client.Send("{\"type\":\"ack\"}", &sent));
        EXPECT_WITH_WIN32(fake.Read(got, payload) && got == sent && payload == "{\"type\":\"ack\"}");
        uint64_t expected = 2;
        HelperMessage message;
        while (expected < 2 + count && NextEvent(client, message)) {
            if (message.id != expected || message.payload != "{\"type\":\"event\",\"n\":" + std::to_string(expected) + "}") { break; }
            ++expected;
        }
        std::printf("fake helper: %llu of %llu events arrived in order through a full mailbox\n", expected - 2, count);
        EXPECT_WITH_WIN32(expected == 2 + count);
        writer.join();
        EXPECT_WITH_WIN32(written);
        EXPECT_WITH_WIN32(client.State() == HelperState::Connected);
        client.Stop();
    }
    // The authentication answer carries the helper's IPC version. A bare SF4N
    // (the Discord companion, or a helper from before the version) connects
    // with version 0, which a room refuses; any other answer fails the client.
    {
        FakePipe fake;
        HelperClient client;
        EXPECT_WITH_WIN32(fake.Connect(client, "SF4N"));
        EXPECT_WITH_WIN32(client.HelperIpcVersion() == 0);
        client.Stop();
    }
    {
        FakePipe fake;
        HelperClient client;
        EXPECT_WITH_WIN32(!fake.Connect(client, "SF4X"));
        EXPECT_WITH_WIN32(client.State() == HelperState::Failed);
        client.Stop();
    }
    // A raw-body frame is a payload like any other to the client: it arrives
    // byte for byte, and a raw-body command goes out the same way.
    {
        FakePipe fake;
        HelperClient client;
        EXPECT_WITH_WIN32(fake.Connect(client));
        const std::string chunk("\0\1\xff", 3);
        const auto event = helper_frame::EncodeRawBody("{\"type\":\"checkpoint_chunk\"}", chunk.data(), chunk.size());
        EXPECT_WITH_WIN32(!event.empty() && fake.Write(2, event));
        HelperMessage message;
        EXPECT_WITH_WIN32(NextEvent(client, message) && message.payload == event);
        uint64_t id = 0, got = 0;
        std::string payload;
        EXPECT_WITH_WIN32(client.Send(event, &id) && fake.Read(got, payload) && got == id && payload == event);
        client.Stop();
    }
    // A header that arrives in pieces is put back together.
    {
        FakePipe fake;
        HelperClient client;
        EXPECT_WITH_WIN32(fake.Connect(client));
        const std::string payload = "{\"type\":\"split\"}";
        const std::string frame = FakePipe::Frame(2, payload);
        EXPECT_WITH_WIN32(fake.WriteRaw(frame.data(), 5));
        Sleep(100);
        EXPECT_WITH_WIN32(fake.WriteRaw(frame.data() + 5, 6));
        Sleep(100);
        EXPECT_WITH_WIN32(fake.WriteRaw(frame.data() + 11, frame.size() - 11));
        HelperMessage message;
        EXPECT_WITH_WIN32(NextEvent(client, message) && message.id == 2 && message.payload == payload);
        EXPECT_WITH_WIN32(client.State() == HelperState::Connected);
        client.Stop();
    }
    // A frame of another version, and an id that goes backwards, fail the
    // client as invalid data; the frame before the bad id is still delivered.
    {
        FakePipe fake;
        HelperClient client;
        EXPECT_WITH_WIN32(fake.Connect(client));
        std::string frame = FakePipe::Frame(2, "{}");
        frame[5] = 2;
        EXPECT_WITH_WIN32(fake.WriteRaw(frame.data(), frame.size()));
        EXPECT_WITH_WIN32(WaitFailed(client));
        EXPECT_WITH_WIN32(client.LastError() == ERROR_INVALID_DATA);
        client.Stop();
    }
    {
        FakePipe fake;
        HelperClient client;
        EXPECT_WITH_WIN32(fake.Connect(client));
        EXPECT_WITH_WIN32(fake.Write(3, "{\"type\":\"first\"}"));
        EXPECT_WITH_WIN32(fake.Write(2, "{\"type\":\"stale\"}"));
        EXPECT_WITH_WIN32(WaitFailed(client));
        EXPECT_WITH_WIN32(client.LastError() == ERROR_INVALID_DATA);
        HelperMessage message;
        EXPECT_WITH_WIN32(client.TryReceive(message) && message.id == 3);
        EXPECT_WITH_WIN32(!client.TryReceive(message));
        client.Stop();
    }
    // A helper that closes its end while the worker waits fails the client at
    // once with the pipe's own error.
    {
        FakePipe fake;
        HelperClient client;
        EXPECT_WITH_WIN32(fake.Connect(client));
        Sleep(50);
        const ULONGLONG started = GetTickCount64();
        fake.Close();
        EXPECT_WITH_WIN32(WaitFailed(client));
        const ULONGLONG took = GetTickCount64() - started;
        std::printf("fake helper closed: client Failed after %llu ms with error %lu\n", took, client.LastError());
        EXPECT_WITH_WIN32(took < 1000);
        EXPECT_WITH_WIN32(client.LastError() == ERROR_BROKEN_PIPE);
        client.Stop();
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { return 2; }
    const std::wstring executable(argv[1]);
    FakePipeCases();
    {
        HelperProcess helper;
        EXPECT_WITH_WIN32(helper.Start(executable, GetCurrentProcessId()));
        if (!helper.IsRunning()) { std::printf("Helper start error %lu\n", helper.LastError()); return 1; }
        const DWORD pid = helper.Bootstrap().helperPid;
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        EXPECT_WITH_WIN32(process != nullptr);
        HelperClient client;
        EXPECT_WITH_WIN32(client.Start(helper.Bootstrap()));
        EXPECT_WITH_WIN32(WaitConnected(client));
        // The helper built with this game answers with the same IPC version.
        EXPECT_WITH_WIN32(client.HelperIpcVersion() == helper_frame::IpcVersion);
        uint64_t request = 0;
        EXPECT_WITH_WIN32(client.Send("{\"type\":\"status\"}", &request) && request == 2);
        EXPECT_WITH_WIN32(WaitMessage(client, "\"request_id\":2"));
        WindowProbe probe = { pid, false }; EnumWindows(FindWindow, reinterpret_cast<LPARAM>(&probe));
        EXPECT_WITH_WIN32(!probe.found);
        EXPECT_WITH_WIN32(client.Send("{\"type\":\"leave\",\"epoch\":999}"));
        EXPECT_WITH_WIN32(WaitMessage(client, "stale_epoch"));
        EXPECT_WITH_WIN32(helper.IsRunning());
        EXPECT_WITH_WIN32(client.Send("{\"type\":\"shutdown\"}"));
        EXPECT_WITH_WIN32(WaitMessage(client, "\"type\":\"stopped\""));
        EXPECT_WITH_WIN32(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0);
        DWORD exitCode = 99; EXPECT_WITH_WIN32(GetExitCodeProcess(process, &exitCode) && exitCode == 0);
        CloseHandle(process); client.Stop(); helper.Stop(0);
        EXPECT_WITH_WIN32(!helper.IsRunning());
    }
    // A helper that dies while the client waits for events ends the wait at
    // once with the client Failed, not when the wait runs out: the room host
    // sleeps here between ticks.
    {
        HelperProcess helper;
        EXPECT_WITH_WIN32(helper.Start(executable, GetCurrentProcessId()));
        HelperClient client;
        EXPECT_WITH_WIN32(client.Start(helper.Bootstrap()));
        EXPECT_WITH_WIN32(WaitConnected(client));
        std::thread killer([&helper] { Sleep(300); helper.Stop(0); });
        const ULONGLONG started = GetTickCount64();
        HelperMessage message;
        while (client.State() != HelperState::Failed && GetTickCount64() - started < 10000) {
            while (client.TryReceive(message)) {}
            client.WaitForIncoming(5000);
        }
        const ULONGLONG took = GetTickCount64() - started;
        std::printf("helper killed: WaitForIncoming saw Failed after %llu ms\n", took);
        EXPECT_WITH_WIN32(client.State() == HelperState::Failed);
        EXPECT_WITH_WIN32(took < 3000);
        killer.join();
        client.Stop();
    }
    // Authentication failures must never reach the command service.
    for (int mode = 0; mode < 3; ++mode) {
        HelperProcess helper;
        EXPECT_WITH_WIN32(helper.Start(executable, GetCurrentProcessId() + (mode == 2 ? 1 : 0)));
        HelperBootstrap invalid = helper.Bootstrap();
        if (mode == 0) { invalid.nonce[0] ^= 1; }
        if (mode == 1) { invalid.helperPid = GetCurrentProcessId(); }
        HelperClient client;
        EXPECT_WITH_WIN32(client.Start(invalid));
        EXPECT_WITH_WIN32(!WaitConnected(client));
        EXPECT_WITH_WIN32(client.State() == HelperState::Failed);
        client.Stop(); helper.Stop(0);
        SecureZeroMemory(&invalid, sizeof(invalid));
    }
    // Closing the supervisor's job kills its own child, even before IPC auth.
    HANDLE child = nullptr;
    {
        HelperProcess supervisor;
        EXPECT_WITH_WIN32(supervisor.Start(executable, GetCurrentProcessId()));
        child = OpenProcess(SYNCHRONIZE, FALSE, supervisor.Bootstrap().helperPid);
        EXPECT_WITH_WIN32(child != nullptr);
    }
    EXPECT_WITH_WIN32(WaitForSingleObject(child, 5000) == WAIT_OBJECT_0);
    CloseHandle(child);
    std::printf("HelperProcess: %d failure(s); x86 supervisor/x64 helper, IPC authentication, hidden startup, shutdown and job cleanup checked\n", TEST_FAILURES());
    return TEST_EXIT_CODE();
}
