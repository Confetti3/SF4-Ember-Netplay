// The connection sentences: what a connection check says about its route,
// where the direct path most likely failed, and why a room failed to open.
// Pure wording over the helper's reports; the room screens draw these as is.
#include "../ui/NetworkFeedback.hxx"
#include "../ui/RoomFeedback.hxx"
#include "../common/Localization.hxx"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() try {
    using namespace sf4e;
    using namespace sf4e::ui;
    sf4e::loc::SetActive(sf4e::loc::Locale::En);

    // A relayed check explains itself; a direct one does not carry the advice.
    ShellView measured;
    measured.probeStatus = "ready"; measured.recommendedDelay = 3; measured.probeRoute = sf4e::RouteKind::Relayed;
    const auto relayed = DescribeConnectionCheck(measured);
    Check(relayed.detail.rfind("Relayed connection.", 0) == 0, "Relayed check result lost its route label");
    Check(relayed.detail.find(sf4e::loc::T("connection.relayed_advice")) != std::string::npos,
        "Relayed check result did not explain the relay");
    measured.probeRoute = sf4e::RouteKind::Direct;
    const auto direct = DescribeConnectionCheck(measured);
    Check(direct.detail.rfind("Direct connection.", 0) == 0 &&
        direct.detail.find(sf4e::loc::T("connection.relayed_advice")) == std::string::npos,
        "Direct check result carried relay advice");
    sf4e::loc::SetActive(sf4e::loc::Locale::Es419);
    measured.probeRoute = sf4e::RouteKind::Relayed;
    const auto translated = DescribeConnectionCheck(measured).detail;
    Check(translated.find(std::string(" ") + sf4e::loc::T("connection.route_relayed") + ".") != std::string::npos &&
        translated.find("Relayed") == std::string::npos,
        "Relayed route label was not translated");
    sf4e::loc::SetActive(sf4e::loc::Locale::En);

    // A relayed check names the relay's region and where the direct path most
    // likely failed, as a likely cause, from both sides' network reports.
    using sf4e::NatClass;
    measured.probeRelay = "euc1"; measured.probeOpponent = "Ken";
    Check(DescribeConnectionCheck(measured).detail.rfind("Relayed (Europe) connection.", 0) == 0, "A relayed check did not name its region");
    measured.probeRelay = "other";
    Check(DescribeConnectionCheck(measured).detail.rfind("Relayed (Other region) connection.", 0) == 0, "An unpinned relay was not reduced to other");
    measured.probeRelay.clear();
    Check(DescribeConnectionCheck(measured).detail.rfind("Relayed connection.", 0) == 0, "A relay without a region changed the route label");
    const auto block = [&](NatClass local, NatClass peer, bool reported = true) {
        measured.netReport.reported = reported; measured.netReport.nat = local; measured.probeOpponentNat = peer;
        return DescribeConnectionCheck(measured);
    };
    const auto has = [](const ConnectionCheckFeedback& check, const char* id, const char* name = "") {
        const std::string sentence = *name ? sf4e::loc::Tf(id, name) : std::string(sf4e::loc::T(id));
        return check.detail.find(sentence) != std::string::npos;
    };
    Check(has(block(NatClass::Unknown, NatClass::Strict, false), "connection.direct_checking"), "A network still being checked did not say so");
    Check(has(block(NatClass::NoUdp, NatClass::Open), "connection.blocked_local_no_udp"), "Blocked local UDP was not named");
    Check(has(block(NatClass::Strict, NatClass::Open), "connection.blocked_local_strict"), "Local strict NAT was not named");
    Check(has(block(NatClass::Open, NatClass::NoUdp), "connection.blocked_peer_no_udp", "Ken") && block(NatClass::Open, NatClass::NoUdp).namesPlayer,
        "The opponent's blocked UDP was not named with their name");
    Check(has(block(NatClass::Open, NatClass::Strict), "connection.blocked_peer_strict", "Ken"), "The opponent's strict NAT was not named");
    Check(has(block(NatClass::Open, NatClass::Open), "connection.blocked_both_open") && !block(NatClass::Open, NatClass::Open).namesPlayer,
        "Two open networks did not point at a firewall or VPN");
    // Nothing is claimed about a peer whose class is not known, and a direct route carries none of this.
    Check(block(NatClass::Open, NatClass::Unknown).detail.find(sf4e::loc::T("connection.blocked_both_open")) == std::string::npos,
        "An unknown opponent was declared open");
    measured.probeRoute = sf4e::RouteKind::Direct;
    Check(!has(block(NatClass::Open, NatClass::Open), "connection.blocked_both_open"), "A direct route carried a blocked-path explanation");
    measured.probeRoute = sf4e::RouteKind::Relayed;

    // A room that failed while opening says where: the relay (named), the host, or the
    // link that kept dropping. Hosting has its own wording, and no stage is the generic one.
    using sf4e::session::FailureStage;
    const auto failure = [](bool hosting, FailureStage stage, const char* region = "") {
        return DescribeOpeningFailure(hosting, stage, region);
    };
    Check(failure(false, FailureStage::RelayUnreachable, "usw1") == sf4e::loc::Tf("runtime.relay_unreachable_region", sf4e::loc::T("network.region_usw1")),
        "An unreachable relay was not named by region");
    Check(failure(true, FailureStage::RelayUnreachable) == sf4e::loc::T("runtime.relay_unreachable") &&
        failure(false, FailureStage::RelayUnreachable, "https://use1-1.relay.n0.iroh.link./") == sf4e::loc::T("runtime.relay_unreachable"),
        "An unreachable relay without a known region did not use the plain sentence");
    Check(failure(false, FailureStage::HostUnreachable) == sf4e::loc::T("runtime.host_unreachable"), "An unreachable host has no sentence");
    Check(failure(false, FailureStage::ControlLost) == sf4e::loc::T("runtime.join_control_lost"), "A dropped link has no sentence");
    Check(failure(false, FailureStage::Unknown) == sf4e::loc::T("runtime.room_join_failed"), "A join without a stage lost its generic sentence");
    Check(failure(false, FailureStage::InviteExpired) == sf4e::loc::T("runtime.invite_expired") &&
        failure(false, FailureStage::InviteOtherBuild) == sf4e::loc::T("runtime.invite_other_build") &&
        failure(false, FailureStage::InviteMalformed) == sf4e::loc::T("runtime.invite_malformed") &&
        failure(false, FailureStage::InviteOwnRoom) == sf4e::loc::T("runtime.invite_own_room") &&
        failure(false, FailureStage::ShortUnavailable) == sf4e::loc::T("runtime.invite_short_unavailable") &&
        failure(false, FailureStage::ShortUnknown) == sf4e::loc::T("runtime.invite_short_unknown"),
        "A refused invitation lost its reason");
    Check(failure(true, FailureStage::InviteExpired) == sf4e::loc::T("runtime.room_host_failed") &&
        failure(true, FailureStage::InviteOtherBuild) == sf4e::loc::T("runtime.room_host_failed") &&
        failure(true, FailureStage::InviteMalformed) == sf4e::loc::T("runtime.room_host_failed") &&
        failure(true, FailureStage::InviteOwnRoom) == sf4e::loc::T("runtime.room_host_failed") &&
        failure(true, FailureStage::ShortUnknown) == sf4e::loc::T("runtime.room_host_failed"),
        "A host was told about an invitation");
    Check(failure(true, FailureStage::Unknown) == sf4e::loc::T("runtime.room_host_failed") &&
        failure(true, FailureStage::HostUnreachable) == sf4e::loc::T("runtime.room_host_failed"),
        "A failed host says it could not join");
    Check(failure(true, FailureStage::Unknown) != failure(false, FailureStage::Unknown), "Host and join failures read the same");
    // A public room's join says what an unnamed failure likely was; a named stage is the same sentence as for any join.
    const auto publicFailure = [](FailureStage stage, const char* region = "") { return DescribePublicOpeningFailure(stage, region); };
    Check(publicFailure(FailureStage::Unknown) == sf4e::loc::T("public.failure.join_failed") &&
        publicFailure(FailureStage::Unknown) != failure(false, FailureStage::Unknown), "A public join without a stage lost its own sentence");
    Check(publicFailure(FailureStage::RelayUnreachable, "usw1") == failure(false, FailureStage::RelayUnreachable, "usw1") &&
        publicFailure(FailureStage::HostUnreachable) == failure(false, FailureStage::HostUnreachable) &&
        publicFailure(FailureStage::ControlLost) == failure(false, FailureStage::ControlLost),
        "A public join changed the wording of a failure with a named stage");

    std::cout << "Connection check, blocked-path and opening-failure wording passed.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
