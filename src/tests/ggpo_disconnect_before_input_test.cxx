// A peer that drops before any of its inputs arrived (round start, "Waiting
// for opponent") has last_frame -1. Upstream GGPO rolled the simulation back
// to that frame, found no saved state and asserted (sync.cpp:238), which
// killed the game. The disconnect-before-input port patch clamps the rollback
// to frame 0. This test synchronises two real sessions, advances the host
// with no remote input, then disconnects the remote player: the host must get
// one ordinary disconnect event and keep advancing.
#include <ggponet.h>
#include <winsock2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "Failed line %d: %s\n", __LINE__, #c); std::exit(1); } } while (false)
static int running = 0, fighterDisconnects = 0, spectatorDisconnects = 0, loads = 0, loadedFrame = -1, replays = 0;
// The session whose rollback is being replayed; a real game calls
// ggpo_advance_frame from its advance callback, and GGPO asserts if not.
static GGPOSession* rollbackSession = nullptr;
static bool __cdecl Begin(const char*) { return true; }
static bool __cdecl Save(unsigned char** buffer, int* len, int* checksum, int frame) {
    *buffer = static_cast<unsigned char*>(std::malloc(sizeof(int))); CHECK(*buffer);
    std::memcpy(*buffer, &frame, sizeof(frame)); *len = sizeof(frame); *checksum = frame; return true;
}
static bool __cdecl Load(unsigned char* buffer, int len) {
    CHECK(len == sizeof(int)); std::memcpy(&loadedFrame, buffer, sizeof(loadedFrame)); ++loads; return true;
}
static bool __cdecl Log(char*, unsigned char*, int) { return true; }
static void __cdecl Free(void* buffer) { std::free(buffer); }
static bool __cdecl Advance(int) {
    ++replays;
    if (rollbackSession) CHECK(ggpo_advance_frame(rollbackSession) == GGPO_OK);
    return true;
}
static bool __cdecl Event(GGPOEvent* event) {
    if (event->code == GGPO_EVENTCODE_RUNNING) ++running;
    if (event->code == GGPO_EVENTCODE_DISCONNECTED_FROM_PEER) {
        if (event->u.disconnected.player >= 1000) ++spectatorDisconnects; else ++fighterDisconnects;
    }
    return true;
}
static unsigned short ReservePort() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); CHECK(s != INVALID_SOCKET);
    sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    int size = sizeof(address); CHECK(getsockname(s, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    closesocket(s); return ntohs(address.sin_port);
}

int main() {
    GGPOSessionCallbacks callbacks = {};
    callbacks.begin_game = Begin; callbacks.save_game_state = Save; callbacks.load_game_state = Load;
    callbacks.log_game_state = Log; callbacks.free_buffer = Free; callbacks.advance_frame = Advance; callbacks.on_event = Event;
    WSADATA winsock{}; CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    const auto hostPort = ReservePort(); auto remotePort = ReservePort();
    while (remotePort == hostPort) remotePort = ReservePort();
    GGPOSession* host = nullptr; GGPOSession* remote = nullptr;
    CHECK(ggpo_start_session(&host, &callbacks, "disconnect-before-input", 2, 1, hostPort) == GGPO_OK);
    CHECK(ggpo_start_session(&remote, &callbacks, "disconnect-before-input", 2, 1, remotePort) == GGPO_OK);
    GGPOPlayerHandle localHandles[2], hostRemoteHandle = GGPO_INVALID_HANDLE;
    for (int side = 0; side < 2; ++side) {
        GGPOPlayer player = {}; player.size = sizeof(player); player.type = GGPO_PLAYERTYPE_LOCAL; player.player_num = side + 1;
        auto* local = side == 0 ? host : remote;
        CHECK(ggpo_add_player(local, &player, &localHandles[side]) == GGPO_OK);
        CHECK(ggpo_set_frame_delay(local, localHandles[side], 0) == GGPO_OK);
        player.type = GGPO_PLAYERTYPE_REMOTE; strcpy_s(player.u.remote.ip_address, "127.0.0.1");
        player.u.remote.port = side == 0 ? hostPort : remotePort; GGPOPlayerHandle handle;
        CHECK(ggpo_add_player(side == 0 ? remote : host, &player, &handle) == GGPO_OK);
        if (side == 1) hostRemoteHandle = handle;
    }
    const auto pump = [&]() { CHECK(ggpo_idle(host, 0) == GGPO_OK); CHECK(ggpo_idle(remote, 0) == GGPO_OK); };
    const auto deadline = GetTickCount64() + 5000;
    while (running < 2 && GetTickCount64() < deadline) { pump(); Sleep(1); }
    CHECK(running == 2);
    // The remote is synchronised but never adds an input, so the host's last
    // received frame for it stays -1. Stay inside the prediction barrier.
    unsigned char inputs[2]; int disconnected = 0;
    for (int frame = 0; frame < 6; ++frame) {
        unsigned char input = static_cast<unsigned char>(frame);
        pump();
        CHECK(ggpo_add_local_input(host, localHandles[0], &input, 1) == GGPO_OK);
        CHECK(ggpo_synchronize_input(host, inputs, sizeof(inputs), &disconnected) == GGPO_OK);
        CHECK(disconnected == 0);
        CHECK(ggpo_advance_frame(host) == GGPO_OK);
    }
    rollbackSession = host;
    CHECK(ggpo_disconnect_player(host, hostRemoteHandle) == GGPO_OK);
    rollbackSession = nullptr;
    CHECK(fighterDisconnects == 1);
    CHECK(spectatorDisconnects == 0);
    // Unpatched GGPO asked for frame -1 here and died; the patch rolls back to
    // the saved frame 0 and replays the six frames with the peer's input zeroed.
    CHECK(loads == 1 && loadedFrame == 0);
    CHECK(replays == 6);
    for (int frame = 6; frame < 9; ++frame) {
        unsigned char input = static_cast<unsigned char>(frame);
        pump();
        CHECK(ggpo_add_local_input(host, localHandles[0], &input, 1) == GGPO_OK);
        CHECK(ggpo_synchronize_input(host, inputs, sizeof(inputs), &disconnected) == GGPO_OK);
        CHECK(disconnected == (1 << 1));
        CHECK(ggpo_advance_frame(host) == GGPO_OK);
    }
    CHECK(ggpo_close_session(host) == GGPO_OK);
    CHECK(ggpo_close_session(remote) == GGPO_OK);
    WSACleanup();
    std::printf("GGPO disconnect before any remote input rolled back to frame 0 and the host kept advancing\n");
}
