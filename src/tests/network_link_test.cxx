#include "../common/NetworkLink.hxx"
#include "../common/NetworkNat.hxx"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>
#include "test_support.hxx"

using namespace sf4e;

int main() {
	using namespace interface_values;
	constexpr unsigned long Unspecified = UnspecifiedMedium, TunnelType = 131, BluetoothMedium = 10, PowerLineMedium = 4;
	// Physical adapters.
	EXPECT(ClassifyInterface(true, EthernetType, Ethernet802_3Medium) == NetworkLink::Wired);
	EXPECT(ClassifyInterface(true, Ieee80211Type, Native80211Medium) == NetworkLink::Wireless);
	// An older Wi-Fi driver: Ethernet type, 802.11 medium.
	EXPECT(ClassifyInterface(true, EthernetType, Native80211Medium) == NetworkLink::Wireless);
	// A wired card reporting no medium is still a cable.
	EXPECT(ClassifyInterface(true, EthernetType, Unspecified) == NetworkLink::Wired);
	// Only a clear answer: the legacy wireless-LAN medium, which some wired
	// drivers report, and other media behind an Ethernet type are Unknown.
	EXPECT(ClassifyInterface(true, EthernetType, WirelessLanMedium) == NetworkLink::Unknown);
	EXPECT(ClassifyInterface(true, EthernetType, BluetoothMedium) == NetworkLink::Unknown);
	EXPECT(ClassifyInterface(true, EthernetType, PowerLineMedium) == NetworkLink::Unknown);
	// Virtual Ethernet (VPN, Hyper-V switch, overlay network) hides the real link.
	EXPECT(ClassifyInterface(false, EthernetType, Ethernet802_3Medium) == NetworkLink::Unknown);
	EXPECT(ClassifyInterface(false, EthernetType, Unspecified) == NetworkLink::Unknown);
	EXPECT(ClassifyInterface(false, Ieee80211Type, Native80211Medium) == NetworkLink::Unknown);
	EXPECT(ClassifyInterface(true, TunnelType, Unspecified) == NetworkLink::Unknown);
	// The wire accepts only known kinds; anything else reads as Unknown.
	EXPECT(NetworkLinkFromWire(1) == NetworkLink::Wired && NetworkLinkFromWire(2) == NetworkLink::Wireless);
	EXPECT(NetworkLinkFromWire(0) == NetworkLink::Unknown && NetworkLinkFromWire(3) == NetworkLink::Unknown &&
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
	EXPECT(ResolveNetworkLink(7, lowerLayers, describe) == NetworkLink::Wired);
	EXPECT(ResolveNetworkLink(26, lowerLayers, describe) == NetworkLink::Wired);
	EXPECT(ResolveNetworkLink(60, lowerLayers, describe) == NetworkLink::Wireless);
	EXPECT(ResolveNetworkLink(28, lowerLayers, describe) == NetworkLink::Unknown);
	EXPECT(ResolveNetworkLink(52, lowerLayers, describe) == NetworkLink::Unknown);
	EXPECT(ResolveNetworkLink(70, lowerLayers, describe) == NetworkLink::Unknown);
	EXPECT(ResolveNetworkLink(80, lowerLayers, describe) == NetworkLink::Unknown);
	EXPECT(ResolveNetworkLink(99, lowerLayers, describe) == NetworkLink::Unknown);
	// An unreadable branch is not proof it holds no other hardware, in either
	// enumeration order.
	for (const bool failedFirst : {false, true}) {
		const auto withUnreadable = [&](unsigned long higher) {
			return higher == 90 ? (failedFirst ? std::vector<unsigned long>{99, 26} : std::vector<unsigned long>{26, 99}) :
				std::vector<unsigned long>{};
		};
		EXPECT(ResolveNetworkLink(90, withUnreadable, describe) == NetworkLink::Unknown);
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
		EXPECT(ResolveNetworkLink(1000, fan(64, wifiFirst), fanFacts) == NetworkLink::Unknown);
		EXPECT(ResolveNetworkLink(1000, fan(63, wifiFirst), fanFacts) == NetworkLink::Unknown);
	}
	const auto ethernetFacts = [&](unsigned long index, InterfaceFacts& facts) {
		facts.hardware = index != 1000; facts.type = EthernetType; facts.medium = Ethernet802_3Medium;
		return true;
	};
	EXPECT(ResolveNetworkLink(1000, fan(63, true), ethernetFacts) == NetworkLink::Wired);
	EXPECT(ResolveNetworkLink(1000, fan(64, true), ethernetFacts) == NetworkLink::Unknown);
	// The NAT class travels the same way as the link: known values only.
	EXPECT(NatClassFromWire(1) == NatClass::Open && NatClassFromWire(2) == NatClass::Strict && NatClassFromWire(3) == NatClass::NoUdp);
	EXPECT(NatClassFromWire(0) == NatClass::Unknown && NatClassFromWire(4) == NatClass::Unknown && NatClassFromWire(-1) == NatClass::Unknown);
	// The helper's names; "checking" and anything unexpected are not a class.
	EXPECT(NatClassFromHelper("open") == NatClass::Open && NatClassFromHelper("strict") == NatClass::Strict &&
		NatClassFromHelper("no_udp") == NatClass::NoUdp);
	EXPECT(NatClassFromHelper("checking") == NatClass::Unknown && NatClassFromHelper("") == NatClass::Unknown &&
		NatClassFromHelper("Open") == NatClass::Unknown && NatClassFromHelper("symmetric") == NatClass::Unknown);
	{
		NetworkSummary summary;
		EXPECT(!summary.reported && summary.Checking() && summary.relay.empty());
		summary.nat = NatClass::Open; EXPECT(!summary.Checking());
		NetworkSummary other = summary; EXPECT(summary == other);
		other.relayConnected = true; EXPECT(summary != other);
	}
	// Where the direct path most likely failed. This side's own check comes first,
	// then blocked UDP on either side, then strict NAT, then two open networks.
	EXPECT(DiagnoseDirectPath(NatClass::Unknown, NatClass::NoUdp) == DirectBlock::Checking);
	EXPECT(DiagnoseDirectPath(NatClass::NoUdp, NatClass::Open) == DirectBlock::LocalNoUdp);
	EXPECT(DiagnoseDirectPath(NatClass::NoUdp, NatClass::NoUdp) == DirectBlock::LocalNoUdp);
	EXPECT(DiagnoseDirectPath(NatClass::Strict, NatClass::NoUdp) == DirectBlock::PeerNoUdp);
	EXPECT(DiagnoseDirectPath(NatClass::Open, NatClass::NoUdp) == DirectBlock::PeerNoUdp);
	EXPECT(DiagnoseDirectPath(NatClass::Strict, NatClass::Strict) == DirectBlock::LocalStrict);
	EXPECT(DiagnoseDirectPath(NatClass::Strict, NatClass::Open) == DirectBlock::LocalStrict);
	EXPECT(DiagnoseDirectPath(NatClass::Open, NatClass::Strict) == DirectBlock::PeerStrict);
	EXPECT(DiagnoseDirectPath(NatClass::Open, NatClass::Open) == DirectBlock::BothOpen);
	// A peer whose class is not known (an older build, or still checking) is not called open.
	EXPECT(DiagnoseDirectPath(NatClass::Open, NatClass::Unknown) == DirectBlock::None);
	EXPECT(DiagnoseDirectPath(NatClass::Strict, NatClass::Unknown) == DirectBlock::LocalStrict);
	// Under Wine, a link is claimed only when the Linux interface clearly is one.
	EXPECT(LinuxInterfaceLink(true, true, "1", "") == NetworkLink::Wireless);
	EXPECT(LinuxInterfaceLink(false, true, "1", "wlan") == NetworkLink::Wireless);
	EXPECT(LinuxInterfaceLink(false, true, "1", "") == NetworkLink::Wired);
	// Bluetooth, mobile, a device with other framing, and a virtual interface.
	EXPECT(LinuxInterfaceLink(false, true, "1", "bluetooth") == NetworkLink::Unknown);
	EXPECT(LinuxInterfaceLink(false, true, "1", "wwan") == NetworkLink::Unknown);
	EXPECT(LinuxInterfaceLink(false, true, "772", "") == NetworkLink::Unknown);
	EXPECT(LinuxInterfaceLink(false, false, "1", "") == NetworkLink::Unknown);
	// The live answer depends on the machine; it must only not fail.
	std::string detail;
	std::printf("This machine's link: %s\n", NetworkLinkLabel(DetectNetworkLink(&detail)));
	std::printf("  %s\n", detail.c_str());
	EXPECT(detail.find("route=if") == 0 || detail == "no default route");
	if (TEST_FAILURES()) return 1;
	std::puts("NetworkLink test passed");
	return 0;
}
