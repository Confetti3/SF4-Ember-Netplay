// A GGPO endpoint that received an input packet starting past the next frame
// it expected used to assert (udp_proto.cpp OnInput), which showed a message box
// and exited the game. A host whose pending output filled stops queueing frames
// for that spectator, so a slow spectator could be sent exactly that packet. The
// input-gap-disconnect port patch turns it into the ordinary "disconnected from
// peer" event. This test runs a real spectator session against a fake host that
// syncs, sends frame 0 and then skips to frame 5.
#include <ggponet.h>
#include <winsock2.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "test_support.hxx"
static int running = 0, disconnects = 0;
static bool __cdecl Begin(const char*) { return true; }
static bool __cdecl Save(unsigned char**, int*, int*, int) { return true; }
static bool __cdecl Load(unsigned char*, int) { return true; }
static bool __cdecl Log(char*, unsigned char*, int) { return true; }
static void __cdecl Free(void*) {}
static bool __cdecl Advance(int) { return true; }
static bool __cdecl Event(GGPOEvent* event) {
    if (event->code == GGPO_EVENTCODE_RUNNING) ++running;
    if (event->code == GGPO_EVENTCODE_DISCONNECTED_FROM_PEER) ++disconnects;
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

static const std::uint16_t Magic = 0xBEEF;
static std::uint16_t sequence = 0;
static void SendToSpectator(SOCKET s, unsigned short port, const unsigned char* data, int size) {
    sockaddr_in to = {}; to.sin_family = AF_INET; to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); to.sin_port = htons(port);
    CHECK(sendto(s, reinterpret_cast<const char*>(data), size, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == size);
}
// The fake host: answers every SyncRequest so the spectator reaches RUNNING.
static void ServeSync(SOCKET s, unsigned short spectatorPort) {
    unsigned char packet[2048];
    for (;;) {
        const int received = recv(s, reinterpret_cast<char*>(packet), sizeof(packet), 0);
        if (received <= 0) return;
        // UdpMsg header: magic u16, sequence u16, type u8; SyncRequest carries random_request u32 at offset 5.
        if (received < 9 || packet[4] != 1) continue;
        unsigned char reply[9] = {}; const std::uint16_t seq = sequence++;
        std::memcpy(reply, &Magic, 2); std::memcpy(reply + 2, &seq, 2); reply[4] = 2; std::memcpy(reply + 5, packet + 5, 4);
        SendToSpectator(s, spectatorPort, reply, sizeof(reply));
    }
}
// One Input message holding a single unchanged frame (delta bit 1, end bit 0).
// Layout after the header: connect_status[4], start_frame u32, ack u32, num_bits u16, input_size u8, bits.
static void SendInputFrame(SOCKET s, unsigned short spectatorPort, std::uint32_t startFrame) {
    unsigned char packet[5 + 16 + 4 + 4 + 2 + 1 + 1] = {}; const std::uint16_t seq = sequence++;
    std::memcpy(packet, &Magic, 2); std::memcpy(packet + 2, &seq, 2); packet[4] = 3;
    std::memcpy(packet + 21, &startFrame, 4);
    const std::uint16_t numBits = 2; std::memcpy(packet + 29, &numBits, 2);
    packet[31] = 2; packet[32] = 1;
    SendToSpectator(s, spectatorPort, packet, sizeof(packet));
}

int main() {
    GGPOSessionCallbacks callbacks = {};
    callbacks.begin_game = Begin; callbacks.save_game_state = Save; callbacks.load_game_state = Load;
    callbacks.log_game_state = Log; callbacks.free_buffer = Free; callbacks.advance_frame = Advance; callbacks.on_event = Event;
    WSADATA winsock{}; CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    unsigned short hostPort = 0; const SOCKET host = BindLoopback(&hostPort);
    auto spectatorPort = ReservePort();
    while (spectatorPort == hostPort) spectatorPort = ReservePort();
    GGPOSession* spectator = nullptr; char loopback[] = "127.0.0.1";
    CHECK(ggpo_start_spectating(&spectator, &callbacks, "input-gap", 2, 1, spectatorPort, loopback, hostPort) == GGPO_OK);
    const auto pump = [&](ULONGLONG ms, const int& until, int target) {
        const auto deadline = GetTickCount64() + ms;
        while (until < target && GetTickCount64() < deadline) { CHECK(ggpo_idle(spectator, 0) == GGPO_OK); ServeSync(host, spectatorPort); Sleep(1); }
    };
    pump(5000, running, 1);
    CHECK(running == 1);
    unsigned char inputs[2]; int disconnected = 0;
    SendInputFrame(host, spectatorPort, 0);
    const auto frameDeadline = GetTickCount64() + 2000;
    while (ggpo_synchronize_input(spectator, inputs, sizeof(inputs), &disconnected) != GGPO_OK && GetTickCount64() < frameDeadline) {
        CHECK(ggpo_idle(spectator, 0) == GGPO_OK); Sleep(1);
    }
    CHECK(ggpo_advance_frame(spectator) == GGPO_OK);
    CHECK(disconnects == 0);
    // Frames 1 to 4 never arrive. Before the patch this exited the process.
    SendInputFrame(host, spectatorPort, 5);
    pump(2000, disconnects, 1);
    CHECK(disconnects == 1);
    SendInputFrame(host, spectatorPort, 9);
    int none = 2; pump(200, none, 3);
    CHECK(disconnects == 1);
    CHECK(ggpo_close_session(spectator) == GGPO_OK);
    closesocket(host); WSACleanup();
    std::printf("GGPO input gap disconnected the spectator and the process survived\n");
}
