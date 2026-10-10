#pragma once
#include "../common/ReplayInputDetails.hxx"
#include "../common/ReplayInputLane.hxx"
#include "../common/ReplayRequest.hxx"
#include "../common/ReplayTransport.hxx"
#include "../platform/ReplayFiles.hxx"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
// Replays, as the runtime publishes them (RuntimeSnapshot::Replays) and the
// shell reads them (ShellView::replays). ready: an archived one can go into
// the game's replay list now (the native main menu, the game's table seen,
// the match list's 30 slots in place). notice: the outcome of the last
// request. logOpens and returns: sf4e__ReplayStore's counts, for Ember's
// menu to get out of the way and to come back on. link: the file a replay
// link asked for, until the player answers.
// archive: the last listing of the archive, or null (platform::replays::WantListing).
struct ReplaysView {
	bool ready = false;
	std::string notice;
	bool noticeError = false;
	std::uint64_t logOpens = 0, returns = 0;
	std::string link;
	// The caption of the export that is recording, to draw over the game.
	replay::Caption caption;
	bool captionShown = false;
	// A replay is playing with the frame meter shown: asked for
	// with it, or turned on (F5) during the playback.
	bool meterShown = false;
	// The replay controls (sf4e__ReplayPlayback.hxx) as this tick
	// left them, and the lanes of the replay Ember plays, or null.
	replaytransport::View playback;
	std::shared_ptr<const replaylane::Lanes> lanes;
	std::shared_ptr<const std::vector<platform::replays::ArchivedReplay>> archive;
	// Completion of the last requested entry (platform::replays::WantDetail).
	replayinputs::DetailCompletion detail;
};
} }
