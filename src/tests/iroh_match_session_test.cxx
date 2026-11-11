#include "session_client_mock.hxx"
#include "iroh_test_room.hxx"
#include "../session/IrohMatchSession.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include <iostream>
#include <memory>

#include "test_support.hxx"

// IrohMatchSession's own state machine against a scripted client and helper
// room. What the runtime does around it is spectator_runtime_test.cxx.
using namespace sf4e;
using nlohmann::json;
namespace protocol = sf4e::SessionProtocol;
using session::IrohMatchSession;
using sf4e::test::FakeIrohRoom;
using sf4e::test::GameConnect;
using sf4e::test::GameEnd;
using sf4e::test::GameGrant;
using sf4e::test::MockClient;
using sf4e::test::P1Peer;
using Phase = IrohMatchSession::Phase;

namespace {

// A spectator (roster slot 2 of 3) with its own grant builder.
// Constructed with slot 1 it is instead P2 of a two-member roster, a fighter
// with the same single dialed link to P1. AcceptGrant only needs a plain
// (non-custom-room) lobby projection, so the fixture skips the room-snapshot
// machinery entirely.
struct Fixture {
	std::size_t localSlot = 2;
	platform::HelperClient helper;
	std::shared_ptr<FakeIrohRoom> room = std::make_shared<FakeIrohRoom>(helper);
	SessionClient::Callbacks callbacks{};
	std::string name = "Spectator";
	SessionClient client{callbacks, "build", 30000, name};
	ULONGLONG now = 1000;
	IrohMatchSession session{client, room, [this] { return now; }};
	MockClient* transport = nullptr;
	std::string p1 = P1Peer();

	explicit Fixture(std::size_t slot = 2) : localSlot(slot) {
		CHECK(localSlot == 1 || localSlot == 2);
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
		client._lobbyData.members.resize(localSlot + 1);
		client._lobbyData.members[0].connId = {"room", "p1"};
		if (localSlot == 2) client._lobbyData.members[1].connId = {"room", "p2"};
		client._lobbyData.members[localSlot].connId = client._cid;
	}

	json Grant(std::uint64_t generation) const {
		std::vector<protocol::ConnectionID> roster;
		for (const auto& member : client._lobbyData.members) roster.push_back(member.connId);
		return GameGrant(*room, generation, roster, localSlot);
	}
};

// (1) game_prepare and game_connect for the same generation arrive in one
// poll, before a single Tick. AcceptGrant stages pendingConnect_ from
// earlyConnectGeneration_ and the session reaches Connecting with a dialed
// link, though StartConnecting itself needs one more Tick to run.
void TestEarlyConnectSameTick() {
	Fixture f;
	f.transport->Push(f.Grant(5));
	f.transport->Push(GameConnect(5));
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
	f.transport->Push(GameConnect(5));
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
	f.transport->Push(GameConnect(5));
	f.transport->Push(GameEnd(5));
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
	f.transport->Push(GameConnect(6));
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
	f.transport->Push(GameConnect(5));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick());
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Connecting);
	f.transport->Push(GameEnd(5));
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
// has not arrived. A spectator has lost its stream and has no result to
// report, so it abandons the generation and returns to the room on the same
// Tick, without waiting even one millisecond.
void TestSpectatorMissingRoomEndReturnsAtOnce() {
	Fixture f;
	f.transport->Push(f.Grant(5));
	f.transport->Push(GameConnect(5));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick());
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Connecting);
	CHECK(f.session.LocalSlot() == 2);
	f.session.End();
	f.room->AbandonMatch(5);
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Idle);
	CHECK(f.session.Error().empty());
	CHECK(f.session.LastFailure().empty());
	CHECK(f.room->endMatchCalls.size() == 1);
	CHECK(f.room->leaveCalls == 0);
	CHECK(f.room->Game(f.p1).state == session::IrohRoom::GameState::Closed);
	std::cout << "TestSpectatorMissingRoomEndReturnsAtOnce passed\n";
}

// (6) The same close for a fighter. Its result report depends on the room's
// game_end, so the wait is bounded and fails through the recoverable abort,
// which stays in the room and returns to Idle.
void TestFighterMissingRoomEndIsBounded() {
	Fixture f(1);
	CHECK(!f.session.Live());
	f.transport->Push(f.Grant(5));
	f.transport->Push(GameConnect(5));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick());
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Connecting);
	CHECK(f.session.Live());
	CHECK(f.session.LocalSlot() == 1);
	f.session.End();
	f.room->AbandonMatch(5);
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Ending);
	CHECK(!f.session.Live());
	f.now += session::MatchTeardownTiming::RoomEndTimeoutMs - 1;
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Ending);
	f.now += 1;
	CHECK(!f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Failed);
	CHECK(!f.session.Live());
	CHECK(f.session.Error() == "match_room_end_timeout");
	CHECK(f.session.Abort());
	// N-005: the runtime traces the session after Abort; the reason stays.
	CHECK(f.session.Error().empty());
	CHECK(f.session.LastFailure() == "match_room_end_timeout");
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Idle);
	CHECK(f.room->leaveCalls == 0);
	std::cout << "TestFighterMissingRoomEndIsBounded passed\n";
}

// (7) A spectator's stream source is P1's link. After the room's game_end the
// spectator keeps its GGPO session while it plays the buffered tail; once
// P1's link closes (P1 left at once) nothing more can arrive, which lets the
// runtime close the view instead of waiting out the 15 s timeout. A
// fighter never reports a closed source.
void TestSpectatorStreamSourceClosed() {
	Fixture f;
	CHECK(!f.session.StreamSourceClosed()); // no grant yet
	f.transport->Push(f.Grant(5));
	f.transport->Push(GameConnect(5));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick());
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Connecting);
	CHECK(!f.session.StreamSourceClosed());
	f.transport->Push(GameEnd(5));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick(true)); // native GGPO still owns the socket
	CHECK(f.session.GetPhase() == Phase::Ending);
	CHECK(!f.session.StreamSourceClosed());
	using Exit = session::MatchTeardownTiming::SpectatorExit;
	f.session.ArmSpectatorExit();
	CHECK(f.session.SpectatorExitStep(true) == Exit::Wait); // P1 still streaming
	f.room->AbandonMatch(5); // P1's link reports closed
	CHECK(f.session.StreamSourceClosed());
	CHECK(f.session.SpectatorExitStep(false) == Exit::Wait); // frames left to play
	CHECK(f.session.SpectatorExitStep(true) == Exit::Retire);
	CHECK(f.session.Tick(true));
	CHECK(f.session.GetPhase() == Phase::Ending); // still waits for GGPO's release
	CHECK(f.session.StreamSourceClosed());

	Fixture fighter(1);
	fighter.transport->Push(fighter.Grant(5));
	fighter.transport->Push(GameConnect(5));
	CHECK(fighter.client.Step() == 0);
	CHECK(fighter.session.Tick());
	CHECK(fighter.session.Tick());
	fighter.room->AbandonMatch(5);
	CHECK(!fighter.session.StreamSourceClosed());
	std::cout << "TestSpectatorStreamSourceClosed passed\n";
}

// (8) P1 of a four-member roster ends a spectator's link when its GGPO drops
// that spectator. Only a spectator slot of a started match, only once; a
// refused command, a full helper queue or the room's game_peer_end for the
// same slot leaves the room and the fight running.
void TestHostEndsDroppedSpectatorLink() {
	platform::HelperClient helper;
	auto room = std::make_shared<FakeIrohRoom>(helper);
	SessionClient::Callbacks callbacks{};
	std::string name = "Host";
	SessionClient client{callbacks, "build", 30000, name};
	ULONGLONG now = 1000;
	IrohMatchSession session{client, room, [&now] { return now; }};
	auto* transport = new MockClient();
	CHECK(client.Connect(std::unique_ptr<session::ClientTransport>(transport), false) == 0);
	client.RequireMatchAuthorization();
	transport->state = session::ConnectionState::Connected;
	CHECK(client.Step() == 0);
	protocol::SessionHelloResp hello; hello.cid = {"room", "p1"};
	transport->Push(json(hello));
	CHECK(client.Step() == 0);
	std::array<std::uint8_t, 16> id{}; id[15] = 1;
	room->roomId = id;
	room->localIdentity = std::string(64, 'L');
	const std::vector<protocol::ConnectionID> roster{client._cid, {"room", "p2"}, {"room", "s1"}, {"room", "s2"}};
	client._lobbyData.members.resize(roster.size());
	for (std::size_t slot = 0; slot < roster.size(); ++slot) client._lobbyData.members[slot].connId = roster[slot];
	const std::array<std::string, 4> peers{"", std::string(64, 'B'), std::string(64, 'C'), std::string(64, 'D')};
	auto grant = GameGrant(*room, 5, roster, 0);
	std::array<std::uint8_t, 32> capability{}; capability[0] = 7;
	grant["links"] = json::array();
	for (std::size_t slot = 1; slot < roster.size(); ++slot)
		grant["links"].push_back({{"slot", slot}, {"peer", peers[slot]}, {"capability", capability}, {"dial", false}});
	grant["spectators_optional"] = true;
	transport->Push(grant);
	CHECK(client.Step() == 0);
	CHECK(session.Tick());
	CHECK(session.GetPhase() == Phase::Preparing);
	CHECK(!session.EndSpectatorStream(2)); // not started yet
	room->LinkUp(5);
	CHECK(session.Tick());
	CHECK(session.GetPhase() == Phase::Prepared);
	transport->Push(GameConnect(5));
	CHECK(client.Step() == 0);
	CHECK(session.Tick());
	CHECK(session.GetPhase() == Phase::Connecting);
	CHECK(session.Tick()); // game_ready, every link up
	transport->Push(json{{"type", "game_start"}, {"generation", 5}});
	CHECK(client.Step() == 0);
	CHECK(session.Tick());
	CHECK(session.GetPhase() == Phase::Started);
	// Fighters and P1 itself are not spectators.
	CHECK(!session.EndSpectatorStream(0) && !session.EndSpectatorStream(1) && !session.EndSpectatorStream(4));
	// A refused command leaves the link and the fight as they were.
	room->refuseEndPeer = true;
	CHECK(session.EndSpectatorStream(2));
	CHECK(session.Tick());
	CHECK(session.GetPhase() == Phase::Started);
	CHECK(room->endPeerCalls.empty() && room->Game(peers[2]).state == session::IrohRoom::GameState::Ready);
	room->refuseEndPeer = false;
	// A full helper queue: the end waits for the match's next Tick, once.
	room->busyEndPeer = 2;
	CHECK(session.EndSpectatorStream(2));
	CHECK(session.EndSpectatorStream(2)); // asked again while it waits
	CHECK(room->endPeerCalls.empty());
	CHECK(session.Tick());
	CHECK(session.GetPhase() == Phase::Started);
	CHECK(room->endPeerCalls.size() == 1 && room->endPeerCalls[0] == peers[2]);
	CHECK(session.Tick());
	CHECK(room->endPeerCalls.size() == 1);
	CHECK(!session.EndSpectatorStream(2));
	CHECK(room->Game(peers[1]).state == session::IrohRoom::GameState::Ready);
	// The room's game_peer_end after the local end is a late duplicate.
	transport->Push(json{{"type", "game_peer_end"}, {"generation", 5}, {"slot", 2}});
	CHECK(client.Step() == 0);
	CHECK(session.Tick());
	CHECK(session.GetPhase() == Phase::Started);
	// A full queue across P1's drop and the room's game_peer_end for the same
	// spectator: both wait on one pending end, which neither fails the room
	// (as EndPeer would on that queue) nor the fight, and the link ends once
	// the queue has room.
	room->busyEndPeer = 100;
	CHECK(session.EndSpectatorStream(3));
	transport->Push(json{{"type", "game_peer_end"}, {"generation", 5}, {"slot", 3}});
	CHECK(client.Step() == 0);
	CHECK(session.Tick());
	CHECK(session.GetPhase() == Phase::Started && !room->failedOnFullQueue);
	CHECK(room->endPeerCalls.size() == 1);
	room->busyEndPeer = 0;
	CHECK(session.Tick());
	CHECK(room->endPeerCalls.size() == 2 && room->endPeerCalls[1] == peers[3]);
	CHECK(session.Tick());
	CHECK(room->endPeerCalls.size() == 2);
	CHECK(!session.EndSpectatorStream(3));
	CHECK(session.GetPhase() == Phase::Started && !room->failedOnFullQueue);
	CHECK(room->Game(peers[1]).state == session::IrohRoom::GameState::Ready);
	// Once the match ends no link is ended for it.
	transport->Push(GameEnd(5));
	CHECK(client.Step() == 0);
	CHECK(session.Tick(true));
	CHECK(session.GetPhase() == Phase::Ending);
	CHECK(!session.EndSpectatorStream(2));
	CHECK(room->endPeerCalls.size() == 2);
	std::cout << "TestHostEndsDroppedSpectatorLink passed\n";
}

// (9) Started, the game has StartTimeoutMs to enter and load the battle and
// reach GGPO's RUNNING. One that never runs fails the match, which the
// runtime turns into the abort that ends the game for the whole table; one
// that runs in time is never timed out, however long it lasts. A room control
// outage does not stop the start's clock: the start needs no room.
void TestStartIsBounded() {
	for (const bool runs : {false, true}) {
		Fixture f(1);
		f.transport->Push(f.Grant(5));
		f.transport->Push(GameConnect(5));
		CHECK(f.client.Step() == 0);
		CHECK(f.session.Tick());
		CHECK(f.session.Tick());
		CHECK(f.session.GetPhase() == Phase::Connecting);
		f.room->LinkUp(5);
		CHECK(f.session.Tick()); // game_ready
		f.transport->Push(json{{"type", "game_start"}, {"generation", 5}});
		CHECK(f.client.Step() == 0);
		CHECK(f.session.Tick());
		CHECK(f.session.GetPhase() == Phase::Started);
		f.room->ready = false;
		f.now += IrohMatchSession::StartTimeoutMs - 1000;
		if (runs) f.session.GameRunning();
		CHECK(f.session.Tick() && f.session.GetPhase() == Phase::Started);
		f.now += 1001;
		CHECK(f.session.Tick() == runs);
		CHECK(f.session.GetPhase() == (runs ? Phase::Started : Phase::Failed));
		CHECK(f.session.Error() == (runs ? "" : "match_start_timeout"));
		f.now += 10 * IrohMatchSession::StartTimeoutMs;
		CHECK(f.session.Tick() == runs);
	}
	std::cout << "TestStartIsBounded passed\n";
}
}
int main() {
	TestEarlyConnectSameTick();
	TestEarlyConnectSeparateTicks();
	TestGameEndCancelsEarlyConnect();
	TestSpectatorHelperTimeoutStaysInRoom();
	TestSpectatorMissingRoomEndReturnsAtOnce();
	TestFighterMissingRoomEndIsBounded();
	TestSpectatorStreamSourceClosed();
	TestHostEndsDroppedSpectatorLink();
	TestStartIsBounded();
	std::cout << "Iroh match session tests passed\n";
	return 0;
}
