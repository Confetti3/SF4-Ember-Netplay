#pragma once
#include <string>

// The phone remote: a small web server on the local network only, so a phone
// can change the Training dummy's settings from its browser. The phone finds
// the PC by its LAN address (or the name ember.local, mDNS) and proves it is in the room with the
// PIN the game shows. Peers outside private address ranges are refused.
namespace sf4e { namespace training { namespace remote {
// Starts the server and the name answer; false when no port was free. Any thread, once.
bool Start();
void Stop();
// The PIN the phone types, and the address it opens ("http://192.168.0.57:47321", the PC's LAN address; ember.local when it has none); empty while stopped.
std::string Pin();
std::string Url();
// What the QR code holds: this PC's private address, the port and the PIN, so the phone signs in by scanning. Empty while stopped or with no private address.
std::string Link();
// Opens Link() in the default browser on this PC; false while stopped or when Windows has no browser to open it.
bool Open();
// Whether the peer's IPv4 address (host order) is loopback, private or link-local.
bool PrivatePeer(unsigned long address);
} } }
