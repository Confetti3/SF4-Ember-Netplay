#include "sf4e__BattleLoad.hxx"

#include <mutex>
#include <optional>

#include <windows.h>
#include "spdlog/spdlog.h"

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../common/LoadWatch.hxx"
#include "sf4e__BattleLoadRead.hxx"
#include "sf4e__CrashDiagnostics.hxx"

namespace {

namespace watch = sf4e::loadwatch;

std::mutex s_lock;
watch::LoadWatch s_watch;
bool s_emberSetUp = false;

constexpr unsigned long long Seconds(std::uint64_t ms) { return (ms + 500) / 1000; }

void Announce(const std::optional<watch::Report>& report) {
	if (!report) return;
	switch (report->kind) {
	case watch::ReportKind::StalledAt20s:
		spdlog::warn("Battle load: no first frame after {} s", Seconds(watch::FirstStallMs));
		sf4e::crash::NoteMatchBoundary("load_stall");
		sf4e::crash::NoteAddressSpace();
		break;
	case watch::ReportKind::StillStalledAt60s:
		sf4e::crash::NoteMatchBoundary("load_stall_60s");
		break;
	case watch::ReportKind::Recovered:
		spdlog::info("Battle load: first frame after {} s", Seconds(report->elapsedMs));
		break;
	}
}

} // namespace

// What the game's Versus or Training event holds for the battle about to load,
// whoever filled it; Unread in any other mode.
static watch::BattleLoadFacts ReadLoad() {
	using namespace Dimps::GameEvents;
	char* versus[] = { "VSMode" };
	if (auto* mode = (VsMode*)Dimps::Event::EventBaseWithEC::FindForegroundEvent(Dimps::App::GetRootEvent(), versus, 1))
		if (auto facts = sf4e::battleload::ReadVersus(mode)) return *facts;
	char* training[] = { "TrainingMode" };
	if (auto* mode = (TrainingMode*)Dimps::Event::EventBaseWithEC::FindForegroundEvent(Dimps::App::GetRootEvent(), training, 1))
		if (auto facts = sf4e::battleload::ReadTraining(mode, TrainingBattle::stagePush)) return *facts;
	return {};
}

void sf4e::battleload::NoteEmberSetUp() {
	std::lock_guard<std::mutex> guard(s_lock);
	s_emberSetUp = true;
}

void sf4e::battleload::Started() {
	bool emberSetUp;
	{
		std::lock_guard<std::mutex> guard(s_lock);
		emberSetUp = s_emberSetUp;
		s_emberSetUp = false;
		s_watch.LoadStarted(GetTickCount64());
	}
	watch::BattleLoadFacts facts = ReadLoad();
	facts.emberSetUp = emberSetUp;
	spdlog::info("Battle load: {}", watch::FormatBattleLoad(facts));
	crash::NoteMatchBoundary("battle_load");
}

void sf4e::battleload::FirstFrame() {
	std::optional<watch::Report> report;
	{
		std::lock_guard<std::mutex> guard(s_lock);
		report = s_watch.FirstFrame(GetTickCount64());
	}
	Announce(report);
}

void sf4e::battleload::Closed() {
	std::lock_guard<std::mutex> guard(s_lock);
	s_watch.BattleClosed(GetTickCount64());
	s_emberSetUp = false;
}

void sf4e::battleload::Tick() {
	std::optional<watch::Report> report;
	{
		std::lock_guard<std::mutex> guard(s_lock);
		report = s_watch.Tick(GetTickCount64());
	}
	Announce(report);
}
