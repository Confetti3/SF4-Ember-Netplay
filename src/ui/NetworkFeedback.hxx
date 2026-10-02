#pragma once
#include "../common/Localization.hxx"
#include "../common/NetworkNat.hxx"
#include "../common/NetworkRoute.hxx"
#include "../session/RoomFailure.hxx"
#include <string>

// The sentences about the player's own network and about where a connection
// failed. Pure functions of what the helper reported, so the settings row, the
// connection check, the failure messages and their tests share one wording.
// Only region codes, never relay addresses, come in here.
namespace sf4e { namespace ui {

// A relay region's name, or null when the code is empty or unknown.
inline const char* RelayRegionName(const std::string& code) {
    if (code == "use1") return loc::T("network.region_use1");
    if (code == "usw1") return loc::T("network.region_usw1");
    if (code == "euc1") return loc::T("network.region_euc1");
    if (code == "aps1") return loc::T("network.region_aps1");
    if (code == "other") return loc::T("network.region_other");
    return nullptr;
}

// "US East (connected)". Before the helper's first report the relay is being
// checked; with no home relay chosen it is not connected.
inline std::string DescribeRelay(const NetworkSummary& network) {
    if (!network.reported) return loc::T("connection.checking");
    const char* region = RelayRegionName(network.relay);
    if (!region) return loc::T("network.relay_none");
    return network.relayConnected ? loc::Tf("network.relay_connected", region) : loc::Tf("network.relay_connecting", region);
}

// "Open", "Strict NAT", "UDP blocked", or "Checking..." until the first net report.
inline std::string DescribeNat(NatClass nat) {
    switch (nat) {
    case NatClass::Open: return loc::T("network.nat_open");
    case NatClass::Strict: return loc::T("network.nat_strict");
    case NatClass::NoUdp: return loc::T("network.nat_no_udp");
    default: return loc::T("connection.checking");
    }
}

inline std::string DescribeNatDetail(const NetworkSummary& network) {
    std::string text;
    switch (network.nat) {
    case NatClass::Open: text = loc::T("network.nat_detail_open"); break;
    case NatClass::Strict: text = loc::T("network.nat_detail_strict"); break;
    case NatClass::NoUdp: text = loc::T("network.nat_detail_no_udp"); break;
    default: text = loc::T("network.nat_detail_checking"); break;
    }
    if (network.captivePortal) text += std::string("\n") + loc::T("network.captive_portal");
    return text;
}

// "Relayed", or "Relayed via US East" when the region of the relay is known.
inline std::string DescribeRelayedRoute(const std::string& region) {
    const char* name = RelayRegionName(region);
    return name ? loc::Tf("connection.route_relayed_via", name) : std::string(loc::T("connection.route_relayed"));
}

// Why the direct path probably failed, worded as a likely cause. Empty when
// there is nothing to say. `peerName` is a player's own text.
inline std::string DescribeDirectBlock(NatClass local, NatClass peer, const std::string& peerName) {
    switch (DiagnoseDirectPath(local, peer)) {
    case DirectBlock::Checking: return loc::T("connection.direct_checking");
    case DirectBlock::LocalNoUdp: return loc::T("connection.blocked_local_no_udp");
    case DirectBlock::PeerNoUdp: return loc::Tf("connection.blocked_peer_no_udp", peerName);
    case DirectBlock::LocalStrict: return loc::T("connection.blocked_local_strict");
    case DirectBlock::PeerStrict: return loc::Tf("connection.blocked_peer_strict", peerName);
    case DirectBlock::BothOpen: return loc::T("connection.blocked_both_open");
    default: return {};
    }
}

// The sentence for a room that failed while opening. `region` is this PC's
// home relay, named when the relay was the problem.
inline std::string DescribeOpeningFailure(bool hosting, session::FailureStage stage, const std::string& region) {
    switch (stage) {
    case session::FailureStage::RelayUnreachable: {
        const char* name = RelayRegionName(region);
        return name ? loc::Tf("runtime.relay_unreachable_region", name) : std::string(loc::T("runtime.relay_unreachable"));
    }
    case session::FailureStage::HostUnreachable: return hosting ? loc::T("runtime.room_host_failed") : loc::T("runtime.host_unreachable");
    case session::FailureStage::ControlLost: return hosting ? loc::T("runtime.room_host_failed") : loc::T("runtime.join_control_lost");
    // Only a join reads an invitation; a host never hears about one.
    case session::FailureStage::InviteExpired: return hosting ? loc::T("runtime.room_host_failed") : loc::T("runtime.invite_expired");
    case session::FailureStage::InviteOtherBuild: return hosting ? loc::T("runtime.room_host_failed") : loc::T("runtime.invite_other_build");
    case session::FailureStage::InviteMalformed: return hosting ? loc::T("runtime.room_host_failed") : loc::T("runtime.invite_malformed");
    case session::FailureStage::InviteOwnRoom: return hosting ? loc::T("runtime.room_host_failed") : loc::T("runtime.invite_own_room");
    case session::FailureStage::ShortUnavailable: return hosting ? loc::T("runtime.room_host_failed") : loc::T("runtime.invite_short_unavailable");
    case session::FailureStage::ShortUnknown: return hosting ? loc::T("runtime.room_host_failed") : loc::T("runtime.invite_short_unknown");
    default: return hosting ? loc::T("runtime.room_host_failed") : loc::T("runtime.room_join_failed");
    }
}

} }
