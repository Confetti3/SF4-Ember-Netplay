// Linux only: HelperClientPosix.cxx against plain pipes in place of a helper.
// A helper that stops draining its stdin must not hold the client past its
// deadline or past Stop(), including for a checkpoint-sized frame, which is
// far larger than the pipe's buffer.
#include "../platform/HelperClient.hxx"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

using namespace sf4e::platform;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

// A checkpoint chunk is 16 KiB of bytes in about 22 KiB of JSON; a frame of
// this size is many times the pipe buffer.
static const size_t FrameBytes = 256 * 1024;

struct Pipes {
    int toHelper[2] = {-1, -1}, fromHelper[2] = {-1, -1};
    Pipes() { if (pipe(toHelper) != 0 || pipe(fromHelper) != 0) { std::printf("pipe failed\n"); std::exit(2); } }
    ~Pipes() { for (int fd : {toHelper[0], toHelper[1], fromHelper[0], fromHelper[1]}) { if (fd >= 0) close(fd); } }
};

static bool WaitFor(const HelperClient& client, HelperState state, uint64_t ms) {
    const uint64_t until = GetTickCount64() + ms;
    while (client.State() != state && GetTickCount64() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    return client.State() == state;
}

int main() {
    // A watchdog, so a blocked client fails the run instead of hanging it.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(40));
        std::printf("FAIL: the client did not return (blocked write)\n");
        std::fflush(stdout);
        _exit(1);
    }).detach();

    // The reader never reads: Stop() must end the blocked frame promptly.
    {
        Pipes p;
        HelperClient client;
        CHECK(client.Start(p.fromHelper[0], p.toHelper[1]));
        p.fromHelper[0] = p.toHelper[1] = -1; // owned by the client now
        CHECK(client.Send(std::string(FrameBytes, 'x')));
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // the pipe is full
        CHECK(client.State() == HelperState::Connected);
        const uint64_t before = GetTickCount64();
        client.Stop();
        const uint64_t took = GetTickCount64() - before;
        std::printf("stalled reader: Stop took %llu ms\n", static_cast<unsigned long long>(took));
        CHECK(took < 2000);
        CHECK(client.State() == HelperState::Stopped);
        CHECK(client.LastError() == ECANCELED);
    }

    // A reader that drains late still gets the whole frame, byte for byte.
    {
        Pipes p;
        HelperClient client;
        CHECK(client.Start(p.fromHelper[0], p.toHelper[1]));
        p.fromHelper[0] = p.toHelper[1] = -1;
        std::string payload(FrameBytes, '\0');
        for (size_t i = 0; i < payload.size(); ++i) { payload[i] = static_cast<char>('a' + i % 26); }
        uint64_t id = 0;
        CHECK(client.Send(payload, &id));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::string received;
        char buffer[4096];
        const size_t total = 14 + payload.size();
        const uint64_t until = GetTickCount64() + 10000;
        while (received.size() < total && GetTickCount64() < until) {
            const ssize_t n = read(p.toHelper[0], buffer, sizeof buffer);
            if (n <= 0) { break; }
            received.append(buffer, static_cast<size_t>(n));
        }
        CHECK(received.size() == total);
        CHECK(received.size() == total && received.compare(14, std::string::npos, payload) == 0);
        CHECK(client.State() == HelperState::Connected);
        client.Stop();
    }

    // A helper that closed its stdin fails the write at once.
    {
        Pipes p;
        HelperClient client;
        CHECK(client.Start(p.fromHelper[0], p.toHelper[1]));
        p.fromHelper[0] = p.toHelper[1] = -1;
        close(p.toHelper[0]); p.toHelper[0] = -1;
        CHECK(client.Send(std::string(FrameBytes, 'x')));
        CHECK(WaitFor(client, HelperState::Failed, 2000));
        CHECK(client.LastError() == EPIPE);
        client.Stop();
    }

    // A helper that holds its stdin open but never reads: the deadline ends it.
    {
        Pipes p;
        HelperClient client;
        CHECK(client.Start(p.fromHelper[0], p.toHelper[1]));
        p.fromHelper[0] = p.toHelper[1] = -1;
        const uint64_t before = GetTickCount64();
        CHECK(client.Send(std::string(FrameBytes, 'x')));
        CHECK(WaitFor(client, HelperState::Failed, 20000));
        const uint64_t took = GetTickCount64() - before;
        std::printf("stalled reader: write failed after %llu ms\n", static_cast<unsigned long long>(took));
        CHECK(took >= 14000 && took < 17000);
        CHECK(client.LastError() == ETIMEDOUT);
        client.Stop();
    }

    // A frame from the helper ends WaitForIncoming at once, long before its
    // timeout, and a wait with nothing arriving lasts the timeout: the room
    // host sleeps on this instead of ticking.
    {
        Pipes p;
        HelperClient client;
        CHECK(client.Start(p.fromHelper[0], p.toHelper[1]));
        p.fromHelper[0] = p.toHelper[1] = -1;
        uint64_t before = GetTickCount64();
        client.WaitForIncoming(200);
        uint64_t took = GetTickCount64() - before;
        CHECK(took >= 150 && took < 1000);
        const std::string payload = "{\"type\":\"ready\"}";
        std::string frame(14, '\0');
        const uint32_t length = static_cast<uint32_t>(10 + payload.size());
        for (int i = 0; i < 4; ++i) { frame[i] = static_cast<char>(length >> (24 - 8 * i)); }
        frame[5] = 1;
        frame[13] = 2; // message id 2
        frame += payload;
        std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            CHECK(write(p.fromHelper[1], frame.data(), frame.size()) == static_cast<ssize_t>(frame.size()));
        }).detach();
        before = GetTickCount64();
        client.WaitForIncoming(5000);
        took = GetTickCount64() - before;
        std::printf("event: WaitForIncoming returned after %llu ms\n", static_cast<unsigned long long>(took));
        CHECK(took >= 50 && took < 1000);
        HelperMessage message;
        CHECK(client.TryReceive(message));
        CHECK(message.id == 2 && message.payload == payload);
        CHECK(!client.TryReceive(message));
        // Nothing new: the next wait runs out.
        before = GetTickCount64();
        client.WaitForIncoming(100);
        took = GetTickCount64() - before;
        CHECK(took >= 80);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        client.Stop();
    }

    std::printf(failures ? "helper_client_posix_test: %d failure(s)\n" : "helper_client_posix_test: ok\n", failures);
    return failures ? 1 : 0;
}
