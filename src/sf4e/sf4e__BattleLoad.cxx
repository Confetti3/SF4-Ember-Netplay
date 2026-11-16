#include "sf4e__BattleLoad.hxx"

#include <mutex>
#include <optional>

#include <windows.h>
#include "spdlog/spdlog.h"

#include "../common/LoadWatch.hxx"
#include "sf4e__CrashDiagnostics.hxx"

namespace {

namespace watch = sf4e::loadwatch;

std::mutex s_lock;
watch::LoadWatch s_watch;
watch::BattleLoadFacts s_noted;

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

void sf4e::battleload::Note(loadwatch::LoadSource source, const Dimps::GameEvents::VsMode::ConfirmedCharaConditions (&sides)[2], int stage) {
	watch::BattleLoadFacts facts;
	facts.source = source;
	facts.stage = stage;
	for (int i = 0; i < 2; ++i)
		facts.sides[i] = { sides[i].charaID, sides[i].costume, sides[i].color, sides[i].ultraCombo, sides[i].unc_edition };
	std::lock_guard<std::mutex> guard(s_lock);
	s_noted = facts;
}

void sf4e::battleload::Started() {
	watch::BattleLoadFacts facts;
	{
		std::lock_guard<std::mutex> guard(s_lock);
		facts = s_noted;
		s_noted = watch::BattleLoadFacts();
		s_watch.LoadStarted(GetTickCount64());
	}
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
	s_noted = watch::BattleLoadFacts();
}

void sf4e::battleload::Tick() {
	std::optional<watch::Report> report;
	{
		std::lock_guard<std::mutex> guard(s_lock);
		report = s_watch.Tick(GetTickCount64());
	}
	Announce(report);
}
