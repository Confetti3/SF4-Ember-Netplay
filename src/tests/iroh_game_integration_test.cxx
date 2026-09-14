#include <winsock2.h>
#include "../session/IrohRoom.hxx"
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <functional>
#include <iostream>

#define CHECK(c) do { if (!(c)) { std::cerr << "Check failed at " << __LINE__ << ": " #c << '\n'; std::exit(1); } } while (false)
using namespace sf4e;

struct LocalUdp {
	SOCKET socket = INVALID_SOCKET;
	std::uint16_t port = 0;
	LocalUdp() {
		socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); CHECK(socket != INVALID_SOCKET);
		sockaddr_in address = {}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		CHECK(bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
		int size = sizeof(address); CHECK(getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0);
		port = ntohs(address.sin_port);
		u_long nonblocking = 1; CHECK(ioctlsocket(socket, FIONBIO, &nonblocking) == 0);
	}
	~LocalUdp() { closesocket(socket); }
	void Connect(std::uint16_t remotePort) {
		sockaddr_in address = {}; address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(remotePort);
		CHECK(connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
	}
	bool Receive(const std::array<char, 1024>& expected) {
		std::array<char, 2048> buffer;
		const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
		if (received == SOCKET_ERROR) { CHECK(WSAGetLastError() == WSAEWOULDBLOCK); return false; }
		CHECK(received == expected.size());
		CHECK(std::equal(expected.begin(), expected.end(), buffer.begin()));
		return true;
	}
	void Send(const std::array<char, 1024>& packet) {
		CHECK(send(socket, packet.data(), static_cast<int>(packet.size()), 0) == packet.size());
	}
};

int wmain(int argc, wchar_t** argv) {
	CHECK(argc == 2 || (argc == 3 && std::wstring(argv[2]) == L"--relay-only"));
    session::IrohRoom::GameSnapshot routeSnapshot;
    routeSnapshot.route="ip:127.0.0.1:1";
    nlohmann::json statistics={{"sent_packets",1},{"received_packets",1},{"sent_bytes",1024},{"received_bytes",1024},
        {"rejected_packets",0},{"congestion_events",0},{"local_drops",0},{"route","relay:test"}};
    routeSnapshot.ObserveStatistics(statistics);
    CHECK(routeSnapshot.route=="relay:test" && routeSnapshot.routeChanges==1);
    routeSnapshot.ObserveStatistics(statistics);
    CHECK(routeSnapshot.routeChanges==1);
    statistics["route"]="ip:127.0.0.1:2";
    routeSnapshot.ObserveStatistics(statistics);
    CHECK(routeSnapshot.route=="ip:127.0.0.1:2" && routeSnapshot.routeChanges==2);
	WSADATA winsock; CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
	platform::HelperProcess hostProcess, guestProcess;
	CHECK(hostProcess.Start(argv[1], GetCurrentProcessId(), argc == 3));
	CHECK(guestProcess.Start(argv[1], GetCurrentProcessId(), argc == 3));
	platform::HelperClient hostHelper, guestHelper;
	CHECK(hostHelper.Start(hostProcess.Bootstrap())); CHECK(guestHelper.Start(guestProcess.Bootstrap()));
	auto host = std::make_shared<session::IrohRoom>(hostHelper);
	auto guest = std::make_shared<session::IrohRoom>(guestHelper);
	unsigned waitNumber = 0;
	auto wait = [&](const std::function<bool()>& done) {
		const auto attempt = ++waitNumber;
		// A graceful two-member shutdown can first transfer the singleton
		// authority and then retire the successor. That election/retry sequence
		// intentionally continues after the 15-second replacement offer.
		const auto deadline = GetTickCount64() + 45000;
		do { host->Poll(); guest->Poll(); if (done()) return; Sleep(2); } while (GetTickCount64() < deadline);
		std::cerr << "Wait " << attempt << " timed out: host=" << static_cast<int>(host->GetState())
			<< " error=" << host->Error() << " term=" << host->Coordination().term
			<< " revision=" << host->Coordination().revision << " rebound=" << host->Coordination().rebound
			<< " controls=" << host->ControlIdentities().size() << " helper=" << static_cast<int>(hostHelper.State())
			<< '/' << hostHelper.LastError() << " guest=" << static_cast<int>(guest->GetState())
			<< " error=" << guest->Error() << " term=" << guest->Coordination().term
			<< " revision=" << guest->Coordination().revision << " rebound=" << guest->Coordination().rebound
			<< " controls=" << guest->ControlIdentities().size() << " helper=" << static_cast<int>(guestHelper.State())
			<< '/' << guestHelper.LastError() << '\n';
		CHECK(false);
	};
	wait([&]() { return hostHelper.State() == platform::HelperState::Connected && guestHelper.State() == platform::HelperState::Connected; });
	CHECK(host->Host("cpp-game-test"));
	wait([&]() { return host->GetState() == session::IrohRoom::State::Ready; });
	CHECK(guest->Join(host->Invitation(), "cpp-game-test"));
	wait([&]() { return guest->GetState() == session::IrohRoom::State::Ready; });
	// The raw gameplay bridge still runs independently of SessionServer, but
	// both helper-backed room routers must have observed the committed native
	// authority and control rebound before accepting a game mapping.
	CHECK(host->Coordination().active && host->Coordination().rebound);
	CHECK(guest->Coordination().active && guest->Coordination().rebound);
	const auto hostId = host->LocalIdentity(), guestId = guest->LocalIdentity();
	CHECK(hostId.size() == 64 && guestId.size() == 64 && hostId != guestId);
	using GameState = session::IrohRoom::GameState;
	for (std::uint64_t generation = 1; generation <= 3; ++generation) {
		LocalUdp hostUdp, guestUdp;
		std::array<std::uint8_t, 32> capability;
		CHECK(BCryptGenRandom(nullptr, capability.data(), static_cast<ULONG>(capability.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0);
		CHECK(!host->PrepareGame(hostId, generation, capability, hostUdp.port, 1024, false));
		CHECK(!host->PrepareGame(guestId, generation, {}, hostUdp.port, 1024, false));
		CHECK(host->PrepareGame(guestId, generation, capability, hostUdp.port, 1024, false));
		CHECK(!host->PrepareGame(guestId, generation, capability, hostUdp.port, 1024, false));
		CHECK(guest->PrepareGame(hostId, generation, capability, guestUdp.port, 1024, true));
		SecureZeroMemory(capability.data(), capability.size());
		wait([&]() { return host->Game(guestId).state == GameState::Ready && guest->Game(hostId).state == GameState::Ready; });
		CHECK(!host->Game(guestId).route.empty() && !guest->Game(hostId).route.empty());
		if(argc==3) CHECK(host->Game(guestId).route.rfind("relay:",0)==0 && guest->Game(hostId).route.rfind("relay:",0)==0);
		std::cout << "Raw bridge generation " << generation << " selected routes=" << host->Game(guestId).route
			<< "," << guest->Game(hostId).route << '\n';
		hostUdp.Connect(host->Game(guestId).virtualPort); guestUdp.Connect(guest->Game(hostId).virtualPort);
		if (generation == 3) {
			CHECK(hostHelper.Send("{\"type\":\"close_control\",\"epoch\":" + std::to_string(host->Epoch()) + ",\"peer\":\"" + guestId + "\"}"));
			wait([&]() { return guest->GetState() == session::IrohRoom::State::Degraded; });
			CHECK(guest->Game(hostId).state == GameState::Ready);
		}
		for (int sample = 0; sample < 10; ++sample) {
			std::array<char, 1024> packet;
			for (std::size_t i = 0; i < packet.size(); ++i) packet[i] = static_cast<char>((i + sample + generation) & 255);
			guestUdp.Send(packet); wait([&]() { return hostUdp.Receive(packet); });
			hostUdp.Send(packet); wait([&]() { return guestUdp.Receive(packet); });
		}
		wait([&]() { return host->Game(guestId).receivedPackets >= 10 && guest->Game(hostId).receivedPackets >= 10; });
        if (generation==2) {
            std::array<char,1025> oversized{};
            CHECK(send(hostUdp.socket,oversized.data(),static_cast<int>(oversized.size()),0)==oversized.size());
            wait([&]() { return host->Game(guestId).state==GameState::Closed && guest->Game(hostId).state==GameState::Closed; });
            CHECK(host->Game(guestId).error=="gameplay_packet_limit");
            CHECK(guest->Game(hostId).error=="gameplay_peer_closed");
            CHECK(host->Game(guestId).receivedPackets>=10);
        }
		CHECK(host->EndMatch(generation)); CHECK(guest->EndMatch(generation));
		CHECK(host->Game(guestId).virtualPort == 0 && guest->Game(hostId).virtualPort == 0);
		wait([&]() { return host->Game(guestId).state == GameState::Closed && guest->Game(hostId).state == GameState::Closed; });
		capability.fill(1);
		CHECK(!host->PrepareGame(guestId, generation, capability, hostUdp.port, 1024, false));
	}
	host->Leave(); guest->Leave();
	wait([&]() { return host->GetState() == session::IrohRoom::State::Idle && guest->GetState() == session::IrohRoom::State::Idle; });
	CHECK(hostHelper.Send("{\"type\":\"shutdown\"}")); CHECK(guestHelper.Send("{\"type\":\"shutdown\"}"));
	wait([&]() { return !hostProcess.IsRunning() && !guestProcess.IsRunning(); });
	std::cout << "C++ raw UDP bridge: 3 generations, 30 exact 1024-byte round trips, statistics and control-loss independence passed; relay-only=" << (argc == 3) << ". No SF4 gameplay tested.\n";
	return 0;
}
