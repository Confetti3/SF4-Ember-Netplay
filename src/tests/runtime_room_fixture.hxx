#pragma once
#include "session_client_mock.hxx"
#include "iroh_test_room.hxx"
#include "test_support.hxx"
#include "../sf4e/sf4e__NetplayRuntime.hxx"
#include "../Dimps/Dimps.hxx"

namespace sf4e { namespace test {
using nlohmann::json;
namespace protocol = sf4e::SessionProtocol;
using session::IrohMatchSession;
// The runtime with a room of its own: its client connected through a scripted
// transport and a fake helper room, this PC a member of `authority`'s room, the
// controller joined and healthy. The runtime itself runs without a helper or a
// game; there is no main menu, so the test ends a recovery the way
// ReleaseFinishedMatch would at one.
struct RuntimeRoom {
	platform::HelperClient helper;
	std::shared_ptr<FakeIrohRoom> room = std::make_shared<FakeIrohRoom>(helper);
	SessionClient::Callbacks callbacks{};
	std::string name = "Spectator";
	MockClient* transport = new MockClient();
	ULONGLONG now = 1000;
	sf4e::NetplayFacade::internal::Runtime* runtime = nullptr;
	Dimps::GameEvents::RootEvent* (*originalRoot)() = nullptr;
	static Dimps::GameEvents::RootEvent* NoRoot() { return nullptr; }

	RuntimeRoom() {
		sf4e::NetplayConfig config = {};
		sf4e::NetplayFacade::InitFromPayload(config);
		sf4e::NetplayFacade::ConfigureHelper({}, ERROR_FILE_NOT_FOUND);
		sf4e::NetplayFacade::StartHelper();
		originalRoot = Dimps::App::GetRootEvent;
		Dimps::App::GetRootEvent = NoRoot;
		sf4e::NetplayFacade::NotifyRuntimeGameReady();
		sf4e::NetplayFacade::TickRuntime();
		runtime = sf4e::NetplayFacade::internal::runtime;
		CHECK(runtime);
		sf4e::UserApp::netplay.reset(new sf4e::UserApp::Netplay(callbacks, "build", 30000, name, 0, 0, 2));
		auto& client = sf4e::UserApp::netplay->client;
		CHECK(client.Connect(std::unique_ptr<session::ClientTransport>(transport), false) == 0);
		client.RequireMatchAuthorization();
		transport->state = session::ConnectionState::Connected;
		CHECK(client.Step() == 0);
		protocol::SessionHelloResp hello; hello.cid = {"room", "spectator"};
		transport->Push(json(hello));
		CHECK(client.Step() == 0);
		std::array<std::uint8_t, 16> id{}; id[15] = 1;
		room->roomId = id;
		room->localIdentity = std::string(64, 'L');
		runtime->match.reset(new IrohMatchSession(client, room, [this] { return now; }));
		runtime->attached = true;
		CHECK(runtime->controller.Execute({sf4e::netplay::CommandKind::HostRoom, runtime->controller.GetSnapshot().generation, {}}).accepted);
		sf4e::NetplayFacade::internal::Apply(sf4e::netplay::EventKind::RoomJoined);
		CHECK(runtime->controller.GetSnapshot().control == sf4e::netplay::Health::Healthy);
	}
	~RuntimeRoom() {
		runtime->attached = false;
		runtime->match.reset();
		sf4e::UserApp::netplay.reset();
		sf4e::NetplayFacade::StopHelper();
		Dimps::App::GetRootEvent = originalRoot;
	}
	SessionClient& Client() { return sf4e::UserApp::netplay->client; }
	IrohMatchSession& Match() { return *runtime->match; }
	void Deliver(const json& message) { transport->Push(message); CHECK(Client().Step() == 0); }
	void Snapshot(const sf4e::room::Snapshot& snapshot) {
		protocol::RoomSnapshotMessage message; message.snapshot = snapshot;
		Deliver(json(message));
	}
};
} }
