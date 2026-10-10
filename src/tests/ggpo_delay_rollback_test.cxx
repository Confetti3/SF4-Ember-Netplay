// Real GGPO, synthetic state: this does not exercise USFIV's mementos or renderer.
// Arguments: P1 delay, P2 delay, one-way ms, jitter ms, drop every N, frames,
// input hold frames, corrupt restore (negative control). Both peers tick at 60 Hz.
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <ggponet.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "test_support.hxx"

using Clock = std::chrono::steady_clock;
struct State { int frame = 0; std::uint64_t digest = 1469598103934665603ULL; };
struct Peer {
    GGPOSession* session = nullptr;
    GGPOPlayerHandle local = GGPO_INVALID_HANDLE;
    State state;
    bool running = false;
    int loads = 0, replay = 0, depth = 0, stalls = 0, zeroLoads = 0;
};
static Peer* active;
static bool corruptRestore;
static void Mix(State& state, const unsigned char* inputs) {
    state.digest = (state.digest ^ static_cast<unsigned>(state.frame)) * 1099511628211ULL;
    for (int side = 0; side < 2; ++side) state.digest = (state.digest ^ inputs[side]) * 1099511628211ULL;
    ++state.frame;
}
static unsigned char Input(int frame, int side, int hold) {
    return static_cast<unsigned char>(1 + ((frame / hold) * (side ? 31 : 17) + side * 67) % 255);
}
static bool __cdecl Begin(const char*) { return true; }
static bool __cdecl Save(unsigned char** buffer, int* length, int* checksum, int frame) {
    CHECK(active->state.frame == frame);
    *buffer = static_cast<unsigned char*>(std::malloc(sizeof(State))); CHECK(*buffer);
    std::memcpy(*buffer, &active->state, sizeof(State)); *length = sizeof(State);
    *checksum = static_cast<int>(active->state.digest); return true;
}
static bool __cdecl Load(unsigned char* buffer, int length) {
    CHECK(length == sizeof(State)); State saved; std::memcpy(&saved, buffer, sizeof(saved));
    ++active->loads; if (!saved.frame) ++active->zeroLoads;
    active->depth = (std::max)(active->depth, active->state.frame - saved.frame);
    const auto wrongDigest = active->state.digest;
    active->state = saved;
    if (corruptRestore) active->state.digest = wrongDigest;
    return true;
}
static bool __cdecl Log(char*, unsigned char*, int) { return true; }
static void __cdecl Free(void* buffer) { std::free(buffer); }
static void Advance() {
    unsigned char inputs[2]{}; int disconnected = 0;
    CHECK(ggpo_synchronize_input(active->session, inputs, sizeof(inputs), &disconnected) == GGPO_OK);
    CHECK(disconnected == 0); Mix(active->state, inputs);
    CHECK(ggpo_advance_frame(active->session) == GGPO_OK);
}
static bool __cdecl Replay(int) { ++active->replay; Advance(); return true; }
static bool __cdecl Event(GGPOEvent* event) {
    if (event->code == GGPO_EVENTCODE_RUNNING) active->running = true;
    CHECK(event->code != GGPO_EVENTCODE_DISCONNECTED_FROM_PEER);
    return true;
}
static SOCKET BoundSocket(unsigned short& port) {
    SOCKET socketValue = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); CHECK(socketValue != INVALID_SOCKET);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(socketValue, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    int size = sizeof(address); CHECK(getsockname(socketValue, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    port = ntohs(address.sin_port); return socketValue;
}
// Single-threaded packet proxy pumped between game ticks. Delay is per direction,
// not RTT. Jitter can reorder datagrams; drops start after session synchronization.
struct Proxy {
    struct Packet { Clock::time_point due; sockaddr_in target; std::vector<char> bytes; };
    SOCKET socketValue;
    unsigned short port = 0, peers[2]{};
    int latency, jitter, dropEvery;
    unsigned sequence[2]{};
    unsigned received = 0, dropped = 0;
    bool armed = false;
    std::vector<Packet> pending;
    Proxy(int latencyMs, int jitterMs, int drop) : latency(latencyMs), jitter(jitterMs), dropEvery(drop) {
        socketValue = BoundSocket(port); u_long nonblocking = 1;
        CHECK(ioctlsocket(socketValue, FIONBIO, &nonblocking) == 0);
    }
    ~Proxy() { closesocket(socketValue); }
    void Pump() {
        for (;;) {
            char bytes[4096]; sockaddr_in source{}; int size = sizeof(source);
            const int length = recvfrom(socketValue, bytes, sizeof(bytes), 0, reinterpret_cast<sockaddr*>(&source), &size);
            if (length == SOCKET_ERROR) { CHECK(WSAGetLastError() == WSAEWOULDBLOCK); break; }
            const int side = ntohs(source.sin_port) == peers[0] ? 0 : 1;
            CHECK(ntohs(source.sin_port) == peers[side]); ++received;
            const auto number = ++sequence[side];
            if (armed && dropEvery && (number + side * 7) % dropEvery == 0) { ++dropped; continue; }
            const auto extra = jitter ? ((number * 2654435761u + side * 101u) % (jitter + 1)) : 0;
            Packet packet; packet.due = Clock::now() + std::chrono::milliseconds(armed ? latency + extra : 0);
            packet.target = {}; packet.target.sin_family = AF_INET;
            packet.target.sin_addr.s_addr = htonl(INADDR_LOOPBACK); packet.target.sin_port = htons(peers[1 - side]);
            packet.bytes.assign(bytes, bytes + length); pending.push_back(std::move(packet));
        }
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->due > Clock::now()) { ++it; continue; }
            CHECK(sendto(socketValue, it->bytes.data(), static_cast<int>(it->bytes.size()), 0,
                reinterpret_cast<sockaddr*>(&it->target), sizeof(it->target)) == static_cast<int>(it->bytes.size()));
            it = pending.erase(it);
        }
    }
};
int main(int argc, char** argv) {
    const auto arg = [&](int index, int fallback) { return argc > index ? std::atoi(argv[index]) : fallback; };
    const int delays[2]{arg(1, 0), arg(2, 5)};
    const int latency = arg(3, 25), jitter = arg(4, 10), drop = arg(5, 17), frames = arg(6, 180), hold = arg(7, 8);
    corruptRestore = arg(8, 0) != 0;
    CHECK(delays[0] >= 0 && delays[0] <= 10 && delays[1] >= 0 && delays[1] <= 10);
    CHECK(latency >= 0 && jitter >= 0 && drop >= 0 && frames > 0 && hold > 0);
    WSADATA winsock{}; CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0); timeBeginPeriod(1);
    bool passed = false;
    {
        Proxy proxy(latency, jitter, drop); Peer peers[2];
        for (auto& port : proxy.peers) { const auto socketValue = BoundSocket(port); closesocket(socketValue); }
        CHECK(proxy.peers[0] != proxy.peers[1]);
        GGPOSessionCallbacks callbacks{};
        callbacks.begin_game = Begin; callbacks.save_game_state = Save; callbacks.load_game_state = Load;
        callbacks.log_game_state = Log; callbacks.free_buffer = Free; callbacks.advance_frame = Replay; callbacks.on_event = Event;
        for (int side = 0; side < 2; ++side) {
            active = &peers[side];
            CHECK(ggpo_start_session(&active->session, &callbacks, "delay-rollback", 2, 1, proxy.peers[side]) == GGPO_OK);
            GGPOPlayer player{}; player.size = sizeof(player); player.type = GGPO_PLAYERTYPE_LOCAL; player.player_num = side + 1;
            CHECK(ggpo_add_player(active->session, &player, &active->local) == GGPO_OK);
            CHECK(ggpo_set_frame_delay(active->session, active->local, delays[side]) == GGPO_OK);
            player.type = GGPO_PLAYERTYPE_REMOTE; player.player_num = 2 - side;
            strcpy_s(player.u.remote.ip_address, "127.0.0.1"); player.u.remote.port = proxy.port;
            GGPOPlayerHandle unused; CHECK(ggpo_add_player(active->session, &player, &unused) == GGPO_OK);
        }
        const auto poll = [&]() {
            proxy.Pump();
            for (auto& peer : peers) { active = &peer; CHECK(ggpo_idle(peer.session, 0) == GGPO_OK); }
        };
        auto deadline = Clock::now() + std::chrono::seconds(10);
        while ((!peers[0].running || !peers[1].running) && Clock::now() < deadline) { poll(); Sleep(1); }
        CHECK(peers[0].running && peers[1].running); proxy.armed = true;
        const auto started = Clock::now(); auto next = started;
        deadline = started + std::chrono::milliseconds(frames * 1000 / 60 + 30000);
        int ticks = 0, overruns = 0;
        while ((peers[0].state.frame < frames || peers[1].state.frame < frames) && Clock::now() < deadline) {
            proxy.Pump();
            if (Clock::now() < next) { Sleep(1); continue; }
            ++ticks; poll();
            // Alternate ordering so neither seat systematically gets polled first.
            for (int i = 0; i < 2; ++i) {
                const int side = (i + ticks) % 2; active = &peers[side];
                if (active->state.frame >= frames) continue;
                unsigned char input = Input(active->state.frame, side, hold);
                const auto code = ggpo_add_local_input(active->session, active->local, &input, 1);
                if (code == GGPO_ERRORCODE_PREDICTION_THRESHOLD) { ++active->stalls; continue; }
                CHECK(code == GGPO_OK); Advance();
            }
            poll(); next += std::chrono::nanoseconds(16666667);
            if (next < Clock::now()) { ++overruns; next = Clock::now(); }
        }
        CHECK(peers[0].state.frame == frames && peers[1].state.frame == frames);
        int confirmed[2]{-1, -1}; deadline = Clock::now() + std::chrono::seconds(10);
        do {
            poll();
            for (int side = 0; side < 2; ++side) {
                active = &peers[side]; CHECK(ggpo_get_last_confirmed_frame(active->session, &confirmed[side]) == GGPO_OK);
            }
            if (confirmed[0] >= frames - 1 && confirmed[1] >= frames - 1) break;
            Sleep(1);
        } while (Clock::now() < deadline);
        State oracle;
        for (int frame = 0; frame < frames; ++frame) {
            unsigned char inputs[2];
            for (int side = 0; side < 2; ++side) inputs[side] = frame < delays[side] ? 0 : Input(frame - delays[side], side, hold);
            Mix(oracle, inputs);
        }
        passed = confirmed[0] >= frames - 1 && confirmed[1] >= frames - 1 &&
            peers[0].state.digest == oracle.digest && peers[1].state.digest == oracle.digest;
        std::printf("{\"passed\":%s,\"native_gameplay\":false,\"delays\":[%d,%d],\"one_way_ms\":%d,\"jitter_ms\":%d,"
            "\"drop_every\":%d,\"frames\":%d,\"hold\":%d,\"corrupt_restore\":%s,\"ticks\":%d,\"overruns\":%d,"
            "\"elapsed_ms\":%lld,\"received\":%u,\"dropped\":%u,\"oracle\":\"%llu\",\"peers\":[",
            passed ? "true" : "false", delays[0], delays[1], latency, jitter, drop, frames, hold,
            corruptRestore ? "true" : "false", ticks, overruns,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()),
            proxy.received, proxy.dropped, static_cast<unsigned long long>(oracle.digest));
        for (int side = 0; side < 2; ++side) {
            active = &peers[side];
            std::printf("%s{\"digest\":\"%llu\",\"confirmed\":%d,\"loads\":%d,\"replay_frames\":%d,\"max_depth\":%d,\"stalls\":%d,\"frame_zero_loads\":%d}",
                side ? "," : "", static_cast<unsigned long long>(active->state.digest), confirmed[side],
                active->loads, active->replay, active->depth, active->stalls, active->zeroLoads);
            CHECK(ggpo_close_session(active->session) == GGPO_OK);
        }
        std::puts("]}");
    }
    timeEndPeriod(1); WSACleanup(); return passed ? 0 : 1;
}
