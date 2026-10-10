#pragma once
#include <windows.h>
#include <winhttp.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <utility>

namespace sf4e { namespace detail {
// The whole upload, from sending the request to reading the last byte of the
// answer, finishes within this or ends as a timeout. Each step gets only what
// is left of it, so slow progress cannot hold the services worker longer.
constexpr std::chrono::seconds ReportPostBudget{60};
// Only the worker calls request APIs or closes the handle. Each async call
// returns before we wait/cancel; callbacks only publish completion data.
// The API seam drives this same lifecycle in offline transport tests.
struct ReportHttpApi {
    decltype(&WinHttpSetOption) option = WinHttpSetOption;
    decltype(&WinHttpSetStatusCallback) callback = WinHttpSetStatusCallback;
    decltype(&WinHttpSendRequest) send = WinHttpSendRequest;
    decltype(&WinHttpWriteData) write = WinHttpWriteData;
    decltype(&WinHttpReceiveResponse) receive = WinHttpReceiveResponse;
    decltype(&WinHttpReadData) read = WinHttpReadData;
    decltype(&WinHttpCloseHandle) close = WinHttpCloseHandle;
};
class ReportHttpRequest {
public:
    explicit ReportHttpRequest(HINTERNET request, ReportHttpApi api = {},
        std::chrono::steady_clock::duration budget = ReportPostBudget)
        : request_(request), api_(api), deadline_(std::chrono::steady_clock::now() + budget) {}
    ~ReportHttpRequest() { Close(); }
    ReportHttpRequest(const ReportHttpRequest&) = delete;
    ReportHttpRequest& operator=(const ReportHttpRequest&) = delete;
    bool Initialize() {
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        // Install the context first: if callback installation fails, no async
        // call has started and Close does not wait for an unregistered callback.
        if (!api_.option(request_, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) return false;
        registered_ = api_.callback(request_, Callback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0)
            != WINHTTP_INVALID_STATUS_CALLBACK;
        return registered_;
    }
    bool Send(const wchar_t* headers, DWORD length, const std::function<bool()>& cancel) {
        return Perform(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, cancel, [&] {
            return api_.send(request_, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, length,
                reinterpret_cast<DWORD_PTR>(this));
        });
    }
    bool Write(const void* bytes, DWORD count, const std::function<bool()>& cancel) {
        return Perform(WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE, cancel, [&] { return api_.write(request_, bytes, count, nullptr); });
    }
    bool Receive(const std::function<bool()>& cancel) {
        return Perform(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, cancel, [&] { return api_.receive(request_, nullptr); });
    }
    bool Read(void* bytes, DWORD count, const std::function<bool()>& cancel) {
        return Perform(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, cancel, [&] { return api_.read(request_, bytes, count, nullptr); });
    }
    DWORD Bytes() const { std::lock_guard<std::mutex> lock(mutex_); return bytes_; }
    DWORD Error() const { std::lock_guard<std::mutex> lock(mutex_); return error_; }
    bool Cancelled() const { return cancelled_; }
    HINTERNET Handle() const { return request_; } // synchronous header queries, between operations only
    void Close() {
        const auto request = std::exchange(request_, nullptr);
        if (!request) return;
        // Never called while an API invocation is on another thread. Pending
        // async operations can be cancelled after their initiating call returns.
        api_.close(request);
        // The return value is not a callback-lifetime fence. Once registered,
        // only HANDLE_CLOSING permits releasing the context or I/O buffers.
        if (registered_) {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return closed_; });
        }
    }
private:
    template<class Start> bool Perform(DWORD completion, const std::function<bool()>& cancel, Start start) {
        if (!request_ || !registered_) return false;
        if (cancel && cancel()) { cancelled_ = true; error_ = ERROR_WINHTTP_OPERATION_CANCELLED; Close(); return false; }
        // Nothing new starts once the whole upload's time is spent.
        if (std::chrono::steady_clock::now() >= deadline_) { error_ = ERROR_WINHTTP_TIMEOUT; Close(); return false; }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            expected_ = completion; completed_ = false; error_ = bytes_ = 0;
        }
        // Call outside the callback mutex: WinHTTP can notify inline. Only this
        // worker can close the request, and only after start() has returned.
        if (!start()) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) { std::lock_guard<std::mutex> lock(mutex_); error_ = error; return false; }
        }
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            // Observe cancellation even when completion raced it. No further
            // operation may start on a closed request.
            lock.unlock(); const bool cancelNow = cancel && cancel(); lock.lock();
            if (cancelNow || std::chrono::steady_clock::now() >= deadline_) {
                cancelled_ = cancelNow;
                lock.unlock(); Close(); lock.lock();
                error_ = cancelNow ? ERROR_WINHTTP_OPERATION_CANCELLED : ERROR_WINHTTP_TIMEOUT;
                return false;
            }
            if (completed_) return error_ == 0;
            wake_.wait_for(lock, std::chrono::milliseconds(20));
        }
    }
    static void CALLBACK Callback(HINTERNET, DWORD_PTR context, DWORD status, void* information, DWORD length) {
        if (!context) return;
        auto& self = *reinterpret_cast<ReportHttpRequest*>(context);
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) self.closed_ = true;
        else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
            self.error_ = static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError; self.completed_ = true;
        } else if (status == self.expected_) {
            if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) self.bytes_ = length;
            else if (status == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE) self.bytes_ = *static_cast<DWORD*>(information);
            self.completed_ = true;
        }
        self.wake_.notify_all();
    }
    HINTERNET request_;
    ReportHttpApi api_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool registered_ = false, completed_ = false, closed_ = false, cancelled_ = false;
    DWORD expected_ = 0, bytes_ = 0, error_ = 0;
    const std::chrono::steady_clock::time_point deadline_;
};
} }
