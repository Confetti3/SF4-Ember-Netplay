// The Replays screen's own rules, without a menu: which tag a replay's row
// carries, how a running export's progress reads, how far an export has got,
// when it can still be cancelled, and what its end is called.
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
	Check(ReplayTagsText(false, false, false).empty(), "A replay with nothing to say was tagged");
	Check(ReplayTagsText(true, false, false) == loc::T("replays.spectated"), "A spectated match was not tagged Spectated");
	Check(ReplayTagsText(false, true, false) == loc::T("replays.seen"), "A replay played back was not tagged Seen");
	Check(ReplayTagsText(true, true, false) == loc::T("replays.spectated_seen"), "Spectated and seen were not one tag");
	Check(ReplayTagsText(false, false, true) == loc::T("replays.video"), "An exported replay was not tagged Video");
	Check(ReplayTagsText(true, true, true) == std::string(loc::T("replays.spectated_seen")) + ", " + loc::T("replays.video"), "Video did not follow the other tags");
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

	// The frame meter, like the caption, is drawn into the video: the picture
	// is taken after Ember's overlay, unless Ember's menu is over the export.
	Check(replay::ExportDrawsOverlay(false, true) && replay::ExportDrawsOverlay(true, false) && !replay::ExportDrawsOverlay(false, false),
		"An export with the frame meter or a caption did not take Ember's overlay");
	const auto meterFrame = replay::ExportGrabOf(replay::ExportDrawsOverlay(false, true), false, false);
	Check(!meterFrame.beforeOverlay && meterFrame.afterOverlay, "An export with the frame meter left it out of the video");
	const auto plainFrame = replay::ExportGrabOf(false, false, false);
	Check(plainFrame.beforeOverlay && !plainFrame.afterOverlay, "A plain export took Ember's overlay");
	const auto menuFrame = replay::ExportGrabOf(true, true, true), opening = replay::ExportGrabOf(true, false, true);
	Check(menuFrame.beforeOverlay && !menuFrame.afterOverlay, "Ember's menu over an export went into the video");
	Check(!opening.beforeOverlay && !opening.afterOverlay, "The frame Ember's menu opened on went into the video");
	Check(replay::ExportGrabOf(false, true, true).beforeOverlay && replay::ExportGrabOf(false, false, true).beforeOverlay,
		"A plain export lost frames to Ember's menu");

	if (failures) std::printf("%d replay screen check(s) failed\n", failures);
	else std::printf("Replay screen text: all checks passed\n");
	return failures ? 1 : 0;
}
