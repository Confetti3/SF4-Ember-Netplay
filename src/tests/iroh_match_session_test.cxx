#include "session_client_mock.hxx"
#include "../session/IrohMatchSession.hxx"
#include "../session/IrohRoom.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../platform/HelperClient.hxx"
#include "../session/RoomModel.hxx"
#include "../sf4e/sf4e__NetplayFacade.hxx"
#include "../sf4e/sf4e__GameEvents.hxx"
#include "../sf4e/sf4e__NetplayRuntime.hxx"
#include "../sf4e/sf4e__UserApp.hxx"
#include "../Dimps/Dimps.hxx"
#include <algorithm>
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
	std::string p1 = std::string(64, 'A');

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
		std::array<std::uint8_t, 32> capability{}; capability[0] = 7;
		const json link = {{"slot", 0}, {"peer", p1}, {"capability", capability}, {"dial", true}};
		return json{{"type", "game_prepare"}, {"generation", generation}, {"version", 1}, {"room", room->roomId},
			{"local_identity", room->localIdentity}, {"max_packet", session::GgpoMaximumPacket},
			{"roster", roster}, {"slot", localSlot}, {"links", json::array({link})}};
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
// has not arrived. A spectator has lost its stream and has no result to
// report, so it abandons the generation and returns to the room on the same
// Tick, without waiting even one millisecond.
void TestSpectatorMissingRoomEndReturnsAtOnce() {
	Fixture f;
	f.transport->Push(f.Grant(5));
	f.transport->Push(Fixture::Connect(5));
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
	f.transport->Push(Fixture::Connect(5));
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
	f.transport->Push(Fixture::Connect(5));
	CHECK(f.client.Step() == 0);
	CHECK(f.session.Tick());
	CHECK(f.session.Tick());
	CHECK(f.session.GetPhase() == Phase::Connecting);
	CHECK(!f.session.StreamSourceClosed());
	f.transport->Push(Fixture::End(5));
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
	fighter.transport->Push(Fixture::Connect(5));
	CHECK(fighter.client.Step() == 0);
	CHECK(fighter.session.Tick());
	CHECK(fighter.session.Tick());
	fighter.room->AbandonMatch(5);
	CHECK(!fighter.session.StreamSourceClosed());
	std::cout << "TestSpectatorStreamSourceClosed passed\n";
}

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
	// The native roster for `generation`: the two fighters, then this PC.
	void Projection(std::uint64_t generation) {
		protocol::SessionDataUpdate update;
		update.lobbyData.members.resize(3);
		update.lobbyData.members[0].connId = {"room", "p1"};
		update.lobbyData.members[1].connId = {"room", "p2"};
		update.lobbyData.members[2].connId = Client()._cid;
		update.matchGeneration = generation;
		Deliver(json(update));
	}
	json Grant(std::uint64_t generation) {
		std::vector<protocol::ConnectionID> roster{{"room", "p1"}, {"room", "p2"}, Client()._cid};
		std::array<std::uint8_t, 32> capability{}; capability[0] = 7;
		const json link = {{"slot", 0}, {"peer", std::string(64, 'A')}, {"capability", capability}, {"dial", true}};
		return json{{"type", "game_prepare"}, {"generation", generation}, {"version", 1}, {"room", room->roomId},
			{"local_identity", room->localIdentity}, {"max_packet", session::GgpoMaximumPacket},
			{"roster", roster}, {"slot", 2}, {"links", json::array({link})}};
	}
	// The room actions this PC sent: Unwatch with keep_watching, for `generation`.
	int KeptUnwatches(std::uint64_t generation) const {
		int count = 0;
		for (const auto& message : transport->sent) {
			if (message.value("type", json()) != json(protocol::MT_ROOM_ACTION)) continue;
			const auto action = message.at("action").get<sf4e::room::Action>();
			if (action.kind == sf4e::room::ActionKind::Unwatch && action.keepWatching && action.matchGeneration == generation) ++count;
		}
		return count;
	}
};

// A spectator, by choice or queued, saw one game end (the room's own end event,
// through the runtime) and retired it, and is taken into the next, whose setup
// then fails before the session records that generation as its own: its port
// cannot be reserved, or its staged grant's projection never comes. Through the
// runtime's real failure path, the failure is reported for the generation it
// attempted, and a refused send is kept and retried for it; the lock release
// is armed for it too. When that game already ended in the room, nothing more
// is sent. Training is refused during the teardown and recovery, holds once
// both are done (Take Go once, then None), and is refused for a newer admission.
void TestFailedSetupThroughRuntime(bool queued, bool staged, bool alreadyEnded) {
	namespace room = sf4e::room;
	namespace facade = sf4e::NetplayFacade;
	namespace internal = sf4e::NetplayFacade::internal;
	using Taken = sf4e::GameEvents::TrainingRequest::Taken;
	auto& record = sf4e::GameEvents::MainMenu::trainingRequest;
	sf4e::GameEvents::TrainingRequest::Pending seen;
	const auto none = [&] { return !record.Peek(GetTickCount64(), seen); };
	if (!none()) record.Consume(seen.serial);
	room::RoomAuthority authority("Failed setup", 16, 95);
	const auto join = [&](int index, bool host) {
		const auto result = authority.Join("Player" + std::to_string(index), room::ConnectionRef{"host", std::to_string(index)}, host);
		CHECK(result.accepted);
		return result.snapshot.members.back().id;
	};
	const auto act = [&](room::MemberId member, room::ActionKind kind, std::uint64_t generation) {
		const auto& view = authority.SnapshotView();
		room::Action action;
		action.kind = kind; action.roomEpoch = view.roomEpoch; action.revision = view.revision;
		action.table = 0; action.tableRevision = view.tables[0].revision;
		action.actionId = member * 1000 + view.revision + 1; action.matchGeneration = generation;
		return authority.Apply(member, action).accepted;
	};
	join(0, true);
	const auto p1 = join(1, false), p2 = join(2, false), member = join(3, false);
	for (const auto fighter : {p1, p2}) CHECK(act(fighter, room::ActionKind::Queue, 0));
	CHECK(act(member, queued ? room::ActionKind::Queue : room::ActionKind::Watch, 0));
	const auto begin = [&] {
		for (const auto fighter : {p1, p2}) CHECK(act(fighter, room::ActionKind::Ready, 0));
		CHECK(authority.BeginMatch(0, p1, p2).accepted);
		return authority.SnapshotView().tables[0].matchGeneration;
	};
	RuntimeRoom r;
	auto* runtime = r.runtime;
	sf4e::netplay::Snapshot session;
	session.generation = {4, 0};
	session.room = sf4e::netplay::RoomState::Joined;
	session.control = sf4e::netplay::Health::Healthy;
	session.match = sf4e::netplay::MatchState::PostMatch;
	room::Snapshot live;
	const auto holds = [&](const sf4e::netplay::Generation& generation) {
		return facade::TrainingHolds(sf4e::TrainingEntry::Room, generation, session, true, live,
			internal::RetiredMatchGeneration());
	};
	const auto tick = [&] { internal::TickMatch(); internal::ReleaseFinishedMatch(); };

	// The first game is watched, its end committed by the room's own event, and retired.
	const auto first = begin();
	r.Snapshot(authority.SnapshotFor(member));
	r.Projection(first);
	r.Deliver(r.Grant(first));
	r.Deliver(Fixture::Connect(first));
	tick(); tick();
	CHECK(r.Match().GetPhase() == Phase::Connecting && r.Match().Generation() == first);
	const auto ended = authority.EndMatch(0, first, room::MatchResult::P1Win);
	CHECK(ended.accepted);
	for (const auto& event : ended.events) {
		if (event.kind != room::Event::Kind::MatchEnded) continue;
		protocol::RoomEventMessage message; message.event = event;
		r.Deliver(json(message));
	}
	internal::DrainRoomEvents();
	CHECK(runtime->committedEndGeneration == first);
	r.Match().End();
	r.room->AbandonMatch(first);
	tick();
	CHECK(r.Match().GetPhase() == Phase::Idle && internal::RetiredMatchGeneration() == first);
	for (const auto who : {p1, p2, member}) CHECK(act(who, room::ActionKind::AcknowledgeTerminal, first));

	// The next game takes the member in.
	const auto second = begin();
	CHECK(authority.SnapshotFor(member).localMatchGenerations[0] == second);
	if (alreadyEnded) CHECK(authority.EndMatch(0, second, room::MatchResult::P2Win).accepted);
	r.Snapshot(authority.SnapshotFor(member));
	// Its setup fails before the session records it, with the first send refused.
	if (staged) {
		r.Deliver(r.Grant(second));
		tick();
		CHECK(r.Match().GetPhase() == Phase::Idle && r.Match().AttemptedGeneration() == second);
		CHECK(internal::RetiredMatchGeneration() == 0);
		r.now += 30001;
	} else {
		r.Match().FailPortReservationForTest(true);
		r.Projection(second);
		r.Deliver(r.Grant(second));
	}
	r.transport->writable = false;
	internal::TickMatch();
	r.Match().FailPortReservationForTest(false);
	CHECK(r.Match().LastFailure() == (staged ? "match_setup_timeout" : "local_port_unavailable"));
	CHECK(r.Match().Generation() == first && r.Match().AttemptedGeneration() == second);
	CHECK(runtime->recoveringMatch);
	if (alreadyEnded) {
		// The room already ended that game: no abort, no lock release, nothing kept.
		CHECK(!runtime->pendingAbort && !runtime->spectatorLockRelease.Pending());
		r.transport->writable = true;
		internal::RetryPendingAbort();
		CHECK(r.KeptUnwatches(second) == 0 && r.KeptUnwatches(first) == 0);
	} else {
		// Refused: kept for the attempted game, with its lock release.
		CHECK(runtime->pendingAbort && runtime->pendingAbort->matchGeneration == second &&
			runtime->pendingAbort->kind == room::ActionKind::Unwatch && runtime->pendingAbort->keepWatching);
		CHECK(runtime->spectatorLockRelease.Pending() && runtime->spectatorLockRelease.Generation() == second);
		CHECK(r.KeptUnwatches(second) == 0);
		// Retried once the transport takes it, and the room accepts it.
		r.transport->writable = true;
		internal::RetryPendingAbort();
		CHECK(!runtime->pendingAbort && r.KeptUnwatches(second) == 1 && r.KeptUnwatches(first) == 0);
		auto sent = r.transport->sent.back().at("action").get<room::Action>();
		// Replayed under this member's own action ids, which the room orders per member.
		sent.actionId = member * 1000 + authority.SnapshotView().revision + 1;
		sent.revision = authority.SnapshotView().revision;
		sent.tableRevision = authority.SnapshotView().tables[0].revision;
		CHECK(authority.Apply(member, sent).accepted);
	}
	live = authority.SnapshotFor(member);
	// Torn down, but recovery not yet ended: refused.
	for (int i = 0; i < 8 && r.Match().GetPhase() != Phase::Idle; ++i) { r.now += 100; tick(); }
	CHECK(r.Match().GetPhase() == Phase::Idle);
	// (When the room had already ended that game there is nothing left to watch.)
	CHECK(internal::RetiredMatchGeneration() == 0 && holds(session.generation) == alreadyEnded);
	// Recovery ends (at the main menu, ReleaseFinishedMatch would).
	runtime->recoveringMatch = false;
	internal::ResetMatchEntry();
	internal::Apply(sf4e::netplay::EventKind::MatchRecovered);
	CHECK(internal::RetiredMatchGeneration() == second);
	CHECK(holds(session.generation));
	sf4e::GameEvents::MainMenu::RequestTraining(session.generation);
	CHECK(record.Take(GetTickCount64(), holds) == Taken::Go && none());
	CHECK(record.Take(GetTickCount64(), holds) == Taken::None);
	// A newer game takes the member in again: refused.
	if (!alreadyEnded) CHECK(authority.EndMatch(0, second, room::MatchResult::P2Win).accepted);
	for (const auto who : {p1, p2, member}) act(who, room::ActionKind::AcknowledgeTerminal, second);
	const auto third = begin();
	live = authority.SnapshotFor(member);
	CHECK(live.localMatchGenerations[0] == third && third > second);
	CHECK(!holds(session.generation));
	sf4e::GameEvents::MainMenu::RequestTraining(session.generation);
	CHECK(record.Take(GetTickCount64(), holds) == Taken::Dropped && none());
	std::cout << "TestFailedSetupThroughRuntime queued=" << queued << " staged=" << staged << " ended=" << alreadyEnded << " passed\n";
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
	for (const bool queued : {false, true})
		for (const bool staged : {false, true}) TestFailedSetupThroughRuntime(queued, staged, false);
	TestFailedSetupThroughRuntime(false, false, true);
	std::cout << "Iroh match session tests passed\n";
	return 0;
}
