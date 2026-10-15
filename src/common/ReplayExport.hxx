#pragma once

// How far a replay's export has got, as sf4e__ReplayStore publishes it and
// the Replays screen shows it, and what its end is called. Nothing here
// touches the game or the encoder, so the rules are tested on their own.

#include <cstdint>

namespace sf4e { namespace replay {

// None: no export. Starting: the battle log is opening the replay; nothing
// is recorded yet. Recording: the replay's battle goes to the encoder.
// Finishing: the encoder is closing the file (platform/VideoLink.hxx: Poll
// is Pending). Cancelling: the player cancelled, and the encoder is ending
// without keeping its file.
enum class ExportStage { None, Starting, Recording, Finishing, Cancelling };

// video: an export was asked for and has not ended. recordingAsked: its
// capture was begun. recording: the capture still takes pictures. cancelled:
// the player cancelled it.
inline ExportStage ExportStageOf(bool video, bool recordingAsked, bool recording, bool cancelled) {
	if (!video) return ExportStage::None;
	if (cancelled) return ExportStage::Cancelling;
	if (!recordingAsked) return ExportStage::Starting;
	return recording ? ExportStage::Recording : ExportStage::Finishing;
}

// Cancel can still keep the file from being made: once the encoder is
// closing it, the video is as good as made, and it is kept.
inline bool ExportCancellable(ExportStage stage) {
	return stage == ExportStage::Starting || stage == ExportStage::Recording;
}

// The notice an export ends with. done: the file holds the video, which is
// said even when a cancel came too late to stop it. A cancelled export is
// the player's choice, not a failure.
struct ExportEnd { const char* key; bool error; };
inline ExportEnd ExportEndOf(bool done, bool cancelled) {
	if (done) return {"replays.exported", false};
	if (cancelled) return {"export.cancelled", false};
	return {"replays.gpu_not_exported", true};
}

// How far an export's replay has played, from what the recorder it plays
// from says (ReplayRecorder.hxx) as the playback observes it after each
// battle update (sf4e__ReplayPlayback.hxx): the frames of the rounds before
// the one playing and the farthest that one has got, out of all its rounds'
// frames. The same counts as the replay's length (ReplayInputs.hxx:
// Summary::frames), intros and knockouts left out. A paused or held replay
// leaves the cursor where it is, and a cursor that goes back to the start
// of its round never takes the export back. Reset for each export.
class ExportClock {
public:
	static constexpr int kRounds = 7;
	void Reset() { *this = ExportClock(); }
	// round, cursor: the recorder's; frames: each round's frame count.
	void Observe(int round, std::uint32_t cursor, const std::uint32_t (&frames)[kRounds]) {
		if (round < 0 || round >= kRounds) return;
		total_ = 0;
		for (const std::uint32_t count : frames) total_ += count;
		if (round != round_) { round_ = round; farthest_ = 0; }
		if (cursor > farthest_) farthest_ = cursor;
		played_ = 0;
		for (int before = 0; before < round; before++) played_ += frames[before];
		played_ += farthest_ < frames[round] ? farthest_ : frames[round];
	}
	std::uint32_t Played() const { return played_; }
	std::uint32_t Total() const { return total_; }
private:
	int round_ = -1;
	std::uint32_t farthest_ = 0, played_ = 0, total_ = 0;
};

} }
