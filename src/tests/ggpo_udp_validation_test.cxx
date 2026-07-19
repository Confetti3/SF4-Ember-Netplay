#include <ggponet.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

bool __cdecl BeginGame(const char*) {
    return true;
}

bool __cdecl SaveGameState(unsigned char** buffer, int* len, int* checksum, int) {
    *buffer = static_cast<unsigned char*>(std::malloc(1));
    if (!*buffer) {
        return false;
    }
    (*buffer)[0] = 0;
    *len = 1;
    *checksum = 0;
    return true;
}

bool __cdecl LoadGameState(unsigned char*, int) {
    return true;
}

bool __cdecl LogGameState(char*, unsigned char*, int) {
    return true;
}

void __cdecl FreeBuffer(void* buffer) {
    std::free(buffer);
}

bool __cdecl AdvanceFrame(int) {
    return true;
}

bool __cdecl OnEvent(GGPOEvent*) {
    return true;
}

bool GetBoundPort(SOCKET socket, unsigned short* port) {
    sockaddr_in address = {};
    int addressLen = sizeof(address);
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &addressLen) == SOCKET_ERROR) {
        return false;
    }
    *port = ntohs(address.sin_port);
    return *port != 0;
}

SOCKET BindLoopbackSocket(unsigned short port, unsigned short* boundPort) {
    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket);
        return INVALID_SOCKET;
    }
    if (!GetBoundPort(socket, boundPort)) {
        closesocket(socket);
        return INVALID_SOCKET;
    }

    u_long nonBlocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
        closesocket(socket);
        return INVALID_SOCKET;
    }
    return socket;
}

bool ReserveUnusedPort(unsigned short* port) {
    unsigned short ignored = 0;
    SOCKET socket = BindLoopbackSocket(0, &ignored);
    if (socket == INVALID_SOCKET) {
        return false;
    }
    *port = ignored;
    closesocket(socket);
    return true;
}

bool SendPacket(SOCKET socket, unsigned short destinationPort, const std::vector<unsigned char>& packet) {
    sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(destinationPort);
    return sendto(
        socket,
        reinterpret_cast<const char*>(packet.data()),
        static_cast<int>(packet.size()),
        0,
        reinterpret_cast<sockaddr*>(&destination),
        sizeof(destination)
    ) == static_cast<int>(packet.size());
}

void Pump(GGPOSession* session, int iterations = 8) {
    for (int i = 0; i < iterations; ++i) {
        ggpo_idle(session, 1);
        Sleep(1);
    }
}

void DrainSocket(SOCKET socket) {
    unsigned char buffer[1024];
    sockaddr_in from = {};
    int fromLen = sizeof(from);
    while (recvfrom(
        socket,
        reinterpret_cast<char*>(buffer),
        sizeof(buffer),
        0,
        reinterpret_cast<sockaddr*>(&from),
        &fromLen
    ) > 0) {
        fromLen = sizeof(from);
    }
}

void WriteU16Host(std::vector<unsigned char>& packet, size_t offset, std::uint16_t value) {
    std::memcpy(packet.data() + offset, &value, sizeof(value));
}

void WriteU32Host(std::vector<unsigned char>& packet, size_t offset, std::uint32_t value) {
    std::memcpy(packet.data() + offset, &value, sizeof(value));
}

bool ValidSyncRequestStillWorks(
    GGPOSession* session,
    SOCKET remoteSocket,
    unsigned short localPort
) {
    const std::uint32_t randomRequest = 0x78563412;
    std::vector<unsigned char> syncRequest(12, 0);
    WriteU16Host(syncRequest, 0, 0x1234);
    WriteU16Host(syncRequest, 2, 1);
    syncRequest[4] = 1; // UdpMsg::SyncRequest
    WriteU32Host(syncRequest, 5, randomRequest);
    WriteU16Host(syncRequest, 9, 0);
    syncRequest[11] = 1;

    DrainSocket(remoteSocket);
    if (!SendPacket(remoteSocket, localPort, syncRequest)) {
        return false;
    }

    for (int attempt = 0; attempt < 50; ++attempt) {
        Pump(session, 1);

        unsigned char response[64] = {};
        sockaddr_in from = {};
        int fromLen = sizeof(from);
        const int received = recvfrom(
            remoteSocket,
            reinterpret_cast<char*>(response),
            sizeof(response),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &fromLen
        );
        if (received >= 9 && response[4] == 2) { // UdpMsg::SyncReply
            std::uint32_t echoedRequest = 0;
            std::memcpy(&echoedRequest, response + 5, sizeof(echoedRequest));
            return echoedRequest == randomRequest;
        }
        Sleep(2);
    }
    return false;
}

} // namespace

int main() {
    WSADATA winsock = {};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    int result = 1;
    SOCKET remoteSocket = INVALID_SOCKET;
    GGPOSession* session = nullptr;

    do {
        unsigned short remotePort = 0;
        remoteSocket = BindLoopbackSocket(0, &remotePort);
        if (remoteSocket == INVALID_SOCKET) {
            std::fprintf(stderr, "Could not bind fake remote socket\n");
            break;
        }

        unsigned short localPort = 0;
        if (!ReserveUnusedPort(&localPort)) {
            std::fprintf(stderr, "Could not reserve GGPO local port\n");
            break;
        }

        GGPOSessionCallbacks callbacks = {};
        callbacks.begin_game = BeginGame;
        callbacks.save_game_state = SaveGameState;
        callbacks.load_game_state = LoadGameState;
        callbacks.log_game_state = LogGameState;
        callbacks.free_buffer = FreeBuffer;
        callbacks.advance_frame = AdvanceFrame;
        callbacks.on_event = OnEvent;

        if (ggpo_start_session(&session, &callbacks, "udp-validation", 2, 1, localPort) != GGPO_OK) {
            std::fprintf(stderr, "ggpo_start_session failed\n");
            break;
        }

        GGPOPlayer localPlayer = {};
        localPlayer.size = sizeof(localPlayer);
        localPlayer.type = GGPO_PLAYERTYPE_LOCAL;
        localPlayer.player_num = 1;
        GGPOPlayerHandle localHandle = GGPO_INVALID_HANDLE;
        if (ggpo_add_player(session, &localPlayer, &localHandle) != GGPO_OK) {
            std::fprintf(stderr, "Could not add local player\n");
            break;
        }

        GGPOPlayer remotePlayer = {};
        remotePlayer.size = sizeof(remotePlayer);
        remotePlayer.type = GGPO_PLAYERTYPE_REMOTE;
        remotePlayer.player_num = 2;
        strcpy_s(remotePlayer.u.remote.ip_address, "127.0.0.1");
        remotePlayer.u.remote.port = remotePort;
        GGPOPlayerHandle remoteHandle = GGPO_INVALID_HANDLE;
        if (ggpo_add_player(session, &remotePlayer, &remoteHandle) != GGPO_OK) {
            std::fprintf(stderr, "Could not add remote player\n");
            break;
        }

        const std::vector<std::vector<unsigned char>> invalidPackets = {
            { 'S' },
            { 'S', 'F' },
            { 'S', 'F', '4' },
            { 'S', 'F', '4', 'R' },
            { 'S', 'F', '4', 'W' },
            { 'S', 'F', '4', 'R', '@' },
            { 0, 0, 0, 0, 0 },
            { 0, 0, 0, 0, 1 },
            { 0, 0, 0, 0, 2 },
            { 0, 0, 0, 0, 3 },
            { 0, 0, 0, 0, 4 },
            { 0, 0, 0, 0, 5 },
            { 0, 0, 0, 0, 7 }
        };

        bool injectedAllPackets = true;
        for (const std::vector<unsigned char>& packet : invalidPackets) {
            if (!SendPacket(remoteSocket, localPort, packet)) {
                std::fprintf(stderr, "Could not inject malformed packet\n");
                injectedAllPackets = false;
                break;
            }
            Pump(session);
        }
        if (!injectedAllPackets) {
            break;
        }

        std::vector<unsigned char> oversizedInput(32, 0);
        oversizedInput[4] = 3; // UdpMsg::Input
        oversizedInput[29] = 0xff;
        oversizedInput[30] = 0xff;
        if (!SendPacket(remoteSocket, localPort, oversizedInput)) {
            std::fprintf(stderr, "Could not inject malformed input packet\n");
            break;
        }
        Pump(session);

        if (!ValidSyncRequestStillWorks(session, remoteSocket, localPort)) {
            std::fprintf(stderr, "Valid GGPO SyncRequest was not accepted after malformed traffic\n");
            break;
        }

        std::printf("GGPO rejected malformed UDP packets and accepted valid traffic\n");
        result = 0;
    } while (false);

    if (session) {
        ggpo_close_session(session);
    }
    if (remoteSocket != INVALID_SOCKET) {
        closesocket(remoteSocket);
    }
    WSACleanup();
    return result;
}
