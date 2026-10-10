// The drawing thread reads netplay state only through the published
// PresentationSnapshot. rc4 let it call GetStatus, which expired and copied the
// game thread's notice string while that thread replaced it; a two-thread run
// of that shape ended in 0xC0000374 within a fraction of a second. This runs
// the same hammer through the publication path, and pins the notice rules and
// the command queue's lifetime across a runtime restart.
#include "../sf4e/sf4e__NetplayFacade.hxx"
#include "../sf4e/sf4e__RuntimeBridge.hxx"
#include "../sf4e/sf4e__GameEvents.hxx"
#include "../Dimps/Dimps.hxx"
#include "../common/MatchNotice.hxx"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <deque>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include "test_support.hxx"

namespace facade = sf4e::NetplayFacade;
namespace bridge = sf4e::NetplayFacade::bridge;
using sf4e::MatchNoticeState;
using sf4e::NoticeSeverity;

static void TestNoticeRules() {
	MatchNoticeState notice;
	notice.Push("", NoticeSeverity::Error, 0);
	notice.Push(nullptr, NoticeSeverity::Error, 0);
	CHECK(notice.Empty());

	notice.Push("Connection restored.", NoticeSeverity::Info, 1000);
	notice.Expire(1000 + MatchNoticeState::InfoMs - 1);
	CHECK(!notice.Empty());
	notice.Expire(1000 + MatchNoticeState::InfoMs);
	CHECK(notice.Empty());

	notice.Push("Connection unstable.", NoticeSeverity::Warning, 1000);
	notice.Expire(1000 + MatchNoticeState::WarningMs - 1);
	CHECK(!notice.Empty());
	notice.Expire(1000 + MatchNoticeState::WarningMs);
	CHECK(notice.Empty());

	// A clock read taken before the notice was pushed is not an age.
	notice.Push("Connection restored.", NoticeSeverity::Info, 5000);
	notice.Expire(4999);
	CHECK(std::strcmp(notice.Text(), "Connection restored.") == 0);

	// Errors never expire and survive a transient clear; a full clear or a newer notice ends them.
	notice.Push("The room was lost.", NoticeSeverity::Error, 0);
	notice.Expire(~0ull);
	notice.ClearTransient();
	CHECK(notice.Severity() == NoticeSeverity::Error && std::strcmp(notice.Text(), "The room was lost.") == 0);
	notice.Push("Returning to the room.", NoticeSeverity::Info, 0);
	CHECK(notice.Severity() == NoticeSeverity::Info);
	notice.ClearTransient();
	CHECK(notice.Empty());

	// Long text keeps 255 bytes at most and never splits a UTF-8 character.
	std::string longText(254, 'a');
	longText += "\xc3\xa9\xc3\xa9";
	notice.Push(longText.c_str(), NoticeSeverity::Warning, 0);
	CHECK(std::strlen(notice.Text()) == 254);
	std::string ascii(300, 'b');
	notice.Push(ascii.c_str(), NoticeSeverity::Warning, 0);
	CHECK(std::strlen(notice.Text()) == 255);
}

static void TestPublishedFrameIsACopy() {
	facade::ClearMatchNotice();
	facade::PushAlert("Connection unstable.", NoticeSeverity::Warning);
	facade::PublishPresentationFrame();
	const auto held = facade::GetPresentationSnapshotShared();
	CHECK(held && held->runtime);
	CHECK(std::strcmp(held->netplay.lastError, "Connection unstable.") == 0);
	CHECK(held->netplay.lastErrorSeverity == NoticeSeverity::Warning);
	CHECK(!held->ggpoSessionActive);

	facade::PushAlert("The room was lost.", NoticeSeverity::Error);
	facade::ClearTransientMatchNotice();
	facade::PublishPresentationFrame();
	const auto next = facade::GetPresentationSnapshotShared();
	CHECK(next->sequence == held->sequence + 1);
	CHECK(std::strcmp(next->netplay.lastError, "The room was lost.") == 0);
	// The frame a reader kept is untouched by later ticks.
	CHECK(std::strcmp(held->netplay.lastError, "Connection unstable.") == 0);
	// Each tick republishes the envelope; the room view it points at is shared.
	CHECK(next->runtime == held->runtime);
	facade::ClearMatchNotice();
}

static void TestTwoThreads() {
	const char* texts[] = {
		"Connection unstable. Waiting for the other player to catch up.",
		"Connexion r\xc3\xa9tablie apr\xc3\xa8s une interruption du r\xc3\xa9seau, la partie continue.",
	};
	std::atomic<bool> stop{false};
	std::atomic<unsigned long long> frames{0};
	std::thread draw([&] {
		while (!stop) {
			const auto frame = facade::GetPresentationSnapshotShared();
			const char* shown = frame->netplay.lastError;
			CHECK(!shown[0] || std::strcmp(shown, texts[0]) == 0 || std::strcmp(shown, texts[1]) == 0);
			CHECK(frame->runtime != nullptr);
			++frames;
		}
	});
	unsigned i = 0;
	const ULONGLONG end = GetTickCount64() + 3000;
	while (GetTickCount64() < end) {
		facade::PushAlert(texts[i++ & 1], NoticeSeverity::Info);
		if ((i & 7) == 0) facade::ClearTransientMatchNotice();
		facade::PublishPresentationFrame();
	}
	stop = true;
	draw.join();
	CHECK(frames > 0);
	facade::ClearMatchNotice();
}

static std::size_t Drain(bridge::CommandMailbox& mailbox) {
	facade::RuntimeCommand command;
	std::size_t count = 0;
	while (mailbox.TryPop(command)) ++count;
	return count;
}

static facade::RuntimeCommand Leave() {
	facade::RuntimeCommand command;
	command.command.kind = sf4e::netplay::CommandKind::LeaveRoom;
	return command;
}

static void TestCommandLifetime() {
	// No runtime: nothing accepts commands.
	CHECK(!facade::SubmitRuntimeCommand(Leave()));

	auto first = std::make_shared<bridge::CommandMailbox>(32, 128 * 1024);
	bridge::OpenCommands(first);
	CHECK(facade::SubmitRuntimeCommand(Leave()));
	CHECK(Drain(*first) == 1);
	// Malformed commands are refused before the queue.
	auto oversized = Leave();
	oversized.displayName.assign(sf4e::NETPLAY_DISPLAY_NAME_LEN, 'x');
	CHECK(!facade::SubmitRuntimeCommand(std::move(oversized)));

	// Producers race a stop and a restart. A closed queue takes nothing more,
	// and nothing meant for it reaches its successor.
	std::atomic<bool> stop{false};
	std::vector<std::thread> producers;
	for (int t = 0; t < 3; ++t)
		producers.emplace_back([&] { while (!stop) facade::SubmitRuntimeCommand(Leave()); });
	Sleep(50);
	bridge::CloseCommands();
	CHECK(!first->TryPush(Leave(), sizeof(facade::RuntimeCommand)));
	auto second = std::make_shared<bridge::CommandMailbox>(32, 128 * 1024);
	bridge::OpenCommands(second);
	for (int round = 0; round < 200; ++round) Drain(*second);
	stop = true;
	for (auto& producer : producers) producer.join();
	CHECK(Drain(*first) == 0);
	bridge::Reset();
	CHECK(!facade::SubmitRuntimeCommand(Leave()));
	CHECK(bridge::LatestRuntime() && facade::GetPresentationSnapshotShared()->runtime);
}

// The menu takes the pending request once: judged now, forgotten either way, and
// a newer one posted while the older one is judged is left for the next look.
static void TestTrainingRequestTake() {
	using Request = sf4e::GameEvents::TrainingRequest;
	using sf4e::netplay::Generation;
	Request request;
	Request::Pending seen;
	const auto holds = [](bool answer) { return [answer](const Generation&) { return answer; }; };
	CHECK(request.Take(100, holds(true)) == Request::Taken::None);
	request.Post(Generation{1, 0}, 100, 2000);
	CHECK(request.Take(150, holds(false)) == Request::Taken::Dropped && !request.Peek(150, seen));
	request.Post(Generation{1, 0}, 200, 2000);
	CHECK(request.Take(250, holds(true)) == Request::Taken::Go && !request.Peek(250, seen));
	CHECK(request.Take(260, holds(true)) == Request::Taken::None);
	request.Post(Generation{1, 0}, 300, 2000);
	unsigned long long newer = 0;
	CHECK(request.Take(350, [&](const Generation&) { newer = request.Post(Generation{2, 0}, 350, 2000); return true; }) == Request::Taken::None);
	CHECK(request.Peek(360, seen) && seen.serial == newer && seen.generation == (Generation{2, 0}));
	CHECK(request.Take(3000, holds(true)) == Request::Taken::None);
}

// A joined, healthy session in `made`, the local member alone at table 0 or with
// an opponent sat opposite.
static sf4e::netplay::Snapshot JoinedSession(const sf4e::netplay::Generation& made) {
	sf4e::netplay::Snapshot session;
	session.generation = made;
	session.room = sf4e::netplay::RoomState::Joined;
	session.control = sf4e::netplay::Health::Healthy;
	return session;
}
static sf4e::room::Snapshot TableRoom(bool opponent) {
	sf4e::room::Snapshot room;
	room.roomEpoch = 3;
	room.localMember = 1;
	room.tables[0].p1 = 1;
	room.tables[0].p2 = opponent ? 2 : 0;
	room.tables[0].phase = sf4e::room::TablePhase::Waiting;
	return room;
}

// The room's own authority decides whom a game takes in. A queued member its
// roster takes in is refused Training; one it leaves out, because a receipt of
// theirs from the last game was still open when it began, stays free to train
// through that game, also once they acknowledge that receipt.
static void TestTrainingRoster(const sf4e::netplay::Snapshot& session, const sf4e::netplay::Generation& made) {
	namespace room = sf4e::room;
	using sf4e::TrainingEntry;
	using sf4e::netplay::Generation;
	using Taken = sf4e::GameEvents::TrainingRequest::Taken;
	auto& record = sf4e::GameEvents::MainMenu::trainingRequest;
	sf4e::GameEvents::TrainingRequest::Pending seen;
	const auto none = [&] { return !record.Peek(GetTickCount64(), seen); };
	room::RoomAuthority authority("Training roster", 8, 77);
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
	const auto p1 = join(1, false), p2 = join(2, false), queuer = join(3, false);
	for (const auto member : {p1, p2, queuer}) CHECK(act(member, room::ActionKind::Queue, 0));
	const auto begin = [&] {
		for (const auto member : {p1, p2}) CHECK(act(member, room::ActionKind::Ready, 0));
		CHECK(authority.BeginMatch(0, p1, p2).accepted);
		return authority.SnapshotView().tables[0].matchGeneration;
	};
	room::Snapshot live = authority.SnapshotFor(queuer);
	const auto judged = [&](const Generation& generation) {
		return facade::TrainingHolds(TrainingEntry::Room, generation, session, true, live, 0);
	};
	// Queued while the table waits: eligible. The game starts and its roster
	// takes the queued member in: the accepted request is dropped.
	CHECK(live.localMatchGenerationsSent && judged(made));
	sf4e::GameEvents::MainMenu::RequestTraining(made);
	const auto first = begin();
	CHECK((authority.MatchRoster(0) == std::vector<room::MemberId>{p1, p2, queuer}));
	live = authority.SnapshotFor(queuer);
	CHECK(live.localMatchGenerationsSent && live.localMatchGenerations[0] == first);
	CHECK(record.Take(GetTickCount64(), judged) == Taken::Dropped && none());
	// The game ends; the fighters acknowledge it, the queued member not yet.
	CHECK(authority.EndMatch(0, first, room::MatchResult::P1Win).accepted);
	CHECK(act(p1, room::ActionKind::AcknowledgeTerminal, first) && act(p2, room::ActionKind::AcknowledgeTerminal, first));
	// The next game leaves them out of its roster, though the lists still show
	// them; then they acknowledge, with the game still being played.
	const auto second = begin();
	CHECK((authority.MatchRoster(0) == std::vector<room::MemberId>{p1, p2}));
	CHECK(act(queuer, room::ActionKind::AcknowledgeTerminal, first));
	live = authority.SnapshotFor(queuer);
	const auto& table = live.tables[0];
	CHECK(table.phase == room::TablePhase::Playing && table.matchGeneration == second);
	CHECK(std::find(table.queue.begin(), table.queue.end(), queuer) != table.queue.end() &&
		std::find(table.spectators.begin(), table.spectators.end(), queuer) != table.spectators.end());
	CHECK(live.localMatchGenerationsSent && live.localMatchGenerations[0] == 0);
	// Free to train through this game: the request is taken once.
	CHECK(judged(made));
	sf4e::GameEvents::MainMenu::RequestTraining(made);
	CHECK(record.Take(GetTickCount64(), judged) == Taken::Go && none());
	CHECK(record.Take(GetTickCount64(), judged) == Taken::None);
	// The same snapshot from a host that does not say falls back to the lists.
	auto older = live;
	older.localMatchGenerationsSent = false;
	CHECK(!facade::TrainingHolds(TrainingEntry::Room, made, session, true, older, 0));

	// A spectator by choice, taken in by the third game: refused while they
	// watch. They stop watching with the game still being played: the roster
	// still holds them (it owes them the game's end) but they watch nothing, and
	// once their side is torn down (PostMatch) they are free to train.
	const auto watcher = join(4, false);
	CHECK(act(queuer, room::ActionKind::Unqueue, 0));
	CHECK(authority.EndMatch(0, second, room::MatchResult::P2Win).accepted);
	CHECK(act(p1, room::ActionKind::AcknowledgeTerminal, second) && act(p2, room::ActionKind::AcknowledgeTerminal, second));
	CHECK(act(watcher, room::ActionKind::Watch, 0));
	const auto third = begin();
	CHECK((authority.MatchRoster(0) == std::vector<room::MemberId>{p1, p2, watcher}));
	const auto watcherJudged = [&](const sf4e::netplay::Snapshot& state) {
		return [&, state](const Generation& generation) {
			return facade::TrainingHolds(TrainingEntry::Room, generation, state, true, live, 0);
		};
	};
	live = authority.SnapshotFor(watcher);
	CHECK(room::FindMember(live, watcher)->status == room::MemberStatus::Watching);
	CHECK(!facade::TrainingHolds(TrainingEntry::Room, made, session, true, live, 0));
	sf4e::GameEvents::MainMenu::RequestTraining(made);
	CHECK(record.Take(GetTickCount64(), watcherJudged(session)) == Taken::Dropped && none());
	CHECK(act(watcher, room::ActionKind::Unwatch, 0));
	CHECK(authority.SnapshotView().tables[0].phase == room::TablePhase::Playing && authority.SnapshotView().tables[0].matchGeneration == third);
	CHECK((authority.MatchRoster(0) == std::vector<room::MemberId>{p1, p2, watcher}));
	live = authority.SnapshotFor(watcher);
	CHECK(room::FindMember(live, watcher)->status == room::MemberStatus::Idle && live.localMatchGenerations[0] == third);
	auto tornDown = session;
	tornDown.match = sf4e::netplay::MatchState::PostMatch;
	CHECK(facade::TrainingHolds(TrainingEntry::Room, made, tornDown, true, live, 0));
	sf4e::GameEvents::MainMenu::RequestTraining(made);
	CHECK(record.Take(GetTickCount64(), watcherJudged(tornDown)) == Taken::Go && none());
	CHECK(record.Take(GetTickCount64(), watcherJudged(tornDown)) == Taken::None);
	// They then queue at the same table, its game still being played: queued
	// for the next one, not taken into this one again. Still free to train.
	CHECK(act(watcher, room::ActionKind::Queue, 0));
	live = authority.SnapshotFor(watcher);
	const auto& requeued = live.tables[0];
	CHECK(room::FindMember(live, watcher)->status == room::MemberStatus::Queued);
	CHECK(requeued.phase == room::TablePhase::Playing && requeued.matchGeneration == third);
	CHECK(std::find(requeued.queue.begin(), requeued.queue.end(), watcher) != requeued.queue.end() &&
		std::find(requeued.spectators.begin(), requeued.spectators.end(), watcher) == requeued.spectators.end());
	CHECK(live.localMatchGenerations[0] == third);
	CHECK(facade::TrainingHolds(TrainingEntry::Room, made, tornDown, true, live, 0));
	sf4e::GameEvents::MainMenu::RequestTraining(made);
	CHECK(record.Take(GetTickCount64(), watcherJudged(tornDown)) == Taken::Go && none());
	CHECK(record.Take(GetTickCount64(), watcherJudged(tornDown)) == Taken::None);
	// Still in the game's teardown (the controller not yet back): no Training.
	auto tearingDown = session;
	tearingDown.match = sf4e::netplay::MatchState::Playing;
	CHECK(!facade::TrainingHolds(TrainingEntry::Room, made, tearingDown, true, live, 0));
}
// One state a member can be in, with the session it is judged in and the game
// this PC has retired (0 for none), and whether Training holds there.
struct TrainingRow { const char* name; sf4e::room::Snapshot room; sf4e::netplay::Snapshot session; std::uint64_t retired; bool holds; };
// Each row: TrainingHolds (which is CanTrain in the session it was asked in)
// gives the row's answer and never holds in another session; a request
// accepted before it is then taken once (Go, then None) or dropped.
static void CheckTrainingRows(const std::vector<TrainingRow>& rows, const sf4e::netplay::Generation& made) {
	using sf4e::TrainingEntry;
	using sf4e::netplay::Generation;
	using Taken = sf4e::GameEvents::TrainingRequest::Taken;
	auto& record = sf4e::GameEvents::MainMenu::trainingRequest;
	sf4e::GameEvents::TrainingRequest::Pending seen;
	const auto none = [&] { return !record.Peek(GetTickCount64(), seen); };
	if (!none()) record.Consume(seen.serial);
	for (const auto& row : rows) {
		const bool holds = facade::TrainingHolds(TrainingEntry::Room, made, row.session, true, row.room, row.retired);
		if (holds != row.holds) std::fprintf(stderr, "Training matrix row \"%s\": holds=%d, expected %d\n", row.name, holds, row.holds);
		CHECK(holds == row.holds);
		CHECK(!facade::TrainingHolds(TrainingEntry::Room, Generation{made.room + 1, 0}, row.session, true, row.room, row.retired));
		const auto judged = [&](const Generation& generation) {
			return facade::TrainingHolds(TrainingEntry::Room, generation, row.session, true, row.room, row.retired);
		};
		sf4e::GameEvents::MainMenu::RequestTraining(made);
		if (row.holds) {
			CHECK(record.Take(GetTickCount64(), judged) == Taken::Go && none());
			CHECK(record.Take(GetTickCount64(), judged) == Taken::None);
		} else CHECK(record.Take(GetTickCount64(), judged) == Taken::Dropped && none());
	}
}
// Every place a member can hold relative to a table whose game is being played,
// against where this PC's own teardown of that game stands and whether a newer
// game began, built with the room authority's own transitions. Each row says
// whether Training holds (CanTrain is TrainingHolds in the session it was asked
// in); a request accepted before the row's state is then taken once (Go, then
// None) or dropped.
static void TestTrainingMatrix(const sf4e::netplay::Snapshot& joined, const sf4e::netplay::Generation& made) {
	namespace room = sf4e::room;
	using sf4e::TrainingEntry;
	using sf4e::netplay::Generation;
	using sf4e::netplay::MatchState;
	using Taken = sf4e::GameEvents::TrainingRequest::Taken;
	auto& record = sf4e::GameEvents::MainMenu::trainingRequest;
	sf4e::GameEvents::TrainingRequest::Pending seen;
	const auto none = [&] { return !record.Peek(GetTickCount64(), seen); };
	room::RoomAuthority authority("Training matrix", 16, 93);
	const auto join = [&](int index, bool host) {
		const auto result = authority.Join("Player" + std::to_string(index), room::ConnectionRef{"host", std::to_string(index)}, host);
		CHECK(result.accepted);
		return result.snapshot.members.back().id;
	};
	const auto act = [&](room::MemberId member, room::ActionKind kind, std::uint8_t table, std::uint64_t generation, bool keepWatching) {
		const auto& view = authority.SnapshotView();
		room::Action action;
		action.kind = kind; action.roomEpoch = view.roomEpoch; action.revision = view.revision;
		action.table = table; action.tableRevision = view.tables[table].revision;
		action.actionId = member * 1000 + view.revision + 1; action.matchGeneration = generation; action.keepWatching = keepWatching;
		return authority.Apply(member, action).accepted;
	};
	const auto listedAt = [&](const room::Snapshot& snapshot, const std::vector<room::MemberId> room::Table::* list, room::MemberId member) {
		const auto& entries = snapshot.tables[0].*list;
		return std::find(entries.begin(), entries.end(), member) != entries.end();
	};
	join(0, true);
	const auto p1 = join(1, false), p2 = join(2, false);
	const auto watcher = join(3, false), failedWatcher = join(4, false), unwatcher = join(5, false), mover = join(6, false), leaver = join(7, false);
	const auto admittedQueue = join(8, false), failedQueue = join(9, false), unqueued = join(10, false);
	const auto lateQueue = join(11, false), nextWatcher = join(12, false);
	for (const auto member : {p1, p2, admittedQueue, failedQueue, unqueued}) CHECK(act(member, room::ActionKind::Queue, 0, 0, false));
	for (const auto member : {watcher, failedWatcher, unwatcher, mover, leaver}) CHECK(act(member, room::ActionKind::Watch, 0, 0, false));
	for (const auto member : {p1, p2}) CHECK(act(member, room::ActionKind::Ready, 0, 0, false));
	CHECK(authority.BeginMatch(0, p1, p2).accepted);
	const auto game = authority.SnapshotView().tables[0].matchGeneration;
	// Before anything else happens: who the game took in.
	const auto admitted = authority.MatchRoster(0);
	for (const auto member : {watcher, failedWatcher, unwatcher, mover, leaver, admittedQueue, failedQueue, unqueued})
		CHECK(std::find(admitted.begin(), admitted.end(), member) != admitted.end());
	const auto watcherAdmitted = authority.SnapshotFor(watcher);
	// Arrivals after the start wait for the next game.
	CHECK(act(lateQueue, room::ActionKind::Queue, 0, 0, false));
	CHECK(act(nextWatcher, room::ActionKind::Watch, 0, 0, false));
	// Departures while it is being played: a failed stream keeps its place
	// (keepWatching), a plain Unwatch leaves the table, an Unqueue keeps the
	// stream it is watching, a Watch elsewhere moves them, a Leave leaves the room.
	CHECK(act(failedWatcher, room::ActionKind::Unwatch, 0, game, true));
	CHECK(act(failedQueue, room::ActionKind::Unwatch, 0, game, true));
	CHECK(act(unwatcher, room::ActionKind::Unwatch, 0, 0, false));
	CHECK(act(unqueued, room::ActionKind::Unqueue, 0, 0, false));
	CHECK(act(mover, room::ActionKind::Watch, 1, 0, false));
	CHECK(authority.Leave(leaver).accepted);
	CHECK(authority.SnapshotView().tables[0].phase == room::TablePhase::Playing && authority.SnapshotView().tables[0].matchGeneration == game);
	const auto view = [&](room::MemberId member) { return authority.SnapshotFor(member); };
	const auto status = [](const room::Snapshot& snapshot, room::MemberId member) {
		const auto* found = room::FindMember(snapshot, member);
		return found ? found->status : room::MemberStatus::Idle;
	};
	// The room's own account of each place, as the rows below rely on it.
	CHECK(status(view(failedWatcher), failedWatcher) == room::MemberStatus::Watching && listedAt(view(failedWatcher), &room::Table::spectators, failedWatcher));
	CHECK(status(view(failedQueue), failedQueue) == room::MemberStatus::Queued && listedAt(view(failedQueue), &room::Table::spectators, failedQueue));
	CHECK(status(view(unwatcher), unwatcher) == room::MemberStatus::Idle && view(unwatcher).localMatchGenerations[0] == game);
	CHECK(status(view(unqueued), unqueued) == room::MemberStatus::Watching && listedAt(view(unqueued), &room::Table::endingWatchers, unqueued));
	CHECK(status(view(lateQueue), lateQueue) == room::MemberStatus::Queued && !listedAt(view(lateQueue), &room::Table::spectators, lateQueue) &&
		view(lateQueue).localMatchGenerations[0] == 0);
	CHECK(status(view(nextWatcher), nextWatcher) == room::MemberStatus::WatchingNext && view(nextWatcher).localMatchGenerations[0] == 0);
	CHECK(!listedAt(view(mover), &room::Table::spectators, mover) && view(mover).localMatchGenerations[0] == game);
	CHECK(view(leaver).localMember == 0);
	const auto withoutKey = [](room::Snapshot snapshot) { snapshot.localMatchGenerationsSent = false; return snapshot; };
	// Each place as the room shows it while the game is still being played.
	const auto fighter = view(p1), failedWatcherView = view(failedWatcher), unwatcherView = view(unwatcher), moverView = view(mover),
		leaverView = view(leaver), admittedQueueView = view(admittedQueue), failedQueueView = view(failedQueue), unqueuedView = view(unqueued),
		lateQueueView = view(lateQueue), nextWatcherView = view(nextWatcher);
	// A newer game at the table: the failed watcher acknowledged the last one
	// and the room took them in again.
	CHECK(authority.EndMatch(0, game, room::MatchResult::P1Win).accepted);
	for (const auto member : {p1, p2, failedWatcher}) CHECK(act(member, room::ActionKind::AcknowledgeTerminal, 0, game, false));
	for (const auto member : {p1, p2}) CHECK(act(member, room::ActionKind::Ready, 0, 0, false));
	CHECK(authority.BeginMatch(0, p1, p2).accepted);
	const auto next = authority.SnapshotView().tables[0].matchGeneration;
	CHECK(next > game);
	const auto failedWatcherNext = view(failedWatcher);
	CHECK(status(failedWatcherNext, failedWatcher) == room::MemberStatus::Watching && failedWatcherNext.localMatchGenerations[0] == next);

	auto playing = joined; playing.match = MatchState::Playing;  // teardown under way: controller not back
	auto postMatch = joined; postMatch.match = MatchState::PostMatch;
	const std::vector<TrainingRow> rows = {
		{"fighter, in the game", fighter, playing, 0, false},
		{"fighter, controller back", fighter, postMatch, game, false},
		{"spectator admitted, not yet started here", watcherAdmitted, joined, 0, false},
		{"spectator admitted, controller back, native teardown pending", watcherAdmitted, postMatch, 0, false},
		{"spectator admitted, this PC retired the game", watcherAdmitted, postMatch, game, true},
		{"spectator failed (keepWatching), teardown under way", failedWatcherView, playing, 0, false},
		{"spectator failed, controller back, native teardown pending", failedWatcherView, postMatch, 0, false},
		{"spectator failed, retired here", failedWatcherView, postMatch, game, true},
		{"spectator unwatched, teardown under way", unwatcherView, playing, 0, false},
		{"spectator unwatched, retired here", unwatcherView, postMatch, game, true},
		{"spectator moved to table 2, teardown under way", moverView, playing, 0, false},
		{"spectator moved to table 2, retired here", moverView, postMatch, game, true},
		{"spectator left the room", leaverView, postMatch, game, false},
		{"queued admitted, not yet started here", admittedQueueView, joined, 0, false},
		{"queued admitted, controller back, native teardown pending", admittedQueueView, postMatch, 0, false},
		{"queued admitted, retired here", admittedQueueView, postMatch, game, true},
		{"queued failed (keepWatching), native teardown pending", failedQueueView, postMatch, 0, false},
		{"queued failed, retired here", failedQueueView, postMatch, game, true},
		{"queued then unqueued, still streaming", unqueuedView, postMatch, 0, false},
		{"queued then unqueued, retired here", unqueuedView, postMatch, game, true},
		{"queued after the start, not admitted", lateQueueView, joined, 0, true},
		{"watching next game only", nextWatcherView, joined, 0, true},
		{"failed watcher, newer game took them in again", failedWatcherNext, postMatch, game, false},
		{"older host: queued admitted, not yet started here", withoutKey(admittedQueueView), joined, 0, false},
		{"older host: queued admitted, retired here", withoutKey(admittedQueueView), postMatch, game, true},
		{"older host: queued after the start", withoutKey(lateQueueView), joined, 0, true},
		{"older host: spectator failed, retired here", withoutKey(failedWatcherView), postMatch, game, true},
	};
	CheckTrainingRows(rows, made);
}
// A game paused over conflicting results is not over: its watchers are still
// in it until this PC retires that exact game, for a by-choice spectator and a
// queued one the start took in, from a host that sends its roster generations
// and from one that does not. A member the game left out (an open receipt) and
// one queued after its start are free throughout.
static void TestTrainingPausedMatrix(const sf4e::netplay::Snapshot& joined, const sf4e::netplay::Generation& made) {
	namespace room = sf4e::room;
	using sf4e::netplay::MatchState;
	room::RoomAuthority authority("Paused matrix", 16, 94);
	const auto join = [&](int index, bool host) {
		const auto result = authority.Join("Player" + std::to_string(index), room::ConnectionRef{"host", std::to_string(index)}, host);
		CHECK(result.accepted);
		return result.snapshot.members.back().id;
	};
	const auto action = [&](room::ActionKind kind, std::uint64_t generation) {
		const auto& view = authority.SnapshotView();
		room::Action value;
		value.kind = kind; value.roomEpoch = view.roomEpoch; value.revision = view.revision;
		value.table = 0; value.tableRevision = view.tables[0].revision; value.matchGeneration = generation;
		return value;
	};
	const auto act = [&](room::MemberId member, room::ActionKind kind, std::uint64_t generation) {
		auto value = action(kind, generation);
		value.actionId = member * 1000 + authority.SnapshotView().revision + 1;
		return authority.Apply(member, value).accepted;
	};
	join(0, true);
	const auto p1 = join(1, false), p2 = join(2, false), watcher = join(3, false), queued = join(4, false), excluded = join(5, false);
	const auto late = join(6, false), leaving = join(7, false), arrival = join(8, false);
	for (const auto member : {p1, p2, excluded}) CHECK(act(member, room::ActionKind::Queue, 0));
	const auto begin = [&] {
		for (const auto member : {p1, p2}) CHECK(act(member, room::ActionKind::Ready, 0));
		CHECK(authority.BeginMatch(0, p1, p2).accepted);
		return authority.SnapshotView().tables[0].matchGeneration;
	};
	// An earlier game leaves the excluded member owing its receipt.
	const auto earlier = begin();
	CHECK(authority.EndMatch(0, earlier, room::MatchResult::P1Win).accepted);
	for (const auto member : {p1, p2}) CHECK(act(member, room::ActionKind::AcknowledgeTerminal, earlier));
	CHECK(act(queued, room::ActionKind::Queue, 0));
	CHECK(act(leaving, room::ActionKind::Queue, 0));
	CHECK(act(watcher, room::ActionKind::Watch, 0));
	const auto game = begin();
	const auto roster = authority.MatchRoster(0);
	const auto inRoster = [&](room::MemberId member) { return std::find(roster.begin(), roster.end(), member) != roster.end(); };
	CHECK(roster.size() == 5 && inRoster(watcher) && inRoster(queued) && inRoster(leaving) && !inRoster(excluded));
	CHECK(act(late, room::ActionKind::Queue, 0));
	// The fighters report different results: the table pauses on that game.
	auto report = [&](room::MemberId member, room::MatchResult result) {
		auto value = action(room::ActionKind::RecordResult, game);
		value.result = result; value.actionId = member * 1000 + authority.SnapshotView().revision + 1;
		return authority.Apply(member, value).accepted;
	};
	CHECK(report(p1, room::MatchResult::P1Win) && report(p2, room::MatchResult::P2Win));
	CHECK(authority.SnapshotView().tables[0].phase == room::TablePhase::Paused && authority.SnapshotView().tables[0].matchGeneration == game);
	const auto watcherView = authority.SnapshotFor(watcher), queuedView = authority.SnapshotFor(queued);
	const auto excludedView = authority.SnapshotFor(excluded), lateView = authority.SnapshotFor(late);
	// An admitted queued member leaves the queue while still streaming the paused
	// game: no longer Watching (that needs Playing), but still among its
	// spectators. Then they Watch the same table again: watching next, place kept.
	CHECK(act(leaving, room::ActionKind::Unqueue, 0));
	const auto unqueuedView = authority.SnapshotFor(leaving);
	CHECK(room::FindMember(unqueuedView, leaving)->status == room::MemberStatus::WatchingNext);
	CHECK(std::find(unqueuedView.tables[0].spectators.begin(), unqueuedView.tables[0].spectators.end(), leaving) != unqueuedView.tables[0].spectators.end());
	CHECK(act(leaving, room::ActionKind::Watch, 0));
	const auto rewatchView = authority.SnapshotFor(leaving);
	CHECK(room::FindMember(rewatchView, leaving)->status == room::MemberStatus::WatchingNext);
	CHECK(std::find(rewatchView.tables[0].spectators.begin(), rewatchView.tables[0].spectators.end(), leaving) != rewatchView.tables[0].spectators.end());
	CHECK(unqueuedView.localMatchGenerations[0] == game && rewatchView.localMatchGenerations[0] == game);
	// A spectator who first watches after the pause was never taken in.
	CHECK(act(arrival, room::ActionKind::Watch, 0));
	const auto arrivalView = authority.SnapshotFor(arrival);
	CHECK(arrivalView.localMatchGenerations[0] == 0);
	CHECK(watcherView.localMatchGenerations[0] == game && queuedView.localMatchGenerations[0] == game);
	CHECK(excludedView.localMatchGenerations[0] == 0 && lateView.localMatchGenerations[0] == 0);
	const auto withoutKey = [](room::Snapshot snapshot) { snapshot.localMatchGenerationsSent = false; return snapshot; };
	auto postMatch = joined; postMatch.match = MatchState::PostMatch;
	std::vector<TrainingRow> rows = {
		{"paused: spectator, nothing retired", watcherView, joined, 0, false},
		{"paused: spectator, an older game retired", watcherView, postMatch, earlier, false},
		{"paused: spectator, this game retired", watcherView, postMatch, game, true},
		{"paused: older host: spectator, nothing retired", withoutKey(watcherView), joined, 0, false},
		{"paused: older host: spectator, an older game retired", withoutKey(watcherView), postMatch, earlier, false},
		{"paused: older host: spectator, this game retired", withoutKey(watcherView), postMatch, game, true},
		{"paused: queued admitted, nothing retired", queuedView, joined, 0, false},
		{"paused: queued admitted, an older game retired", queuedView, postMatch, earlier, false},
		{"paused: queued admitted, this game retired", queuedView, postMatch, game, true},
		{"paused: older host: queued admitted, nothing retired", withoutKey(queuedView), joined, 0, false},
		{"paused: older host: queued admitted, an older game retired", withoutKey(queuedView), postMatch, earlier, false},
		{"paused: older host: queued admitted, this game retired", withoutKey(queuedView), postMatch, game, true},
		{"paused: queued, left out of the game", excludedView, joined, 0, true},
		{"paused: queued after the start", lateView, joined, 0, true},
		{"paused: older host: queued after the start", withoutKey(lateView), joined, 0, true},
		{"paused: spectator arrived after the pause", arrivalView, joined, 0, true},
	};
	// The member still streaming the paused game after Unqueue, and after Watch
	// again: refused until this PC retires that exact game, from either host,
	// with the controller not started or back.
	std::deque<std::string> names;
	const std::pair<const char*, const room::Snapshot*> streaming[] = {{"unqueued", &unqueuedView}, {"unqueued then watched", &rewatchView}};
	for (const auto& place : streaming)
		for (const bool key : {true, false})
			for (const sf4e::netplay::Snapshot* state : {&joined, static_cast<const sf4e::netplay::Snapshot*>(&postMatch)})
				for (const auto retired : {std::uint64_t(0), earlier, game}) {
					names.push_back(std::string("paused: ") + (key ? "" : "older host: ") + place.first + (state == &joined ? ", controller none" : ", controller back") +
						(retired == game ? ", this game retired" : retired ? ", an older game retired" : ", nothing retired"));
					rows.push_back({names.back().c_str(), key ? *place.second : withoutKey(*place.second), *state, retired, retired == game});
				}
	CheckTrainingRows(rows, made);
}
// The runtime's own policy, with a runtime running: a room entry holds while
// the room's gate is open, and not once a newer room snapshot of the same
// session seats an opponent; the native menu then never moves. An accepted
// entry is posted once and taken once.
static void TestTrainingPolicy() {
	using sf4e::TrainingEntry;
	using sf4e::netplay::Generation;
	using Taken = sf4e::GameEvents::TrainingRequest::Taken;
	auto& record = sf4e::GameEvents::MainMenu::trainingRequest;
	sf4e::GameEvents::TrainingRequest::Pending seen;
	const auto none = [&] { return !record.Peek(GetTickCount64(), seen); };
	if (!none()) record.Consume(seen.serial);
	const Generation made = {4, 0};
	const auto session = JoinedSession(made);
	const auto alone = TableRoom(false), challenged = TableRoom(true);
	CHECK(facade::TrainingHolds(TrainingEntry::Room, made, session, true, alone, 0));
	CHECK(!facade::TrainingHolds(TrainingEntry::Room, made, session, true, challenged, 0));
	CHECK(!facade::TrainingHolds(TrainingEntry::Room, made, session, false, alone, 0));
	CHECK(!facade::TrainingHolds(TrainingEntry::Room, Generation{5, 0}, session, true, alone, 0));
	CHECK(!facade::TrainingHolds(TrainingEntry::Room, Generation{4, 1}, session, true, alone, 0));
	auto readying = session; readying.readyPending = true;
	CHECK(!facade::TrainingHolds(TrainingEntry::Room, made, readying, true, alone, 0));
	CHECK(!facade::TrainingHolds(TrainingEntry::None, made, session, true, alone, 0));
	// Accepted while alone; the client then applies a newer room snapshot of the
	// same session, an opponent sat down, before the menu looks: no Training.
	sf4e::room::Snapshot live = alone;
	const auto judged = [&](const Generation& generation) {
		return facade::TrainingHolds(TrainingEntry::Room, generation, session, true, live, 0);
	};
	CHECK(judged(made));
	sf4e::GameEvents::MainMenu::RequestTraining(made);
	CHECK(record.Peek(GetTickCount64(), seen));
	const auto first = seen.serial;
	live = challenged;
	CHECK(record.Take(GetTickCount64(), judged) == Taken::Dropped && none());
	// Still alone when the menu looks: one post, one move, nothing left after.
	live = alone;
	CHECK(judged(made));
	sf4e::GameEvents::MainMenu::RequestTraining(made);
	CHECK(record.Peek(GetTickCount64(), seen) && seen.serial == first + 1 && seen.generation == made);
	CHECK(record.Take(GetTickCount64(), judged) == Taken::Go && none());
	CHECK(record.Take(GetTickCount64(), judged) == Taken::None);
	// In the room at no table: eligible. The client then applies a newer snapshot
	// of the same session closing the room, the member kept and every table
	// closed, before the menu looks: no Training.
	sf4e::room::Snapshot unseated;
	unseated.roomEpoch = 3;
	unseated.localMember = 1;
	unseated.members.resize(1);
	unseated.members[0].id = 1;
	live = unseated;
	CHECK(judged(made));
	sf4e::GameEvents::MainMenu::RequestTraining(made);
	CHECK(!none());
	auto closed = unseated;
	closed.closed = true;
	for (auto& table : closed.tables) table.phase = sf4e::room::TablePhase::Closed;
	CHECK(!facade::TrainingHolds(TrainingEntry::Room, made, session, true, closed, 0));
	live = closed;
	CHECK(record.Take(GetTickCount64(), judged) == Taken::Dropped && none());
	// Waiting to watch table 0, where two others sit: eligible. A newer snapshot
	// of the same session starts the game with this PC in its roster (Watching),
	// while the controller still says None or PostMatch: no Training. The same
	// for a locked-in watcher while no start is held.
	for (const bool locked : {false, true}) {
		auto after = session;
		if (locked) after.match = sf4e::netplay::MatchState::PostMatch;
		sf4e::room::Snapshot waiting = unseated;
		waiting.tables[0].p1 = 2; waiting.tables[0].p2 = 3;
		waiting.tables[0].phase = sf4e::room::TablePhase::Waiting;
		waiting.tables[0].watchingNext = {1};
		waiting.members[0].status = sf4e::room::MemberStatus::WatchingNext;
		waiting.members[0].table = 0;
		waiting.members[0].spectatorLocked = locked;
		CHECK(facade::TrainingHolds(TrainingEntry::Room, made, after, true, waiting, 0));
		auto watching = waiting;
		watching.tables[0].phase = sf4e::room::TablePhase::Playing;
		watching.tables[0].watchingNext.clear();
		watching.tables[0].spectators = {1};
		watching.members[0].status = sf4e::room::MemberStatus::Watching;
		CHECK(!facade::TrainingHolds(TrainingEntry::Room, made, after, true, watching, 0));
		live = waiting;
		const auto judgedAfter = [&](const Generation& generation) {
			return facade::TrainingHolds(TrainingEntry::Room, generation, after, true, live, 0);
		};
		CHECK(judgedAfter(made));
		sf4e::GameEvents::MainMenu::RequestTraining(made);
		live = watching;
		CHECK(record.Take(GetTickCount64(), judgedAfter) == Taken::Dropped && none());
		// Listed among the spectators but left out of this game's roster: free until
		// the next when the host says so (generation 0 here). An older host cannot
		// say, so the place among the spectators refuses.
		auto excluded = watching;
		excluded.members[0].status = sf4e::room::MemberStatus::WatchingNext;
		CHECK(!facade::TrainingHolds(TrainingEntry::Room, made, after, true, excluded, 0));
		excluded.localMatchGenerationsSent = true;
		CHECK(facade::TrainingHolds(TrainingEntry::Room, made, after, true, excluded, 0));
		if (!locked) continue;
		// Both fighters ready and the start held for this locked-in watcher: part
		// of the game starting, so no Training. Without the lock, the same start
		// waits for nobody here.
		auto holding = waiting;
		holding.tables[0].phase = sf4e::room::TablePhase::Ready;
		holding.tables[0].ready[0] = holding.tables[0].ready[1] = true;
		holding.tables[0].spectatorHold = true;
		holding.tables[0].holdRemainingMs = 8000;
		CHECK(!facade::TrainingHolds(TrainingEntry::Room, made, after, true, holding, 0));
		auto unlocked = holding;
		unlocked.members[0].spectatorLocked = false;
		CHECK(facade::TrainingHolds(TrainingEntry::Room, made, after, true, unlocked, 0));
		live = waiting;
		CHECK(judgedAfter(made));
		sf4e::GameEvents::MainMenu::RequestTraining(made);
		live = holding;
		CHECK(record.Take(GetTickCount64(), judgedAfter) == Taken::Dropped && none());
	}
	// From a host that does not send its roster generations (no key): queued at
	// table 0 while it waits, eligible, as is a queue place taken after the game
	// began. Queued and listed among the spectators the game started with (the
	// status stays Queued): taken to be admitted, so no Training.
	sf4e::room::Snapshot queued = unseated;
	queued.tables[0].p1 = 2; queued.tables[0].p2 = 3;
	queued.tables[0].phase = sf4e::room::TablePhase::Waiting;
	queued.tables[0].queue = {1};
	queued.members[0].status = sf4e::room::MemberStatus::Queued;
	queued.members[0].table = 0;
	CHECK(facade::TrainingHolds(TrainingEntry::Room, made, session, true, queued, 0));
	auto lateQueue = queued;
	lateQueue.tables[0].phase = sf4e::room::TablePhase::Playing;
	lateQueue.tables[0].spectators = {4};
	CHECK(facade::TrainingHolds(TrainingEntry::Room, made, session, true, lateQueue, 0));
	auto admitted = lateQueue;
	admitted.tables[0].spectators = {4, 1};
	CHECK(!facade::TrainingHolds(TrainingEntry::Room, made, session, true, admitted, 0));
	live = queued;
	CHECK(judged(made));
	sf4e::GameEvents::MainMenu::RequestTraining(made);
	live = admitted;
	CHECK(record.Take(GetTickCount64(), judged) == Taken::Dropped && none());
	// Once the host says, its roster decides, not the lists.
	auto said = admitted;
	said.localMatchGenerationsSent = true;
	said.tables[0].matchGeneration = 6;
	CHECK(facade::TrainingHolds(TrainingEntry::Room, made, session, true, said, 0));
	said.localMatchGenerations[0] = 6;
	CHECK(!facade::TrainingHolds(TrainingEntry::Room, made, session, true, said, 0));
	said.localMatchGenerations[0] = 5;
	CHECK(facade::TrainingHolds(TrainingEntry::Room, made, session, true, said, 0));
	TestTrainingRoster(session, made);
	TestTrainingMatrix(session, made);
	TestTrainingPausedMatrix(session, made);
}

static Dimps::GameEvents::RootEvent* NoRootEvent() { return nullptr; }

// Through the real queue: the runtime refuses a room entry outside a room and
// one made in another session, and Play offline only starts offline. None of
// them reaches the native menu.
static void TestRuntimeJudgesTraining() {
	using sf4e::TrainingEntry;
	namespace netplay = sf4e::netplay;
	auto& record = sf4e::GameEvents::MainMenu::trainingRequest;
	sf4e::GameEvents::TrainingRequest::Pending seen;
	sf4e::NetplayConfig config = {};
	facade::InitFromPayload(config);
	facade::ConfigureHelper({}, ERROR_FILE_NOT_FOUND);
	facade::StartHelper();
	const auto originalRoot = Dimps::App::GetRootEvent;
	Dimps::App::GetRootEvent = NoRootEvent;
	facade::NotifyRuntimeGameReady();
	facade::TickRuntime();
	facade::NotifyRuntimeEventSystemReady();
	facade::TickRuntime();
	const auto current = facade::GetRuntimeSnapshot().session.generation;
	facade::RuntimeCommand room;
	room.command.generation = current;
	room.training = TrainingEntry::Room;
	CHECK(facade::SubmitRuntimeCommand(room));
	room.command.generation = {current.room + 1, 0};
	CHECK(facade::SubmitRuntimeCommand(room));
	facade::TickRuntime();
	CHECK(!record.Peek(GetTickCount64(), seen));
	CHECK(facade::GetRuntimeSnapshot().session.room == netplay::RoomState::Idle);
	// Play offline sends the game to its own menus, Training among them, and
	// asks the menu for nothing.
	facade::RuntimeCommand offline;
	offline.command = {netplay::CommandKind::StartOffline, current, {}};
	CHECK(facade::SubmitRuntimeCommand(offline));
	facade::TickRuntime();
	CHECK(facade::GetRuntimeSnapshot().offlineRequested && !record.Peek(GetTickCount64(), seen));
	// The menu's own question, live: no main menu is up, so nothing holds.
	CHECK(!facade::TrainingRequestHolds(current));
	TestTrainingPolicy();
	facade::StopHelper();
	Dimps::App::GetRootEvent = originalRoot;
}

// The pending request is one record: a newer request made while an older one is
// being judged is neither erased by the older one's consume nor paired with its deadline.
static void TestTrainingRequestRecord() {
	using Request = sf4e::GameEvents::TrainingRequest;
	using sf4e::netplay::Generation;
	Request request;
	Request::Pending seen;
	CHECK(!request.Peek(100, seen));
	const auto a = request.Post(Generation{1, 0}, 100, 2000);
	CHECK(request.Peek(150, seen) && seen.serial == a && seen.generation == (Generation{1, 0}) && seen.deadline == 2100);
	// The game thread has A and is judging it; B arrives with its own session and deadline.
	const auto b = request.Post(Generation{2, 0}, 500, 2000);
	CHECK(b != a);
	CHECK(!request.Consume(a));
	CHECK(request.Peek(600, seen) && seen.serial == b && seen.generation == (Generation{2, 0}) && seen.deadline == 2500);
	CHECK(request.Consume(b) && !request.Consume(b) && !request.Peek(600, seen));
	// A request that ran out is forgotten, and cannot be consumed afterwards.
	const auto c = request.Post(Generation{3, 0}, 1000, 2000);
	CHECK(!request.Peek(3001, seen) && !request.Consume(c));
}

int main() {
	spdlog::set_level(spdlog::level::off);
	TestNoticeRules();
	TestPublishedFrameIsACopy();
	TestTwoThreads();
	TestCommandLifetime();
	TestTrainingRequestRecord();
	TestTrainingRequestTake();
	TestRuntimeJudgesTraining();
	std::cout << "Presentation snapshot passed\n";
	return 0;
}
