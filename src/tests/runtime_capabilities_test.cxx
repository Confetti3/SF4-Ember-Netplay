// The runtime under a game of its own (fake_game.cxx), linked with nothing of
// SF4's hooks, adapter, engine or catalog: what a capability the game turns
// off closes, how the Ready check judges a pick by the game's catalog, and
// how a host's refusal of a join is worded for the way the room was entered
// (RuntimeSession::Start's error callback).
#include "fake_game.hxx"
#include "iroh_test_room.hxx"
#include "session_client_mock.hxx"
#include "test_support.hxx"
#include "../common/Localization.hxx"
#include "../session/IrohMatchSession.hxx"
#include "../session/RoomModel.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../sf4e/sf4e__NetplayFacade.hxx"
#include "../sf4e/sf4e__NetplayRuntime.hxx"
#include "../sf4e/sf4e__RuntimeSession.hxx"
#include <winsock2.h>
#include <array>
#include <iostream>
#include <memory>
#include <string>

using namespace sf4e;
using nlohmann::json;
namespace facade = sf4e::NetplayFacade;
namespace internal = sf4e::NetplayFacade::internal;
using ember::test::Fake;
using ember::test::FakeCapabilities;
using Phase = session::IrohMatchSession::Phase;

namespace {

// The runtime at Home: no room, no helper, the game at its main menu.
struct Home {
	internal::Runtime* runtime = nullptr;
	Home() {
		ember::test::ResetFake();
		NetplayConfig config = {};
		facade::InitFromPayload(config);
		facade::ConfigureHelper({}, ERROR_FILE_NOT_FOUND);
		facade::StartHelper();
		facade::NotifyRuntimeGameReady();
		facade::NotifyRuntimeEventSystemReady();
		facade::TickRuntime();
		runtime = internal::runtime;
		CHECK(runtime);
		CHECK(internal::AtMainMenu());
		runtime->error.clear();
		facade::ClearMatchNotice();
	}
	~Home() {
		facade::ShutdownNetplay(false);
		facade::StopHelper();
		facade::ClearMatchNotice();
	}
};

// The runtime joined to a room of its own, as runtime_room_fixture.hxx's
// RuntimeRoom, with the game's main menu answered by the fake game.
struct InRoom : Home {
	platform::HelperClient helper;
	std::shared_ptr<test::FakeIrohRoom> room = std::make_shared<test::FakeIrohRoom>(helper);
	SessionClient::Callbacks callbacks{};
	std::string name = "Watcher";
	test::MockClient* transport = new test::MockClient();
	ULONGLONG now = 1000;
	InRoom() {
		ember::RuntimeSession::netplay.reset(new ember::RuntimeSession::Netplay(callbacks, "build", 30000, name, 0, 0, 2));
		auto& client = Client();
		CHECK(client.Connect(std::unique_ptr<session::ClientTransport>(transport), false) == 0);
		client.RequireMatchAuthorization();
		transport->state = session::ConnectionState::Connected;
		CHECK(client.Step() == 0);
		SessionProtocol::SessionHelloResp hello; hello.cid = {"room", "watcher"};
		Deliver(json(hello));
		std::array<std::uint8_t, 16> id{}; id[15] = 1;
		room->roomId = id;
		room->localIdentity = std::string(64, 'L');
		runtime->match.reset(new session::IrohMatchSession(client, room, [this] { return now; }));
		runtime->attached = true;
		CHECK(runtime->controller.Execute({netplay::CommandKind::HostRoom, runtime->controller.GetSnapshot().generation, {}}).accepted);
		internal::Apply(netplay::EventKind::RoomJoined);
		CHECK(runtime->controller.GetSnapshot().control == netplay::Health::Healthy);
	}
	~InRoom() {
		runtime->attached = false;
		runtime->match.reset();
	}
	SessionClient& Client() { return ember::RuntimeSession::netplay->client; }
	void Deliver(const json& message) { transport->Push(message); CHECK(Client().Step() == 0); }
	// The native roster for `generation`: the two fighters, then this PC.
	void Projection(std::uint64_t generation) {
		SessionProtocol::SessionDataUpdate update;
		update.lobbyData.members.resize(3);
		update.lobbyData.members[0].connId = {"room", "p1"};
		update.lobbyData.members[1].connId = {"room", "p2"};
		update.lobbyData.members[2].connId = Client()._cid;
		update.matchGeneration = generation;
		Deliver(json(update));
	}
};

// Training, from Home: offered, the runtime asks the game to leave for it;
// not offered, it neither holds nor is asked of the game, and the player is told.
void TestTrainingFromHome(bool offered) {
	Home home;
	FakeCapabilities().training = offered;
	const auto session = home.runtime->controller.GetSnapshot();
	CHECK(session.room == netplay::RoomState::Idle && session.match == netplay::MatchState::None);
	CHECK(facade::TrainingHolds(TrainingEntry::Home, session.generation, session, true, room::Snapshot{}, 0) == offered);
	CHECK(facade::TrainingRequestHolds(session.generation) == offered);
	facade::RuntimeCommand training{};
	training.command.generation = session.generation;
	training.training = TrainingEntry::Home;
	CHECK(facade::SubmitRuntimeCommand(std::move(training)));
	internal::DrainCommands(true);
	CHECK(Fake().trainingRequests == (offered ? 1 : 0));
	CHECK(home.runtime->error == (offered ? std::string() : std::string(loc::T("home.training_unavailable"))));
	std::cout << "TestTrainingFromHome offered=" << offered << " passed\n";
}

// A tournament match's watcher joins its room as an observer. Offered, the join
// goes on to the runtime's own checks, which refuse it here (no helper);
// not offered, it is dropped before them, so nothing is opened or said.
void TestObserverJoin(bool offered) {
	Home home;
	FakeCapabilities().spectators = offered;
	const auto before = home.runtime->controller.GetSnapshot().generation;
	const auto outcome = internal::DispatchTournamentRoomCommand({netplay::CommandKind::JoinInvite, {}, "invitation"}, facade::JoinKind::Observer);
	CHECK(outcome == netplay::DispatchOutcome::Dropped);
	CHECK(home.runtime->controller.GetSnapshot().generation == before);
	CHECK(home.runtime->controller.GetSnapshot().room == netplay::RoomState::Idle);
	CHECK(home.runtime->error == (offered ? std::string(loc::T("runtime.return_main_menu")) : std::string()));
	std::cout << "TestObserverJoin offered=" << offered << " passed\n";
}

// Watching a tournament match. Offered, the watch goes on to the runtime's own
// checks (refused here: no helper); not offered, no watch starts, so no
// match_watch is sent and no observer join follows.
void TestTournamentWatch(bool offered) {
	Home home;
	FakeCapabilities().spectators = offered;
	facade::RuntimeCommand watch{};
	watch.tournament.op = netplay::tournament::Command::Op::Watch;
	watch.tournament.bridgeId = "bridge";
	watch.tournament.matchId = "match";
	CHECK(facade::SubmitRuntimeCommand(std::move(watch)));
	internal::DrainCommands(false);
	CHECK(!home.runtime->watch.Active());
	CHECK(home.runtime->error == (offered ? std::string(loc::T("runtime.return_main_menu")) : std::string()));
	std::cout << "TestTournamentWatch offered=" << offered << " passed\n";
}

// A game the room has this PC only watch (slot 2 of its roster), up to its
// start. Offered, the runtime enters it as the session starts; not offered,
// the same start enters nothing and ends the session at once.
void TestWatchedGameEntry(bool offered) {
	InRoom r;
	FakeCapabilities().spectators = offered;
	const std::uint64_t generation = 41;
	const auto tick = [] { internal::TickMatch(); internal::ReleaseFinishedMatch(); };
	r.Projection(generation);
	r.Deliver(test::GameGrant(*r.room, generation, {{"room", "p1"}, {"room", "p2"}, r.Client()._cid}, 2));
	r.Deliver(test::GameConnect(generation));
	tick(); tick();
	CHECK(r.runtime->match->Generation() == generation && r.runtime->match->LocalSlot() == 2);
	r.room->LinkUp(generation);
	tick();
	CHECK(r.runtime->match->GetPhase() == Phase::Connecting && Fake().battleEntries == 0);
	r.Deliver(json{{"type", "game_start"}, {"generation", generation}});
	tick();
	CHECK(!r.runtime->entry.matchEntered);
	if (offered) CHECK(Fake().battleEntries == 1 && r.runtime->match->GetPhase() == Phase::Started);
	else {
		CHECK(Fake().battleEntries == 0 && r.runtime->match->GetPhase() == Phase::Ending);
		// The grant the PC sat out ends: Preparing is released to PostMatch, which
		// is where a game this PC never fought stands, and the shell reopens.
		CHECK(r.runtime->controller.GetSnapshot().match == netplay::MatchState::Preparing);
		tick();
		CHECK(r.runtime->controller.GetSnapshot().match == netplay::MatchState::PostMatch);
	}
	std::cout << "TestWatchedGameEntry offered=" << offered << " entries=" << Fake().battleEntries << " passed\n";
}

// A room's Watch press. Offered, it goes on to the room (a send, or a parked
// retry); not offered, it is dropped, and the snapshot the shell reads says the
// game has no watching.
void TestRoomWatch(bool offered) {
	InRoom r;
	FakeCapabilities().spectators = offered;
	CHECK(internal::CurrentGates().canWatch == offered);
	internal::Publish();
	CHECK(facade::GetRuntimeSnapshotShared()->canWatch == offered);
	const auto sentBefore = r.transport->sent.size();
	facade::RuntimeCommand watch{};
	watch.command.kind = netplay::CommandKind::RoomAction;
	watch.command.generation = r.runtime->controller.GetSnapshot().generation;
	watch.roomAction.kind = room::ActionKind::Watch;
	CHECK(facade::SubmitRuntimeCommand(std::move(watch)));
	internal::DrainCommands(true);
	CHECK((r.runtime->roomActionIntent.Parked() || r.transport->sent.size() > sentBefore) == offered);
	std::cout << "TestRoomWatch offered=" << offered << " passed\n";
}

// A Ready press judged by the game's catalog (fake_catalog.hxx: 60 fighters,
// 20 costumes, 100 colours, 40 stages, no Random stage), not SF4's: fighter 59
// in colour 99, which SF4 refuses, passes the stage, fighter and pick checks
// and stops only at the seat this PC has not taken; past each limit, and on
// SF4's Random stage, the press is refused with that check's own words.
void TestReadyUnderGameCatalog() {
	InRoom r;
	input::Device pad;
	pad.type = 0; pad.index = 0; pad.connected = true;
	r.runtime->input.Adopt(pad);
	const auto ready = [&](int fighter, int costume, int color, int stage) {
		facade::RuntimeCommand press{};
		press.command.kind = netplay::CommandKind::Ready;
		press.command.generation = r.runtime->controller.GetSnapshot().generation;
		press.character.charaID = static_cast<std::uint8_t>(fighter);
		press.character.costume = static_cast<std::uint8_t>(costume);
		press.character.color = static_cast<std::uint8_t>(color);
		press.stage = stage;
		r.runtime->readyFailure.clear();
		CHECK(facade::SubmitRuntimeCommand(std::move(press)));
		internal::DrainCommands(true);
		return r.runtime->readyFailure;
	};
	CHECK(ready(59, 19, 99, 39) == loc::T("runtime.ready.take_seat"));
	CHECK(ready(60, 0, 0, 0) == loc::T("runtime.ready.fighter_unavailable"));
	CHECK(ready(59, 20, 0, 0) == loc::T("runtime.ready.selection_unavailable"));
	CHECK(ready(59, 0, 100, 0) == loc::T("runtime.ready.selection_unavailable"));
	CHECK(ready(59, 0, 0, 40) == loc::T("runtime.ready.stage_unavailable"));
	CHECK(ready(59, 0, 0, 255) == loc::T("runtime.ready.stage_unavailable"));
	std::cout << "TestReadyUnderGameCatalog passed\n";
}

// The stages a player may not choose (UnavailableStagesNow, SFV Ember's
// EMBER_LIBRARY.md section 38): at Home the game's own missing stages; in a
// room before any seat, the same; seated as P1, both seats'. P1's Ready on a
// stage the opponent's copy lacks is refused with that reason before anything
// is sent; one both copies have passes the stage check.
void TestStagesAtTheTable() {
	{
		Home home;
		Fake().missingStages = {7};
		CHECK(internal::UnavailableStagesNow() == std::vector<std::int64_t>{7});
		Fake().missingStages.clear();
		CHECK(internal::UnavailableStagesNow().empty());
	}
	InRoom r;
	Fake().missingStages = {7};
	CHECK(internal::UnavailableStagesNow() == std::vector<std::int64_t>{7});
	input::Device pad;
	pad.type = 0; pad.index = 0; pad.connected = true;
	r.runtime->input.Adopt(pad);
	// The room as its owner sends it to this PC: this PC is P1 at table 0, and
	// the opponent's copy lacks stage 5.
	room::RoomAuthority authority("Stages", 8, 9);
	room::MemberProfile own, theirs;
	own.missingStages = {7};
	theirs.missingStages = {5};
	const auto local = authority.Join("Watcher", {"room", "watcher"}, true, own).snapshot.members.back().id;
	const auto other = authority.Join("Opponent", {"room", "p2"}, false, theirs).snapshot.members.back().id;
	std::uint64_t actionId = 0;
	for (const auto member : {local, other}) {
		room::Action queue;
		queue.kind = room::ActionKind::Queue; queue.roomEpoch = authority.SnapshotView().roomEpoch;
		queue.revision = authority.SnapshotView().revision; queue.tableRevision = authority.SnapshotView().tables[0].revision;
		queue.actionId = ++actionId;
		CHECK(authority.Apply(member, queue).accepted);
	}
	CHECK(authority.SnapshotView().tables[0].p1 == local);
	SessionProtocol::RoomSnapshotMessage message;
	message.snapshot = authority.SnapshotFor(local);
	r.Deliver(json(message));
	CHECK(internal::UnavailableStagesNow() == (std::vector<std::int64_t>{5, 7}));
	const auto ready = [&](int stage) {
		facade::RuntimeCommand press{};
		press.command.kind = netplay::CommandKind::Ready;
		press.command.generation = r.runtime->controller.GetSnapshot().generation;
		press.character.charaID = 1;
		press.stage = stage;
		r.runtime->readyFailure.clear();
		CHECK(facade::SubmitRuntimeCommand(std::move(press)));
		internal::DrainCommands(true);
		return r.runtime->readyFailure;
	};
	CHECK(ready(5) == loc::T("runtime.ready.stage_not_shared"));
	CHECK(ready(6) != loc::T("runtime.ready.stage_not_shared"));
	Fake().missingStages.clear();
	std::cout << "TestStagesAtTheTable passed\n";
}

// The host refuses the join as full: worded for a public room's lock when the
// room was entered with a public ticket, else as a full room.
void TestJoinRefusalWording(bool publicJoin) {
	Home home;
	home.runtime->join = publicJoin ? facade::JoinKind::Public : facade::JoinKind::Private;
	CHECK(facade::IsRuntimePublicJoin() == publicJoin);
	auto* transport = new test::MockClient();
	std::string name = "Refused";
	ember::RuntimeSession::Start(std::unique_ptr<session::ClientTransport>(transport), 30000, name, 0, 0, 2);
	auto& client = ember::RuntimeSession::netplay->client;
	transport->state = session::ConnectionState::Connected;
	CHECK(client.Step() == 0);
	SessionProtocol::SessionJoinReject reject;
	reject.result = SessionProtocol::JoinResult::JR_LOBBY_FULL;
	transport->Push(json(reject));
	client.Step();
	const std::string expected = loc::T(publicJoin ? SessionClient::PublicJoinRejectionKey(SessionClient::SCE_JOIN_REJECTED_LOBBY_FULL) :
		SessionClient::JoinRejectionKey(SessionClient::SCE_JOIN_REJECTED_LOBBY_FULL));
	CHECK(expected == loc::T(publicJoin ? "public.failure.full_or_locked" : "runtime.room_full"));
	CHECK_MSG(std::string(facade::GetStatus().lastError) == expected, "got '%s'", facade::GetStatus().lastError);
	std::cout << "TestJoinRefusalWording public=" << publicJoin << " passed\n";
}

} // namespace

int main() {
	for (const bool offered : {true, false}) {
		TestTrainingFromHome(offered);
		TestObserverJoin(offered);
		TestTournamentWatch(offered);
		TestWatchedGameEntry(offered);
		TestRoomWatch(offered);
	}
	TestReadyUnderGameCatalog();
	TestStagesAtTheTable();
	TestJoinRefusalWording(false);
	TestJoinRefusalWording(true);
	std::cout << "Runtime capabilities tests passed\n";
	return 0;
}
