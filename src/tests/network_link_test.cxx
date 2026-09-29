#include "../common/NetworkLink.hxx"
#include "../common/NetworkNat.hxx"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

using namespace sf4e;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

int main() {
	using namespace interface_values;
	constexpr unsigned long Ethernet802_3Medium = 14, Unspecified = 0, TunnelType = 131;
	// Physical adapters.
	CHECK(ClassifyInterface(true, EthernetType, Ethernet802_3Medium) == NetworkLink::Wired);
	CHECK(ClassifyInterface(true, Ieee80211Type, Native80211Medium) == NetworkLink::Wireless);
	// An older Wi-Fi driver: Ethernet type, 802.11 medium.
	CHECK(ClassifyInterface(true, EthernetType, Native80211Medium) == NetworkLink::Wireless);
	CHECK(ClassifyInterface(true, EthernetType, WirelessLanMedium) == NetworkLink::Wireless);
	// Virtual Ethernet (VPN, Hyper-V switch, overlay network) hides the real link.
	CHECK(ClassifyInterface(false, EthernetType, Ethernet802_3Medium) == NetworkLink::Unknown);
	CHECK(ClassifyInterface(false, EthernetType, Unspecified) == NetworkLink::Unknown);
	CHECK(ClassifyInterface(false, Ieee80211Type, Native80211Medium) == NetworkLink::Unknown);
	CHECK(ClassifyInterface(true, TunnelType, Unspecified) == NetworkLink::Unknown);
	// The wire accepts only known kinds; anything else reads as Unknown.
	CHECK(NetworkLinkFromWire(1) == NetworkLink::Wired && NetworkLinkFromWire(2) == NetworkLink::Wireless);
	CHECK(NetworkLinkFromWire(0) == NetworkLink::Unknown && NetworkLinkFromWire(3) == NetworkLink::Unknown &&
		NetworkLinkFromWire(-1) == NetworkLink::Unknown);
	// Stacks as Windows reports them. Real NIC 26 and Wi-Fi 11 are hardware;
	// the rest are virtual: vEthernet over a filter over a vSwitch over a
	// filter over its NIC (a Hyper-V external switch), an internal switch with
	// nothing below, a tunnel, and a loop.
	const std::vector<std::pair<unsigned long, unsigned long>> stack = {
		{7, 32}, {32, 16}, {16, 42}, {42, 26},        // external switch on the wired NIC
		{28, 27}, {27, 20},                           // internal switch
		{60, 61}, {61, 11},                           // external switch on Wi-Fi
		{70, 26}, {70, 11},                           // a team of both kinds
		{80, 81}, {81, 80},                           // a loop
	};
	const auto lowerLayers = [&](unsigned long higher) {
		std::vector<unsigned long> lower;
		for (const auto& edge : stack) if (edge.first == higher) lower.push_back(edge.second);
		return lower;
	};
	const auto describe = [&](unsigned long index, InterfaceFacts& facts) {
		if (index == 99) return false;
		facts.hardware = index == 26 || index == 11;
		facts.type = index == 11 ? Ieee80211Type : EthernetType;
		facts.medium = index == 11 ? Native80211Medium : Ethernet802_3Medium;
		return true;
	};
	CHECK(ResolveNetworkLink(7, lowerLayers, describe) == NetworkLink::Wired);
	CHECK(ResolveNetworkLink(26, lowerLayers, describe) == NetworkLink::Wired);
	CHECK(ResolveNetworkLink(60, lowerLayers, describe) == NetworkLink::Wireless);
	CHECK(ResolveNetworkLink(28, lowerLayers, describe) == NetworkLink::Unknown);
	CHECK(ResolveNetworkLink(52, lowerLayers, describe) == NetworkLink::Unknown);
	CHECK(ResolveNetworkLink(70, lowerLayers, describe) == NetworkLink::Unknown);
	CHECK(ResolveNetworkLink(80, lowerLayers, describe) == NetworkLink::Unknown);
	CHECK(ResolveNetworkLink(99, lowerLayers, describe) == NetworkLink::Unknown);
	// An unreadable branch is not proof it holds no other hardware, in either
	// enumeration order.
	for (const bool failedFirst : {false, true}) {
		const auto withUnreadable = [&](unsigned long higher) {
			return higher == 90 ? (failedFirst ? std::vector<unsigned long>{99, 26} : std::vector<unsigned long>{26, 99}) :
				std::vector<unsigned long>{};
		};
		CHECK(ResolveNetworkLink(90, withUnreadable, describe) == NetworkLink::Unknown);
	}
	// A stack past the size limit is never answered from the part that was
	// read: a root over N hardware interfaces, one of them Wi-Fi, placed first
	// or last. At the limit (root plus 63) the walk completes.
	const auto fan = [](unsigned long children, bool wifiFirst) {
		return [=](unsigned long higher) {
			std::vector<unsigned long> lower;
			if (higher != 1000) return lower;
			for (unsigned long i = 0; i < children; ++i) lower.push_back(1001 + i);
			if (!wifiFirst) std::reverse(lower.begin(), lower.end());
			return lower;
		};
	};
	const auto fanFacts = [&](unsigned long index, InterfaceFacts& facts) {
		facts.hardware = index != 1000;
		facts.type = index == 1001 ? Ieee80211Type : EthernetType;
		facts.medium = index == 1001 ? Native80211Medium : Ethernet802_3Medium;
		return true;
	};
	for (const bool wifiFirst : {false, true}) {
		CHECK(ResolveNetworkLink(1000, fan(64, wifiFirst), fanFacts) == NetworkLink::Unknown);
		CHECK(ResolveNetworkLink(1000, fan(63, wifiFirst), fanFacts) == NetworkLink::Unknown);
	}
	const auto ethernetFacts = [&](unsigned long index, InterfaceFacts& facts) {
		facts.hardware = index != 1000; facts.type = EthernetType; facts.medium = Ethernet802_3Medium;
		return true;
	};
	CHECK(ResolveNetworkLink(1000, fan(63, true), ethernetFacts) == NetworkLink::Wired);
	CHECK(ResolveNetworkLink(1000, fan(64, true), ethernetFacts) == NetworkLink::Unknown);
	// The NAT class travels the same way as the link: known values only.
	CHECK(NatClassFromWire(1) == NatClass::Open && NatClassFromWire(2) == NatClass::Strict && NatClassFromWire(3) == NatClass::NoUdp);
	CHECK(NatClassFromWire(0) == NatClass::Unknown && NatClassFromWire(4) == NatClass::Unknown && NatClassFromWire(-1) == NatClass::Unknown);
	// The helper's names; "checking" and anything unexpected are not a class.
	CHECK(NatClassFromHelper("open") == NatClass::Open && NatClassFromHelper("strict") == NatClass::Strict &&
		NatClassFromHelper("no_udp") == NatClass::NoUdp);
	CHECK(NatClassFromHelper("checking") == NatClass::Unknown && NatClassFromHelper("") == NatClass::Unknown &&
		NatClassFromHelper("Open") == NatClass::Unknown && NatClassFromHelper("symmetric") == NatClass::Unknown);
	{
		NetworkSummary summary;
		CHECK(!summary.reported && summary.Checking() && summary.relay.empty());
		summary.nat = NatClass::Open; CHECK(!summary.Checking());
		NetworkSummary other = summary; CHECK(summary == other);
		other.relayConnected = true; CHECK(summary != other);
	}
	// Where the direct path most likely failed. This side's own check comes first,
	// then blocked UDP on either side, then strict NAT, then two open networks.
	CHECK(DiagnoseDirectPath(NatClass::Unknown, NatClass::NoUdp) == DirectBlock::Checking);
	CHECK(DiagnoseDirectPath(NatClass::NoUdp, NatClass::Open) == DirectBlock::LocalNoUdp);
	CHECK(DiagnoseDirectPath(NatClass::NoUdp, NatClass::NoUdp) == DirectBlock::LocalNoUdp);
	CHECK(DiagnoseDirectPath(NatClass::Strict, NatClass::NoUdp) == DirectBlock::PeerNoUdp);
	CHECK(DiagnoseDirectPath(NatClass::Open, NatClass::NoUdp) == DirectBlock::PeerNoUdp);
	CHECK(DiagnoseDirectPath(NatClass::Strict, NatClass::Strict) == DirectBlock::LocalStrict);
	CHECK(DiagnoseDirectPath(NatClass::Strict, NatClass::Open) == DirectBlock::LocalStrict);
	CHECK(DiagnoseDirectPath(NatClass::Open, NatClass::Strict) == DirectBlock::PeerStrict);
	CHECK(DiagnoseDirectPath(NatClass::Open, NatClass::Open) == DirectBlock::BothOpen);
	// A peer whose class is not known (an older build, or still checking) is not called open.
	CHECK(DiagnoseDirectPath(NatClass::Open, NatClass::Unknown) == DirectBlock::None);
	CHECK(DiagnoseDirectPath(NatClass::Strict, NatClass::Unknown) == DirectBlock::LocalStrict);
	// The live answer depends on the machine; it must only not fail.
	std::printf("This machine's link: %s\n", NetworkLinkLabel(DetectNetworkLink()));
	if (failures) return 1;
	std::puts("NetworkLink test passed");
	return 0;
}
