// HttpPostReport's cancel against a local server that never answers: a cancel
// must end a post waiting on the answer at once, not after its timeout, so a
// window closed during a report upload lets the launcher exit. The posts go
// through the loopback seam, which runs the same upload over plain HTTP.

#include <winsock2.h>
#include <ws2tcpip.h>

#include "../common/sf4e__NetUtil.hxx"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "loopback_server.hxx"
#include "test_support.hxx"

using Clock = std::chrono::steady_clock;

namespace {
// One connection at a time on 127.0.0.1. It reads a whole request, then
// answers 202 when `answer` is set, or holds the connection open, silent,
// until the client goes.
class FakeIntake {
public:
    explicit FakeIntake(bool answer) : server_([this, answer](SOCKET client) { Serve(client, answer); }) {}
    int Port() const { return server_.Port(); }
    bool Received() const { return received_.load(); }
    int Connections() const { return server_.Connections(); }
private:
    void Serve(SOCKET client, bool answer) {
        std::string request;
        char buffer[4096];
        std::size_t expected = std::string::npos;
        for (;;) {
            const int got = recv(client, buffer, sizeof(buffer), 0);
            if (got <= 0) return;
            request.append(buffer, static_cast<std::size_t>(got));
            const auto head = request.find("\r\n\r\n");
            if (head == std::string::npos) continue;
            if (expected == std::string::npos) {
                const auto at = request.find("Content-Length: ");
                expected = head + 4 + (at == std::string::npos ? 0 : std::strtoul(request.c_str() + at + 16, nullptr, 10));
            }
            if (request.size() < expected) continue;
            received_ = true;
            if (answer) {
                const std::string reply = "HTTP/1.1 202 Accepted\r\nContent-Type: application/json\r\nContent-Length: 41\r\n"
                    "Connection: close\r\n\r\n{\"id\":\"0123456789abcdef0123456789abcdef\"}";
                send(client, reply.data(), static_cast<int>(reply.size()), 0);
                return;
            }
            // Silent: wait for the client to give up or go.
            while (recv(client, buffer, sizeof(buffer), 0) > 0) {}
            return;
        }
    }
    std::atomic<bool> received_{false};
    // Last, so it only starts serving once the rest is built.
    sf4e_test::LoopbackServer server_;
};

sf4e::HttpResponse Post(int port, const std::atomic<bool>* cancel) {
    // The upload's own waits are 20 seconds, so only a cancel ends a silent post quickly.
    return sf4e::testing::HttpPostReportLoopback(port, "multipart/form-data; boundary=test", std::string(200 * 1024, 'x'),
        [cancel] { return cancel && cancel->load(); });
}
}

// A post waiting on an answer that never comes ends within a second of the cancel.
static void TestCancelEndsAPendingAnswer() {
    FakeIntake intake(false);
    std::atomic<bool> cancel{false}, done{false};
    sf4e::HttpResponse result;
    std::thread post([&] { result = Post(intake.Port(), &cancel); done = true; });
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!intake.Received() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(intake.Received());
    // The whole body is in; the post now waits for the answer.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!done);
    const auto cancelled = Clock::now();
    cancel = true;
    post.join();
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - cancelled).count();
    std::printf("post ended %lld ms after the cancel\n", static_cast<long long>(took));
    CHECK(took < 1000);
    CHECK(result.cancelled && !result.request.ok && result.body.empty());
}

// Cancelled before it starts, a post never connects.
static void TestCancelBeforeStart() {
    FakeIntake intake(false);
    std::atomic<bool> cancel{true};
    const auto started = Clock::now();
    const auto result = Post(intake.Port(), &cancel);
    CHECK(result.cancelled && !result.request.ok);
    CHECK(Clock::now() - started < std::chrono::seconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(intake.Connections() == 0);
}

// An answered post reads the answer; a cancel that comes afterwards changes nothing.
static void TestAnsweredPost() {
    FakeIntake intake(true);
    std::atomic<bool> cancel{false};
    auto result = Post(intake.Port(), &cancel);
    CHECK(result.request.ok && result.request.statusCode == 202 && !result.cancelled);
    CHECK(result.body.find("0123456789abcdef0123456789abcdef") != std::string::npos);
    cancel = true;
    CHECK(result.request.ok);
    // Without a cancel the post works the same.
    result = Post(intake.Port(), nullptr);
    CHECK(result.request.ok && result.request.statusCode == 202);
    // The seam is loopback only and takes the same arguments as the intake.
    CHECK(sf4e::testing::HttpPostReportLoopback(0, "multipart/form-data", "x", {}).request.error == sf4e::HttpErrorKind::InvalidArgs);
    CHECK(sf4e::testing::HttpPostReportLoopback(intake.Port(), "a\r\nb", "x", {}).request.error == sf4e::HttpErrorKind::InvalidArgs);
}

int main() {
    WSADATA data;
    CHECK(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    TestCancelBeforeStart();
    TestAnsweredPost();
    TestCancelEndsAPendingAnswer();
    WSACleanup();
    std::printf("http_post_cancel_test: all tests passed\n");
    return 0;
}
