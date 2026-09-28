// spectator-catch-up.patch: a real spectator (ggpo_start_spectating) keeps
// receiving P1's confirmed frames while it does not play them. Upstream kept
// only 64 frames, so once the spectator was about a second behind the host an
// unplayed frame was overwritten and ggpo_synchronize_input returned
// GENERAL_FAILURE (-1), ending the view: the v0.9.9 field logs show it mid-match
// and just after the result. The spectator now holds 1024 frames and reports
// its backlog through ggpo_get_spectator_backlog, so it can catch up.
#include <ggponet.h>
#include <winsock2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../common/SpectatorCatchUp.hxx"
#include "test_support.hxx"

static int running = 0, disconnects = 0;
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
    if (event->code == GGPO_EVENTCODE_DISCONNECTED_FROM_PEER) ++disconnects;
    return true;
}
static unsigned short ReservePort() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); CHECK(s != INVALID_SOCKET);
    sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    int size = sizeof(address); CHECK(getsockname(s, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    closesocket(s);
    return ntohs(address.sin_port);
}
static unsigned char InputFor(int side, int frame) { return static_cast<unsigned char>(frame * 7 + side * 101); }

int main() {
    GGPOSessionCallbacks callbacks = {};
    callbacks.begin_game = Begin; callbacks.save_game_state = Save; callbacks.load_game_state = Load;
    callbacks.log_game_state = Log; callbacks.free_buffer = Free; callbacks.advance_frame = Advance; callbacks.on_event = Event;
    WSADATA winsock{}; CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);

    unsigned short ports[3] = {};
    for (int i = 0; i < 3; ++i) {
        bool unique;
        do {
            ports[i] = ReservePort(); unique = true;
            for (int j = 0; j < i; ++j) unique = unique && ports[j] != ports[i];
        } while (!unique);
    }
    GGPOSession* host = nullptr; GGPOSession* remote = nullptr; GGPOSession* watcher = nullptr;
    CHECK(ggpo_start_session(&host, &callbacks, "spectator-backlog", 2, 1, ports[0]) == GGPO_OK);
    CHECK(ggpo_start_session(&remote, &callbacks, "spectator-backlog", 2, 1, ports[1]) == GGPO_OK);
    GGPOPlayerHandle handles[2];
    for (int side = 0; side < 2; ++side) {
        GGPOPlayer player = {}; player.size = sizeof(player); player.type = GGPO_PLAYERTYPE_LOCAL; player.player_num = side + 1;
        auto* local = side == 0 ? host : remote;
        CHECK(ggpo_add_player(local, &player, &handles[side]) == GGPO_OK);
        CHECK(ggpo_set_frame_delay(local, handles[side], 0) == GGPO_OK);
        player.type = GGPO_PLAYERTYPE_REMOTE; strcpy_s(player.u.remote.ip_address, "127.0.0.1");
        player.u.remote.port = ports[side]; GGPOPlayerHandle unused;
        CHECK(ggpo_add_player(side == 0 ? remote : host, &player, &unused) == GGPO_OK);
    }
    GGPOPlayer spectator = {}; spectator.size = sizeof(spectator); spectator.type = GGPO_PLAYERTYPE_SPECTATOR; spectator.player_num = 3;
    strcpy_s(spectator.u.remote.ip_address, "127.0.0.1"); spectator.u.remote.port = ports[2]; GGPOPlayerHandle unused;
    CHECK(ggpo_add_player(host, &spectator, &unused) == GGPO_OK);
    char hostAddress[] = "127.0.0.1";
    CHECK(ggpo_start_spectating(&watcher, &callbacks, "spectator-backlog", 2, 1, ports[2], hostAddress, ports[0]) == GGPO_OK);

    // The accessor: spectator sessions only, with the usual argument checks.
    int backlog = -1;
    CHECK(ggpo_get_spectator_backlog(nullptr, &backlog) == GGPO_ERRORCODE_INVALID_SESSION);
    CHECK(ggpo_get_spectator_backlog(watcher, nullptr) == GGPO_ERRORCODE_INVALID_REQUEST);
    CHECK(ggpo_get_spectator_backlog(host, &backlog) == GGPO_ERRORCODE_UNSUPPORTED);
    CHECK(ggpo_get_spectator_backlog(watcher, &backlog) == GGPO_OK && backlog == 0);

    const auto pump = [&]() {
        CHECK(ggpo_idle(host, 0) == GGPO_OK); CHECK(ggpo_idle(remote, 0) == GGPO_OK); CHECK(ggpo_idle(watcher, 0) == GGPO_OK);
    };
    auto deadline = GetTickCount64() + 5000;
    while (running < 3 && GetTickCount64() < deadline) { pump(); Sleep(1); }
    CHECK(running == 3);

    // The fighters play Frames frames. The spectator keeps polling the whole
    // time (so it receives and acknowledges every frame, and P1 never drops
    // it), but plays nothing until StallFrames have been confirmed: far past
    // upstream's 64-frame ring.
    const int Frames = 600, StallFrames = 400;
    int played = 0, maxBacklog = 0, catchUpFrames = 0, lastBacklog = -1;
    const auto playOne = [&]() -> bool {
        unsigned char inputs[2] = {}; int disconnected = 0;
        const auto result = ggpo_synchronize_input(watcher, inputs, sizeof(inputs), &disconnected);
        CHECK(result != GGPO_ERRORCODE_GENERAL_FAILURE);
        if (result == GGPO_ERRORCODE_PREDICTION_THRESHOLD) return false; // not received yet
        CHECK(result == GGPO_OK);
        CHECK(inputs[0] == InputFor(0, played) && inputs[1] == InputFor(1, played));
        CHECK(ggpo_advance_frame(watcher) == GGPO_OK);
        ++played;
        return true;
    };
    for (int frame = 0; frame < Frames; ++frame) {
        unsigned char p1 = InputFor(0, frame), p2 = InputFor(1, frame);
        CHECK(ggpo_add_local_input(host, handles[0], &p1, 1) == GGPO_OK);
        CHECK(ggpo_add_local_input(remote, handles[1], &p2, 1) == GGPO_OK);
        int confirmed = -1, remoteConfirmed = -1; deadline = GetTickCount64() + 1000;
        do {
            pump();
            CHECK(ggpo_get_last_confirmed_frame(host, &confirmed) == GGPO_OK);
            CHECK(ggpo_get_last_confirmed_frame(remote, &remoteConfirmed) == GGPO_OK);
            if (confirmed < frame || remoteConfirmed < frame) Sleep(1);
        } while ((confirmed < frame || remoteConfirmed < frame) && GetTickCount64() < deadline);
        CHECK(confirmed == frame && remoteConfirmed == frame);
        unsigned char inputs[2]; int disconnected = 0;
        CHECK(ggpo_synchronize_input(host, inputs, sizeof(inputs), &disconnected) == GGPO_OK && disconnected == 0);
        CHECK(ggpo_synchronize_input(remote, inputs, sizeof(inputs), &disconnected) == GGPO_OK && disconnected == 0);
        CHECK(inputs[0] == p1 && inputs[1] == p2);
        CHECK(ggpo_advance_frame(host) == GGPO_OK);
        CHECK(ggpo_advance_frame(remote) == GGPO_OK);
        // Once the stall is over the spectator runs one frame per tick plus
        // the catch-up frames, exactly as fSystem::BattleUpdate does.
        CHECK(ggpo_get_spectator_backlog(watcher, &backlog) == GGPO_OK);
        CHECK(backlog >= 0 && backlog <= frame + 1 - played);
        if (backlog > maxBacklog) maxBacklog = backlog;
        if (frame >= StallFrames && playOne()) {
            CHECK(ggpo_get_spectator_backlog(watcher, &backlog) == GGPO_OK);
            const int extra = sf4e::SpectatorCatchUp::ExtraFrames(backlog);
            for (int i = 0; i < extra; ++i) { CHECK(playOne()); ++catchUpFrames; }
            CHECK(ggpo_get_spectator_backlog(watcher, &lastBacklog) == GGPO_OK);
        }
    }
    // The spectator was hundreds of frames behind and never lost a frame, and
    // the catch-up brought it back within its reserve while the fighters
    // were still playing (about 150 of the 200 remaining ticks).
    CHECK(maxBacklog > 300);
    CHECK(catchUpFrames > 0);
    CHECK(lastBacklog >= 0 && lastBacklog <= sf4e::SpectatorCatchUp::ReserveFrames + 1);
    // Frames still in flight are delivered and the rest is played out.
    deadline = GetTickCount64() + 3000;
    while (played < Frames && GetTickCount64() < deadline) { pump(); if (!playOne()) Sleep(1); }
    CHECK(played == Frames);
    CHECK(ggpo_get_spectator_backlog(watcher, &backlog) == GGPO_OK && backlog == 0);
    CHECK(disconnects == 0);

    // The backlog counts only frames GGPO has read from its socket. Here P1
    // sends its last frames and then closes, while the spectator is not
    // polled: its backlog still reads 0 until the next poll. This is why the
    // game decides a spectator's stream is played out only after the outer
    // tick's GGPO poll (NetplayFacade::PollSpectatorExit), not when P1's link
    // is first seen closed.
    const int TailFrames = 12;
    for (int frame = Frames; frame < Frames + TailFrames; ++frame) {
        unsigned char p1 = InputFor(0, frame), p2 = InputFor(1, frame);
        CHECK(ggpo_add_local_input(host, handles[0], &p1, 1) == GGPO_OK);
        CHECK(ggpo_add_local_input(remote, handles[1], &p2, 1) == GGPO_OK);
        int confirmed = -1; deadline = GetTickCount64() + 1000;
        do {
            CHECK(ggpo_idle(host, 0) == GGPO_OK); CHECK(ggpo_idle(remote, 0) == GGPO_OK);
            CHECK(ggpo_get_last_confirmed_frame(host, &confirmed) == GGPO_OK);
            if (confirmed < frame) Sleep(1);
        } while (confirmed < frame && GetTickCount64() < deadline);
        CHECK(confirmed == frame);
        unsigned char inputs[2]; int disconnected = 0;
        CHECK(ggpo_synchronize_input(host, inputs, sizeof(inputs), &disconnected) == GGPO_OK);
        CHECK(ggpo_synchronize_input(remote, inputs, sizeof(inputs), &disconnected) == GGPO_OK);
        CHECK(ggpo_advance_frame(host) == GGPO_OK);
        CHECK(ggpo_advance_frame(remote) == GGPO_OK);
    }
    CHECK(ggpo_idle(host, 0) == GGPO_OK);
    CHECK(ggpo_close_session(host) == GGPO_OK);
    host = nullptr;
    Sleep(50);
    CHECK(ggpo_get_spectator_backlog(watcher, &backlog) == GGPO_OK && backlog == 0);
    CHECK(ggpo_idle(watcher, 0) == GGPO_OK);
    CHECK(ggpo_get_spectator_backlog(watcher, &backlog) == GGPO_OK && backlog == TailFrames);
    while (playOne()) {}
    CHECK(played == Frames + TailFrames);
    CHECK(ggpo_get_spectator_backlog(watcher, &backlog) == GGPO_OK && backlog == 0);

    CHECK(ggpo_close_session(watcher) == GGPO_OK);
    CHECK(ggpo_close_session(remote) == GGPO_OK);
    std::puts("GGPO spectator backlog passed");
    return 0;
}
