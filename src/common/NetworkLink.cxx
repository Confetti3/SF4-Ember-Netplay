#include "NetworkLink.hxx"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

namespace sf4e {
static_assert(interface_values::EthernetType == IF_TYPE_ETHERNET_CSMACD, "IF_TYPE_ETHERNET_CSMACD");
static_assert(interface_values::Ieee80211Type == IF_TYPE_IEEE80211, "IF_TYPE_IEEE80211");
static_assert(interface_values::WirelessLanMedium == NdisPhysicalMediumWirelessLan, "NdisPhysicalMediumWirelessLan");
static_assert(interface_values::Native80211Medium == NdisPhysicalMediumNative802_11, "NdisPhysicalMediumNative802_11");

namespace {
// Documentation addresses (RFC 5737, RFC 3849): off every local network, so
// the route to them is the default route.
bool DefaultRouteInterface(DWORD& index) {
    sockaddr_in v4 = {};
    v4.sin_family = AF_INET;
    inet_pton(AF_INET, "192.0.2.1", &v4.sin_addr);
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&v4), &index) == NO_ERROR) return true;
    sockaddr_in6 v6 = {};
    v6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "2001:db8::1", &v6.sin6_addr);
    return GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&v6), &index) == NO_ERROR;
}
}

NetworkLink DetectNetworkLink() {
    // Wine reports every host interface as Ethernet, Wi-Fi included.
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return NetworkLink::Unknown;
    DWORD index = 0;
    if (!DefaultRouteInterface(index)) return NetworkLink::Unknown;
    // Without a stack table only the route's own interface can be read.
    PMIB_IFSTACK_TABLE stack = nullptr;
    if (GetIfStackTable(&stack) != NO_ERROR) stack = nullptr;
    const auto lowerLayers = [stack](unsigned long higher) {
        std::vector<unsigned long> lower;
        for (ULONG i = 0; stack && i < stack->NumEntries; ++i)
            if (stack->Table[i].HigherLayerInterfaceIndex == higher && stack->Table[i].LowerLayerInterfaceIndex)
                lower.push_back(stack->Table[i].LowerLayerInterfaceIndex);
        return lower;
    };
    const auto describe = [](unsigned long interfaceIndex, InterfaceFacts& facts) {
        MIB_IF_ROW2 row = {};
        row.InterfaceIndex = interfaceIndex;
        if (GetIfEntry2(&row) != NO_ERROR) return false;
        facts.hardware = row.InterfaceAndOperStatusFlags.HardwareInterface != FALSE;
        facts.type = row.Type;
        facts.medium = static_cast<unsigned long>(row.PhysicalMediumType);
        return true;
    };
    const auto link = ResolveNetworkLink(index, lowerLayers, describe);
    if (stack) FreeMibTable(stack);
    return link;
}
}
