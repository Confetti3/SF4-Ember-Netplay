// The sync-retry-timer port patch times a sync retry from GGPO's own last
// request and resends that round's challenge, so neither answering the peer's
// requests nor a round trip longer than the 500 ms first retry stops the
// handshake. Each case runs real GGPO endpoints on loopback UDP: three against
// a scripted host whose packets the test controls, and one between two
// fighters whose links GGPO's own send delay slows down.
#include <ggponet.h>
#include <winsock2.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

#include "test_support.hxx"
static int running = 0;
static bool __cdecl Begin(const char*) { return true; }
static bool __cdecl Save(unsigned char** buffer, int* len, int* checksum, int frame) {
    *buffer = static_cast<unsigned char*>(std::malloc(sizeof(int))); CHECK(*buffer);
    std::memcpy(*buffer, &frame, sizeof(frame)); *len = sizeof(frame); *checksum = frame; return true;
}
static bool __cdecl Load(unsigned char*, int) { return true; }
static bool __cdecl Log(char*, unsigned char*, int) { return true; }
static void __cdecl Free(void* buffer) { std::free(buffer); }
static bool __cdecl Advance(int) { return true; }
static bool __cdecl Event(GGPOEvent* event) { if (event->code == GGPO_EVENTCODE_RUNNING) ++running; return true; }
static GGPOSessionCallbacks Callbacks() {
    GGPOSessionCallbacks callbacks = {};
    callbacks.begin_game = Begin; callbacks.save_game_state = Save; callbacks.load_game_state = Load;
    callbacks.log_game_state = Log; callbacks.free_buffer = Free; callbacks.advance_frame = Advance; callbacks.on_event = Event;
    return callbacks;
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

// The host end of a spectator's link, driven by hand. UdpMsg: magic u16,
// sequence u16, type u8, then a SyncRequest's random u32, magic u16 and
// endpoint u8, or a SyncReply's random u32.
struct ScriptedHost {
    SOCKET socket; unsigned short port = 0, ggpoPort = 0; std::uint16_t sequence = 0;
    std::vector<std::uint32_t> challenges; // every SyncRequest GGPO sent, retries included
    struct Reply { std::uint64_t at; std::uint32_t random; };
    std::deque<Reply> replies;
    ScriptedHost() { socket = BindLoopback(&port); }
    ~ScriptedHost() { closesocket(socket); }
    void Send(std::uint8_t type, std::uint32_t random) {
        unsigned char packet[12] = {}; const std::uint16_t magic = 0xBEEF; ++sequence;
        std::memcpy(packet, &magic, 2); std::memcpy(packet + 2, &sequence, 2); packet[4] = type; std::memcpy(packet + 5, &random, 4);
        const int size = type == 1 ? 12 : 9;
        sockaddr_in to = {}; to.sin_family = AF_INET; to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); to.sin_port = htons(ggpoPort);
        CHECK(sendto(socket, reinterpret_cast<const char*>(packet), size, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == size);
    }
    // Records GGPO's challenges and, from answerFromMs on, schedules each
    // reply delayMs later. Replies are sent twice, so a duplicate always
    // follows the one GGPO accepts.
    void Serve(std::uint64_t now, std::uint64_t answerFrom, std::uint64_t delayMs) {
        unsigned char packet[2048];
        for (int received; (received = recv(socket, reinterpret_cast<char*>(packet), sizeof(packet), 0)) > 0;) {
            if (received != 12 || packet[4] != 1) continue;
            std::uint32_t random; std::memcpy(&random, packet + 5, 4);
            challenges.push_back(random);
            if (now >= answerFrom) replies.push_back({ now + delayMs, random });
        }
        for (; !replies.empty() && replies.front().at <= now; replies.pop_front()) {
            Send(2, replies.front().random); Send(2, replies.front().random);
        }
    }
    int Rounds() const {
        int rounds = challenges.empty() ? 0 : 1;
        for (std::size_t i = 1; i < challenges.size(); ++i) rounds += challenges[i] != challenges[i - 1];
        return rounds;
    }
    bool Resent() const {
        for (std::size_t i = 1; i < challenges.size(); ++i) if (challenges[i] == challenges[i - 1]) return true;
        return false;
    }
};

// A spectator session synchronizing with a scripted host. requestEveryMs > 0
// makes the host also run its own sync, so GGPO is busy answering it.
static std::uint64_t SpectatorSync(ScriptedHost& host, std::uint64_t answerFrom, std::uint64_t delayMs,
        std::uint64_t requestEveryMs, std::uint64_t limitMs) {
    running = 0;
    host.ggpoPort = ReservePort();
    auto callbacks = Callbacks(); GGPOSession* session = nullptr; char ip[] = "127.0.0.1";
    CHECK(ggpo_start_spectating(&session, &callbacks, "sync-retry", 2, 1, host.ggpoPort, ip, host.port) == GGPO_OK);
    const auto start = GetTickCount64(); std::uint64_t lastRequest = 0;
    for (std::uint64_t now = start; !running && now < start + limitMs; now = GetTickCount64()) {
        CHECK(ggpo_idle(session, 0) == GGPO_OK);
        host.Serve(now - start, answerFrom, delayMs);
        if (requestEveryMs && now - lastRequest >= requestEveryMs) { host.Send(1, 0x1234); lastRequest = now; }
        Sleep(1);
    }
    const auto took = GetTickCount64() - start;
    CHECK(ggpo_close_session(session) == GGPO_OK);
    return running ? took : 0;
}

int main() {
    WSADATA winsock{}; CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    {
        // A steady 650 ms round trip, longer than the first retry. A retry
        // resends the same challenge, so the first reply still counts; every
        // reply arrives twice and the retries' replies arrive late, yet only a
        // reply to the current round's challenge advances the handshake: five
        // rounds, five challenges.
        ScriptedHost host;
        const auto took = SpectatorSync(host, 0, 650, 0, 15000);
        std::printf("650 ms round trip: running after %llu ms, %zu requests, %d rounds\n",
            static_cast<unsigned long long>(took), host.challenges.size(), host.Rounds());
        CHECK(took >= 5 * 650 && took < 8000);
        CHECK(host.Rounds() == 5);
        CHECK(host.Resent());
    }
    {
        // The startup race: GGPO's first request is lost while the host is not
        // up, and the host's own sync requests keep GGPO answering every 100 ms.
        // Answering them used to hold GGPO's retry back for as long as they came.
        ScriptedHost host;
        const auto took = SpectatorSync(host, 1000, 0, 100, 6000);
        std::printf("Startup race: running after %llu ms, %zu requests, %d rounds\n",
            static_cast<unsigned long long>(took), host.challenges.size(), host.Rounds());
        CHECK(took >= 1000 && took < 2500);
        CHECK(host.Rounds() == 5);
    }
    {
        // A prompt host: one request per round, no retry.
        ScriptedHost host;
        const auto took = SpectatorSync(host, 0, 0, 0, 3000);
        CHECK(took > 0 && took < 1000);
        CHECK(host.Rounds() == 5 && host.challenges.size() == 5);
    }
    {
        // Two fighters, both real GGPO, each answering the other's sync while
        // running its own, over links GGPO itself delays by 267-400 ms each
        // way: round trips of 533-800 ms.
        running = 0;
        CHECK(SetEnvironmentVariableA("ggpo.network.delay", "400"));
        const auto ports = [] { auto a = ReservePort(), b = ReservePort(); while (b == a) b = ReservePort(); return std::make_pair(a, b); }();
        auto callbacks = Callbacks(); GGPOSession* sessions[2] = {};
        for (int side = 0; side < 2; ++side)
            CHECK(ggpo_start_session(&sessions[side], &callbacks, "sync-retry", 2, 1, side == 0 ? ports.first : ports.second) == GGPO_OK);
        CHECK(SetEnvironmentVariableA("ggpo.network.delay", nullptr));
        for (int side = 0; side < 2; ++side) {
            GGPOPlayer player = {}; player.size = sizeof(player); player.type = GGPO_PLAYERTYPE_LOCAL; player.player_num = side + 1;
            GGPOPlayerHandle handle;
            CHECK(ggpo_add_player(sessions[side], &player, &handle) == GGPO_OK);
            player.type = GGPO_PLAYERTYPE_REMOTE; strcpy_s(player.u.remote.ip_address, "127.0.0.1");
            player.u.remote.port = side == 0 ? ports.first : ports.second;
            CHECK(ggpo_add_player(sessions[1 - side], &player, &handle) == GGPO_OK);
        }
        const auto start = GetTickCount64();
        while (running < 2 && GetTickCount64() < start + 15000) {
            CHECK(ggpo_idle(sessions[0], 0) == GGPO_OK); CHECK(ggpo_idle(sessions[1], 0) == GGPO_OK); Sleep(1);
        }
        std::printf("Fighters at 533-800 ms: %d running after %llu ms\n", running, static_cast<unsigned long long>(GetTickCount64() - start));
        CHECK(running == 2);
        for (auto* session : sessions) CHECK(ggpo_close_session(session) == GGPO_OK);
    }
    WSACleanup();
    std::printf("GGPO sync retry passed\n");
}
