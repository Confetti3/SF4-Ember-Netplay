// The drawing thread reads netplay state only through the published
// PresentationSnapshot. rc4 let it call GetStatus, which expired and copied the
// game thread's notice string while that thread replaced it; a two-thread run
// of that shape ended in 0xC0000374 within a fraction of a second. This runs
// the same hammer through the publication path, and pins the notice rules and
// the command queue's lifetime across a runtime restart.
#include "../sf4e/sf4e__NetplayFacade.hxx"
#include "../sf4e/sf4e__RuntimeBridge.hxx"
#include "../sf4e/sf4e__GameEvents.hxx"
#include "../common/MatchNotice.hxx"
#include <spdlog/spdlog.h>
#include <atomic>
#include <cstring>
#include <iostream>
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

// A Training request is judged against the live state when the menu would act on it.
static void TestTrainingRequestJudgement() {
	using Menu = sf4e::GameEvents::MainMenu;
	using Verdict = Menu::TrainingVerdict;
	using namespace sf4e::netplay;
	facade::RuntimeSnapshot live;
	live.atMainMenu = true;
	live.session.generation = {4, 0};
	const Generation made = {4, 0};
	// From a room: the room's own gate decides, and a session that moved on drops it.
	live.session.room = RoomState::Joined;
	live.canTrain = true;
	CHECK(Menu::JudgeTrainingRequest(live, made, true) == Verdict::Proceed);
	live.canTrain = false;
	CHECK(Menu::JudgeTrainingRequest(live, made, true) == Verdict::Drop);
	live.canTrain = true;
	live.session.generation = {5, 0};
	CHECK(Menu::JudgeTrainingRequest(live, made, true) == Verdict::Drop);
	live.session.generation = made;
	live.atMainMenu = false;
	CHECK(Menu::JudgeTrainingRequest(live, made, true) == Verdict::Drop);
	// Offline: it waits for the runtime to take its StartOffline, and drops if the controller cannot.
	live = facade::RuntimeSnapshot();
	live.atMainMenu = true;
	live.session.generation = made;
	CHECK(Menu::JudgeTrainingRequest(live, made, false) == Verdict::Wait);
	live.offlineRequested = true;
	CHECK(Menu::JudgeTrainingRequest(live, made, false) == Verdict::Proceed);
	live.session.room = RoomState::Joined;
	CHECK(Menu::JudgeTrainingRequest(live, made, false) == Verdict::Drop);
	live.session.room = RoomState::Idle;
	live.session.match = MatchState::Playing;
	CHECK(Menu::JudgeTrainingRequest(live, made, false) == Verdict::Drop);
	live.session.match = MatchState::None;
	live.session.generation = {4, 1};
	CHECK(Menu::JudgeTrainingRequest(live, made, false) == Verdict::Drop);
}

// The pending request is one record: a newer request made while an older one is
// being judged is neither erased by the older one's consume nor paired with its deadline.
static void TestTrainingRequestRecord() {
	using Request = sf4e::GameEvents::TrainingRequest;
	using sf4e::netplay::Generation;
	Request request;
	Request::Pending seen;
	CHECK(!request.Peek(100, seen));
	const auto a = request.Post(Generation{1, 0}, true, 100, 2000);
	CHECK(request.Peek(150, seen) && seen.serial == a && seen.fromRoom && seen.generation == (Generation{1, 0}) && seen.deadline == 2100);
	// The game thread has A and is judging it; B arrives with its own origin and deadline.
	const auto b = request.Post(Generation{2, 0}, false, 500, 2000);
	CHECK(b != a);
	CHECK(!request.Consume(a));
	CHECK(request.Peek(600, seen) && seen.serial == b && !seen.fromRoom && seen.generation == (Generation{2, 0}) && seen.deadline == 2500);
	CHECK(request.Consume(b) && !request.Consume(b) && !request.Peek(600, seen));
	// A request that ran out is forgotten, and cannot be consumed afterwards.
	const auto c = request.Post(Generation{3, 0}, true, 1000, 2000);
	CHECK(!request.Peek(3001, seen) && !request.Consume(c));
}

int main() {
	spdlog::set_level(spdlog::level::off);
	TestNoticeRules();
	TestPublishedFrameIsACopy();
	TestTwoThreads();
	TestCommandLifetime();
	TestTrainingRequestJudgement();
	TestTrainingRequestRecord();
	std::cout << "Presentation snapshot passed\n";
	return 0;
}
