#pragma once

// How far a replay's export has got, as sf4e__ReplayStore publishes it and
// the Replays screen shows it, and what its end is called. Nothing here
// touches the game or the encoder, so the rules are tested on their own.

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

// An export takes its picture after Ember's overlay when Ember draws into
// the video: the caption, or the frame meter (drawn over a replay by
// OverlayLayers.cxx). Otherwise it is the game's picture alone.
inline bool ExportDrawsOverlay(bool caption, bool meter) { return caption || meter; }

// Which picture a frame of the export takes, around Ember's overlay
// (sf4e__Platform.cxx). drawsOverlay: ExportDrawsOverlay. menuBefore: Ember's
// menu was shown on the last frame; menuAfter: it is shown on this one. The
// menu, opened over an export to follow or cancel it, stays out of the video:
// while it is shown the picture is taken before the overlay, and the frame it
// opens on is not taken at all.
struct ExportGrab { bool beforeOverlay, afterOverlay; };
inline ExportGrab ExportGrabOf(bool drawsOverlay, bool menuBefore, bool menuAfter) {
	return {!drawsOverlay || menuBefore, drawsOverlay && !menuBefore && !menuAfter};
}

} }
