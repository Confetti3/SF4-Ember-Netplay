#pragma once
#include <cstdint>
#include "../common/ReplayInputLane.hxx"

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
};
// The strip: state, speed, place in the round and the controls' legend, one
// row under the frame meter's band, between the two super meters.
void DrawReplayStrip(const ReplayHudView& view);
// Two narrow columns below the life bars, player 1 at the left edge of the
// game's picture and player 2 at the right.
void DrawReplayLanes(const ReplayHudView& view);
// At most this wide, at 720p, in pixels; it scales like the training HUD.
constexpr float ReplayStripMostWidth = 460, ReplayStripMostHeight = 30;
} }
