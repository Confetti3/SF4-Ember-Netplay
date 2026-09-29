#pragma once
#include <cstdint>
#include <string>

namespace sf4e {
// How a player's network treats the direct path to a peer, as the helper's
// net report saw it. Only this class and a relay region code leave the helper,
// never an address, so logs, exports, the room and the interface stay clear of
// them. Unknown covers both "still checking" and a peer from a build that does
// not send it.
enum class NatClass : std::uint8_t { Unknown = 0, Open, Strict, NoUdp };

// A value from a newer peer that this build does not know reads as Unknown,
// so a new class never locks that peer out of a room.
inline NatClass NatClassFromWire(long long value) {
    return value == static_cast<long long>(NatClass::Open) ? NatClass::Open :
        value == static_cast<long long>(NatClass::Strict) ? NatClass::Strict :
        value == static_cast<long long>(NatClass::NoUdp) ? NatClass::NoUdp : NatClass::Unknown;
}

// The helper's names in its network_report event; "checking" and anything
// else read as Unknown.
inline NatClass NatClassFromHelper(const std::string& name) {
    return name == "open" ? NatClass::Open : name == "strict" ? NatClass::Strict :
        name == "no_udp" ? NatClass::NoUdp : NatClass::Unknown;
}

// Untranslated names for logs and diagnostics exports.
inline const char* NatClassLabel(NatClass nat) {
    switch (nat) {
    case NatClass::Open: return "open";
    case NatClass::Strict: return "strict NAT";
    case NatClass::NoUdp: return "UDP blocked";
    default: return "checking";
    }
}

// The local endpoint's network as the helper last reported it. `relay` is a
// region code ("use1", "usw1", "euc1", "aps1", "other") or empty while no home
// relay is chosen; `nat` stays Unknown until the first net report.
struct NetworkSummary {
    bool reported = false;
    NatClass nat = NatClass::Unknown;
    bool udp = false, captivePortal = false, relayConnected = false;
    std::string relay;
    bool Checking() const { return nat == NatClass::Unknown; }
    bool operator==(const NetworkSummary& other) const {
        return reported == other.reported && nat == other.nat && udp == other.udp &&
            captivePortal == other.captivePortal && relayConnected == other.relayConnected && relay == other.relay;
    }
    bool operator!=(const NetworkSummary& other) const { return !(*this == other); }
};

// Where the direct path most likely failed, from both sides' classes. These
// are likely causes, not proof: a strict NAT on one side often still connects
// to an open one, and a firewall or VPN can block two open networks.
enum class DirectBlock {
    None,        // nothing to say (the other side's class is not known)
    Checking,    // this side's own check has not finished
    LocalNoUdp,
    PeerNoUdp,
    LocalStrict,
    PeerStrict,
    BothOpen
};

inline DirectBlock DiagnoseDirectPath(NatClass local, NatClass peer) {
    if (local == NatClass::Unknown) return DirectBlock::Checking;
    if (local == NatClass::NoUdp) return DirectBlock::LocalNoUdp;
    if (peer == NatClass::NoUdp) return DirectBlock::PeerNoUdp;
    if (local == NatClass::Strict) return DirectBlock::LocalStrict;
    if (peer == NatClass::Strict) return DirectBlock::PeerStrict;
    return peer == NatClass::Open ? DirectBlock::BothOpen : DirectBlock::None;
}
}
