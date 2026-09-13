#include <ggponet.h>
#include <winsock2.h>
#include "../common/ConfirmedCheckpoint.hxx"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "Failed line %d: %s\n", __LINE__, #c); std::exit(1); } } while (false)
static int saveFrame = -1, saves = 0, running = 0;
static bool __cdecl Begin(const char*) { return true; }
static bool __cdecl Save(unsigned char** buffer, int* len, int* checksum, int frame) {
    *buffer = static_cast<unsigned char*>(std::malloc(sizeof(int))); CHECK(*buffer);
    std::memcpy(*buffer, &frame, sizeof(frame)); *len = sizeof(frame); *checksum = frame;
    saveFrame = frame; ++saves; return true;
}
static bool __cdecl Load(unsigned char*, int) { return true; }
static bool __cdecl Log(char*, unsigned char*, int) { return true; }
static void __cdecl Free(void* buffer) { std::free(buffer); }
static bool __cdecl Advance(int) { return true; }
static bool __cdecl Event(GGPOEvent* event) { if (event->code == GGPO_EVENTCODE_RUNNING) ++running; return true; }
static unsigned short ReservePort() {
    SOCKET socketValue = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); CHECK(socketValue != INVALID_SOCKET);
    sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(socketValue, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    int size = sizeof(address); CHECK(getsockname(socketValue, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    const auto port = ntohs(address.sin_port); closesocket(socketValue); return port;
}
int main() {
    using sf4e::statehash::IsConfirmedCheckpoint;
    CHECK(!IsConfirmedCheckpoint(-1, 100)); CHECK(!IsConfirmedCheckpoint(0, -1));
    CHECK(!IsConfirmedCheckpoint(61, 59)); CHECK(IsConfirmedCheckpoint(61, 60));
    CHECK(IsConfirmedCheckpoint(61, 100));
    GGPOSessionCallbacks callbacks = {};
    callbacks.begin_game = Begin; callbacks.save_game_state = Save; callbacks.load_game_state = Load;
    callbacks.log_game_state = Log; callbacks.free_buffer = Free; callbacks.advance_frame = Advance; callbacks.on_event = Event;
    WSADATA winsock{}; CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    GGPOSession* session = nullptr; GGPOSession* remote = nullptr;
    const auto localPort = ReservePort(); auto remotePort = ReservePort();
    while (remotePort == localPort) remotePort = ReservePort();
    int confirmed = 99;
    CHECK(ggpo_get_last_confirmed_frame(nullptr, &confirmed) == GGPO_ERRORCODE_INVALID_SESSION);
    CHECK(ggpo_start_session(&session, &callbacks, "confirmed-frame", 2, 1, localPort) == GGPO_OK);
    CHECK(ggpo_start_session(&remote, &callbacks, "confirmed-frame", 2, 1, remotePort) == GGPO_OK);
    CHECK(ggpo_get_last_confirmed_frame(session, nullptr) == GGPO_ERRORCODE_INVALID_REQUEST);
    CHECK(ggpo_get_last_confirmed_frame(session, &confirmed) == GGPO_OK && confirmed == -1);
    GGPOPlayerHandle handles[2];
    for (int side = 0; side < 2; ++side) {
        GGPOPlayer player = {}; player.size = sizeof(player); player.type = GGPO_PLAYERTYPE_LOCAL; player.player_num = side + 1;
        auto* local = side == 0 ? session : remote;
        CHECK(ggpo_add_player(local, &player, &handles[side]) == GGPO_OK);
        CHECK(ggpo_set_frame_delay(local, handles[side], 0) == GGPO_OK);
        player.type = GGPO_PLAYERTYPE_REMOTE; strcpy_s(player.u.remote.ip_address, "127.0.0.1");
        player.u.remote.port = side == 0 ? localPort : remotePort; GGPOPlayerHandle unused;
        CHECK(ggpo_add_player(side == 0 ? remote : session, &player, &unused) == GGPO_OK);
    }
    const auto pump = [&]() { CHECK(ggpo_idle(session, 0) == GGPO_OK); CHECK(ggpo_idle(remote, 0) == GGPO_OK); };
    const auto deadline = GetTickCount64() + 5000;
    while (running < 2 && GetTickCount64() < deadline) { pump(); Sleep(1); }
    CHECK(running == 2);
    for (int frame = 0; frame < 120; ++frame) {
        unsigned char input = 0, inputs[2]; int disconnected = 0;
        CHECK(ggpo_add_local_input(session, handles[0], &input, 1) == GGPO_OK);
        CHECK(ggpo_add_local_input(remote, handles[1], &input, 1) == GGPO_OK);
        int remoteConfirmed = -1; const auto frameDeadline = GetTickCount64() + 1000;
        do {
            pump();
            CHECK(ggpo_get_last_confirmed_frame(session, &confirmed) == GGPO_OK);
            CHECK(ggpo_get_last_confirmed_frame(remote, &remoteConfirmed) == GGPO_OK);
            if (confirmed < frame || remoteConfirmed < frame) Sleep(1);
        } while ((confirmed < frame || remoteConfirmed < frame) && GetTickCount64() < frameDeadline);
        CHECK(confirmed == frame && remoteConfirmed == frame);
        CHECK(ggpo_synchronize_input(session, inputs, sizeof(inputs), &disconnected) == GGPO_OK);
        CHECK(ggpo_synchronize_input(remote, inputs, sizeof(inputs), &disconnected) == GGPO_OK);
        CHECK(ggpo_advance_frame(session) == GGPO_OK);
        CHECK(ggpo_advance_frame(remote) == GGPO_OK);
        CHECK(ggpo_idle(session, 0) == GGPO_OK);
        CHECK(ggpo_get_last_confirmed_frame(session, &confirmed) == GGPO_OK);
        CHECK(confirmed == frame && saveFrame == frame + 1);
        CHECK(IsConfirmedCheckpoint(saveFrame, confirmed));
        const int before = saves;
        for (int read = 0; read < 10; ++read) CHECK(ggpo_get_last_confirmed_frame(session, &confirmed) == GGPO_OK && confirmed == frame);
        CHECK(saves == before); // The accessor is observational, never advances.
    }
    CHECK(ggpo_close_session(session) == GGPO_OK);
    CHECK(ggpo_close_session(remote) == GGPO_OK); WSACleanup();
    std::puts("GGPO confirmed-frame accessor and zero-delay boundary tests passed");
}
