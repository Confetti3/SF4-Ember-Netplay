// spectator-handle-control.patch: ggpo_add_player reports a spectator's handle
// (queue + 1000; upstream left it unwritten), and the host can read that
// spectator's network stats and drop it by that handle without touching
// the fighters, and a dropped spectator no longer holds back initial sync.
// Ember uses this to cut a slow spectator loose before GGPO's own 63-frame
// pending-output limit, and to start the fighters when a spectator's link
// never finishes synchronizing.
#include <ggponet.h>
#include <winsock2.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "Failed line %d: %s\n", __LINE__, #c); std::exit(1); } } while (false)
static int running = 0, spectatorDisconnects = 0, fighterDisconnects = 0;
static bool __cdecl Begin(const char*) { return true; }
static bool __cdecl Save(unsigned char** buffer, int* len, int* checksum, int frame) {
    *buffer = static_cast<unsigned char*>(std::malloc(sizeof(int))); CHECK(*buffer);
    std::memcpy(*buffer, &frame, sizeof(frame)); *len = sizeof(frame); *checksum = frame; return true;
}
static bool __cdecl Load(unsigned char*, int) { return true; }
static bool __cdecl Log(char*, unsigned char*, int) { return true; }
static void __cdecl Free(void* buffer) { std::free(buffer); }
static bool __cdecl Advance(int) { return true; }
static bool __cdecl Event(GGPOEvent* event) {
    if (event->code == GGPO_EVENTCODE_RUNNING) ++running;
    if (event->code == GGPO_EVENTCODE_DISCONNECTED_FROM_PEER) {
        if (event->u.disconnected.player >= 1000) ++spectatorDisconnects; else ++fighterDisconnects;
    }
    return true;
}
static SOCKET BindLoopback(unsigned short* port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); CHECK(s != INVALID_SOCKET);
    sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    int size = sizeof(address); CHECK(getsockname(s, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    *port = ntohs(address.sin_port); u_long nonBlocking = 1; CHECK(ioctlsocket(s, FIONBIO, &nonBlocking) == 0);
    return s;
}
static unsigned short ReservePort() { unsigned short port; closesocket(BindLoopback(&port)); return port; }

// Drains the fake spectator's socket. With answerSync it completes the sync
// handshake (so the host reaches RUNNING) and then never acks input; without
// it the spectator never synchronizes at all.
static void ServeSpectator(SOCKET s, unsigned short hostPort, bool answerSync, int* syncReplies) {
    unsigned char packet[2048]; sockaddr_in from = {}; int fromLen = sizeof(from);
    for (;;) {
        const int received = recvfrom(s, reinterpret_cast<char*>(packet), sizeof(packet), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        fromLen = sizeof(from);
        if (received <= 0) return;
        // UdpMsg header: magic u16, sequence u16, type u8; SyncRequest carries random_request u32 at offset 5.
        if (!answerSync || received < 9 || packet[4] != 1) continue;
        unsigned char reply[9] = {}; const std::uint16_t magic = 0xBEEF; const std::uint16_t sequence = static_cast<std::uint16_t>(*syncReplies);
        std::memcpy(reply, &magic, 2); std::memcpy(reply + 2, &sequence, 2); reply[4] = 2; std::memcpy(reply + 5, packet + 5, 4);
        sockaddr_in host = {}; host.sin_family = AF_INET; host.sin_addr.s_addr = htonl(INADDR_LOOPBACK); host.sin_port = htons(hostPort);
        CHECK(sendto(s, reinterpret_cast<const char*>(reply), sizeof(reply), 0, reinterpret_cast<sockaddr*>(&host), sizeof(host)) == sizeof(reply));
        ++*syncReplies;
    }
}

struct Match {
    GGPOSession* host = nullptr; GGPOSession* remote = nullptr;
    GGPOPlayerHandle handles[2] = {}, spectator = GGPO_INVALID_HANDLE;
    SOCKET spectatorSocket = INVALID_SOCKET; unsigned short hostPort = 0; bool answerSync = false; int syncReplies = 0;
};

static Match Start(GGPOSessionCallbacks* callbacks, bool answerSync) {
    Match m; m.answerSync = answerSync;
    unsigned short spectatorPort = 0; m.spectatorSocket = BindLoopback(&spectatorPort);
    m.hostPort = ReservePort(); auto remotePort = ReservePort();
    while (remotePort == m.hostPort) remotePort = ReservePort();
    CHECK(ggpo_start_session(&m.host, callbacks, "spectator-control", 2, 1, m.hostPort) == GGPO_OK);
    CHECK(ggpo_start_session(&m.remote, callbacks, "spectator-control", 2, 1, remotePort) == GGPO_OK);
    for (int side = 0; side < 2; ++side) {
        GGPOPlayer player = {}; player.size = sizeof(player); player.type = GGPO_PLAYERTYPE_LOCAL; player.player_num = side + 1;
        auto* local = side == 0 ? m.host : m.remote;
        CHECK(ggpo_add_player(local, &player, &m.handles[side]) == GGPO_OK);
        CHECK(ggpo_set_frame_delay(local, m.handles[side], 0) == GGPO_OK);
        player.type = GGPO_PLAYERTYPE_REMOTE; strcpy_s(player.u.remote.ip_address, "127.0.0.1");
        player.u.remote.port = side == 0 ? m.hostPort : remotePort; GGPOPlayerHandle unused;
        CHECK(ggpo_add_player(side == 0 ? m.remote : m.host, &player, &unused) == GGPO_OK);
    }
    GGPOPlayer watcher = {}; watcher.size = sizeof(watcher); watcher.type = GGPO_PLAYERTYPE_SPECTATOR; watcher.player_num = 3;
    strcpy_s(watcher.u.remote.ip_address, "127.0.0.1"); watcher.u.remote.port = spectatorPort;
    CHECK(ggpo_add_player(m.host, &watcher, &m.spectator) == GGPO_OK);
    return m;
}

static void Pump(Match& m) {
    CHECK(ggpo_idle(m.host, 0) == GGPO_OK); CHECK(ggpo_idle(m.remote, 0) == GGPO_OK);
    ServeSpectator(m.spectatorSocket, m.hostPort, m.answerSync, &m.syncReplies);
}

static void Close(Match& m) {
    CHECK(ggpo_close_session(m.host) == GGPO_OK);
    CHECK(ggpo_close_session(m.remote) == GGPO_OK);
    closesocket(m.spectatorSocket);
}

static void Step(Match& m, int frame, bool waitForConfirmation) {
    unsigned char input = static_cast<unsigned char>(frame), inputs[2]; int disconnected = 0;
    CHECK(ggpo_add_local_input(m.host, m.handles[0], &input, 1) == GGPO_OK);
    CHECK(ggpo_add_local_input(m.remote, m.handles[1], &input, 1) == GGPO_OK);
    int confirmed = -1; const auto deadline = GetTickCount64() + 1000;
    do {
        Pump(m);
        CHECK(ggpo_get_last_confirmed_frame(m.host, &confirmed) == GGPO_OK);
    } while (waitForConfirmation && confirmed < frame && GetTickCount64() < deadline);
    CHECK(ggpo_synchronize_input(m.host, inputs, sizeof(inputs), &disconnected) == GGPO_OK && disconnected == 0);
    CHECK(ggpo_synchronize_input(m.remote, inputs, sizeof(inputs), &disconnected) == GGPO_OK && disconnected == 0);
    CHECK(ggpo_advance_frame(m.host) == GGPO_OK);
    CHECK(ggpo_advance_frame(m.remote) == GGPO_OK);
}

int main() {
    GGPOSessionCallbacks callbacks = {};
    callbacks.begin_game = Begin; callbacks.save_game_state = Save; callbacks.load_game_state = Load;
    callbacks.log_game_state = Log; callbacks.free_buffer = Free; callbacks.advance_frame = Advance; callbacks.on_event = Event;
    WSADATA winsock{}; CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    {
        // A spectator that never synchronizes holds back RUNNING until the host
        // drops it by handle. The fighters then start without it.
        auto m = Start(&callbacks, false);
        CHECK(m.spectator == 1000);
        const auto deadline = GetTickCount64() + 1500;
        while (GetTickCount64() < deadline) { Pump(m); Sleep(1); }
        CHECK(running == 1); // the remote fighter only; the host waits for its spectator
        GGPONetworkStats stats = {};
        CHECK(ggpo_get_network_stats(m.host, m.spectator, &stats) == GGPO_OK);
        CHECK(ggpo_get_network_stats(m.host, m.spectator + 1, &stats) == GGPO_ERRORCODE_INVALID_PLAYER_HANDLE);
        CHECK(ggpo_disconnect_player(m.host, m.spectator + 1) == GGPO_ERRORCODE_INVALID_PLAYER_HANDLE);
        CHECK(ggpo_disconnect_player(m.host, m.spectator) == GGPO_OK);
        CHECK(spectatorDisconnects == 1 && fighterDisconnects == 0);
        CHECK(running == 2);
        CHECK(ggpo_disconnect_player(m.host, m.spectator) == GGPO_ERRORCODE_PLAYER_DISCONNECTED);
        for (int frame = 0; frame < 30; ++frame) Step(m, frame, true);
        CHECK(fighterDisconnects == 0);
        Close(m);
    }
    running = spectatorDisconnects = fighterDisconnects = 0;
    {
        // A synchronized spectator that stops acking: its send_queue_len counts
        // the unacknowledged frames, so the host can drop it at a threshold of
        // its own choosing, long before GGPO's 63-frame limit.
        auto m = Start(&callbacks, true);
        const auto deadline = GetTickCount64() + 5000;
        while (running < 2 && GetTickCount64() < deadline) { Pump(m); Sleep(1); }
        CHECK(running == 2);
        int lastQueue = 0;
        for (int frame = 0; frame < 40; ++frame) {
            Step(m, frame, true);
            GGPONetworkStats stats = {};
            CHECK(ggpo_get_network_stats(m.host, m.spectator, &stats) == GGPO_OK);
            CHECK(stats.network.send_queue_len >= lastQueue);
            lastQueue = stats.network.send_queue_len;
        }
        CHECK(lastQueue >= 30 && lastQueue < 63);
        CHECK(ggpo_disconnect_player(m.host, m.spectator) == GGPO_OK);
        CHECK(spectatorDisconnects == 1 && fighterDisconnects == 0);
        for (int frame = 40; frame < 50; ++frame) Step(m, frame, true);
        CHECK(fighterDisconnects == 0);
        Close(m);
    }
    WSACleanup();
    std::printf("GGPO spectator handle control passed\n");
}
