// Stock GGPO asserted on any sendto error (udp.cpp Udp::SendTo), and the
// assertion exits the game. It also ignored a failed bind, so a session whose
// port was taken asserted on its first datagram. B188: one player's every match
// and spectate ended that way. The send-errors port patch:
// - fails ggpo_start_session and ggpo_start_spectating when the socket cannot
//   bind, leaving the Winsock error for WSAGetLastError;
// - loses only the datagram on a Transient send error;
// - disconnects only the endpoint that sent on a Lasting one, and
//   ggpo_get_send_error names that error for that endpoint's handle.
#include <ggponet.h>
#include <winsock2.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../vcpkg-overlays/ports/ggpo/send-errors.h"
#include "test_support.hxx"

static int running = 0, disconnects = 0;
static GGPOPlayerHandle lastDisconnected = GGPO_INVALID_HANDLE;
static bool __cdecl Begin(const char*) { return true; }
static bool __cdecl Save(unsigned char** buffer, int* len, int* checksum, int) {
    static unsigned char state = 0; *buffer = &state; *len = 1; *checksum = 0; return true;
}
static bool __cdecl Load(unsigned char*, int) { return true; }
static bool __cdecl Log(char*, unsigned char*, int) { return true; }
static void __cdecl Free(void*) {}
static bool __cdecl Advance(int) { return true; }
static bool __cdecl Event(GGPOEvent* event) {
    if (event->code == GGPO_EVENTCODE_RUNNING) ++running;
    if (event->code == GGPO_EVENTCODE_DISCONNECTED_FROM_PEER) { ++disconnects; lastDisconnected = event->u.disconnected.player; }
    return true;
}
// GGPO exits the process after this returns; print the assertion instead of a message box.
static void __cdecl AssertFailed(const char* message) { std::printf("%s\n", message); std::fflush(stdout); }

static void ClassifiesEachError() {
    using sf4e::netplay::ClassifySendError;
    using sf4e::netplay::SendError;
    const int transient[] = {WSAEWOULDBLOCK, WSAENOBUFS, WSAECONNRESET, WSAENETRESET, WSAEHOSTUNREACH, WSAENETUNREACH};
    for (const int error : transient) CHECK_MSG(ClassifySendError(error) == SendError::Transient, "error %d", error);
    const int lasting[] = {WSAEACCES, WSAENOTSOCK, WSAEINVAL, WSAEFAULT, WSAEADDRNOTAVAIL, WSAENETDOWN, WSAEMSGSIZE};
    for (const int error : lasting) CHECK_MSG(ClassifySendError(error) == SendError::Lasting, "error %d", error);
}

// A wildcard bind with SO_EXCLUSIVEADDRUSE, which GGPO's own wildcard bind on
// the same port cannot share.
static SOCKET HoldPort(unsigned short* port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); CHECK(s != INVALID_SOCKET);
    BOOL exclusive = TRUE;
    CHECK(setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0);
    sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_ANY);
    CHECK(bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    int size = sizeof(address); CHECK(getsockname(s, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    *port = ntohs(address.sin_port);
    return s;
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

static void StartFailsOnHeldPort(GGPOSessionCallbacks& callbacks) {
    unsigned short port = 0; const SOCKET held = HoldPort(&port);
    char loopback[] = "127.0.0.1";
    GGPOSession* session = reinterpret_cast<GGPOSession*>(1);
    // Before the patch both started, and the first sync request asserted.
    CHECK(ggpo_start_session(&session, &callbacks, "send-failure", 2, 1, port) == GGPO_ERRORCODE_GENERAL_FAILURE);
    int error = WSAGetLastError();
    std::printf("start_session on a held port: Winsock error %d\n", error);
    CHECK(session == nullptr && (error == WSAEACCES || error == WSAEADDRINUSE));
    session = reinterpret_cast<GGPOSession*>(1);
    CHECK(ggpo_start_spectating(&session, &callbacks, "send-failure", 2, 1, port, loopback, ReservePort()) == GGPO_ERRORCODE_GENERAL_FAILURE);
    error = WSAGetLastError();
    std::printf("start_spectating on a held port: Winsock error %d\n", error);
    CHECK(session == nullptr && (error == WSAEACCES || error == WSAEADDRINUSE));
    CHECK(disconnects == 0 && running == 0);
    closesocket(held);
}

// The fake peer fighter: answers every SyncRequest and counts the Input messages.
static const std::uint16_t Magic = 0xBEEF;
static std::uint16_t sequence = 0;
static int ServePeer(SOCKET s, unsigned short sessionPort) {
    int inputs = 0;
    unsigned char packet[2048];
    for (;;) {
        const int received = recv(s, reinterpret_cast<char*>(packet), sizeof(packet), 0);
        if (received <= 0) return inputs;
        // UdpMsg header: magic u16, sequence u16, type u8; SyncRequest carries random_request u32 at offset 5.
        if (received >= 5 && packet[4] == 3) ++inputs; // UdpMsg::Input
        if (received < 9 || packet[4] != 1) continue;
        unsigned char reply[9] = {}; const std::uint16_t seq = sequence++;
        std::memcpy(reply, &Magic, 2); std::memcpy(reply + 2, &seq, 2); reply[4] = 2; std::memcpy(reply + 5, packet + 5, 4);
        sockaddr_in to = {}; to.sin_family = AF_INET; to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); to.sin_port = htons(sessionPort);
        CHECK(sendto(s, reinterpret_cast<const char*>(reply), sizeof(reply), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == sizeof(reply));
    }
}

// A host with one spectator whose address cannot be sent to (broadcast without
// SO_BROADCAST, WSAEACCES): that spectator's endpoint disconnects, the peer
// fighter's endpoint keeps running.
static void SpectatorErrorLeavesPeerRunning(GGPOSessionCallbacks& callbacks) {
    unsigned short peerPort = 0; const SOCKET peer = BindLoopback(&peerPort);
    auto port = ReservePort();
    while (port == peerPort) port = ReservePort();
    GGPOSession* host = nullptr;
    CHECK(ggpo_start_session(&host, &callbacks, "send-failure", 2, 1, port) == GGPO_OK);
    GGPOPlayer local = {}; local.size = sizeof(local); local.type = GGPO_PLAYERTYPE_LOCAL; local.player_num = 1;
    GGPOPlayer remote = {}; remote.size = sizeof(remote); remote.type = GGPO_PLAYERTYPE_REMOTE; remote.player_num = 2;
    strcpy_s(remote.u.remote.ip_address, "127.0.0.1"); remote.u.remote.port = peerPort;
    GGPOPlayer watcher = {}; watcher.size = sizeof(watcher); watcher.type = GGPO_PLAYERTYPE_SPECTATOR; watcher.player_num = 3;
    strcpy_s(watcher.u.remote.ip_address, "255.255.255.255"); watcher.u.remote.port = 9;
    GGPOPlayerHandle handles[3] = {GGPO_INVALID_HANDLE, GGPO_INVALID_HANDLE, GGPO_INVALID_HANDLE};
    CHECK(ggpo_add_player(host, &local, &handles[0]) == GGPO_OK);
    CHECK(ggpo_add_player(host, &remote, &handles[1]) == GGPO_OK);
    // Before the patch the spectator's first sync request asserted here.
    CHECK(ggpo_add_player(host, &watcher, &handles[2]) == GGPO_OK);
    const auto deadline = GetTickCount64() + 5000;
    while (running < 1 && GetTickCount64() < deadline) { CHECK(ggpo_idle(host, 0) == GGPO_OK); ServePeer(peer, port); Sleep(1); }
    CHECK(running == 1);
    CHECK(disconnects == 1 && lastDisconnected == handles[2]);
    // The error belongs to the spectator's endpoint, not to the peer's.
    int error = -1;
    CHECK(ggpo_get_send_error(host, handles[1], &error) == GGPO_OK && error == 0);
    CHECK(ggpo_get_send_error(host, handles[2], &error) == GGPO_OK);
    std::printf("spectator at a broadcast address: Winsock error %d\n", error);
    CHECK(error == WSAEACCES);
    // The peer still gets this PC's inputs.
    unsigned char input = 1; int inputs = 0;
    CHECK(ggpo_add_local_input(host, handles[0], &input, sizeof(input)) == GGPO_OK);
    for (int i = 0; i < 100 && inputs == 0; ++i) { CHECK(ggpo_idle(host, 0) == GGPO_OK); inputs += ServePeer(peer, port); Sleep(1); }
    CHECK(inputs > 0 && disconnects == 1);
    CHECK(ggpo_close_session(host) == GGPO_OK);
    closesocket(peer);
}

// A spectator whose host cannot be sent to: its one endpoint disconnects
// (player 0) and the error is reported for it, whatever the handle.
static void SpectatorHostErrorIsReported(GGPOSessionCallbacks& callbacks) {
    disconnects = running = 0; lastDisconnected = GGPO_INVALID_HANDLE;
    GGPOSession* spectator = nullptr; char broadcast[] = "255.255.255.255";
    CHECK(ggpo_start_spectating(&spectator, &callbacks, "send-failure", 2, 1, ReservePort(), broadcast, 9) == GGPO_OK);
    for (int i = 0; i < 50 && disconnects == 0; ++i) { CHECK(ggpo_idle(spectator, 0) == GGPO_OK); Sleep(1); }
    CHECK(disconnects == 1 && lastDisconnected == 0 && running == 0);
    int error = 0;
    CHECK(ggpo_get_send_error(spectator, 0, &error) == GGPO_OK);
    std::printf("spectator's host at a broadcast address: Winsock error %d\n", error);
    CHECK(error == WSAEACCES);
    CHECK(ggpo_close_session(spectator) == GGPO_OK);
}

// Synctest has no socket.
static void SynctestHasNoSendError(GGPOSessionCallbacks& callbacks) {
    GGPOSession* synctest = nullptr; char game[] = "send-failure";
    CHECK(ggpo_start_synctest(&synctest, &callbacks, game, 2, 1, 1) == GGPO_OK);
    int error = -1;
    CHECK(ggpo_get_send_error(synctest, 1, &error) == GGPO_ERRORCODE_UNSUPPORTED && error == 0);
    CHECK(ggpo_close_session(synctest) == GGPO_OK);
}

int main() {
    ClassifiesEachError();

    GGPOSessionCallbacks callbacks = {};
    callbacks.begin_game = Begin; callbacks.save_game_state = Save; callbacks.load_game_state = Load;
    callbacks.log_game_state = Log; callbacks.free_buffer = Free; callbacks.advance_frame = Advance; callbacks.on_event = Event;
    ggpo_set_assert_handler(AssertFailed);
    WSADATA winsock{}; CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    StartFailsOnHeldPort(callbacks);
    SpectatorErrorLeavesPeerRunning(callbacks);
    SpectatorHostErrorIsReported(callbacks);
    SynctestHasNoSendError(callbacks);
    WSACleanup();
    std::printf("GGPO send failures passed\n");
    return 0;
}
