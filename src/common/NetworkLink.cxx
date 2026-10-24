#include "NetworkLink.hxx"

#include <cstdio>
#include <fstream>
#include <iterator>
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

// Under Wine an interface's alias is its Linux name and Z: maps to the
// host's root, so the host's own entry for it says what it is.
NetworkLink DetectLinuxNetworkLink(DWORD index, std::string* detail) {
    MIB_IF_ROW2 row = {};
    row.InterfaceIndex = index;
    if (GetIfEntry2(&row) != NO_ERROR) { if (detail) *detail += " [wine, unreadable]"; return NetworkLink::Unknown; }
    const std::wstring entry = std::wstring(L"Z:\\sys\\class\\net\\") + row.Alias + L"\\";
    const auto exists = [&](const wchar_t* leaf) { return GetFileAttributesW((entry + leaf).c_str()) != INVALID_FILE_ATTRIBUTES; };
    const auto read = [&](const wchar_t* leaf) {
        std::ifstream file(entry + leaf);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    };
    const bool wireless = exists(L"wireless"), device = exists(L"device");
    std::string type = read(L"type");
    type = type.substr(0, type.find('\n'));
    const std::string uevent = read(L"uevent");
    const auto at = uevent.find("DEVTYPE=");
    const std::string devtype = at == std::string::npos ? std::string() : uevent.substr(at + 8, uevent.find('\n', at) - at - 8);
    if (detail) {
        char line[256];
        std::snprintf(line, sizeof(line), " [wine %ls wireless=%d device=%d type=%s devtype=%s]", row.Alias, wireless ? 1 : 0, device ? 1 : 0,
            type.c_str(), devtype.c_str());
        *detail += line;
    }
    return LinuxInterfaceLink(wireless, device, type, devtype);
}
}

NetworkLink DetectNetworkLink(std::string* detail) {
    DWORD index = 0;
    if (!DefaultRouteInterface(index)) { if (detail) *detail = "no default route"; return NetworkLink::Unknown; }
    if (detail) *detail = "route=if" + std::to_string(index);
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return DetectLinuxNetworkLink(index, detail);
    const auto note = [detail](const MIB_IF_ROW2& row) {
        if (!detail) return;
        char model[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, row.Description, -1, model, sizeof(model) - 1, nullptr, nullptr);
        char line[256];
        std::snprintf(line, sizeof(line), " [if%lu hw=%d type=%lu medium=%lu up=%d '%s']", row.InterfaceIndex,
            row.InterfaceAndOperStatusFlags.HardwareInterface ? 1 : 0, static_cast<unsigned long>(row.Type),
            static_cast<unsigned long>(row.PhysicalMediumType), row.OperStatus == IfOperStatusUp ? 1 : 0, model);
        *detail += line;
    };
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
    std::vector<unsigned long> walked;
    const auto describe = [&](unsigned long interfaceIndex, InterfaceFacts& facts) {
        MIB_IF_ROW2 row = {};
        row.InterfaceIndex = interfaceIndex;
        if (GetIfEntry2(&row) != NO_ERROR) return false;
        note(row);
        walked.push_back(interfaceIndex);
        facts.hardware = row.InterfaceAndOperStatusFlags.HardwareInterface != FALSE;
        facts.type = row.Type;
        facts.medium = static_cast<unsigned long>(row.PhysicalMediumType);
        return true;
    };
    const auto link = ResolveNetworkLink(index, lowerLayers, describe);
    if (stack) FreeMibTable(stack);
    // Other hardware adapters that are up, such as a cable plugged in beside
    // Wi-Fi that Windows does not route through.
    PMIB_IF_TABLE2 table = nullptr;
    if (detail && GetIfTable2(&table) == NO_ERROR) {
        *detail += " others:";
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const auto& row = table->Table[i];
            if (!row.InterfaceAndOperStatusFlags.HardwareInterface || row.OperStatus != IfOperStatusUp) continue;
            if (std::find(walked.begin(), walked.end(), row.InterfaceIndex) != walked.end()) continue;
            note(row);
        }
        FreeMibTable(table);
    }
    return link;
}
}
