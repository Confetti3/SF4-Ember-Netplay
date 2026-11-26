#pragma once

#include <winsock2.h>

namespace sf4e {
namespace netplay {

// What a failed sendto on GGPO's UDP socket means for the endpoint that sent.
// Stock GGPO asserted on every error, which exits the game. UDP may lose any
// datagram and GGPO resends what the session needs, so a Transient error only
// loses this datagram. A Lasting one disconnects the endpoint that sent; an
// error about the whole socket fails every endpoint's own next send the same way.
enum class SendError { Transient, Lasting };

// The sendto errors that describe this moment or an earlier datagram, not the
// destination or the socket (Winsock sendto reference).
inline SendError ClassifySendError(int wsaerr) {
    switch (wsaerr) {
    case WSAEWOULDBLOCK:  // a nonblocking socket's send buffer is full
    case WSAENOBUFS:      // no buffer space is available
    case WSAECONNRESET:   // an earlier datagram drew ICMP port unreachable
    case WSAENETRESET:    // an earlier datagram's time to live expired
    case WSAEHOSTUNREACH: // no route to the host
    case WSAENETUNREACH:  // the network cannot be reached at this time
        return SendError::Transient;
    default:
        return SendError::Lasting;
    }
}

} // namespace netplay
} // namespace sf4e
