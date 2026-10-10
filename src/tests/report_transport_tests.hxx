#pragma once
#include "../common/ReportHttpRequest.hxx"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace report_transport_tests {
using sf4e::detail::ReportHttpRequest;
struct Fake {
    WINHTTP_STATUS_CALLBACK callback = nullptr;
    DWORD_PTR context = 0;
    int cancelDuring = 0, calls = 0, closes = 0;
    // Each operation completes this long after it starts, on another thread.
    DWORD delayMs = 0;
    std::vector<std::thread> late;
    std::atomic<bool> cancelled{false}, inCall{false}, closingDelivered{false};
    bool closed = false;
    std::thread cancellation, teardown;
    ~Fake() {
        if (cancellation.joinable()) cancellation.join();
        for (auto& thread : late) if (thread.joinable()) thread.join();
        if (teardown.joinable()) teardown.join();
    }
    void Notify(DWORD status, void* data = nullptr, DWORD length = 0) {
        callback(reinterpret_cast<HINTERNET>(this), context, status, data, length);
    }
    BOOL Start(int stage, DWORD completion, void* data = nullptr, DWORD length = 0) {
        CHECK(!closed); CHECK(!inCall.exchange(true)); ++calls;
        if (stage == cancelDuring) cancellation = std::thread([this] { Sleep(5); cancelled = true; });
        else if (delayMs) {
            const DWORD bytes = data && length == sizeof(DWORD) ? *static_cast<DWORD*>(data) : 0;
            late.emplace_back([this, completion, bytes] { Sleep(delayMs); DWORD written = bytes; Notify(completion, &written, sizeof(written)); });
        }
        else Notify(completion, data, length); // includes inline completion
        CHECK(inCall.exchange(false)); return TRUE;
    }
    static Fake& From(HINTERNET handle) { return *reinterpret_cast<Fake*>(handle); }
    static BOOL WINAPI Option(HINTERNET handle, DWORD option, void* value, DWORD) {
        CHECK(option == WINHTTP_OPTION_CONTEXT_VALUE); From(handle).context = *static_cast<DWORD_PTR*>(value); return TRUE;
    }
    static WINHTTP_STATUS_CALLBACK WINAPI Callback(HINTERNET handle, WINHTTP_STATUS_CALLBACK callback, DWORD flags, DWORD_PTR) {
        CHECK(flags & WINHTTP_CALLBACK_FLAG_HANDLES); From(handle).callback = callback; return nullptr;
    }
    static BOOL WINAPI Send(HINTERNET handle, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR context) {
        auto& self = From(handle); CHECK(context == self.context);
        return self.Start(1, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
    }
    static BOOL WINAPI Write(HINTERNET handle, LPCVOID, DWORD bytes, LPDWORD output) {
        CHECK(!output); return From(handle).Start(2, WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE, &bytes, sizeof(bytes));
    }
    static BOOL WINAPI Receive(HINTERNET handle, LPVOID) { return From(handle).Start(3, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE); }
    static BOOL WINAPI Read(HINTERNET handle, LPVOID buffer, DWORD, LPDWORD output) {
        CHECK(!output); return From(handle).Start(4, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, buffer, 0);
    }
    static BOOL WINAPI Close(HINTERNET handle) {
        auto& self = From(handle);
        CHECK(!self.inCall); CHECK(!self.closed); self.closed = true; ++self.closes;
        self.teardown = std::thread([&self] {
            // Close returns before the final callback. Completion can still
            // arrive for a cancelled request before HANDLE_CLOSING.
            Sleep(5);
            // A completion still on its way arrives before HANDLE_CLOSING.
            for (auto& thread : self.late) if (thread.joinable()) thread.join();
            self.cancelled = true;
            // An already pending operation may still succeed after closure.
            // Its notification must not restart the closed request.
            const DWORD completions[] = {0, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE,
                WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, WINHTTP_CALLBACK_STATUS_READ_COMPLETE};
            DWORD bytes = 4;
            if (self.cancelDuring) self.Notify(completions[self.cancelDuring], &bytes, sizeof(bytes));
            WINHTTP_ASYNC_RESULT error{}; error.dwError = ERROR_WINHTTP_OPERATION_CANCELLED;
            self.Notify(WINHTTP_CALLBACK_STATUS_REQUEST_ERROR, &error, sizeof(error));
            self.closingDelivered = true;
            self.Notify(WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING);
        });
        return TRUE;
    }
};
inline void Run() {
    // Cancellation during send, body write, receive, body read and teardown.
    // The production coordinator has no request polling/closing thread.
    for (int stage = 0; stage <= 4; ++stage) {
        Fake fake; fake.cancelDuring = stage;
        sf4e::detail::ReportHttpApi api{Fake::Option, Fake::Callback, Fake::Send, Fake::Write, Fake::Receive, Fake::Read, Fake::Close};
        {
            ReportHttpRequest request(reinterpret_cast<HINTERNET>(&fake), api);
            CHECK(request.Initialize());
            const auto cancel = [&] { return fake.cancelled.load(); };
            bool ok = request.Send(L"Content-Type: text/plain\r\n", 4, cancel);
            if (ok) ok = request.Write("body", 4, cancel);
            if (ok) ok = request.Receive(cancel);
            char body[16] = {};
            if (ok) ok = request.Read(body, sizeof(body), cancel);
            CHECK(ok == (stage == 0));
            CHECK(fake.calls == (stage ? stage : 4));
            if (stage) {
                CHECK(request.Cancelled()); CHECK(request.Error() == ERROR_WINHTTP_OPERATION_CANCELLED);
                CHECK(!request.Handle()); CHECK(fake.closingDelivered);
                CHECK(!request.Receive(cancel)); CHECK(fake.calls == stage);
            }
            request.Close();
            CHECK(!request.Handle()); CHECK(fake.closingDelivered); CHECK(fake.closes == 1);
        }
        // Destruction cannot close again or outlive its callback context.
        CHECK(fake.closes == 1);
    }
    // Every step makes progress, but slowly: the upload still ends at its one
    // deadline, not after a fresh wait for each step.
    {
        Fake fake; fake.delayMs = 40;
        sf4e::detail::ReportHttpApi api{Fake::Option, Fake::Callback, Fake::Send, Fake::Write, Fake::Receive, Fake::Read, Fake::Close};
        const auto started = std::chrono::steady_clock::now();
        ReportHttpRequest request(reinterpret_cast<HINTERNET>(&fake), api, std::chrono::milliseconds(150));
        CHECK(request.Initialize());
        const auto never = [] { return false; };
        bool ok = request.Send(L"Content-Type: text/plain\r\n", 40, never);
        int writes = 0;
        while (ok && writes < 20) { ok = request.Write("body", 4, never); if (ok) ++writes; }
        const auto took = std::chrono::steady_clock::now() - started;
        CHECK(!ok && writes > 0 && writes < 20);
        CHECK(request.Error() == ERROR_WINHTTP_TIMEOUT && !request.Cancelled() && !request.Handle());
        CHECK(took < std::chrono::milliseconds(400));
        // Nothing starts once the time is spent.
        const int calls = fake.calls;
        CHECK(!request.Receive(never) && fake.calls == calls);
        request.Close();
        CHECK(fake.closingDelivered && fake.closes == 1);
    }
}
}
