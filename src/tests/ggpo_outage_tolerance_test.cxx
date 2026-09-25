// An rc1 match ended when the path between the two PCs went silent in both
// directions for about 5 s: GGPO disconnected the opponent after 3 s while
// room control rode the drop out. The sidecar now applies an 8 s tolerance
// (common/GgpoDisconnectTolerance.hxx). This test synchronises two real GGPO
// sessions with that tolerance, silences one for 6.5 s (past GGPO's own 5 s
// default) and requires the other to warn, not disconnect, and resume; then it
// silences it for good and requires the disconnect at about 8 s. The inputs
// never change, so GGPO never rolls back and only the timeout is measured.
#include <ggponet.h>
#include <winsock2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../common/GgpoDisconnectTolerance.hxx"
#include "test_support.hxx"

namespace Tolerance = sf4e::GgpoDisconnectTolerance;

namespace {
struct Events {
    int running = 0, interrupted = 0, resumed = 0, disconnected = 0;
    int interruptedTimeoutMs = -1;
    ULONGLONG disconnectedAt = 0;
};
Events hostEvents, remoteEvents;
GGPOSession* host = nullptr;
GGPOSession* remote = nullptr;

bool Record(Events& events, const GGPOEvent* event) {
    switch (event->code) {
    case GGPO_EVENTCODE_RUNNING: ++events.running; break;
    case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
        ++events.interrupted;
        events.interruptedTimeoutMs = event->u.connection_interrupted.disconnect_timeout;
        break;
    case GGPO_EVENTCODE_CONNECTION_RESUMED: ++events.resumed; break;
    case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
        if (!events.disconnected++) events.disconnectedAt = GetTickCount64();
        break;
    default: break;
    }
    return true;
}
bool __cdecl HostEvent(GGPOEvent* event) { return Record(hostEvents, event); }
bool __cdecl RemoteEvent(GGPOEvent* event) { return Record(remoteEvents, event); }
bool __cdecl HostAdvance(int) { CHECK(ggpo_advance_frame(host) == GGPO_OK); return true; }
bool __cdecl RemoteAdvance(int) { CHECK(ggpo_advance_frame(remote) == GGPO_OK); return true; }
bool __cdecl Begin(const char*) { return true; }
bool __cdecl Save(unsigned char** buffer, int* len, int* checksum, int frame) {
    *buffer = static_cast<unsigned char*>(std::malloc(sizeof(int))); CHECK(*buffer);
    std::memcpy(*buffer, &frame, sizeof(frame)); *len = sizeof(frame); *checksum = frame; return true;
}
bool __cdecl Load(unsigned char*, int) { return true; }
bool __cdecl Log(char*, unsigned char*, int) { return true; }
void __cdecl Free(void* buffer) { std::free(buffer); }

unsigned short ReservePort() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); CHECK(s != INVALID_SOCKET);
    sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    int size = sizeof(address); CHECK(getsockname(s, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    closesocket(s); return ntohs(address.sin_port);
}

// One game tick: poll, then advance a frame unless GGPO is at its prediction
// limit. Returns whether a frame advanced; `flags` gets the disconnect mask.
bool Step(GGPOSession* session, GGPOPlayerHandle local, int& flags) {
    CHECK(ggpo_idle(session, 0) == GGPO_OK);
    unsigned char input = 0;
    const GGPOErrorCode added = ggpo_add_local_input(session, local, &input, 1);
    if (added == GGPO_ERRORCODE_PREDICTION_THRESHOLD) return false;
    CHECK(added == GGPO_OK);
    unsigned char inputs[2];
    CHECK(ggpo_synchronize_input(session, inputs, sizeof(inputs), &flags) == GGPO_OK);
    CHECK(ggpo_advance_frame(session) == GGPO_OK);
    return true;
}
}

int main() {
    // The contract, pinned apart from the policy so an accidental change fails here.
    CHECK(Tolerance::TimeoutMs == 8000 && Tolerance::NotifyMs == 1500);

    GGPOSessionCallbacks hostCallbacks = {};
    hostCallbacks.begin_game = Begin; hostCallbacks.save_game_state = Save; hostCallbacks.load_game_state = Load;
    hostCallbacks.log_game_state = Log; hostCallbacks.free_buffer = Free;
    GGPOSessionCallbacks remoteCallbacks = hostCallbacks;
    hostCallbacks.advance_frame = HostAdvance; hostCallbacks.on_event = HostEvent;
    remoteCallbacks.advance_frame = RemoteAdvance; remoteCallbacks.on_event = RemoteEvent;

    WSADATA winsock{}; CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    const auto hostPort = ReservePort(); auto remotePort = ReservePort();
    while (remotePort == hostPort) remotePort = ReservePort();
    CHECK(ggpo_start_session(&host, &hostCallbacks, "outage-tolerance", 2, 1, hostPort) == GGPO_OK);
    CHECK(ggpo_start_session(&remote, &remoteCallbacks, "outage-tolerance", 2, 1, remotePort) == GGPO_OK);
    Tolerance::Apply(host);
    Tolerance::Apply(remote);
    GGPOPlayerHandle hostLocal = GGPO_INVALID_HANDLE, remoteLocal = GGPO_INVALID_HANDLE;
    for (int side = 0; side < 2; ++side) {
        GGPOPlayer player = {}; player.size = sizeof(player); player.type = GGPO_PLAYERTYPE_LOCAL; player.player_num = side + 1;
        auto* local = side == 0 ? host : remote;
        auto& localHandle = side == 0 ? hostLocal : remoteLocal;
        CHECK(ggpo_add_player(local, &player, &localHandle) == GGPO_OK);
        CHECK(ggpo_set_frame_delay(local, localHandle, 0) == GGPO_OK);
        player.type = GGPO_PLAYERTYPE_REMOTE; strcpy_s(player.u.remote.ip_address, "127.0.0.1");
        player.u.remote.port = side == 0 ? hostPort : remotePort; GGPOPlayerHandle handle;
        CHECK(ggpo_add_player(side == 0 ? remote : host, &player, &handle) == GGPO_OK);
    }

    auto deadline = GetTickCount64() + 5000;
    while ((hostEvents.running == 0 || remoteEvents.running == 0) && GetTickCount64() < deadline) {
        CHECK(ggpo_idle(host, 0) == GGPO_OK); CHECK(ggpo_idle(remote, 0) == GGPO_OK); Sleep(1);
    }
    CHECK(hostEvents.running == 1 && remoteEvents.running == 1);

    // Both fighters play until each has advanced `frames` more frames with
    // the opponent connected.
    const auto playTogether = [&](int frames) {
        int hostFrames = 0, remoteFrames = 0;
        const auto until = GetTickCount64() + 5000;
        while ((hostFrames < frames || remoteFrames < frames) && GetTickCount64() < until) {
            int flags = 0;
            if (Step(host, hostLocal, flags)) { CHECK(flags == 0); ++hostFrames; }
            if (Step(remote, remoteLocal, flags)) { CHECK(flags == 0); ++remoteFrames; }
            Sleep(1);
        }
        CHECK(hostFrames >= frames && remoteFrames >= frames);
    };
    // The host keeps its game loop running while the remote sends nothing.
    const auto silenceRemote = [&](ULONGLONG forMs, bool stopAtDisconnect) {
        const auto until = GetTickCount64() + forMs;
        while (GetTickCount64() < until && !(stopAtDisconnect && hostEvents.disconnected)) {
            int flags = 0; Step(host, hostLocal, flags); Sleep(1);
        }
    };
    playTogether(30);

    // Phase A: 6.5 s of silence warns, then resumes.
    silenceRemote(6500, false);
    CHECK(hostEvents.interrupted == 1);
    CHECK(hostEvents.interruptedTimeoutMs == 6500); // the 8 s timeout minus the 1.5 s notice
    CHECK(hostEvents.disconnected == 0);
    deadline = GetTickCount64() + 3000;
    while (hostEvents.resumed == 0 && GetTickCount64() < deadline) {
        int flags = 0; Step(host, hostLocal, flags); Step(remote, remoteLocal, flags); Sleep(1);
    }
    CHECK(hostEvents.resumed == 1);
    playTogether(30);
    CHECK(hostEvents.disconnected == 0 && remoteEvents.disconnected == 0);
    std::printf("A 6.5 s silence warned with %d ms left and resumed without a disconnect\n", hostEvents.interruptedTimeoutMs);

    // Phase B: a silence that does not end disconnects at about 8 s.
    const auto silenceStarted = GetTickCount64();
    silenceRemote(12000, true);
    CHECK(hostEvents.disconnected == 1);
    const auto disconnectedAfterMs = hostEvents.disconnectedAt - silenceStarted;
    CHECK(disconnectedAfterMs >= 7500 && disconnectedAfterMs <= 9500);
    std::printf("A lasting silence disconnected after %llu ms\n", disconnectedAfterMs);

    CHECK(ggpo_close_session(host) == GGPO_OK);
    CHECK(ggpo_close_session(remote) == GGPO_OK);
    WSACleanup();
}
