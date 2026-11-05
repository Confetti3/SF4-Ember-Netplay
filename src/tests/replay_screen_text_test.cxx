// The Replays screen's own rules, without a menu: which tag a replay's row
// carries, how a running export's progress reads, how far an export has got,
// when it can still be cancelled, and what its end is called.
#include "../common/FighterCatalog.hxx"
#include "../ui/ReplaySummaryText.hxx"
#include <cstdio>
#include <string>

namespace {
int failures = 0;
void Check(bool condition, const char* description) {
	if (!condition) { ++failures; std::printf("FAIL: %s\n", description); }
}
}

int main() {
	using namespace sf4e;
	using namespace sf4e::ui;
	using replay::ExportStage;

	// Spectated: this PC only watched the match live. Seen: its replay was played with Watch now.
	Check(ReplayTagsText(false, false, false, false).empty(), "A replay with nothing to say was tagged");
	Check(ReplayTagsText(true, false, false, false) == loc::T("replays.spectated"), "A spectated match was not tagged Spectated");
	Check(ReplayTagsText(false, true, false, false) == loc::T("replays.seen"), "A replay played back was not tagged Seen");
	Check(ReplayTagsText(true, true, false, false) == loc::T("replays.spectated_seen"), "Spectated and seen were not one tag");
	Check(ReplayTagsText(false, false, true, false) == loc::T("replays.video"), "An exported replay was not tagged Video");
	Check(ReplayTagsText(true, true, true, false) == std::string(loc::T("replays.spectated_seen")) + ", " + loc::T("replays.video"), "Video did not follow the other tags");
	Check(ReplayTagsText(false, false, false, true) == loc::T("replays.kept"), "A kept replay was not tagged Kept");
	Check(ReplayTagsText(false, true, true, true) == std::string(loc::T("replays.seen")) + ", " + loc::T("replays.video") + ", " + loc::T("replays.kept"), "Kept did not come last");
	Check(std::string(loc::T("replays.spectated")) == "Spectated" && std::string(loc::T("replays.seen")) == "Seen" &&
		std::string(loc::T("replays.spectated_seen")) == "Spectated, seen", "The English tags are not Spectated and Seen");

	// The row's label names the players; a fighter stands in for a name Ember did not note.
	{
		const std::string names[2] = {"Ann", ""};
		const int fighters[2] = {0, 99};
		Check(ReplayRowLabel("2026-10-05 23:35", names, fighters) ==
			"2026-10-05 23:35  " + loc::Tf("replays.fighters", std::string("Ann"), std::string(loc::T("common.unavailable"))),
			"A replay row's label did not name its players");
		const std::string none[2];
		const int known[2] = {0, 0};
		const std::string fighter = selection::FindFighter(0)->name;
		Check(ReplayRowLabel("d", none, known) == "d  " + loc::Tf("replays.fighters", fighter, fighter), "An unnamed replay did not fall back to its fighters");
	}

	// Show lists by what Ember knows of a replay; Search by a player's or
	// fighter's name, in any case.
	{
		platform::replays::ArchivedReplay mine, watched, seen, video, kept;
		mine.names[0] = "Alice"; mine.names[1] = "Bob"; mine.fighters[0] = 0; mine.fighters[1] = 1;
		watched.provenance = replayfiles::Provenance::Spectated; watched.names[0] = "Carol";
		seen.watched = true; video.video = true; kept.kept = true;
		using Show = ReplayShow; const std::wstring none;
		Check(ReplayShown(mine, Show::All, none) && ReplayShown(watched, Show::All, none) && ReplayShown(seen, Show::All, none) && ReplayShown(video, Show::All, none), "All did not show every replay");
		Check(ReplayShown(mine, Show::Mine, none) && !ReplayShown(watched, Show::Mine, none), "My matches showed a spectated one");
		Check(!ReplayShown(mine, Show::Spectated, none) && ReplayShown(watched, Show::Spectated, none), "Spectated showed a played one");
		Check(ReplayShown(mine, Show::Unseen, none) && !ReplayShown(seen, Show::Unseen, none), "Not seen yet showed a played-back one");
		Check(!ReplayShown(mine, Show::Video, none) && ReplayShown(video, Show::Video, none), "With video showed one without");
		Check(!ReplayShown(mine, Show::Kept, none) && ReplayShown(kept, Show::Kept, none), "Kept showed one the player does not keep");
		Check(ReplayShown(mine, Show::All, LoweredText("ali")) && ReplayShown(mine, Show::All, LoweredText("BOB")) && !ReplayShown(mine, Show::All, LoweredText("carol")), "Search did not match a player's name in any case");
		const std::string fighter = selection::FindFighter(0)->name;
		Check(ReplayShown(mine, Show::All, LoweredText(fighter.substr(1, 2))) && !ReplayShown(watched, Show::All, LoweredText(fighter)), "Search did not match a fighter's name");
		Check(!ReplayShown(watched, Show::Mine, LoweredText("carol")), "Search showed a replay Show hides");
		Check(std::size(kReplayShowKeys) == static_cast<std::size_t>(Show::Kept) + 1 && std::string(kReplayShowKeys[static_cast<int>(Show::Kept)]) == "replays.show.kept", "Show's words are not in their order");
	}

	// Progress: the fight recorded so far against the replay's length.
	Check(ExportProgressText(62 * 60, 161 * 60) == "1:02 / 2:41", "Progress did not read elapsed / total");
	Check(ExportProgressText(0, 161 * 60) == "0:00 / 2:41", "Progress at the start was wrong");
	Check(ExportProgressText(200 * 60, 161 * 60) == "2:41 / 2:41", "Progress ran past the replay's length");
	Check(ExportProgressText(62 * 60, 0) == "1:02", "A replay of unknown length did not show the time alone");
	Check(ExportStageText(ExportStage::Starting, 0, 60) == loc::T("export.starting"), "Starting was not said");
	Check(ExportStageText(ExportStage::Recording, 60, 120) == ExportProgressText(60, 120), "Recording did not show its progress");
	Check(ExportStageText(ExportStage::Finishing, 60, 120) == loc::T("export.finishing"), "Finishing was not said");
	Check(ExportStageText(ExportStage::Cancelling, 60, 120) == loc::T("export.cancelling"), "Cancelling was not said");
	Check(ExportStageText(ExportStage::None, 60, 120).empty(), "No export still had a progress");

	// The stage, from what the replay operation and the capture say.
	Check(replay::ExportStageOf(false, true, true, true) == ExportStage::None, "An ended export still had a stage");
	Check(replay::ExportStageOf(true, false, false, false) == ExportStage::Starting, "An export before its battle was not starting");
	Check(replay::ExportStageOf(true, true, true, false) == ExportStage::Recording, "A capturing export was not recording");
	Check(replay::ExportStageOf(true, true, false, false) == ExportStage::Finishing, "A closing export was not finishing");
	Check(replay::ExportStageOf(true, true, true, true) == ExportStage::Cancelling && replay::ExportStageOf(true, true, false, true) == ExportStage::Cancelling,
		"A cancelled export was not cancelling");

	// Cancel only while it can keep the video from being made.
	Check(replay::ExportCancellable(ExportStage::Starting) && replay::ExportCancellable(ExportStage::Recording), "A running export could not be cancelled");
	Check(!replay::ExportCancellable(ExportStage::None) && !replay::ExportCancellable(ExportStage::Finishing) && !replay::ExportCancellable(ExportStage::Cancelling),
		"Cancel was offered where it can change nothing");

	// The end: a file is reported as saved even when a cancel came too late; a cancel is no failure.
	const auto saved = replay::ExportEndOf(true, false), late = replay::ExportEndOf(true, true);
	const auto cancelled = replay::ExportEndOf(false, true), failed = replay::ExportEndOf(false, false);
	Check(std::string(saved.key) == "replays.exported" && !saved.error, "A made video was not reported saved");
	Check(std::string(late.key) == "replays.exported" && !late.error, "A video made despite a late cancel was not reported");
	Check(std::string(cancelled.key) == "export.cancelled" && !cancelled.error, "A cancelled export was not reported as cancelled");
	Check(std::string(failed.key) == "replays.gpu_not_exported" && failed.error, "A failed export was not an error");

	// The export's progress is the replay's own clock: rounds before the one
	// playing, and how far it has got, against all its rounds.
	{
		const std::uint32_t frames[replay::ExportClock::kRounds] = {600, 900, 300};
		replay::ExportClock clock;
		Check(clock.Played() == 0 && clock.Total() == 0, "A new export had progress");
		clock.Observe(0, 100, frames);
		Check(clock.Played() == 100 && clock.Total() == 1800, "The first round's frames were not the progress");
		// Paused, held or at the native pause menu: the cursor stands, and so does the export.
		for (int tick = 0; tick < 50; tick++) clock.Observe(0, 100, frames);
		Check(clock.Played() == 100, "A paused replay moved the export on");
		clock.Observe(0, 600, frames);
		// The next round starts its cursor over; the rounds before still count.
		clock.Observe(1, 0, frames);
		Check(clock.Played() == 600, "A new round took the export back");
		clock.Observe(1, 450, frames);
		Check(clock.Played() == 1050, "The second round's frames did not add to the first's");
		// A cursor that goes back to its round's start (the stream's end) does not take it back.
		clock.Observe(1, 0, frames);
		Check(clock.Played() == 1050, "A cursor reset took the export back");
		// Never past a round's own length, nor the replay's.
		clock.Observe(2, 5000, frames);
		Check(clock.Played() == 1800 && clock.Played() <= clock.Total(), "The export ran past its replay");
		// The capture closing leaves the last progress; a new export starts over.
		clock.Observe(-1, 0, frames);
		Check(clock.Played() == 1800, "An unknown round changed the progress");
		clock.Reset();
		Check(clock.Played() == 0 && clock.Total() == 0, "A new export kept the last one's progress");
	}

	if (failures) std::printf("%d replay screen check(s) failed\n", failures);
	else std::printf("Replay screen text: all checks passed\n");
	return failures ? 1 : 0;
}
