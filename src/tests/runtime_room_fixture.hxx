#pragma once
// The netplay runtime joined to a room of its own, for tests that drive the
// runtime's real command, reply and teardown paths (spectator_runtime_test.cxx).
#include "session_client_mock.hxx"
#include "iroh_test_room.hxx"
#include "test_support.hxx"
#include "../session/IrohMatchSession.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../sf4e/sf4e__NetplayFacade.hxx"
#include "../sf4e/sf4e__GameEvents.hxx"
#include "../sf4e/sf4e__NetplayRuntime.hxx"
#include "../sf4e/sf4e__UserApp.hxx"
#include "../Dimps/Dimps.hxx"
#include <winsock2.h>
#include <array>
#include <memory>
#include <string>

namespace sf4e { namespace test {

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
	::Dimps::GameEvents::RootEvent* (*originalRoot)() = nullptr;
	static ::Dimps::GameEvents::RootEvent* NoRoot() { return nullptr; }

	RuntimeRoom() {
		sf4e::NetplayConfig config = {};
		sf4e::NetplayFacade::InitFromPayload(config);
		sf4e::NetplayFacade::ConfigureHelper({}, ERROR_FILE_NOT_FOUND);
		sf4e::NetplayFacade::StartHelper();
		originalRoot = ::Dimps::App::GetRootEvent;
		::Dimps::App::GetRootEvent = NoRoot;
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
		SessionProtocol::SessionHelloResp hello; hello.cid = {"room", "spectator"};
		transport->Push(nlohmann::json(hello));
		CHECK(client.Step() == 0);
		std::array<std::uint8_t, 16> id{}; id[15] = 1;
		room->roomId = id;
		room->localIdentity = std::string(64, 'L');
		runtime->match.reset(new session::IrohMatchSession(client, room, [this] { return now; }));
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
		::Dimps::App::GetRootEvent = originalRoot;
	}
	SessionClient& Client() { return sf4e::UserApp::netplay->client; }
	session::IrohMatchSession& Match() { return *runtime->match; }
	void Deliver(const nlohmann::json& message) { transport->Push(message); CHECK(Client().Step() == 0); }
	void Snapshot(const sf4e::room::Snapshot& snapshot) {
		SessionProtocol::RoomSnapshotMessage message; message.snapshot = snapshot;
		Deliver(nlohmann::json(message));
	}
	// The native roster for `generation`: the two fighters, then this PC; or,
	// with `local` 0 or 1, this PC in that fighter's slot.
	void Projection(std::uint64_t generation, std::size_t local = 2) {
		SessionProtocol::SessionDataUpdate update;
		update.lobbyData.members.resize(3);
		int other = 0;
		for (std::size_t slot = 0; slot < update.lobbyData.members.size(); ++slot) {
			if (slot == local) update.lobbyData.members[slot].connId = Client()._cid;
			else update.lobbyData.members[slot].connId = {"room", other++ ? "p2" : "p1"};
		}
		update.matchGeneration = generation;
		Deliver(nlohmann::json(update));
	}
	nlohmann::json Grant(std::uint64_t generation) { return GameGrant(*room, generation, {{"room", "p1"}, {"room", "p2"}, Client()._cid}, 2); }
	// The room actions this PC sent: Unwatch with keep_watching, for `generation`.
	int KeptUnwatches(std::uint64_t generation) const {
		int count = 0;
		for (const auto& message : transport->sent) {
			if (message.value("type", nlohmann::json()) != nlohmann::json(SessionProtocol::MT_ROOM_ACTION)) continue;
			const auto action = message.at("action").get<sf4e::room::Action>();
			if (action.kind == sf4e::room::ActionKind::Unwatch && action.keepWatching && action.matchGeneration == generation) ++count;
		}
		return count;
	}
};

} }
