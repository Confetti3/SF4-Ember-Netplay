// HttpBoundedRequest as the release check uses it, against a local server:
// the answers a check can get, the cap on a reply, one overall deadline, and
// a cancel that ends a check waiting on headers or reading the body at once.
// No GitHub dependency.

#include <winsock2.h>
#include <ws2tcpip.h>

#include "../common/sf4e__NetUtil.hxx"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "loopback_server.hxx"
#include "test_support.hxx"

using Clock = std::chrono::steady_clock;

namespace {
constexpr std::size_t ReleaseListCap = 1024 * 1024;

// One connection at a time on 127.0.0.1. It reads a request, sends `reply`
// as it is, then closes, or when `hold` is set keeps the connection open,
// silent, until the client goes.
class FakeReleaseServer {
public:
    explicit FakeReleaseServer(std::string reply, bool hold = false)
        : reply_(std::move(reply)), server_([this, hold](SOCKET client) { Serve(client, hold); }) {}
    int Port() const { return server_.Port(); }
    bool Received() const { return received_.load(); }
private:
    void Serve(SOCKET client, bool hold) {
        std::string request;
        char buffer[4096];
        int count = 0;
        while (request.find("\r\n\r\n") == std::string::npos && (count = recv(client, buffer, sizeof(buffer), 0)) > 0) {
            request.append(buffer, count);
        }
        received_ = true;
        for (std::size_t sent = 0; sent < reply_.size();) {
            count = send(client, reply_.data() + sent, static_cast<int>(reply_.size() - sent), 0);
            if (count <= 0) break;
            sent += count;
        }
        while (hold && recv(client, buffer, sizeof(buffer), 0) > 0) {}
    }
    std::string reply_;
    std::atomic<bool> received_{false};
    // Last, so it only starts serving once the rest is built.
    sf4e_test::LoopbackServer server_;
};

sf4e::HttpResponse Get(const FakeReleaseServer& server, int budgetMs = 3000, const std::atomic<bool>* cancel = nullptr) {
    return sf4e::HttpBoundedRequest("GET", "127.0.0.1", server.Port(), false, "/release", "Accept: application/vnd.github+json\r\n",
        {}, ReleaseListCap, std::chrono::milliseconds(budgetMs), [cancel] { return cancel && cancel->load(); });
}

std::string Reply(int status, const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status) + " Result\r\nContent-Length: " + std::to_string(body.size())
        + "\r\nConnection: close\r\n\r\n" + body;
}
}

// A complete answer is returned with its status, whether sized or chunked.
static void TestCompleteAnswers() {
    FakeReleaseServer success(Reply(200, "{\"tag_name\":\"v1.4.0\"}"));
    auto result = Get(success);
    CHECK(result.request.ok && result.request.statusCode == 200 && result.body == "{\"tag_name\":\"v1.4.0\"}");
    FakeReleaseServer chunked("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n2\r\n{}\r\n0\r\n\r\n");
    result = Get(chunked);
    CHECK(result.request.ok && result.request.statusCode == 200 && result.body == "{}");
    // The status is the caller's to judge: a rate limited answer is complete too.
    FakeReleaseServer limited(Reply(403, "rate limited"));
    result = Get(limited);
    CHECK(result.request.ok && result.request.statusCode == 403 && result.body == "rate limited");
    FakeReleaseServer empty(Reply(200, ""));
    result = Get(empty);
    CHECK(result.request.ok && result.request.statusCode == 200 && result.body.empty());
}

// A redirect is not followed, so its status is what the caller sees.
static void TestRedirectRefused() {
    FakeReleaseServer redirected("HTTP/1.1 301 Moved\r\nLocation: http://127.0.0.1:1/untrusted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    const auto result = Get(redirected);
    CHECK(result.request.ok && result.request.statusCode == 301 && result.body.empty());
}

// An oversized or truncated answer is never returned as complete.
static void TestIncompleteAnswers() {
    FakeReleaseServer huge(Reply(200, std::string(ReleaseListCap + 1, 'x')));
    CHECK(!Get(huge).request.ok);
    FakeReleaseServer truncated("HTTP/1.1 200 OK\r\nContent-Length: 500\r\nConnection: close\r\n\r\npartial");
    CHECK(!Get(truncated).request.ok);
}

// Cancelled before it starts, a request never connects.
static void TestCancelBeforeStart() {
    FakeReleaseServer early("", true);
    const std::atomic<bool> cancelled{true};
    const auto result = Get(early, 3000, &cancelled);
    CHECK(result.cancelled && !result.request.ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!early.Received());
}

// A cancel ends a request waiting on headers, and one in the middle of the body, within a second.
static void TestCancelWhileWaiting() {
    for (const std::string reply : {std::string(), std::string("HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\npartial")}) {
        FakeReleaseServer silent(reply, true);
        std::atomic<bool> cancel{false}, done{false};
        sf4e::HttpResponse result;
        std::thread get([&] { result = Get(silent, 10000, &cancel); done = true; });
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!silent.Received() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(silent.Received());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(!done);
        const auto cancelled = Clock::now();
        cancel = true;
        get.join();
        CHECK(Clock::now() - cancelled < std::chrono::seconds(1));
        CHECK(result.cancelled && !result.request.ok);
    }
}

// A server that never answers ends the request at its budget.
static void TestDeadline() {
    FakeReleaseServer silent("", true);
    const auto started = Clock::now();
    const auto result = Get(silent, 200);
    CHECK(!result.request.ok && result.request.error == sf4e::HttpErrorKind::Timeout);
    CHECK(Clock::now() - started < std::chrono::seconds(2));
}

int main() {
    WSADATA data;
    CHECK(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    TestCompleteAnswers();
    TestRedirectRefused();
    TestIncompleteAnswers();
    TestCancelBeforeStart();
    TestCancelWhileWaiting();
    TestDeadline();
    WSACleanup();
    std::printf("http_update_metadata_test: all tests passed\n");
    return 0;
}
