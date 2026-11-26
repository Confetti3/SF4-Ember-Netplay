#pragma once
#include <cstdint>
#include <functional>
#include "../common/ReplayInputLane.hxx"
#include "../common/ReplayTakeover.hxx"

namespace sf4e { namespace ui {
// The passive HUD over a replay the game plays: the control strip at the
// bottom centre and the two input lanes at the sides. Presentation only;
// the overlay fills it from the published replay controls
// (common/ReplayTransport.hxx: View) and nothing here reads the game.
struct ReplayHudView {
    // A replay plays, not for a video: the strip and lanes may draw.
    bool shown = false;
    // The controls act now (the fight, no pause menu). Outside it the strip
    // shows the chosen speed dimmed.
    bool armed = false, paused = false;
    int divisor = 1;
    // The recorder's round (from 0) and cursor.
    int round = 0;
    std::uint32_t cursor = 0;
    // The legend's glyphs: the pad's when its last control came from one.
    bool pad = false;
    // The replay's inputs are known, so the legend offers the lanes.
    bool inputsKnown = false;
    // The strip's opacity (ReplayTransport.hxx: StripAlpha); 0 hides it.
    float stripAlpha = 0;
    // A control was pressed outside the fight: "Controls work during the fight".
    bool unavailable = false;
    // The lanes, newest row first.
    bool lanes = false;
    replaylane::Row rows[2][replaylane::kRows];
    int rowCount[2] = {0, 0};
    // The frame meter over the replay; drawn during an export as well.
    bool meter = false;
    // The replay takeover (common/ReplayTakeover.hxx): its stage and side, the
    // countdown's seconds, and that the recording ran out. Its timeline in the
    // battle frames the camera timeline counts (CameraControl.hxx): the frame
    // shown, the earliest a jump reaches, the round's frames and where the
    // last takeover began (-1: none).
    replaytransport::Stage takeover = replaytransport::Stage::Off;
    int takeoverSide = 0, countdownSeconds = 0;
    int frame = 0, floor = 0, total = 0, takeoverFrame = -1;
    bool recordingEnded = false;
    // A takeover could not start or go back, for a while after.
    bool takeoverFailed = false;
};
struct ReplaysView;
// Fills the view from the published controls each frame, timing the strip
// (shown for a while after each control) and the "unavailable" chip (after
// one pressed outside the fight) from the counts' changes. `now` is seconds
// on a clock that outlives ImGui's, which restarts with a device reset.
class ReplayHudFeed {
public:
    void Fill(ReplayHudView& view, const ReplaysView& replays, bool keys, double now);
private:
    std::uint32_t takenSeen_ = 0, refusedSeen_ = 0, failedSeen_ = 0;
    double takenAt_ = -1, refusedAt_ = -1, failedAt_ = -1;
};
// The strip: a button per control, its keys over what it does, then the
// place in the round, at the bottom centre. During a takeover it is the
// takeover's strip, its timeline or its countdown.
void DrawReplayStrip(const ReplayHudView& view);
// The playback's keys this frame, as commands for the replay controls, in order
// (ReplayTransport.hxx: the keyboard's controls).
void ReplayKeys(const replaytransport::View& playback, const std::function<void(replaytransport::Command)>& send);
// Two narrow columns below the life bars, player 1 at the left edge of the
// game's picture and player 2 at the right.
void DrawReplayLanes(const ReplayHudView& view);
} }
