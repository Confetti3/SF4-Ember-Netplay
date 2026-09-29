#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sf4e {
// How a player's machine reaches the internet, shown beside their name.
// Wired and Wireless are claimed only when the adapter clearly is one;
// Unknown covers everything else (VPN tunnels, internal virtual switches,
// mixed or unreadable adapters, Bluetooth, mobile, Wine), so the room never
// claims a link it did not see.
enum class NetworkLink : std::uint8_t { Unknown = 0, Wired, Wireless };

// A value from a newer peer that this build does not know reads as Unknown,
// so a new kind never locks that peer out of a room.
inline NetworkLink NetworkLinkFromWire(long long value) {
    return value == static_cast<long long>(NetworkLink::Wired) ? NetworkLink::Wired :
        value == static_cast<long long>(NetworkLink::Wireless) ? NetworkLink::Wireless : NetworkLink::Unknown;
}

// Untranslated names for logs.
inline const char* NetworkLinkLabel(NetworkLink link) {
    return link == NetworkLink::Wired ? "wired" : link == NetworkLink::Wireless ? "wireless" : "unknown";
}

// The Windows values ClassifyInterface reads (IF_TYPE_*, NDIS_PHYSICAL_MEDIUM);
// NetworkLink.cxx checks them against the SDK.
namespace interface_values {
constexpr unsigned long EthernetType = 6, Ieee80211Type = 71;
constexpr unsigned long UnspecifiedMedium = 0, WirelessLanMedium = 1, Native80211Medium = 9, Ethernet802_3Medium = 14;
}

// One interface as GetIfEntry2 describes it. Only a hardware interface
// counts: VPNs, Hyper-V switches and overlay networks present virtual
// Ethernet, which says nothing about the cable or radio underneath (see
// ResolveNetworkLink). Only a clear answer is given:
// - Wi-Fi is the 802.11 interface type or the native 802.11 medium, which
//   older Wi-Fi drivers pair with the Ethernet type;
// - a cable is the Ethernet type with the 802.3 medium or none;
// - anything else is Unknown, including the Ethernet type with the legacy
//   wireless-LAN medium, which some wired drivers report, and other media
//   such as Bluetooth or powerline behind an Ethernet type.
inline NetworkLink ClassifyInterface(bool hardware, unsigned long type, unsigned long medium) {
    using namespace interface_values;
    if (!hardware) return NetworkLink::Unknown;
    if (type == Ieee80211Type || medium == Native80211Medium) return NetworkLink::Wireless;
    if (type == EthernetType && (medium == Ethernet802_3Medium || medium == UnspecifiedMedium)) return NetworkLink::Wired;
    return NetworkLink::Unknown;
}

struct InterfaceFacts {
    bool hardware = false;
    unsigned long type = 0, medium = 0;
};

// The link under an interface. A virtual one (a Hyper-V external switch, a
// bridge, a team) is followed down its interface stack to the hardware it is
// bound to. None underneath (an internal switch, a VPN tunnel) is Unknown, and
// so is hardware of more than one kind. A known answer needs the whole stack:
// an interface that cannot be read, or a stack past the size limit, leaves it
// Unknown. `lowerLayers(index)` lists the interfaces directly below;
// `describe(index, facts)` fills one in or fails.
template <typename LowerLayers, typename Describe>
NetworkLink ResolveNetworkLink(unsigned long index, LowerLayers lowerLayers, Describe describe) {
    constexpr std::size_t MaximumInterfaces = 64;
    std::vector<unsigned long> pending{index}, seen;
    bool found = false;
    NetworkLink link = NetworkLink::Unknown;
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (std::find(seen.begin(), seen.end(), current) != seen.end()) continue;
        if (seen.size() == MaximumInterfaces) return NetworkLink::Unknown;
        seen.push_back(current);
        InterfaceFacts facts;
        if (!describe(current, facts)) return NetworkLink::Unknown;
        if (facts.hardware) {
            const auto kind = ClassifyInterface(true, facts.type, facts.medium);
            if (found && kind != link) return NetworkLink::Unknown;
            found = true;
            link = kind;
            continue;
        }
        for (const auto lower : lowerLayers(current)) pending.push_back(lower);
    }
    return link;
}

// The link carrying this machine's default route. Reads the routing table and
// interface entries only; sends nothing. `detail`, when given, names for the
// log what was read: the route's interface and every interface in its stack
// (index, hardware, type, medium, adapter model), and any other hardware
// adapter that is up, so a wrong answer can be traced to the adapter that
// gave it.
NetworkLink DetectNetworkLink(std::string* detail = nullptr);
}
