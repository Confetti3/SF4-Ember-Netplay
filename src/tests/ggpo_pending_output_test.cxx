// A GGPO endpoint whose peer stops acking used to overflow the 64-entry
// pending-output ring after 63 confirmed frames and assert (ring_buffer.h:39),
// which killed the host mid-fight. The pending-output-disconnect port patch
// turns that into the ordinary "endpoint disconnected" path. This test runs a
// real two-player session plus a spectator that completes the sync handshake
// and then never acks: the host must drop that spectator and keep fighting.
#include <ggponet.h>
#include <winsock2.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "test_support.hxx"
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
        // Peer2PeerBackend::QueueToSpectatorHandle is queue + 1000.
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

// The fake spectator: answers every SyncRequest so the host reaches RUNNING,
// then swallows everything else without ever sending an InputAck.
static void ServeSilentSpectator(SOCKET s, unsigned short hostPort, int* syncReplies) {
    unsigned char packet[2048]; sockaddr_in from = {}; int fromLen = sizeof(from);
    for (;;) {
        const int received = recvfrom(s, reinterpret_cast<char*>(packet), sizeof(packet), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        fromLen = sizeof(from);
        if (received <= 0) return;
        // UdpMsg header: magic u16, sequence u16, type u8; SyncRequest carries random_request u32 at offset 5.
        if (received < 9 || packet[4] != 1) continue;
        unsigned char reply[9] = {}; const std::uint16_t magic = 0xBEEF; const std::uint16_t sequence = static_cast<std::uint16_t>(*syncReplies);
        std::memcpy(reply, &magic, 2); std::memcpy(reply + 2, &sequence, 2); reply[4] = 2; std::memcpy(reply + 5, packet + 5, 4);
        sockaddr_in host = {}; host.sin_family = AF_INET; host.sin_addr.s_addr = htonl(INADDR_LOOPBACK); host.sin_port = htons(hostPort);
        CHECK(sendto(s, reinterpret_cast<const char*>(reply), sizeof(reply), 0, reinterpret_cast<sockaddr*>(&host), sizeof(host)) == sizeof(reply));
        ++*syncReplies;
    }
}

int main() {
    GGPOSessionCallbacks callbacks = {};
    callbacks.begin_game = Begin; callbacks.save_game_state = Save; callbacks.load_game_state = Load;
    callbacks.log_game_state = Log; callbacks.free_buffer = Free; callbacks.advance_frame = Advance; callbacks.on_event = Event;
    WSADATA winsock{}; CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    unsigned short spectatorPort = 0; const SOCKET spectator = BindLoopback(&spectatorPort);
    const auto hostPort = ReservePort(); auto remotePort = ReservePort();
    while (remotePort == hostPort) remotePort = ReservePort();
    GGPOSession* host = nullptr; GGPOSession* remote = nullptr;
    CHECK(ggpo_start_session(&host, &callbacks, "pending-output", 2, 1, hostPort) == GGPO_OK);
    CHECK(ggpo_start_session(&remote, &callbacks, "pending-output", 2, 1, remotePort) == GGPO_OK);
    GGPOPlayerHandle handles[2];
    for (int side = 0; side < 2; ++side) {
        GGPOPlayer player = {}; player.size = sizeof(player); player.type = GGPO_PLAYERTYPE_LOCAL; player.player_num = side + 1;
        auto* local = side == 0 ? host : remote;
        CHECK(ggpo_add_player(local, &player, &handles[side]) == GGPO_OK);
        CHECK(ggpo_set_frame_delay(local, handles[side], 0) == GGPO_OK);
        player.type = GGPO_PLAYERTYPE_REMOTE; strcpy_s(player.u.remote.ip_address, "127.0.0.1");
        player.u.remote.port = side == 0 ? hostPort : remotePort; GGPOPlayerHandle unused;
        CHECK(ggpo_add_player(side == 0 ? remote : host, &player, &unused) == GGPO_OK);
    }
    {
        GGPOPlayer watcher = {}; watcher.size = sizeof(watcher); watcher.type = GGPO_PLAYERTYPE_SPECTATOR; watcher.player_num = 3;
        strcpy_s(watcher.u.remote.ip_address, "127.0.0.1"); watcher.u.remote.port = spectatorPort; GGPOPlayerHandle unused;
        CHECK(ggpo_add_player(host, &watcher, &unused) == GGPO_OK);
    }
    // No disconnect timeout on purpose: only the full pending-output queue may drop the spectator.
    int syncReplies = 0;
    const auto pump = [&]() {
        CHECK(ggpo_idle(host, 0) == GGPO_OK); CHECK(ggpo_idle(remote, 0) == GGPO_OK);
        ServeSilentSpectator(spectator, hostPort, &syncReplies);
    };
    const auto deadline = GetTickCount64() + 5000;
    while (running < 2 && GetTickCount64() < deadline) { pump(); Sleep(1); }
    CHECK(running == 2); CHECK(syncReplies >= 5);
    int dropFrame = -1;
    for (int frame = 0; frame < 150; ++frame) {
        unsigned char input = static_cast<unsigned char>(frame), inputs[2]; int disconnected = 0;
        CHECK(ggpo_add_local_input(host, handles[0], &input, 1) == GGPO_OK);
        CHECK(ggpo_add_local_input(remote, handles[1], &input, 1) == GGPO_OK);
        int confirmed = -1, remoteConfirmed = -1; const auto frameDeadline = GetTickCount64() + 1000;
        do {
            pump();
            CHECK(ggpo_get_last_confirmed_frame(host, &confirmed) == GGPO_OK);
            CHECK(ggpo_get_last_confirmed_frame(remote, &remoteConfirmed) == GGPO_OK);
            if (confirmed < frame || remoteConfirmed < frame) Sleep(1);
        } while ((confirmed < frame || remoteConfirmed < frame) && GetTickCount64() < frameDeadline);
        CHECK(confirmed == frame && remoteConfirmed == frame);
        CHECK(ggpo_synchronize_input(host, inputs, sizeof(inputs), &disconnected) == GGPO_OK);
        CHECK(ggpo_synchronize_input(remote, inputs, sizeof(inputs), &disconnected) == GGPO_OK);
        CHECK(disconnected == 0);
        CHECK(ggpo_advance_frame(host) == GGPO_OK);
        CHECK(ggpo_advance_frame(remote) == GGPO_OK);
        if (dropFrame < 0 && spectatorDisconnects) dropFrame = frame;
    }
    // The host must have dropped exactly the spectator, no earlier than the ring
    // could fill and well before the fight ended, and must never touch a fighter.
    CHECK(spectatorDisconnects == 1);
    CHECK(fighterDisconnects == 0);
    CHECK(dropFrame >= 60 && dropFrame < 100);
    CHECK(ggpo_close_session(host) == GGPO_OK);
    CHECK(ggpo_close_session(remote) == GGPO_OK);
    closesocket(spectator); WSACleanup();
    std::printf("GGPO pending-output overflow dropped the silent spectator at frame %d and the fight continued\n", dropFrame);
}
