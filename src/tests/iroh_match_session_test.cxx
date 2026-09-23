#include "session_client_mock.hxx"
#include "../session/IrohMatchSession.hxx"
#include "../session/IrohRoom.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../platform/HelperClient.hxx"
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>

#include "test_support.hxx"

using namespace sf4e;
using nlohmann::json;
namespace protocol = sf4e::SessionProtocol;
using session::IrohMatchSession;
using sf4e::test::MockClient;
using Phase = IrohMatchSession::Phase;

namespace {

// IrohMatchSession only calls the nine IrohRoom methods overridden below.
// Recording their calls lets a test assert on dial behaviour without a real
// helper process; every other IrohRoom method is left untouched.
class FakeIrohRoom final : public session::IrohRoom {
public:
	explicit FakeIrohRoom(platform::HelperClient& helper) : IrohRoom(helper) {}
	std::array<std::uint8_t, 16> roomId{};
	std::string localIdentity;
	bool ready = true;
	struct PrepareCall { std::string peer; std::uint64_t generation; std::uint16_t localPort; std::size_t maxPacket; bool dial; };
	std::vector<PrepareCall> prepareCalls;
	std::vector<std::uint64_t> endMatchCalls;
	int leaveCalls = 0;

	bool ReadyForMatch() const override { return ready; }
	std::array<std::uint8_t, 16> RoomId() const override { return roomId; }
	const std::string& LocalIdentity() const override { return localIdentity; }
	const CoordinationSnapshot& Coordination() const override { return coordination_; }
	GameSnapshot Game(const std::string& peer) const override {
		const auto found = snapshots_.find(peer);
		return found == snapshots_.end() ? GameSnapshot() : found->second;
	}
	bool PrepareGame(const std::string& peer, std::uint64_t generation, const std::array<std::uint8_t, 32>&,
		std::uint16_t localPort, std::size_t maxPacket, bool dial) override {
		prepareCalls.push_back({peer, generation, localPort, maxPacket, dial});
		GameSnapshot snapshot; snapshot.state = GameState::Preparing; snapshot.generation = generation; snapshot.maxPacket = maxPacket;
		snapshots_[peer] = snapshot;
		return true;
	}
	bool EndPeer(const std::string& peer, std::uint64_t) override { snapshots_.erase(peer); return true; }
	bool EndMatch(std::uint64_t generation) override { endMatchCalls.push_back(generation); return true; }
	void AbandonMatch(std::uint64_t generation) override {
		for (auto& snapshot : snapshots_) if (snapshot.second.generation == generation) snapshot.second.state = GameState::Closed;
	}
	void Leave(bool) override { ++leaveCalls; }
private:
	CoordinationSnapshot coordination_;
	std::map<std::string, GameSnapshot> snapshots_;
};

// A spectator (roster slot 2 of 3) with its own grant/connect/end builders.
// AcceptGrant only needs a plain (non-custom-room) lobby projection, so the
// fixture skips the room-snapshot machinery entirely.
struct Fixture {
	platform::HelperClient helper;
	std::shared_ptr<FakeIrohRoom> room = std::make_shared<FakeIrohRoom>(helper);
	SessionClient::Callbacks callbacks{};
	std::string name = "Spectator";
	SessionClient client{callbacks, "build", 30000, name};
	ULONGLONG now = 1000;
	IrohMatchSession session{client, room, [this] { return now; }};
	MockClient* transport = nullptr;
	std::string p1 = std::string(64, 'A');

	Fixture() {
		transport = new MockClient();
		CHECK(client.Connect(std::unique_ptr<session::ClientTransport>(transport), false) == 0);
		// Connect() disconnects first, which clears match authorization; the
		// session's constructor already requested it once, before the client
		// was connected, so it must be re-armed here for the gameplay message
		// path to accept game_prepare/game_connect/game_end below.
		client.RequireMatchAuthorization();
		transport->state = session::ConnectionState::Connected;
		CHECK(client.Step() == 0);
		protocol::SessionHelloResp hello; hello.cid = {"room", "spectator"};
		transport->Push(json(hello));
		CHECK(client.Step() == 0);
		std::array<std::uint8_t, 16> id{}; id[15] = 1;
		room->roomId = id;
		room->localIdentity = std::string(64, 'L');
		client._lobbyData.members.resize(3);
		client._lobbyData.members[0].connId = {"room", "p1"};
		client._lobbyData.members[1].connId = {"room", "p2"};
		client._lobbyData.members[2].connId = client._cid;
	}

	json Grant(std::uint64_t generation) const {
		std::vector<protocol::ConnectionID> roster{
			client._lobbyData.members[0].connId, client._lobbyData.members[1].connId, client._lobbyData.members[2].connId};
		std::array<std::uint8_t, 32> capability{}; capability[0] = 7;
		const json link = {{"slot", 0}, {"peer", p1}, {"capability", capability}, {"dial", true}};
		return json{{"type", "game_prepare"}, {"generation", generation}, {"version", 1}, {"room", room->roomId},
			{"local_identity", room->localIdentity}, {"max_packet", session::GgpoMaximumPacket},
			{"roster", roster}, {"slot", std::size_t(2)}, {"links", json::array({link})}};
	}
	static json Connect(std::uint64_t generation) { return json{{"type", "game_connect"}, {"generation", generation}}; }
	static json End(std::uint64_t generation) { return json{{"type", "game_end"}, {"generation", generation}}; }
};

// (1) game_prepare and game_connect for the same generation arrive in one
// poll, before a single Tick. AcceptGrant stages pendingConnect_ from
// earlyConnectGeneration_ and the session reaches Connecting with a dialed
// link, though StartConnecting itself needs one more Tick to run.
void TestEarlyConnectSameTick() {
	Fixture f;
	f.transport->Push(f.Grant(5));
	f.transport->Push(Fixture::Connect(5));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Prepared);
	CHECK(f.session.Generation() == 5);
	CHECK(f.room->prepareCalls.empty());
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Connecting);
	CHECK(f.room->prepareCalls.size() == 1);
	CHECK(f.room->prepareCalls[0].dial);
	CHECK(f.room->prepareCalls[0].peer == f.p1);
	std::cout << "TestEarlyConnectSameTick passed\n";
}

// (2) The same two messages delivered on separate Ticks reach the same
// place, via the ordinary phase_==Prepared branch instead of the early note.
void TestEarlyConnectSeparateTicks() {
	Fixture f;
	f.transport->Push(f.Grant(5));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Prepared);
	CHECK(f.room->prepareCalls.empty());
	f.transport->Push(Fixture::Connect(5));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Connecting);
	CHECK(f.room->prepareCalls.size() == 1);
	CHECK(f.room->prepareCalls[0].dial);
	CHECK(f.room->prepareCalls[0].peer == f.p1);
	std::cout << "TestEarlyConnectSeparateTicks passed\n";
}

// (3) A game_end for the still-staged generation cancels the grant before it
// is ever accepted, but earlyConnectGeneration_ is generation-scoped rather
// than a bare flag, so a later grant for a higher generation must not replay
// it as an auto-connect.
void TestGameEndCancelsEarlyConnect() {
	Fixture f;
	f.transport->Push(f.Grant(5));
	f.transport->Push(Fixture::Connect(5));
	f.transport->Push(Fixture::End(5));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Idle);
	CHECK(f.session.Generation() == 0);
	CHECK(f.room->prepareCalls.empty());
	f.transport->Push(f.Grant(6));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Prepared);
	CHECK(f.session.Generation() == 6);
	CHECK(f.room->prepareCalls.empty());
	for (int i = 0; i < 3; ++i) {
		CHECK(f.session.Tick());
		CHECK(f.session.GetPhase() == Phase::Prepared);
	}
	CHECK(f.room->prepareCalls.empty());
	// The session still connects normally once its own game_connect arrives.
	f.transport->Push(Fixture::Connect(6));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Connecting);
	CHECK(f.room->prepareCalls.size() == 1);
	std::cout << "TestGameEndCancelsEarlyConnect passed\n";
}

// (4) The helper never confirms the spectator's close. A fighter fails closed
// and leaves the room; a spectator gives up the wait and stays a member.
void TestSpectatorHelperTimeoutStaysInRoom() {
	Fixture f;
	f.transport->Push(f.Grant(5));
	f.transport->Push(Fixture::Connect(5));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick());
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Connecting);
	f.transport->Push(Fixture::End(5));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Ending);
	CHECK(f.room->endMatchCalls.size() == 1);
	f.now += session::MatchTeardownTiming::HelperTimeoutMs - 1;
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Ending);
	f.now += 1;
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Idle);
	CHECK(f.session.Error().empty());
	CHECK(f.room->leaveCalls == 0);
	// The abandoned mapping no longer blocks the next generation.
	CHECK(f.room->Game(f.p1).state == session::IrohRoom::GameState::Closed);
	std::cout << "TestSpectatorHelperTimeoutStaysInRoom passed\n";
}

// (5) The native battle ends and the helper closes, but the room's game_end
// never arrives. The wait is bounded and fails through the recoverable abort,
// which stays in the room and returns to Idle.
void TestMissingRoomEndIsBounded() {
	Fixture f;
	f.transport->Push(f.Grant(5));
	f.transport->Push(Fixture::Connect(5));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick());
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Connecting);
	f.session.End();
	f.room->AbandonMatch(5);
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Ending);
	f.now += session::MatchTeardownTiming::RoomEndTimeoutMs - 1;
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Ending);
	f.now += 1;
	CHECK(!f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Failed);
	CHECK(f.session.Error() == "match_room_end_timeout");
	CHECK(f.session.Abort());
	// N-005: the runtime traces the session after Abort; the reason stays.
	CHECK(f.session.Error().empty());
	CHECK(f.session.LastFailure() == "match_room_end_timeout");
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Idle);
	CHECK(f.room->leaveCalls == 0);
	std::cout << "TestMissingRoomEndIsBounded passed\n";
}

}

int main() {
	TestEarlyConnectSameTick();
	TestEarlyConnectSeparateTicks();
	TestGameEndCancelsEarlyConnect();
	TestSpectatorHelperTimeoutStaysInRoom();
	TestMissingRoomEndIsBounded();
	std::cout << "Iroh match session tests passed\n";
	return 0;
}
