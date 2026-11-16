#pragma once
#include <winsock2.h>
#include <atomic>

// The phone remote's name on the local network: answers mDNS questions for
// ember.local with this PC's address, so a phone can open the remote by name
// (RemoteServer.cxx starts it beside the web server).
namespace sf4e { namespace training { namespace remote { namespace mdns {
// The socket that hears the network's mDNS questions; INVALID_SOCKET when it cannot be had.
SOCKET Open();
// Answers A questions for ember.local from private addresses on `socket` until `running` goes false.
void Answer(SOCKET socket, const std::atomic<bool>& running);
} } } }
