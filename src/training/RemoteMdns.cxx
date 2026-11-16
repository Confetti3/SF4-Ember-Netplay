#include "RemoteMdns.hxx"
#include "RemoteServer.hxx"
#include <ws2tcpip.h>
#include <cstring>

namespace sf4e { namespace training { namespace remote { namespace mdns {
SOCKET Open() {
    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket == INVALID_SOCKET) return socket;
    BOOL reuse = TRUE; setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in any{}; any.sin_family = AF_INET; any.sin_port = htons(5353);
    ip_mreq group{}; inet_pton(AF_INET, "224.0.0.251", &group.imr_multiaddr);
    if (bind(socket, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0 ||
        setsockopt(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&group), sizeof(group)) != 0) { closesocket(socket); return INVALID_SOCKET; }
    return socket;
}
// Answers an A query for ember.local with the address this PC has on the route to the asker.
void Answer(SOCKET dns, const std::atomic<bool>& running) {
    while (running) {
        fd_set set; FD_ZERO(&set); FD_SET(dns, &set);
        timeval wait{0, 200000};
        if (select(0, &set, nullptr, nullptr, &wait) <= 0) continue;
        unsigned char packet[512]; sockaddr_in from{}; int size = sizeof(from);
        const int got = recvfrom(dns, reinterpret_cast<char*>(packet), sizeof(packet), 0, reinterpret_cast<sockaddr*>(&from), &size);
        // Header 12 bytes, then "\5ember\5local\0", type A or ANY, class IN; a response (QR set) is not a question.
        static const unsigned char name[] = {5, 'e', 'm', 'b', 'e', 'r', 5, 'l', 'o', 'c', 'a', 'l', 0};
        if (got < 12 + static_cast<int>(sizeof(name)) + 4 || (packet[2] & 0x80) || !PrivatePeer(ntohl(from.sin_addr.s_addr))) continue;
        if (std::memcmp(packet + 12, name, sizeof(name)) != 0) continue;
        const int type = packet[12 + sizeof(name)] << 8 | packet[13 + sizeof(name)];
        if (type != 1 && type != 255) continue;
        SOCKET probe = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in route = from; route.sin_port = htons(9);
        sockaddr_in mine{}; int mineSize = sizeof(mine);
        const bool found = probe != INVALID_SOCKET && connect(probe, reinterpret_cast<sockaddr*>(&route), sizeof(route)) == 0 && getsockname(probe, reinterpret_cast<sockaddr*>(&mine), &mineSize) == 0;
        if (probe != INVALID_SOCKET) closesocket(probe);
        if (!found) continue;
        unsigned char answer[64] = {packet[0], packet[1], 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0};
        std::size_t at = 12;
        std::memcpy(answer + at, name, sizeof(name)); at += sizeof(name);
        const unsigned char tail[] = {0, 1, 0x80, 1, 0, 0, 0, 120, 0, 4};
        std::memcpy(answer + at, tail, sizeof(tail)); at += sizeof(tail);
        std::memcpy(answer + at, &mine.sin_addr, 4); at += 4;
        // Unicast to the asker: enough for a one-shot lookup, and spares the multicast group.
        sendto(dns, reinterpret_cast<const char*>(answer), static_cast<int>(at), 0, reinterpret_cast<sockaddr*>(&from), sizeof(from));
    }
}
} } } }
