#include "session_client_mock.hxx"
#include "iroh_test_room.hxx"
#include "runtime_room_fixture.hxx"
#include "../session/IrohMatchSession.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../session/RoomModel.hxx"
#include "../sf4e/sf4e__NetplayFacade.hxx"
#include "../sf4e/sf4e__GameEvents.hxx"
#include "../sf4e/sf4e__NetplayRuntime.hxx"
#include "../sf4e/sf4e__UserApp.hxx"
#include "../sf4e/sf4e__OverlayPrefs.hxx"
#include "../sf4e/sf4e__Game__Battle__System.hxx"
#include "../common/Localization.hxx"
#include "../common/NextBattleRequest.hxx"
#include "../Dimps/Dimps.hxx"
#include <winsock2.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

#include "test_support.hxx"

// What the netplay runtime does around a spectator's match, through its real
// command, reply and teardown paths: a failed setup, the room's unscored end,
// leaving a watched game, the lock-in notices, the battle entry's cleanup, and
// a watcher's room settings.
using namespace sf4e;
using nlohmann::json;
namespace protocol = sf4e::SessionProtocol;
using session::IrohMatchSession;
using sf4e::test::FakeIrohRoom;
using sf4e::test::GameConnect;
using sf4e::test::GameGrant;
using sf4e::test::MockClient;
using Phase = IrohMatchSession::Phase;

namespace {

using sf4e::test::RuntimeRoom;

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
	r.Deliver(GameConnect(first));
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
	CHECK(runtime->teardown.committedEndGeneration == first);
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
		CHECK(!runtime->teardown.pendingAbort && !runtime->spectatorLockRelease.Pending());
		r.transport->writable = true;
		internal::RetryPendingAbort();
		CHECK(r.KeptUnwatches(second) == 0 && r.KeptUnwatches(first) == 0);
	} else {
		// Refused: kept for the attempted game, with its lock release.
		CHECK(runtime->teardown.pendingAbort && runtime->teardown.pendingAbort->matchGeneration == second &&
			runtime->teardown.pendingAbort->kind == room::ActionKind::Unwatch && runtime->teardown.pendingAbort->keepWatching);
		CHECK(runtime->spectatorLockRelease.Pending() && runtime->spectatorLockRelease.Generation() == second);
		CHECK(r.KeptUnwatches(second) == 0);
		// Retried once the transport takes it, and the room accepts it.
		r.transport->writable = true;
		internal::RetryPendingAbort();
		CHECK(!runtime->teardown.pendingAbort && r.KeptUnwatches(second) == 1 && r.KeptUnwatches(first) == 0);
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

// A room, as this PC's runtime watches its table 0, and the game it is in.
// enter false: the game is begun and granted, but this PC's setup of it
// fails (its port cannot be reserved) at the next TickMatch. queued: this PC
// waits in the table's queue rather than watching by choice, and each game is
// a whole set, so the end of one rotates the queue.
struct WatchedGame {
	sf4e::room::RoomAuthority authority;
	sf4e::room::MemberId p1 = 0, p2 = 0, member = 0;
	std::uint64_t generation = 0;
	RuntimeRoom r;
	explicit WatchedGame(bool practice, bool enter = true, bool queued = false) :
		authority("Watched", 16, 96, TableRules(queued), practice) {
		namespace room = sf4e::room;
		const auto join = [&](int index, bool host) {
			const auto result = authority.Join("Player" + std::to_string(index), room::ConnectionRef{"host", std::to_string(index)}, host);
			CHECK(result.accepted);
			return result.snapshot.members.back().id;
		};
		join(0, true);
		p1 = join(1, false); p2 = join(2, false); member = join(3, false);
		for (const auto fighter : {p1, p2}) CHECK(Act(fighter, room::ActionKind::Queue));
		CHECK(Act(member, queued ? room::ActionKind::Queue : room::ActionKind::Watch));
		for (const auto fighter : {p1, p2}) CHECK(Act(fighter, room::ActionKind::Ready));
		CHECK(authority.BeginMatch(0, p1, p2).accepted);
		generation = authority.SnapshotView().tables[0].matchGeneration;
		r.Snapshot(authority.SnapshotFor(member));
		if (!enter) r.Match().FailPortReservationForTest(true);
		r.Projection(generation);
		r.Deliver(r.Grant(generation));
		if (!enter) return;
		r.Deliver(GameConnect(generation));
		Tick(); Tick();
		CHECK(r.Match().Generation() == generation);
	}
	static sf4e::room::Rules TableRules(bool queued) {
		sf4e::room::Rules rules;
		if (queued) rules.format = sf4e::room::SetFormat::Ft1;
		return rules;
	}
	bool Act(sf4e::room::MemberId who, sf4e::room::ActionKind kind) {
		const auto& view = authority.SnapshotView();
		sf4e::room::Action action;
		action.kind = kind; action.roomEpoch = view.roomEpoch; action.revision = view.revision;
		action.table = 0; action.tableRevision = view.tables[0].revision; action.actionId = who * 1000 + view.revision + 1;
		return authority.Apply(who, action).accepted;
	}
	void Tick() { sf4e::NetplayFacade::internal::TickMatch(); sf4e::NetplayFacade::internal::ReleaseFinishedMatch(); }
	// The room ends the game; its end reaches this PC through the runtime's own event handler.
	void End(sf4e::room::MatchResult result) {
		const auto ended = authority.EndMatch(0, generation, result);
		CHECK(ended.accepted);
		for (const auto& event : ended.events) {
			if (event.kind != sf4e::room::Event::Kind::MatchEnded) continue;
			protocol::RoomEventMessage message; message.event = event;
			r.Deliver(json(message));
		}
		sf4e::NetplayFacade::internal::DrainRoomEvents();
	}
};

static bool __cdecl GgpoBegin(const char*) { return true; }
static bool __cdecl GgpoSave(unsigned char**, int*, int*, int) { return false; }
static bool __cdecl GgpoLoad(unsigned char*, int) { return true; }
static bool __cdecl GgpoLog(char*, unsigned char*, int) { return true; }
static void __cdecl GgpoFree(void*) {}
static bool __cdecl GgpoAdvance(int) { return true; }
static bool __cdecl GgpoEvent(GGPOEvent*) { return true; }
// A live GGPO session towards a port nobody answers: something for the end to retire.
static void StartGgpoSession() {
	GGPOSessionCallbacks callbacks = {};
	callbacks.begin_game = GgpoBegin; callbacks.save_game_state = GgpoSave; callbacks.load_game_state = GgpoLoad;
	callbacks.log_game_state = GgpoLog; callbacks.free_buffer = GgpoFree; callbacks.advance_frame = GgpoAdvance; callbacks.on_event = GgpoEvent;
	char host[] = "127.0.0.1";
	CHECK(ggpo_start_spectating(&sf4e::Game::Battle::System::ggpo, &callbacks, "unscored-end", 2, 2, 0, host, 9) == GGPO_OK);
}

// The game's battle took its stream: what entering the match records, with a
// live GGPO session for a leave to retire.
static void StreamWatchedGame(WatchedGame& game) {
	StartGgpoSession();
	auto* runtime = game.r.runtime;
	runtime->entry.matchEntered = true;
	runtime->entry.enteredGeneration = game.generation;
	runtime->entry.enteredTable = 0;
	const auto watched = sf4e::NetplayFacade::internal::WatchedGameNow();
	CHECK(watched.Active() && watched.table == 0 && watched.generation == game.generation);
}

// Stop watching as the shell submits it: the leave of the watched game, by name.
static void SubmitStopWatching(WatchedGame& game) {
	sf4e::NetplayFacade::RuntimeCommand leave{};
	leave.command.generation = game.r.runtime->controller.GetSnapshot().generation;
	leave.leaveWatchedGame = {0, game.generation};
	CHECK(sf4e::NetplayFacade::SubmitRuntimeCommand(std::move(leave)));
}

// The room actions of `kind` this PC sent from its `from`th message on.
static std::vector<sf4e::room::Action> SentActions(const WatchedGame& game, sf4e::room::ActionKind kind, std::size_t from) {
	std::vector<sf4e::room::Action> sent;
	const auto& messages = game.r.transport->sent;
	for (std::size_t i = from; i < messages.size(); ++i) {
		if (messages[i].value("type", json()) != json(protocol::MT_ROOM_ACTION)) continue;
		const auto action = messages[i].at("action").get<sf4e::room::Action>();
		if (action.kind == kind) sent.push_back(action);
	}
	return sent;
}

// The player's own leave of the stream: a passing note, not an error.
static void CheckLeftTheStream() {
	const auto status = sf4e::NetplayFacade::GetStatus();
	CHECK(!sf4e::Game::Battle::System::ggpo);
	CHECK(std::string(status.lastError) == sf4e::loc::T("runtime.leaving_spectator") && status.lastErrorSeverity == sf4e::NoticeSeverity::Info);
}

// The room applies `action` from `who` at its current revisions, under the
// member's own action ids, which it orders per member.
static sf4e::room::Result ApplyAs(WatchedGame& game, sf4e::room::MemberId who, sf4e::room::Action action) {
	const auto& view = game.authority.SnapshotView();
	action.roomEpoch = view.roomEpoch; action.revision = view.revision; action.tableRevision = view.tables[0].revision;
	action.actionId = who * 1000 + view.revision + 1;
	return game.authority.Apply(who, action);
}
static sf4e::room::Action LockAction(bool locked) {
	sf4e::room::Action lock;
	lock.kind = sf4e::room::ActionKind::LockSpectating; lock.locked = locked; lock.table = 0;
	return lock;
}
// The member locks in, or a stream failure's release unlocks it (the watch
// stays), and this PC's runtime reads the projection that shows it.
static void SetLocked(WatchedGame& game, bool locked) {
	CHECK(ApplyAs(game, game.member, LockAction(locked)).accepted);
	game.r.Snapshot(game.authority.SnapshotFor(game.member));
	sf4e::NetplayFacade::internal::DrainActionReplies();
}

// A game the room ends with no result tells this PC so, from the room's
// outcome rather than from its session: with a live session, which is retired
// first, or with none left. In a Practice room an Abort is only the end of the
// practice, a passing note; anywhere else, or a host's Cancel, the game was
// cancelled. A PC that had already left the game keeps the reason it gave.
void TestUnscoredEndNotice() {
	namespace room = sf4e::room;
	namespace facade = sf4e::NetplayFacade;
	using fSystem = sf4e::Game::Battle::System;
	WSADATA winsock{}; CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
	struct Case { bool practice; room::MatchResult result; bool session; const char* notice; sf4e::NoticeSeverity severity; };
	const Case cases[] = {
		{true, room::MatchResult::Abort, true, "runtime.practice_ended", sf4e::NoticeSeverity::Info},
		{true, room::MatchResult::Abort, false, "runtime.practice_ended", sf4e::NoticeSeverity::Info},
		{false, room::MatchResult::Abort, true, "runtime.table_game_cancelled", sf4e::NoticeSeverity::Error},
		{false, room::MatchResult::Abort, false, "runtime.table_game_cancelled", sf4e::NoticeSeverity::Error},
		{true, room::MatchResult::Cancel, false, "runtime.table_game_cancelled", sf4e::NoticeSeverity::Error},
	};
	for (const auto& test : cases) {
		WatchedGame game(test.practice);
		facade::ClearMatchNotice();
		// A notice the session raised is replaced by the room's outcome.
		facade::PushAlert("Connection unstable.", sf4e::NoticeSeverity::Error);
		if (test.session) StartGgpoSession();
		game.End(test.result);
		const auto status = facade::GetStatus();
		CHECK(!fSystem::ggpo && game.r.runtime->recoveringMatch);
		CHECK(std::string(status.lastError) == sf4e::loc::T(test.notice) && status.lastErrorSeverity == test.severity);
	}
	{
		// This PC had already left the game and said why: the room's end changes nothing.
		WatchedGame game(true);
		facade::ClearMatchNotice();
		StartGgpoSession();
		sf4e::NetplayFacade::internal::AbortLocalMatch("The spectator stream was dropped.");
		CHECK(!fSystem::ggpo && std::string(facade::GetStatus().lastError) == "The spectator stream was dropped.");
		game.End(room::MatchResult::Abort);
		CHECK(std::string(facade::GetStatus().lastError) == "The spectator stream was dropped.");
	}
	facade::ClearMatchNotice();
	std::cout << "TestUnscoredEndNotice passed\n";
}

// A locked-in spectator leaves the game it watches (Stop watching): its
// Unwatch goes out at once while the stream is live, and the session is
// retired as the player's own leave, a passing note. That is no stream
// failure, so no lock release is armed and no abort is reported. The room
// lists the member locked in until it applies the Unwatch, and a release sent
// meanwhile would be refused there. The next tick, with the room still joined
// and that projection still in hand, sends neither.
void TestSpectatorLeavesWatchedGame() {
	namespace room = sf4e::room;
	namespace facade = sf4e::NetplayFacade;
	using fSystem = sf4e::Game::Battle::System;
	WatchedGame game(false);
	auto* runtime = game.r.runtime;
	// The projection the leave is made from, kept to the end: listed, locked in.
	SetLocked(game, true);
	const auto lockedIn = [&] {
		const auto& snapshot = game.r.Client().GetRoomSnapshot();
		const auto* local = room::FindMember(snapshot, snapshot.localMember);
		const auto& watchers = snapshot.tables[0].spectators;
		return local && local->spectatorLocked && std::find(watchers.begin(), watchers.end(), local->id) != watchers.end();
	};
	CHECK(lockedIn());
	StreamWatchedGame(game);
	facade::ClearMatchNotice();
	const auto sentBefore = game.r.transport->sent.size();
	const auto releaseClear = [&] {
		return !runtime->spectatorLockRelease.Pending() && !runtime->teardown.pendingAbort && runtime->entry.spectatorStreamFailedGeneration == 0;
	};
	SubmitStopWatching(game);
	facade::internal::DrainCommands(true);
	CheckLeftTheStream();
	CHECK(runtime->recoveringMatch && releaseClear());
	facade::TickRuntime();
	CHECK(releaseClear());
	CHECK(runtime->attached && runtime->controller.GetSnapshot().room == sf4e::netplay::RoomState::Joined);
	CHECK(lockedIn());
	const auto unwatches = SentActions(game, room::ActionKind::Unwatch, sentBefore);
	CHECK(unwatches.size() == 1 && unwatches[0].matchGeneration == game.generation && !unwatches[0].keepWatching);
	CHECK(SentActions(game, room::ActionKind::LockSpectating, sentBefore).empty());
	CHECK(SentActions(game, room::ActionKind::AbortMatch, sentBefore).empty());
	facade::ClearMatchNotice();
	std::cout << "TestSpectatorLeavesWatchedGame passed\n";
}

// Release lock-in as the shell submits it, taken by the game thread.
static void SubmitRelease(WatchedGame& game) {
	const auto& seen = game.r.Client().GetRoomSnapshot();
	sf4e::NetplayFacade::RuntimeCommand release{};
	release.command = {sf4e::netplay::CommandKind::RoomAction, game.r.runtime->controller.GetSnapshot().generation, {}};
	release.roomAction = LockAction(false);
	release.roomAction.roomEpoch = seen.roomEpoch; release.roomAction.revision = seen.revision;
	release.roomAction.tableRevision = seen.tables[0].revision;
	CHECK(sf4e::NetplayFacade::SubmitRuntimeCommand(std::move(release)));
	sf4e::NetplayFacade::internal::DrainCommands(true);
}

// The room's answer to the last `kind` this PC sent, with its projection after
// it: taken, or refused for a raced table revision. The runtime reads it.
static void Answer(WatchedGame& game, sf4e::room::ActionKind kind, bool take) {
	const auto sent = SentActions(game, kind, 0);
	CHECK(!sent.empty());
	protocol::RoomResultMessage message;
	message.actionId = sent.back().actionId;
	if (take) CHECK((message.result = ApplyAs(game, game.member, sent.back())).accepted);
	else message.result.reason = sf4e::room::RejectReason::StaleTable;
	message.result.snapshot = game.authority.SnapshotFor(game.member);
	game.r.Deliver(json(message));
	sf4e::NetplayFacade::internal::DrainActionReplies();
}

// A spectator's own view failing while it is locked in is the one end of a
// lock-in the runtime reports (Runtime::lockInDrops): counted where the
// failure arms the lock release, once per failed game however many paths
// record it, and published to the shell. A failure while not locked in counts
// nothing, and so does the player's own Release or Stop watching, taken or
// refused, in any order, at any game.
void TestFailedViewCountsLockInDrops() {
	namespace room = sf4e::room;
	namespace facade = sf4e::NetplayFacade;
	using fSystem = sf4e::Game::Battle::System;
	const auto drops = [](WatchedGame& game) { return game.r.runtime->lockInDrops; };
	struct Case { bool locked, releaseRefused; };
	for (const auto test : {Case{true, false}, Case{true, true}, Case{false, false}}) {
		WatchedGame game(false);
		const auto before = drops(game);
		if (test.locked) SetLocked(game, true);
		if (test.releaseRefused) { SubmitRelease(game); Answer(game, room::ActionKind::LockSpectating, false); }
		StreamWatchedGame(game);
		fSystem::AbortGgpoMatch(sf4e::loc::T("runtime.spectator_stream_lost"));
		facade::NotifyRuntimeSpectatorStreamFailed();
		game.Tick(); facade::TickRuntime();
		CHECK(drops(game) == before + (test.locked ? 1 : 0));
		CHECK(facade::GetRuntimeSnapshotShared()->lockInDrops == drops(game));
		facade::ClearMatchNotice();
	}
	{
		// Release sent during one game, accepted once the table has begun the next.
		WatchedGame game(false);
		const auto before = drops(game);
		SetLocked(game, true);
		SubmitRelease(game);
		game.End(room::MatchResult::P1Win);
		room::Action ack;
		ack.kind = room::ActionKind::AcknowledgeTerminal; ack.table = 0; ack.matchGeneration = game.generation;
		for (const auto who : {game.p1, game.p2, game.member}) CHECK(ApplyAs(game, who, ack).accepted);
		for (const auto fighter : {game.p1, game.p2}) CHECK(game.Act(fighter, room::ActionKind::Ready));
		CHECK(game.authority.BeginMatch(0, game.p1, game.p2).accepted);
		game.r.Snapshot(game.authority.SnapshotFor(game.member));
		CHECK(game.r.Client().GetRoomSnapshot().tables[0].matchGeneration > game.generation);
		Answer(game, room::ActionKind::LockSpectating, true);
		game.Tick(); facade::TickRuntime();
		CHECK(!room::FindMember(game.r.Client().GetRoomSnapshot(), game.member)->spectatorLocked && drops(game) == before);
		facade::ClearMatchNotice();
	}
	for (const bool releaseTaken : {true, false}) {
		// Release and Stop watching in flight together, answered on separate ticks, one refused.
		WatchedGame game(false);
		const auto before = drops(game);
		SetLocked(game, true);
		StreamWatchedGame(game);
		SubmitRelease(game);
		SubmitStopWatching(game);
		facade::internal::DrainCommands(true);
		Answer(game, room::ActionKind::LockSpectating, releaseTaken);
		facade::TickRuntime();
		Answer(game, room::ActionKind::Unwatch, !releaseTaken);
		facade::TickRuntime();
		CHECK(drops(game) == before && !game.r.runtime->spectatorLockRelease.Pending());
		facade::ClearMatchNotice();
	}
	std::cout << "TestFailedViewCountsLockInDrops passed\n";
}

// A locked spectator's own Queue or Watch over the live game (here at the
// table it watches, and at another) leaves the view as a departure: the
// stream's end is no failure, so no lock release is armed, no drop is counted
// and the shell tells no "dropped". A real failure still does all three
// (TestFailedViewCountsLockInDrops).
void TestTableChangeLeavesWithoutFailure() {
	namespace room = sf4e::room;
	namespace facade = sf4e::NetplayFacade;
	struct Case { room::ActionKind kind; std::uint8_t table; };
	for (const auto test : {Case{room::ActionKind::Queue, 0}, Case{room::ActionKind::Watch, 1}}) {
		WatchedGame game(false);
		auto* runtime = game.r.runtime;
		SetLocked(game, true);
		StreamWatchedGame(game);
		facade::ClearMatchNotice();
		const auto before = runtime->lockInDrops;
		const auto sentBefore = game.r.transport->sent.size();
		const auto& seen = game.r.Client().GetRoomSnapshot();
		sf4e::NetplayFacade::RuntimeCommand change{};
		change.command = {sf4e::netplay::CommandKind::RoomAction, runtime->controller.GetSnapshot().generation, {}};
		change.roomAction.kind = test.kind; change.roomAction.table = test.table;
		change.roomAction.roomEpoch = seen.roomEpoch; change.roomAction.revision = seen.revision;
		change.roomAction.tableRevision = seen.tables[test.table].revision;
		CHECK(facade::SubmitRuntimeCommand(std::move(change)));
		facade::internal::DrainCommands(true);
		const auto status = facade::GetStatus();
		CHECK(!sf4e::Game::Battle::System::ggpo && runtime->recoveringMatch);
		CHECK(std::string(status.lastError) == sf4e::loc::T("runtime.returning_room") && status.lastErrorSeverity == sf4e::NoticeSeverity::Info);
		facade::TickRuntime();
		CHECK(!runtime->spectatorLockRelease.Pending() && runtime->entry.spectatorStreamFailedGeneration == 0);
		CHECK(runtime->lockInDrops == before && facade::GetRuntimeSnapshotShared()->lockInDrops == before);
		CHECK(SentActions(game, room::ActionKind::LockSpectating, sentBefore).empty());
		facade::ClearMatchNotice();
	}
	std::cout << "TestTableChangeLeavesWithoutFailure passed\n";
}

// A queued watcher's leave (Leave this game) is an Unwatch of the watched game
// that keeps its place, decided by the runtime when it runs.
void TestQueuedWatcherLeavesTheGame() {
	namespace room = sf4e::room;
	WatchedGame game(false, true, true);
	StreamWatchedGame(game);
	sf4e::NetplayFacade::ClearMatchNotice();
	const auto sentBefore = game.r.transport->sent.size();
	SubmitStopWatching(game);
	sf4e::NetplayFacade::internal::DrainCommands(true);
	CheckLeftTheStream();
	const auto kept = SentActions(game, room::ActionKind::Unwatch, sentBefore);
	CHECK(kept.size() == 1 && kept[0].matchGeneration == game.generation && kept[0].keepWatching);
	sf4e::NetplayFacade::ClearMatchNotice();
	std::cout << "TestQueuedWatcherLeavesTheGame passed\n";
}

// Stop watching parks when its first send finds the control link unable to
// take it, and goes once the link can, while the room is still catching up on
// its checkpoint (healthy control, authority not yet writable), just as its
// first send would have: one Unwatch for the watched game, then the player's
// own leave of the stream.
void TestParkedStopWatchingCatchesUp() {
	namespace room = sf4e::room;
	namespace facade = sf4e::NetplayFacade;
	using fSystem = sf4e::Game::Battle::System;
	WatchedGame game(false);
	auto* runtime = game.r.runtime;
	StreamWatchedGame(game);
	// Connected, but the checkpoint has not applied for longer than the grace.
	auto& controller = runtime->controller;
	CHECK(controller.ObserveCoordination(1, 1, true, 1000, false));
	CHECK(controller.ObserveCoordination(1, 1, true, 5000, false));
	const auto state = controller.GetSnapshot();
	CHECK(state.control == sf4e::netplay::Health::Healthy && state.coordinated && !state.authorityWritable);
	facade::ClearMatchNotice();
	const auto sentBefore = game.r.transport->sent.size();
	game.r.transport->writable = false;
	SubmitStopWatching(game);
	facade::internal::DrainCommands(true);
	CHECK(fSystem::ggpo && runtime->roomActionIntent.Parked());
	CHECK(SentActions(game, room::ActionKind::Unwatch, sentBefore).empty());
	game.r.transport->writable = true;
	facade::internal::ResolvePendingIntents(true);
	CheckLeftTheStream();
	CHECK(runtime->recoveringMatch && !runtime->roomActionIntent.Parked());
	facade::internal::ResolvePendingIntents(true);
	const auto sent = SentActions(game, room::ActionKind::Unwatch, sentBefore);
	CHECK(sent.size() == 1 && sent[0].matchGeneration == game.generation && !sent[0].keepWatching);
	facade::ClearMatchNotice();
	std::cout << "TestParkedStopWatchingCatchesUp passed\n";
}

// The room has finished the game this PC still watches: the table is back in
// Waiting, a queued watcher was rotated into a seat, or the table plays its
// next game. Stop watching then owes the room nothing: no Unwatch goes, for
// that game or the next, nothing is owed for a stream failure, and only the
// rest of the stream ends, as the player's own leave.
void TestFinishedWatchEndsOnlyTheView() {
	namespace room = sf4e::room;
	namespace facade = sf4e::NetplayFacade;
	enum class After { Waiting, RotatedIntoSeat, NextGame };
	for (const auto after : {After::Waiting, After::RotatedIntoSeat, After::NextGame}) {
		WatchedGame game(false, true, after == After::RotatedIntoSeat);
		auto* runtime = game.r.runtime;
		StreamWatchedGame(game);
		game.End(room::MatchResult::P1Win);
		const auto& view = game.authority.SnapshotView();
		if (after == After::RotatedIntoSeat)
			CHECK(room::PlaceOf(view, game.member).kind == room::Place::Kind::Seat);
		if (after == After::NextGame) {
			for (const auto fighter : {game.p1, game.p2}) {
				room::Action ack;
				ack.kind = room::ActionKind::AcknowledgeTerminal; ack.table = 0; ack.matchGeneration = game.generation;
				ack.roomEpoch = view.roomEpoch; ack.revision = view.revision; ack.tableRevision = view.tables[0].revision;
				ack.actionId = fighter * 1000 + view.revision + 1;
				CHECK(game.authority.Apply(fighter, ack).accepted);
			}
			for (const auto fighter : {game.p1, game.p2}) CHECK(game.Act(fighter, room::ActionKind::Ready));
			CHECK(game.authority.BeginMatch(0, game.p1, game.p2).accepted);
			CHECK(view.tables[0].phase == room::TablePhase::Playing && view.tables[0].matchGeneration > game.generation);
		} else CHECK(view.tables[0].phase != room::TablePhase::Playing);
		game.r.Snapshot(game.authority.SnapshotFor(game.member));
		facade::ClearMatchNotice();
		const auto sentBefore = game.r.transport->sent.size();
		SubmitStopWatching(game);
		facade::internal::DrainCommands(true);
		CheckLeftTheStream();
		CHECK(SentActions(game, room::ActionKind::Unwatch, sentBefore).empty());
		CHECK(!runtime->spectatorLockRelease.Pending() && !runtime->teardown.pendingAbort);
		facade::ClearMatchNotice();
	}
	std::cout << "TestFinishedWatchEndsOnlyTheView passed\n";
}

// Stop watching is queued, and the stream retires before the game thread
// takes it (the spectator drain ran out, or the stream failed), whether the
// room still plays the game or has finished it. The leave finds no view: it
// sends nothing, raises no error and leaves nothing parked.
void TestLeaveAfterTheViewRetired() {
	namespace room = sf4e::room;
	namespace facade = sf4e::NetplayFacade;
	for (const bool finished : {false, true}) {
		WatchedGame game(false);
		auto* runtime = game.r.runtime;
		StreamWatchedGame(game);
		if (finished) {
			game.End(room::MatchResult::P1Win);
			game.r.Snapshot(game.authority.SnapshotFor(game.member));
			CHECK(sf4e::Game::Battle::System::ggpo);
		}
		facade::ClearMatchNotice();
		runtime->error.clear();
		const auto sentBefore = game.r.transport->sent.size();
		SubmitStopWatching(game);
		facade::internal::RetireFinishedMatch("test_drain");
		CHECK(!sf4e::Game::Battle::System::ggpo && !facade::internal::WatchedGameNow().Active());
		facade::internal::DrainCommands(true);
		facade::internal::ResolvePendingIntents(true);
		CHECK(SentActions(game, room::ActionKind::Unwatch, sentBefore).empty());
		CHECK(runtime->error.empty() && !runtime->roomActionIntent.Parked());
		CHECK(std::string(facade::GetStatus().lastError).empty());
		facade::ClearMatchNotice();
	}
	std::cout << "TestLeaveAfterTheViewRetired passed\n";
}

// A spectator away from the main menu when its game starts sits it out once
// P1 stops waiting, and is told it watches the next one: a passing note.
void TestSpectatorSitOutNotice() {
	namespace facade = sf4e::NetplayFacade;
	WatchedGame game(false);
	auto* runtime = game.r.runtime;
	// A match the game can take, so its entry only waits for the main menu.
	auto& match = game.r.Client()._matchData;
	match.stageID = 0;
	for (auto& chara : match.chara) { chara.charaID = 0; chara.ultraCombo = 0; chara.unc_edition = 14; }
	game.r.room->LinkUp(game.generation);
	game.Tick();
	game.r.Deliver(json{{"type", "game_start"}, {"generation", game.generation}});
	game.Tick();
	CHECK(game.r.Match().GetPhase() == Phase::Started && !runtime->entry.matchEntered);
	// The entry has waited for the main menu since before P1's deadline.
	runtime->entry.entryDeferredGeneration = game.generation;
	runtime->entry.entryDeferredSinceMs = GetTickCount64() - sf4e::SpectatorPolicy::SyncDeadlineMs;
	facade::ClearMatchNotice();
	facade::internal::TickMatch();
	const auto status = facade::GetStatus();
	CHECK(game.r.Match().GetPhase() != Phase::Started);
	CHECK(std::string(status.lastError) == sf4e::loc::T("runtime.spectator_sat_out") && status.lastErrorSeverity == sf4e::NoticeSeverity::Info);
	facade::ClearMatchNotice();
	std::cout << "TestSpectatorSitOutNotice passed\n";
}

// What an entered room match arms for its battle (its seed and Training rules)
// reaches no later battle when the entry ends before the game prepares one:
// the netplay session shut down, the game left, or the entry reset.
void TestEnteredBattleRequestCancelled() {
	namespace facade = sf4e::NetplayFacade;
	auto& armed = sf4e::GameEvents::VsBattle::nextRequest;
	const auto arm = [&] { armed = sf4e::battle::NextRequest::ForMatch(true, 1234); };
	// What the next battle the game prepares would take.
	const auto prepared = [&] { const auto next = armed.Take(); return next.training || next.overrideSeed; };
	{
		WatchedGame game(true);
		arm();
		sf4e::NetplayFacade::internal::AbortLocalMatch("");
		CHECK(!prepared());
	}
	{
		// This PC's setup of the game fails in TickMatch: cancelled at once,
		// before its session is retired or the game is back at the main menu.
		WatchedGame game(true, false);
		arm();
		sf4e::NetplayFacade::internal::TickMatch();
		CHECK(game.r.runtime->recoveringMatch && game.r.Match().LastFailure() == "local_port_unavailable");
		CHECK(!prepared());
		game.r.Match().FailPortReservationForTest(false);
	}
	{
		// The player leaves a coordinated room during the game: the room is
		// left after the session retires, but the game is cancelled at once.
		WatchedGame game(true);
		auto* runtime = game.r.runtime;
		game.r.room->Coordinate();
		runtime->room = game.r.room;
		arm();
		sf4e::NetplayFacade::RuntimeCommand leave;
		leave.command = {sf4e::netplay::CommandKind::LeaveRoom, runtime->controller.GetSnapshot().generation, {}};
		CHECK(facade::SubmitRuntimeCommand(std::move(leave)));
		sf4e::NetplayFacade::internal::DrainCommands(true);
		CHECK(runtime->leave.leaveRequested && game.r.room->leaveCalls == 0);
		CHECK(!prepared());
		runtime->room.reset();
		runtime->leave.leaveRequested = false;
	}
	{
		WatchedGame game(true);
		arm();
		sf4e::NetplayFacade::internal::ResetMatchEntry();
		CHECK(!prepared());
	}
	{
		// Closing the room shuts netplay down (room close, failure, Start offline).
		WatchedGame game(true);
		arm();
		sf4e::NetplayFacade::internal::CloseRoom();
		CHECK(!prepared());
	}
	arm();
	facade::ShutdownNetplay(true);
	CHECK(!prepared());
	// An entry the game prepares keeps it for that battle alone.
	arm();
	CHECK(prepared() && !prepared());
	std::cout << "TestEnteredBattleRequestCancelled passed\n";
}

// The gates a press is judged by now (CurrentGates) are the ones Publish shows,
// computed in one place, when nothing has moved in between.
void TestGatesMatchPublished() {
	namespace facade = sf4e::NetplayFacade;
	const auto same = [] {
		facade::internal::Publish();
		const auto gates = facade::internal::CurrentGates();
		const auto published = facade::GetRuntimeSnapshotShared();
		CHECK(gates.canEditPreferences == published->canEditPreferences && gates.canEditSelection == published->canEditSelection);
		CHECK(gates.canEditLobby == published->canEditLobby && gates.canChangeController == published->canChangeController);
		CHECK(gates.readyGate == published->readyGate && gates.canReady == published->canReady);
		CHECK(gates.canOpenRoom == published->canOpenRoom && gates.canTrain == published->canTrain);
		CHECK(gates.discordCanSwitch == published->discordCanSwitch && gates.delayLocked == published->delayLocked);
		CHECK(gates.canProbe == published->canProbe && gates.canApplyDelay == published->canApplyDelay);
		CHECK(gates.localSlot == published->localSlot && gates.selectedDelay == published->selectedDelay);
		CHECK(gates.editionSelect == published->lobbySettings.editionSelect);
	};
	{
		RuntimeRoom r;
		same();
		r.Projection(1);
		same();
	}
	std::cout << "TestGatesMatchPublished passed\n";
}

// A Ready parked by hand, as Dispatch parks one, for the controller's generation now.
static void ParkReady(sf4e::NetplayFacade::internal::Runtime& runtime) {
	namespace internal = sf4e::NetplayFacade::internal;
	internal::commands::Room ready;
	ready.command = {sf4e::netplay::CommandKind::Ready, runtime.controller.GetSnapshot().generation, {}};
	runtime.readyIntent.Defer(internal::TypedCommand(ready), ready.command.generation, GetTickCount64(),
		internal::Intent::Budget::Timed);
	CHECK(runtime.readyIntent.Parked());
}

// Decisions read the gates when they decide, not as the last Publish left them.
// A parked Ready whose seat is gone this tick is cleared quietly by this tick's
// ResolvePendingIntents, though the published snapshot still shows the seat.
// And within one DrainCommands batch a command sees what an earlier one did: an
// Unready clears the parked Ready, so a controller change pressed after it in
// the same batch is taken, though the published gate is still closed.
void TestGatesAreCurrent() {
	namespace facade = sf4e::NetplayFacade;
	namespace internal = sf4e::NetplayFacade::internal;
	{
		RuntimeRoom r;
		auto& runtime = *r.runtime;
		// Still seated: the parked press stays.
		r.Projection(1, 0);
		ParkReady(runtime);
		runtime.error.clear();
		internal::ResolvePendingIntents(true);
		CHECK(runtime.readyIntent.Parked() && runtime.error.empty());
		runtime.readyIntent.Clear();
		// Seated when published, then moved to the spectator slot this tick.
		internal::Publish();
		CHECK(facade::GetRuntimeSnapshotShared()->localSlot == 0);
		r.Projection(1, 2);
		ParkReady(runtime);
		internal::ResolvePendingIntents(true);
		CHECK(facade::GetRuntimeSnapshotShared()->localSlot == 0);
		CHECK(!runtime.readyIntent.Active() && runtime.error.empty());
	}
	{
		RuntimeRoom r;
		auto& runtime = *r.runtime;
		r.Projection(1, 0);
		// A controller change (an input action), or an Unready room action.
		const auto submit = [&](bool unready) {
			facade::RuntimeCommand command;
			command.command = {sf4e::netplay::CommandKind::RoomAction, runtime.controller.GetSnapshot().generation, {}};
			if (unready) command.roomAction.kind = sf4e::room::ActionKind::Unready;
			else command.inputAction = sf4e::input::Action::BeginCapture;
			CHECK(facade::SubmitRuntimeCommand(std::move(command)));
		};
		// The last Publish shows the gate closed (it ran away from the main menu).
		internal::Publish();
		CHECK(!facade::GetRuntimeSnapshotShared()->canChangeController);
		// At the main menu from here on, with no Publish: Publish at the main
		// menu reads the game's pads, which this process does not have.
		internal::SetAtMainMenuForTest(true);
		CHECK(internal::CurrentGates().canChangeController);
		// A parked Ready closes the gate: a controller change alone is refused.
		ParkReady(runtime);
		CHECK(!internal::CurrentGates().canChangeController);
		submit(false);
		internal::DrainCommands(true);
		CHECK(runtime.input.State() == sf4e::input::Capture::Idle && runtime.readyIntent.Parked());
		// Unready, then a controller change, in one batch with no Publish between.
		submit(true);
		submit(false);
		internal::DrainCommands(true);
		CHECK(!runtime.readyIntent.Active());
		CHECK(!facade::GetRuntimeSnapshotShared()->canChangeController);
		CHECK(runtime.input.State() == sf4e::input::Capture::ReleaseAll);
		runtime.input.Cancel();
		internal::SetAtMainMenuForTest(std::nullopt);
	}
	std::cout << "TestGatesAreCurrent passed\n";
}

// A seated fighter's Unready while the runtime still holds its Ready (parked
// behind the last game's close) withdraws the press and sends the room nothing.
void TestUnreadyWithdrawsAParkedReady() {
	namespace room = sf4e::room;
	namespace facade = sf4e::NetplayFacade;
	room::RoomAuthority authority("Held ready", 16, 96);
	const auto join = [&](int index, bool host) {
		const auto result = authority.Join("Player" + std::to_string(index), room::ConnectionRef{"host", std::to_string(index)}, host);
		CHECK(result.accepted);
		return result.snapshot.members.back().id;
	};
	join(0, true);
	const auto fighter = join(1, false), opponent = join(2, false);
	for (const auto who : {fighter, opponent}) {
		const auto& view = authority.SnapshotView();
		room::Action queue;
		queue.kind = room::ActionKind::Queue; queue.roomEpoch = view.roomEpoch; queue.revision = view.revision;
		queue.table = 0; queue.tableRevision = view.tables[0].revision; queue.actionId = who * 1000 + view.revision + 1;
		CHECK(authority.Apply(who, queue).accepted);
	}
	RuntimeRoom r;
	r.Snapshot(authority.SnapshotFor(fighter));
	CHECK(room::PlaceOf(r.Client().GetRoomSnapshot(), fighter).kind == room::Place::Kind::Seat);
	auto* runtime = r.runtime;
	const auto generation = runtime->controller.GetSnapshot().generation;
	ParkReady(*runtime);
	const auto sentBefore = r.transport->sent.size();
	facade::RuntimeCommand unready{};
	unready.command = {sf4e::netplay::CommandKind::RoomAction, generation, {}};
	unready.roomAction.kind = room::ActionKind::Unready; unready.roomAction.table = 0; unready.roomAction.seat = 0;
	CHECK(facade::SubmitRuntimeCommand(std::move(unready)));
	facade::internal::DrainCommands(true);
	CHECK(!runtime->readyIntent.Active());
	for (std::size_t i = sentBefore; i < r.transport->sent.size(); ++i)
		CHECK(r.transport->sent[i].value("type", json()) != json(protocol::MT_ROOM_ACTION));
	std::cout << "TestUnreadyWithdrawsAParkedReady passed\n";
}
// A watcher away from the main menu, in a joined room and fighting no game
// here, saves the settings the room leaves open the way the shell sends them
// (ShellView::CanEditRoomSettings, netplay::RoomSettingsOpen): the runtime takes
// what was sent, all of it, publishes it, which is what the shell's
// UpdatePreferenceSave waits to see, and the writer saves it. A button layout
// itself changes at the main menu only (CanEditButtonLayoutNow).
void TestWatcherSavesRoomSettings() {
	namespace facade = sf4e::NetplayFacade;
	namespace internal = facade::internal;
	namespace prefs = sf4e::OverlayPrefs;
	const auto directory = std::filesystem::temp_directory_path() /
		(L"sf4e-room-settings-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
	std::filesystem::create_directories(directory);
	prefs::UseDirectoryForTest(directory.wstring());
	prefs::StartPersistence();
	{
		RuntimeRoom r;
		auto& runtime = *r.runtime;
		r.Projection(1);
		internal::SetAtMainMenuForTest(false);
		const auto session = runtime.controller.GetSnapshot();
		CHECK(session.room == netplay::RoomState::Joined && !netplay::Fighting(session.match));
		CHECK(!internal::CurrentGates().canEditPreferences);
		auto sent = runtime.preferences;
		sent.roomColumn = sent.roomColumn == netplay::MaxRoomColumn ? netplay::MinRoomColumn : netplay::MaxRoomColumn;
		sent.roomShowChat = !sent.roomShowChat;
		const gamepads::Identity pad{"guid", "serial", "path", "Pad"};
		sent.buttonLayouts.push_back({pad, gamepads::DefaultLayout});
		facade::RuntimeCommand save;
		save.command = {netplay::CommandKind::SavePreferences, session.generation, {}};
		save.roomSettings = true; save.preferences = sent;
		runtime.error.clear();
		CHECK(facade::SubmitRuntimeCommand(std::move(save)));
		internal::DrainCommands(true);
		CHECK_MSG(runtime.error.empty(), "%s", runtime.error.c_str());
		CHECK(runtime.preferences.roomColumn == sent.roomColumn && runtime.preferences.roomShowChat == sent.roomShowChat);
		CHECK(runtime.preferences.buttonLayouts == sent.buttonLayouts);
		internal::Publish();
		const auto published = facade::GetRuntimeSnapshotShared();
		CHECK(published->preferences.roomColumn == sent.roomColumn && published->preferences.roomShowChat == sent.roomShowChat);
		CHECK(published->preferences.buttonLayouts == sent.buttonLayouts);
		const auto deadline = GetTickCount64() + 5000;
		while (prefs::PersistencePending() && GetTickCount64() < deadline) Sleep(10);
		CHECK(!prefs::PersistencePending() && prefs::PersistenceError().empty());
		std::ifstream file(directory / L"settings.json", std::ios::binary);
		const auto saved = json::parse(file, nullptr, false);
		CHECK(!saved.is_discarded() && saved.dump().find("\"roomColumn\":" + std::to_string(sent.roomColumn)) != std::string::npos);
		// The pad's own layout edit: refused here, taken at the main menu.
		gamepads::PadView served; served.id = 7; served.identity = pad; served.slot = 0;
		runtime.pads = {served}; runtime.padLayer = gamepads::Layer::Active;
		auto edited = gamepads::DefaultLayout; std::swap(edited[0], edited[1]);
		const auto edit = [&] {
			facade::RuntimeCommand layout;
			layout.command = {netplay::CommandKind::SavePreferences, session.generation, {}};
			layout.padAction.kind = gamepads::ControllerRequest::Kind::Layout; layout.padAction.id = served.id; layout.padAction.buttons = edited;
			CHECK(facade::SubmitRuntimeCommand(std::move(layout)));
			internal::DrainCommands(true);
		};
		edit();
		CHECK(runtime.preferences.buttonLayouts == sent.buttonLayouts);
		internal::SetAtMainMenuForTest(true);
		edit();
		const auto taken = std::find_if(runtime.preferences.buttonLayouts.begin(), runtime.preferences.buttonLayouts.end(),
			[&](const gamepads::StoredLayout& layout) { return layout.identity == pad; });
		CHECK(taken != runtime.preferences.buttonLayouts.end() && taken->buttons == edited);
		internal::SetAtMainMenuForTest(std::nullopt);
	}
	prefs::StopPersistence();
	prefs::UseDirectoryForTest({});
	std::error_code ignored;
	std::filesystem::remove_all(directory, ignored);
	std::cout << "TestWatcherSavesRoomSettings passed\n";
}
}
int main() {
	for (const bool queued : {false, true})
		for (const bool staged : {false, true}) TestFailedSetupThroughRuntime(queued, staged, false);
	TestFailedSetupThroughRuntime(false, false, true);
	TestUnscoredEndNotice();
	TestSpectatorLeavesWatchedGame();
	TestQueuedWatcherLeavesTheGame();
	TestFailedViewCountsLockInDrops();
	TestTableChangeLeavesWithoutFailure();
	TestLeaveAfterTheViewRetired();
	TestParkedStopWatchingCatchesUp();
	TestFinishedWatchEndsOnlyTheView();
	TestSpectatorSitOutNotice();
	TestEnteredBattleRequestCancelled();
	TestGatesMatchPublished();
	TestGatesAreCurrent();
	TestUnreadyWithdrawsAParkedReady();
	TestWatcherSavesRoomSettings();
	std::cout << "Spectator runtime tests passed\n";
	return 0;
}
